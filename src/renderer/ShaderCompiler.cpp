#include "renderer/ShaderCompiler.hpp"
#include "core/Debug.hpp"
#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <vector>
#ifdef _WIN32
#   include <process.h>
#endif

#if KROM_HAS_SHADERC
#   include <shaderc/shaderc.hpp>
#endif

namespace engine::renderer {
namespace {
std::string ToLower(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

uint64_t HashBytes(const void* data, size_t size) noexcept
{
    const auto* bytes = static_cast<const uint8_t*>(data);
    uint64_t h = 1469598103934665603ull;
    for (size_t i = 0; i < size; ++i)
    {
        h ^= static_cast<uint64_t>(bytes[i]);
        h *= 1099511628211ull;
    }
    return h;
}

void SetError(std::string* outError, const std::string& msg)
{
    if (outError)
        *outError = msg;
}


std::string StageToGlslangSuffix(assets::ShaderStage stage)
{
    switch (stage)
    {
    case assets::ShaderStage::Vertex:   return ".vert";
    case assets::ShaderStage::Fragment: return ".frag";
    case assets::ShaderStage::Compute:  return ".comp";
    case assets::ShaderStage::Geometry: return ".geom";
    case assets::ShaderStage::Hull:     return ".tesc";
    case assets::ShaderStage::Domain:   return ".tese";
    default:                            return ".glsl";
    }
}

std::string BuildShaderSource(const assets::ShaderAsset& asset,
                              const std::vector<std::string>& defines)
{
    std::string source;
    for (const auto& d : defines)
        source += "#define " + d + " 1\n";
    source += asset.sourceCode;
    return source;
}

bool ReadBinaryFile(const std::filesystem::path& path, std::vector<uint8_t>& outBytes)
{
    std::ifstream in(path, std::ios::binary | std::ios::ate);
    if (!in)
        return false;
    const std::streamsize size = in.tellg();
    if (size <= 0)
    {
        outBytes.clear();
        return size == 0;
    }
    outBytes.resize(static_cast<size_t>(size));
    in.seekg(0, std::ios::beg);
    return static_cast<bool>(in.read(reinterpret_cast<char*>(outBytes.data()), size));
}

bool CompileToSpirvWithTool(const assets::ShaderAsset& asset,
                            const std::vector<std::string>& defines,
                            assets::CompiledShaderArtifact& outCompiled,
                            std::string* outError)
{
    namespace fs = std::filesystem;

    fs::path toolPath;
    if (const char* sdk = std::getenv("VULKAN_SDK"))
    {
#ifdef _WIN32
        const fs::path bin = fs::path(sdk) / "Bin";
        const fs::path candidateA = bin / "glslangValidator.exe";
        const fs::path candidateB = bin / "glslc.exe";
#else
        const fs::path bin = fs::path(sdk) / "bin";
        const fs::path candidateA = bin / "glslangValidator";
        const fs::path candidateB = bin / "glslc";
#endif
        if (fs::exists(candidateA))
            toolPath = candidateA;
        else if (fs::exists(candidateB))
            toolPath = candidateB;
    }
    if (toolPath.empty())
    {
#ifdef _WIN32
        toolPath = "glslangValidator.exe";
#else
        toolPath = "glslangValidator";
#endif
    }

    const fs::path tempDir = fs::temp_directory_path() / "krom_shaderc_fallback";
    std::error_code ec;
    fs::create_directories(tempDir, ec);
    const auto unique = std::to_string(static_cast<unsigned long long>(HashBytes(asset.sourceCode.data(), asset.sourceCode.size())));
    const fs::path srcPath = tempDir / (unique + StageToGlslangSuffix(asset.stage));
    const fs::path spvPath = tempDir / (unique + ".spv");
    const fs::path logPath = tempDir / (unique + ".log");

    {
        std::ofstream src(srcPath, std::ios::binary);
        if (!src)
        {
            SetError(outError, "failed to create temporary shader source file");
            return false;
        }
        src << BuildShaderSource(asset, defines);
    }

    const std::string entryPoint = asset.entryPoint.empty() ? "main" : asset.entryPoint;
    int rc = -1;
#ifdef _WIN32
    const std::wstring toolW = toolPath.wstring();
    const std::wstring srcW = srcPath.wstring();
    const std::wstring spvW = spvPath.wstring();
    const std::wstring entryW(entryPoint.begin(), entryPoint.end());
    std::vector<const wchar_t*> args;
    args.push_back(toolW.c_str());
    args.push_back(L"-V");
    args.push_back(L"--target-env");
    args.push_back(L"vulkan1.2");
    args.push_back(L"-e");
    args.push_back(entryW.c_str());
    args.push_back(L"-o");
    args.push_back(spvW.c_str());
    if (asset.sourceLanguage == assets::ShaderSourceLanguage::HLSL)
        args.push_back(L"-D");
    args.push_back(srcW.c_str());
    args.push_back(nullptr);
    rc = _wspawnv(_P_WAIT, toolW.c_str(), args.data());
#else
    std::string command = "\"" + toolPath.string() + "\" -V --target-env vulkan1.2 -e \"" + entryPoint + "\" -o \"" + spvPath.string() + "\" ";
    if (asset.sourceLanguage == assets::ShaderSourceLanguage::HLSL)
        command += "-D ";
    command += "\"" + srcPath.string() + "\"";
    rc = std::system(command.c_str());
#endif
    if (rc != 0 || !fs::exists(spvPath))
    {
        std::string log;
#ifdef _WIN32
        log = "glslangValidator/glslc failed to compile Vulkan SPIR-V artifact";
#else
        {
            std::ifstream in(logPath, std::ios::binary);
            log.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
        }
#endif
        if (log.empty())
            log = "glslangValidator/glslc failed to compile Vulkan SPIR-V artifact";
        SetError(outError, log);
        fs::remove(srcPath, ec);
        fs::remove(spvPath, ec);
        fs::remove(logPath, ec);
        return false;
    }

    if (!ReadBinaryFile(spvPath, outCompiled.bytecode))
    {
        SetError(outError, "failed to read compiled SPIR-V output");
        fs::remove(srcPath, ec);
        fs::remove(spvPath, ec);
        fs::remove(logPath, ec);
        return false;
    }

    outCompiled.sourceText.clear();
    outCompiled.defines = defines;
    outCompiled.sourceHash = HashBytes(outCompiled.bytecode.data(), outCompiled.bytecode.size());
    fs::remove(srcPath, ec);
    fs::remove(spvPath, ec);
    fs::remove(logPath, ec);
    return true;
}

#if KROM_HAS_SHADERC
shaderc_shader_kind ToShadercKind(assets::ShaderStage stage) noexcept
{
    switch (stage)
    {
    case assets::ShaderStage::Vertex:   return shaderc_glsl_vertex_shader;
    case assets::ShaderStage::Fragment: return shaderc_glsl_fragment_shader;
    case assets::ShaderStage::Compute:  return shaderc_glsl_compute_shader;
    case assets::ShaderStage::Geometry: return shaderc_glsl_geometry_shader;
    case assets::ShaderStage::Hull:     return shaderc_glsl_tess_control_shader;
    case assets::ShaderStage::Domain:   return shaderc_glsl_tess_evaluation_shader;
    default:                            return shaderc_glsl_infer_from_source;
    }
}

bool CompileToSpirv(const assets::ShaderAsset& asset,
                    const std::vector<std::string>& defines,
                    assets::CompiledShaderArtifact& outCompiled,
                    std::string* outError)
{
    shaderc::Compiler compiler;
    shaderc::CompileOptions options;
    options.SetTargetEnvironment(shaderc_target_env_vulkan, shaderc_env_version_vulkan_1_2);
    options.SetTargetSpirv(shaderc_spirv_version_1_5);
    options.SetOptimizationLevel(shaderc_optimization_level_zero);
    options.SetGenerateDebugInfo();
    options.SetAutoBindUniforms(false);
    options.SetAutoMapLocations(true);
    options.SetInvertY(false);

    switch (asset.sourceLanguage)
    {
    case assets::ShaderSourceLanguage::HLSL:
        options.SetSourceLanguage(shaderc_source_language_hlsl);
        break;
    case assets::ShaderSourceLanguage::GLSL:
    default:
        options.SetSourceLanguage(shaderc_source_language_glsl);
        break;
    }

    for (const auto& d : defines)
        options.AddMacroDefinition(d);

    const std::string entryPoint = asset.entryPoint.empty() ? "main" : asset.entryPoint;
    const std::string sourceName = asset.debugName.empty() ? asset.path : asset.debugName;
    const shaderc_shader_kind kind = ToShadercKind(asset.stage);

    const auto result = compiler.CompileGlslToSpv(
        asset.sourceCode,
        kind,
        sourceName.c_str(),
        entryPoint.c_str(),
        options);

    if (result.GetCompilationStatus() != shaderc_compilation_status_success)
    {
        SetError(outError, result.GetErrorMessage());
        return false;
    }

    outCompiled.bytecode.resize((result.end() - result.begin()) * sizeof(uint32_t));
    std::memcpy(outCompiled.bytecode.data(), result.begin(), outCompiled.bytecode.size());
    outCompiled.sourceText.clear();
    outCompiled.defines = defines;
    outCompiled.sourceHash = HashBytes(outCompiled.bytecode.data(), outCompiled.bytecode.size());
    return true;
}
#endif
}

assets::ShaderTargetProfile ShaderCompiler::ResolveTargetProfile(const IDevice& device)
{
    const std::string backend = ToLower(device.GetBackendName() ? device.GetBackendName() : "");
    if (backend.find("dx12") != std::string::npos || backend.find("directx12") != std::string::npos)
        return assets::ShaderTargetProfile::DirectX12_SM6;
    if (backend.find("dx11") != std::string::npos || backend.find("directx11") != std::string::npos)
        return assets::ShaderTargetProfile::DirectX11_SM5;
    if (backend.find("vulkan") != std::string::npos)
        return assets::ShaderTargetProfile::Vulkan_SPIRV;
    if (backend.find("opengl") != std::string::npos || backend.find("gl") != std::string::npos)
        return assets::ShaderTargetProfile::OpenGL_GLSL450;
    if (backend.find("null") != std::string::npos)
        return assets::ShaderTargetProfile::Null;
    return assets::ShaderTargetProfile::Generic;
}

const char* ShaderCompiler::ToString(assets::ShaderTargetProfile profile) noexcept
{
    switch (profile)
    {
    case assets::ShaderTargetProfile::Null: return "Null";
    case assets::ShaderTargetProfile::DirectX11_SM5: return "DirectX11_SM5";
    case assets::ShaderTargetProfile::DirectX12_SM6: return "DirectX12_SM6";
    case assets::ShaderTargetProfile::Vulkan_SPIRV: return "Vulkan_SPIRV";
    case assets::ShaderTargetProfile::OpenGL_GLSL450: return "OpenGL_GLSL450";
    default: return "Generic";
    }
}

bool ShaderCompiler::IsRuntimeConsumable(const assets::CompiledShaderArtifact& shader) noexcept
{
    return !shader.bytecode.empty() || !shader.sourceText.empty();
}

bool ShaderCompiler::CompileForTarget(const assets::ShaderAsset& asset,
                                      assets::ShaderTargetProfile target,
                                      assets::CompiledShaderArtifact& outCompiled,
                                      std::string* outError)
{
    outCompiled = {};
    outCompiled.target = target;
    outCompiled.stage = asset.stage;
    outCompiled.entryPoint = asset.entryPoint.empty() ? "main" : asset.entryPoint;
    outCompiled.debugName = asset.debugName;

    if (!asset.bytecode.empty())
    {
        outCompiled.bytecode = asset.bytecode;
        outCompiled.sourceHash = HashBytes(outCompiled.bytecode.data(), outCompiled.bytecode.size());
        return true;
    }

    if (asset.sourceCode.empty())
    {
        SetError(outError, "shader asset has neither source nor bytecode");
        return false;
    }

    switch (target)
    {
    case assets::ShaderTargetProfile::Vulkan_SPIRV:
    {
#if KROM_HAS_SHADERC
        if (!CompileToSpirv(asset, {}, outCompiled, outError))
            return false;
#else
        if (!CompileToSpirvWithTool(asset, {}, outCompiled, outError))
            return false;
#endif
        break;
    }
    case assets::ShaderTargetProfile::DirectX11_SM5:
    case assets::ShaderTargetProfile::DirectX12_SM6:
        outCompiled.sourceText = asset.sourceCode;
        break;
    case assets::ShaderTargetProfile::OpenGL_GLSL450:
    case assets::ShaderTargetProfile::Null:
    case assets::ShaderTargetProfile::Generic:
    default:
        outCompiled.sourceText = asset.sourceCode;
        break;
    }

    if (!outCompiled.bytecode.empty())
        outCompiled.sourceHash = HashBytes(outCompiled.bytecode.data(), outCompiled.bytecode.size());
    else
        outCompiled.sourceHash = HashBytes(outCompiled.sourceText.data(), outCompiled.sourceText.size());

    if (!IsRuntimeConsumable(outCompiled))
    {
        SetError(outError, "compiled shader artifact is empty");
        return false;
    }
    return true;
}

std::vector<std::string> ShaderCompiler::VariantFlagsToDefines(ShaderVariantFlag flags) noexcept
{
    std::vector<std::string> defines;
    if (HasFlag(flags, ShaderVariantFlag::Skinned))     defines.emplace_back("KROM_SKINNING");
    if (HasFlag(flags, ShaderVariantFlag::VertexColor)) defines.emplace_back("KROM_VERTEX_COLOR");
    if (HasFlag(flags, ShaderVariantFlag::AlphaTest))   defines.emplace_back("KROM_ALPHA_TEST");
    if (HasFlag(flags, ShaderVariantFlag::NormalMap))   defines.emplace_back("KROM_NORMAL_MAP");
    if (HasFlag(flags, ShaderVariantFlag::Unlit))       defines.emplace_back("KROM_UNLIT");
    if (HasFlag(flags, ShaderVariantFlag::ShadowPass))  defines.emplace_back("KROM_SHADOW_PASS");
    if (HasFlag(flags, ShaderVariantFlag::Instanced))   defines.emplace_back("KROM_INSTANCED");
    return defines;
}

bool ShaderCompiler::CompileVariant(const assets::ShaderAsset& asset,
                                    assets::ShaderTargetProfile target,
                                    ShaderVariantFlag flags,
                                    assets::CompiledShaderArtifact& outCompiled,
                                    std::string* outError)
{
    const auto defines = VariantFlagsToDefines(flags);

    if (target == assets::ShaderTargetProfile::Vulkan_SPIRV)
    {
#if KROM_HAS_SHADERC
        outCompiled = {};
        outCompiled.target = target;
        outCompiled.stage = asset.stage;
        outCompiled.entryPoint = asset.entryPoint.empty() ? "main" : asset.entryPoint;
        outCompiled.debugName = asset.debugName;
        return CompileToSpirv(asset, defines, outCompiled, outError);
#else
        outCompiled = {};
        outCompiled.target = target;
        outCompiled.stage = asset.stage;
        outCompiled.entryPoint = asset.entryPoint.empty() ? "main" : asset.entryPoint;
        outCompiled.debugName = asset.debugName;
        return CompileToSpirvWithTool(asset, defines, outCompiled, outError);
#endif
    }

    if (!CompileForTarget(asset, target, outCompiled, outError))
        return false;

    outCompiled.defines = defines;

    uint64_t h = outCompiled.sourceHash;
    for (const auto& d : outCompiled.defines)
    {
        h ^= HashBytes(d.data(), d.size());
        h *= 1099511628211ull;
    }
    outCompiled.sourceHash = h;
    return true;
}

} // namespace engine::renderer
