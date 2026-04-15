#include "ShaderCompilerInternal.hpp"
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

namespace engine::renderer::internal {


bool CompileToSpirvWithTool(const assets::ShaderAsset& asset,
                            const SourceBundle& bundle,
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
    const auto unique = Hex64(HashBytes(bundle.preprocessedSource.data(), bundle.preprocessedSource.size())) + "_" + Hex64(HashString(asset.entryPoint));
    const fs::path srcPath = tempDir / (unique + StageToToolExtension(asset.stage, asset.sourceLanguage));
    const fs::path spvPath = tempDir / (unique + ".spv");

    {
        std::ofstream src(srcPath, std::ios::binary | std::ios::trunc);
        if (!src)
        {
            SetError(outError, "failed to create temporary shader source file");
            return false;
        }
        src << BuildShaderSource(bundle, defines);
    }

    const std::string entryPoint = asset.entryPoint.empty() ? "main" : asset.entryPoint;
    intptr_t rc = static_cast<intptr_t>(-1);
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
        SetError(outError, "glslangValidator/glslc failed to compile Vulkan SPIR-V artifact");
        fs::remove(srcPath, ec);
        fs::remove(spvPath, ec);
        return false;
    }

    if (!ReadBinaryFile(spvPath, outCompiled.bytecode))
    {
        SetError(outError, "failed to read compiled SPIR-V output");
        fs::remove(srcPath, ec);
        fs::remove(spvPath, ec);
        return false;
    }

    outCompiled.sourceText.clear();
    fs::remove(srcPath, ec);
    fs::remove(spvPath, ec);
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
                    const SourceBundle& bundle,
                    const std::vector<std::string>& defines,
                    assets::CompiledShaderArtifact& outCompiled,
                    std::string* outError)
{
    shaderc::Compiler compiler;
    shaderc::CompileOptions options;
    options.SetTargetEnvironment(shaderc_target_env_vulkan, shaderc_env_version_vulkan_1_2);
    options.SetTargetSpirv(shaderc_spirv_version_1_5);
#ifndef NDEBUG
    options.SetOptimizationLevel(shaderc_optimization_level_zero);
    options.SetGenerateDebugInfo();
#else
    options.SetOptimizationLevel(shaderc_optimization_level_performance);
#endif
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
    const std::string sourceName = bundle.canonicalSourcePath.empty() ? (asset.debugName.empty() ? asset.path : asset.debugName) : bundle.canonicalSourcePath.string();
    const shaderc_shader_kind kind = ToShadercKind(asset.stage);
    const std::string mergedSource = bundle.preprocessedSource;
    const auto result = compiler.CompileGlslToSpv(
        mergedSource,
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
    return true;
}
#endif

} // namespace engine::renderer::internal
