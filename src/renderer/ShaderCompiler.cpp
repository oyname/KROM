#include "renderer/ShaderCompiler.hpp"
#include "renderer/ShaderBindingModel.hpp"
#include "core/Debug.hpp"
#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>
#include <string_view>
#include <vector>
#ifdef _WIN32
#   include <process.h>
#   include <windows.h>
#   include <d3dcompiler.h>
#endif

#if KROM_HAS_SHADERC
#   include <shaderc/shaderc.hpp>
#endif

namespace engine::renderer {
ShaderTargetApi ResolveTargetApiNameSpaceSafe(assets::ShaderTargetProfile profile) noexcept;
namespace {
void SetError(std::string* outError, const std::string& msg);
#ifdef _WIN32
using D3DCompileFn = HRESULT (WINAPI*)(LPCVOID, SIZE_T, LPCSTR, const D3D_SHADER_MACRO*, ID3DInclude*, LPCSTR, LPCSTR, UINT, UINT, ID3DBlob**, ID3DBlob**);

D3DCompileFn ResolveD3DCompile(std::string* outError)
{
    static D3DCompileFn fn = nullptr;
    static bool attempted = false;
    if (attempted)
    {
        if (!fn)
            SetError(outError, "failed to load D3DCompile from d3dcompiler DLL");
        return fn;
    }

    attempted = true;
    const wchar_t* dllNames[] = { L"d3dcompiler_47.dll", L"d3dcompiler_46.dll", L"d3dcompiler_43.dll" };
    for (const wchar_t* dllName : dllNames)
    {
        HMODULE module = ::LoadLibraryW(dllName);
        if (!module)
            continue;
        fn = reinterpret_cast<D3DCompileFn>(::GetProcAddress(module, "D3DCompile"));
        if (fn)
            return fn;
    }

    SetError(outError, "failed to load D3DCompile from d3dcompiler_47/46/43.dll");
    return nullptr;
}
#endif

constexpr uint32_t kShaderArtifactCacheSchemaVersion = 4u;
constexpr uint32_t kShaderArtifactCacheLegacySchemaVersion = 5u;
constexpr std::string_view kCacheMagic = "KROM_SHADER_CACHE_V1";

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

uint64_t HashString(std::string_view value) noexcept
{
    return HashBytes(value.data(), value.size());
}

uint64_t HashCombine(uint64_t seed, uint64_t value) noexcept
{
    seed ^= value + 0x9e3779b97f4a7c15ull + (seed << 6u) + (seed >> 2u);
    return seed;
}

std::string Hex64(uint64_t value)
{
    static constexpr char kHex[] = "0123456789abcdef";
    std::string out(16u, '0');
    for (int i = 15; i >= 0; --i)
    {
        out[static_cast<size_t>(i)] = kHex[value & 0xFu];
        value >>= 4u;
    }
    return out;
}

void SetError(std::string* outError, const std::string& msg)
{
    if (outError)
        *outError = msg;
}

uint32_t StageMaskBits(assets::ShaderStage stage) noexcept
{
    return 1u << static_cast<uint32_t>(stage);
}

void AppendEngineBinding(std::vector<ShaderBindingDeclaration>& bindings,
                         const char* name,
                         uint32_t logicalSlot,
                         uint32_t apiBinding,
                         uint32_t space,
                         ShaderBindingClass bindingClass,
                         uint32_t stageMask)
{
    ShaderBindingDeclaration decl{};
    decl.name = name;
    decl.logicalSlot = logicalSlot;
    decl.apiBinding = apiBinding;
    decl.space = space;
    decl.bindingClass = bindingClass;
    decl.stageMask = stageMask;
    bindings.push_back(std::move(decl));
}

ShaderInterfaceLayout BuildEngineInterfaceLayout(assets::ShaderStage stage)
{
    ShaderInterfaceLayout layout{};
    layout.usesEngineBindingModel = true;
    const uint32_t stageMask = StageMaskBits(stage);

    AppendEngineBinding(layout.bindings, "PerFrame", CBSlots::PerFrame, BindingRegisterRanges::CB(CBSlots::PerFrame), BindingRegisterRanges::RegisterSpace, ShaderBindingClass::ConstantBuffer, stageMask);
    AppendEngineBinding(layout.bindings, "PerObject", CBSlots::PerObject, BindingRegisterRanges::CB(CBSlots::PerObject), BindingRegisterRanges::RegisterSpace, ShaderBindingClass::ConstantBuffer, stageMask);
    AppendEngineBinding(layout.bindings, "PerMaterial", CBSlots::PerMaterial, BindingRegisterRanges::CB(CBSlots::PerMaterial), BindingRegisterRanges::RegisterSpace, ShaderBindingClass::ConstantBuffer, stageMask);
    AppendEngineBinding(layout.bindings, "PerPass", CBSlots::PerPass, BindingRegisterRanges::CB(CBSlots::PerPass), BindingRegisterRanges::RegisterSpace, ShaderBindingClass::ConstantBuffer, stageMask);

    if (stage == assets::ShaderStage::Fragment || stage == assets::ShaderStage::Compute)
    {
        for (uint32_t i = 0u; i < TexSlots::COUNT; ++i)
            AppendEngineBinding(layout.bindings, "Texture", i, BindingRegisterRanges::SRV(i), BindingRegisterRanges::RegisterSpace, ShaderBindingClass::ShaderResource, stageMask);
        for (uint32_t i = 0u; i < SamplerSlots::COUNT; ++i)
            AppendEngineBinding(layout.bindings, "Sampler", i, BindingRegisterRanges::SMP(i), BindingRegisterRanges::RegisterSpace, ShaderBindingClass::Sampler, stageMask);
    }

    if (stage == assets::ShaderStage::Compute)
    {
        for (uint32_t i = 0u; i < UAVSlots::COUNT; ++i)
            AppendEngineBinding(layout.bindings, "UAV", i, BindingRegisterRanges::UAV(i), BindingRegisterRanges::RegisterSpace, ShaderBindingClass::UnorderedAccess, stageMask);
    }

    uint64_t hash = 1469598103934665603ull;
    for (const auto& binding : layout.bindings)
    {
        hash = HashCombine(hash, HashString(binding.name));
        hash = HashCombine(hash, binding.logicalSlot);
        hash = HashCombine(hash, binding.apiBinding);
        hash = HashCombine(hash, binding.space);
        hash = HashCombine(hash, static_cast<uint64_t>(binding.bindingClass));
        hash = HashCombine(hash, binding.stageMask);
    }
    layout.layoutHash = hash;
    return layout;
}


uint64_t HashBindingLayoutDescLocal(const PipelineBindingLayoutDesc& desc)
{
    return HashPipelineBindingLayoutDesc(desc);
}


uint64_t HashDescriptorRuntimeLayoutDescLocal(const DescriptorRuntimeLayoutDesc& desc)
{
    return engine::renderer::HashDescriptorRuntimeLayoutDesc(desc);
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

std::string StageToToolExtension(assets::ShaderStage stage, assets::ShaderSourceLanguage language)
{
    if (language == assets::ShaderSourceLanguage::HLSL)
    {
        switch (stage)
        {
        case assets::ShaderStage::Vertex:   return ".vert.hlsl";
        case assets::ShaderStage::Fragment: return ".frag.hlsl";
        case assets::ShaderStage::Compute:  return ".comp.hlsl";
        case assets::ShaderStage::Geometry: return ".geom.hlsl";
        case assets::ShaderStage::Hull:     return ".tesc.hlsl";
        case assets::ShaderStage::Domain:   return ".tese.hlsl";
        default:                            return ".shader.hlsl";
        }
    }
    return StageToGlslangSuffix(stage);
}

std::string Trim(std::string_view value)
{
    size_t begin = 0u;
    size_t end = value.size();
    while (begin < end && std::isspace(static_cast<unsigned char>(value[begin])))
        ++begin;
    while (end > begin && std::isspace(static_cast<unsigned char>(value[end - 1u])))
        --end;
    return std::string(value.substr(begin, end - begin));
}

std::filesystem::path GetShaderCacheRoot()
{
    return std::filesystem::current_path() / ".krom" / "shader_artifacts";
}

struct SourceBundle
{
    std::filesystem::path canonicalSourcePath;
    std::string preprocessedSource;
    std::vector<assets::ShaderDependencyRecord> dependencies;
};

bool ReadBinaryFile(const std::filesystem::path& path, std::vector<uint8_t>& outBytes)
{
    std::ifstream in(path, std::ios::binary | std::ios::ate);
    if (!in)
        return false;
    const std::streamsize size = in.tellg();
    if (size < 0)
        return false;
    outBytes.resize(static_cast<size_t>(size));
    in.seekg(0, std::ios::beg);
    return size == 0 ? true : static_cast<bool>(in.read(reinterpret_cast<char*>(outBytes.data()), size));
}

bool ReadTextFile(const std::filesystem::path& path, std::string& outText)
{
    std::ifstream in(path, std::ios::binary);
    if (!in)
        return false;
    outText.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    return true;
}

std::filesystem::path NormalizePath(const std::filesystem::path& path)
{
    std::error_code ec;
    const auto weak = std::filesystem::weakly_canonical(path, ec);
    return ec ? path.lexically_normal() : weak;
}

bool ParseInclude(std::string_view line, std::string& outInclude)
{
    const std::string trimmed = Trim(line);
    if (trimmed.rfind("#include", 0u) != 0u)
        return false;

    const size_t firstQuote = trimmed.find('"');
    const size_t lastQuote = trimmed.find_last_of('"');
    if (firstQuote != std::string::npos && lastQuote != std::string::npos && lastQuote > firstQuote)
    {
        outInclude = trimmed.substr(firstQuote + 1u, lastQuote - firstQuote - 1u);
        return !outInclude.empty();
    }

    const size_t firstAngle = trimmed.find('<');
    const size_t lastAngle = trimmed.find_last_of('>');
    if (firstAngle != std::string::npos && lastAngle != std::string::npos && lastAngle > firstAngle)
    {
        outInclude = trimmed.substr(firstAngle + 1u, lastAngle - firstAngle - 1u);
        return !outInclude.empty();
    }

    return false;
}

bool ExpandIncludesRecursive(const std::filesystem::path& path,
                             std::set<std::string>& recursionGuard,
                             std::set<std::string>& dependencySet,
                             SourceBundle& outBundle,
                             std::string* outError)
{
    const std::filesystem::path canonical = NormalizePath(path);
    const std::string canonicalKey = canonical.generic_string();
    if (recursionGuard.count(canonicalKey) > 0u)
    {
        SetError(outError, "cyclic shader include detected: " + canonicalKey);
        return false;
    }

    std::string source;
    if (!ReadTextFile(canonical, source))
    {
        SetError(outError, "failed to read shader source/include: " + canonicalKey);
        return false;
    }

    recursionGuard.insert(canonicalKey);
    if (dependencySet.insert(canonicalKey).second)
    {
        assets::ShaderDependencyRecord dep{};
        dep.path = canonicalKey;
        dep.contentHash = HashBytes(source.data(), source.size());
        outBundle.dependencies.push_back(std::move(dep));
    }

    std::istringstream input(source);
    std::string line;
    const auto parent = canonical.parent_path();
    while (std::getline(input, line))
    {
        std::string includePath;
        if (ParseInclude(line, includePath))
        {
            const std::filesystem::path resolved = NormalizePath(parent / includePath);
            outBundle.preprocessedSource += "\n/*BEGIN_INCLUDE:" + resolved.generic_string() + "*/\n";
            if (!ExpandIncludesRecursive(resolved, recursionGuard, dependencySet, outBundle, outError))
                return false;
            outBundle.preprocessedSource += "\n/*END_INCLUDE:" + resolved.generic_string() + "*/\n";
            continue;
        }
        outBundle.preprocessedSource += line;
        outBundle.preprocessedSource.push_back('\n');
    }

    recursionGuard.erase(canonicalKey);
    return true;
}

SourceBundle BuildSourceBundle(const assets::ShaderAsset& asset, std::string* outError)
{
    SourceBundle bundle{};

    if (!asset.resolvedPath.empty())
    {
        bundle.canonicalSourcePath = NormalizePath(asset.resolvedPath);
        std::set<std::string> recursionGuard;
        std::set<std::string> dependencySet;
        if (!ExpandIncludesRecursive(bundle.canonicalSourcePath, recursionGuard, dependencySet, bundle, outError))
            return {};
        std::sort(bundle.dependencies.begin(), bundle.dependencies.end(), [](const auto& a, const auto& b) {
            return a.path < b.path;
        });
        return bundle;
    }

    bundle.preprocessedSource = asset.sourceCode;
    assets::ShaderDependencyRecord dep{};
    dep.path = asset.path.empty() ? asset.debugName : asset.path;
    dep.contentHash = HashBytes(asset.sourceCode.data(), asset.sourceCode.size());
    bundle.dependencies.push_back(std::move(dep));
    return bundle;
}

std::string BuildShaderSource(const SourceBundle& bundle, const std::vector<std::string>& defines)
{
    std::string source;
    source.reserve(bundle.preprocessedSource.size() + (defines.size() * 32u));
    for (const auto& d : defines)
        source += "#define " + d + " 1\n";
    source += bundle.preprocessedSource;
    return source;
}

std::string GetTargetProfileString(assets::ShaderTargetProfile profile)
{
    return ShaderCompiler::ToString(profile);
}

std::string GetBinaryFormatString(ShaderBinaryFormat format)
{
    switch (format)
    {
    case ShaderBinaryFormat::SourceText: return "source";
    case ShaderBinaryFormat::Spirv: return "spirv";
    case ShaderBinaryFormat::Dxbc: return "dxbc";
    case ShaderBinaryFormat::Dxil: return "dxil";
    case ShaderBinaryFormat::GlslSource: return "glsl";
    default: return "none";
    }
}

std::string BuildArtifactCacheKeyForSchema(const assets::ShaderAsset& asset,
                                           assets::ShaderTargetProfile target,
                                           ShaderBinaryFormat binaryFormat,
                                           const ShaderInterfaceLayout& interfaceLayout,
                                           const SourceBundle& bundle,
                                           const std::vector<std::string>& defines,
                                           uint32_t schemaVersion)
{
    uint64_t h0 = 1469598103934665603ull;
    uint64_t h1 = 1099511628211ull;

    auto mix = [&](std::string_view label, uint64_t value)
    {
        h0 = HashCombine(h0, HashString(label));
        h0 = HashCombine(h0, value);
        h1 = HashCombine(h1, value ^ HashString(label));
    };

    mix("schema", schemaVersion);
    mix("stage", static_cast<uint64_t>(asset.stage));
    mix("source_language", static_cast<uint64_t>(asset.sourceLanguage));
    mix("target", static_cast<uint64_t>(target));
    mix("binary_format", static_cast<uint64_t>(binaryFormat));
    mix("interface_layout", interfaceLayout.layoutHash);
    mix("entry_point", HashString(asset.entryPoint.empty() ? std::string_view("main") : std::string_view(asset.entryPoint)));
    mix("source_path", HashString(bundle.canonicalSourcePath.generic_string()));
    mix("source_text", HashBytes(bundle.preprocessedSource.data(), bundle.preprocessedSource.size()));

    for (const auto& dep : bundle.dependencies)
    {
        mix("dep_path", HashString(dep.path));
        mix("dep_hash", dep.contentHash);
    }

    for (const auto& def : defines)
        mix("define", HashString(def));

    std::ostringstream oss;
    oss << "v" << schemaVersion
        << '_' << Hex64(h0)
        << Hex64(h1)
        << '_' << GetTargetProfileString(target)
        << '_' << GetBinaryFormatString(binaryFormat);
    return oss.str();
}


std::string BuildArtifactCacheKey(const assets::ShaderAsset& asset,
                                  assets::ShaderTargetProfile target,
                                  ShaderBinaryFormat binaryFormat,
                                  const ShaderInterfaceLayout& interfaceLayout,
                                  const SourceBundle& bundle,
                                  const std::vector<std::string>& defines)
{
    return BuildArtifactCacheKeyForSchema(asset, target, binaryFormat, interfaceLayout, bundle, defines, kShaderArtifactCacheSchemaVersion);
}

struct BinaryReader
{
    const std::vector<uint8_t>& bytes;
    size_t offset = 0u;

    template<typename T>
    bool Read(T& out)
    {
        if (offset + sizeof(T) > bytes.size())
            return false;
        std::memcpy(&out, bytes.data() + offset, sizeof(T));
        offset += sizeof(T);
        return true;
    }

    bool ReadString(std::string& out)
    {
        uint32_t size = 0u;
        if (!Read(size) || offset + size > bytes.size())
            return false;
        out.assign(reinterpret_cast<const char*>(bytes.data() + offset), size);
        offset += size;
        return true;
    }

    bool ReadBytes(std::vector<uint8_t>& out)
    {
        uint32_t size = 0u;
        if (!Read(size) || offset + size > bytes.size())
            return false;
        out.assign(bytes.begin() + static_cast<std::ptrdiff_t>(offset), bytes.begin() + static_cast<std::ptrdiff_t>(offset + size));
        offset += size;
        return true;
    }
};

struct BinaryWriter
{
    std::vector<uint8_t> bytes;

    template<typename T>
    void Write(const T& value)
    {
        const auto* ptr = reinterpret_cast<const uint8_t*>(&value);
        bytes.insert(bytes.end(), ptr, ptr + sizeof(T));
    }

    void WriteString(const std::string& value)
    {
        const uint32_t size = static_cast<uint32_t>(value.size());
        Write(size);
        bytes.insert(bytes.end(), value.begin(), value.end());
    }

    void WriteBytes(const std::vector<uint8_t>& value)
    {
        const uint32_t size = static_cast<uint32_t>(value.size());
        Write(size);
        bytes.insert(bytes.end(), value.begin(), value.end());
    }
};

bool LoadArtifactFromDisk(const std::filesystem::path& cachePath,
                          const std::string& expectedCacheKey,
                          assets::CompiledShaderArtifact& outCompiled)
{
    std::vector<uint8_t> fileBytes;
    if (!ReadBinaryFile(cachePath, fileBytes))
        return false;

    BinaryReader reader{ fileBytes };
    std::string magic;
    if (!reader.ReadString(magic) || magic != kCacheMagic)
        return false;

    uint32_t schemaVersion = 0u;
    if (!reader.Read(schemaVersion) || schemaVersion != kShaderArtifactCacheSchemaVersion)
        return false;

    uint32_t target = 0u;
    uint32_t stage = 0u;
    uint32_t binaryFormat = 0u;
    uint32_t stageMask = 0u;
    uint64_t sourceHash = 0ull;
    uint64_t layoutHash = 0ull;
    uint64_t contractHash = 0ull;
    uint8_t usesBindingModel = 0u;
    if (!reader.Read(target) || !reader.Read(stage) || !reader.Read(binaryFormat) || !reader.Read(stageMask) ||
        !reader.Read(sourceHash) || !reader.Read(layoutHash) || !reader.Read(contractHash) || !reader.Read(usesBindingModel))
    {
        return false;
    }

    if (!reader.ReadString(outCompiled.entryPoint) ||
        !reader.ReadString(outCompiled.debugName) ||
        !reader.ReadString(outCompiled.cacheKey) ||
        !reader.ReadString(outCompiled.sourceText) ||
        !reader.ReadBytes(outCompiled.bytecode))
    {
        return false;
    }

    uint32_t defineCount = 0u;
    if (!reader.Read(defineCount))
        return false;
    outCompiled.defines.clear();
    outCompiled.defines.reserve(defineCount);
    for (uint32_t i = 0; i < defineCount; ++i)
    {
        std::string value;
        if (!reader.ReadString(value))
            return false;
        outCompiled.defines.push_back(std::move(value));
    }

    uint32_t depCount = 0u;
    if (!reader.Read(depCount))
        return false;
    outCompiled.dependencies.clear();
    outCompiled.dependencies.reserve(depCount);
    for (uint32_t i = 0; i < depCount; ++i)
    {
        assets::ShaderDependencyRecord dep{};
        if (!reader.ReadString(dep.path) || !reader.Read(dep.contentHash))
            return false;
        outCompiled.dependencies.push_back(std::move(dep));
    }

    if (outCompiled.cacheKey != expectedCacheKey)
        return false;

    outCompiled.cacheSchemaVersion = schemaVersion;
    outCompiled.target = static_cast<assets::ShaderTargetProfile>(target);
    outCompiled.stage = static_cast<assets::ShaderStage>(stage);
    outCompiled.sourceHash = sourceHash;
    outCompiled.contract.api = ResolveTargetApiNameSpaceSafe(outCompiled.target);
    outCompiled.contract.binaryFormat = static_cast<ShaderBinaryFormat>(binaryFormat);
    outCompiled.contract.stageMask = stageMask;
    outCompiled.contract.interfaceLayout = BuildEngineInterfaceLayout(outCompiled.stage);
    outCompiled.contract.interfaceLayout.layoutHash = layoutHash;
    outCompiled.contract.interfaceLayout.usesEngineBindingModel = usesBindingModel != 0u;
    outCompiled.contract.pipelineBinding.bindingLayout = BuildEnginePipelineBindingLayout();
    outCompiled.contract.pipelineBinding.runtimeLayout = BuildEngineDescriptorRuntimeLayout();
    outCompiled.contract.pipelineBinding.bindingSignature = DerivePipelineBindingSignatureDesc(outCompiled.contract.pipelineBinding.runtimeLayout);
    outCompiled.contract.pipelineBinding.bindingLayoutHash = HashBindingLayoutDescLocal(outCompiled.contract.pipelineBinding.bindingLayout);
    outCompiled.contract.pipelineBinding.runtimeLayoutHash = HashDescriptorRuntimeLayoutDescLocal(outCompiled.contract.pipelineBinding.runtimeLayout);
    outCompiled.contract.pipelineBinding.bindingSignatureHash = HashPipelineBindingSignatureDesc(outCompiled.contract.pipelineBinding.bindingSignature);
    outCompiled.contract.pipelineBinding.interfaceLayoutHash = layoutHash;
    outCompiled.contract.pipelineBinding.bindingSignatureKey = BuildPipelineBindingSignatureKey(outCompiled.contract.pipelineBinding.runtimeLayout,
                                                                                layoutHash,
                                                                                outCompiled.contract.pipelineBinding.bindingSignature.staticSamplerPolicy);
    outCompiled.contract.pipelineBinding.computeRuntime = ComputeRuntimeDesc{};
    outCompiled.contract.pipelineBinding.computeRuntimeHash = HashComputeRuntimeDesc(outCompiled.contract.pipelineBinding.computeRuntime);
    outCompiled.contract.pipelineStateKey = HashCombine(outCompiled.contract.pipelineBinding.bindingSignatureKey,
                                                        HashCombine(outCompiled.contract.interfaceLayout.layoutHash,
                                                                    HashCombine(static_cast<uint64_t>(outCompiled.contract.stageMask),
                                                                                HashCombine(static_cast<uint64_t>(outCompiled.contract.binaryFormat),
                                                                                            outCompiled.contract.pipelineBinding.computeRuntimeHash))));
    outCompiled.contract.contractHash = contractHash;
    return outCompiled.IsValid();
}

bool SaveArtifactToDisk(const std::filesystem::path& cachePath,
                        const assets::CompiledShaderArtifact& compiled)
{
    BinaryWriter writer{};
    writer.WriteString(std::string(kCacheMagic));
    writer.Write(compiled.cacheSchemaVersion);
    writer.Write(static_cast<uint32_t>(compiled.target));
    writer.Write(static_cast<uint32_t>(compiled.stage));
    writer.Write(static_cast<uint32_t>(compiled.contract.binaryFormat));
    writer.Write(compiled.contract.stageMask);
    writer.Write(compiled.sourceHash);
    writer.Write(compiled.contract.interfaceLayout.layoutHash);
    writer.Write(compiled.contract.contractHash);
    const uint8_t usesBindingModel = compiled.contract.interfaceLayout.usesEngineBindingModel ? 1u : 0u;
    writer.Write(usesBindingModel);
    writer.WriteString(compiled.entryPoint);
    writer.WriteString(compiled.debugName);
    writer.WriteString(compiled.cacheKey);
    writer.WriteString(compiled.sourceText);
    writer.WriteBytes(compiled.bytecode);

    const uint32_t defineCount = static_cast<uint32_t>(compiled.defines.size());
    writer.Write(defineCount);
    for (const auto& def : compiled.defines)
        writer.WriteString(def);

    const uint32_t depCount = static_cast<uint32_t>(compiled.dependencies.size());
    writer.Write(depCount);
    for (const auto& dep : compiled.dependencies)
    {
        writer.WriteString(dep.path);
        writer.Write(dep.contentHash);
    }

    std::error_code ec;
    std::filesystem::create_directories(cachePath.parent_path(), ec);
    const auto tempPath = cachePath.string() + ".tmp";
    {
        std::ofstream out(tempPath, std::ios::binary | std::ios::trunc);
        if (!out)
            return false;
        out.write(reinterpret_cast<const char*>(writer.bytes.data()), static_cast<std::streamsize>(writer.bytes.size()));
        if (!out)
            return false;
    }
    std::filesystem::rename(tempPath, cachePath, ec);
    if (ec)
    {
        std::filesystem::remove(cachePath, ec);
        ec.clear();
        std::filesystem::rename(tempPath, cachePath, ec);
    }
    return !ec;
}

#ifdef _WIN32
std::string GetHlslTargetProfile(assets::ShaderStage stage, assets::ShaderTargetProfile target)
{
    const char* suffix = "vs_5_0";
    switch (stage)
    {
    case assets::ShaderStage::Vertex: suffix = target == assets::ShaderTargetProfile::DirectX12_SM6 ? "vs_6_0" : "vs_5_0"; break;
    case assets::ShaderStage::Fragment: suffix = target == assets::ShaderTargetProfile::DirectX12_SM6 ? "ps_6_0" : "ps_5_0"; break;
    case assets::ShaderStage::Compute: suffix = target == assets::ShaderTargetProfile::DirectX12_SM6 ? "cs_6_0" : "cs_5_0"; break;
    case assets::ShaderStage::Geometry: suffix = target == assets::ShaderTargetProfile::DirectX12_SM6 ? "gs_6_0" : "gs_5_0"; break;
    case assets::ShaderStage::Hull: suffix = target == assets::ShaderTargetProfile::DirectX12_SM6 ? "hs_6_0" : "hs_5_0"; break;
    case assets::ShaderStage::Domain: suffix = target == assets::ShaderTargetProfile::DirectX12_SM6 ? "ds_6_0" : "ds_5_0"; break;
    }
    return suffix;
}
#endif

bool CompileToD3DBytecode(const assets::ShaderAsset& asset,
                          assets::ShaderTargetProfile target,
                          const SourceBundle& bundle,
                          const std::vector<std::string>& defines,
                          assets::CompiledShaderArtifact& outCompiled,
                          std::string* outError)
{
#ifdef _WIN32
    if (asset.sourceLanguage != assets::ShaderSourceLanguage::HLSL)
    {
        SetError(outError, "DirectX shader compilation requires HLSL shader sources");
        return false;
    }

    std::vector<D3D_SHADER_MACRO> macros;
    macros.reserve(defines.size() + 1u);
    for (const auto& def : defines)
    {
        D3D_SHADER_MACRO macro{};
        macro.Name = def.c_str();
        macro.Definition = "1";
        macros.push_back(macro);
    }
    macros.push_back({ nullptr, nullptr });

    UINT flags = D3DCOMPILE_ENABLE_STRICTNESS;
#ifndef NDEBUG
    flags |= D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#else
    flags |= D3DCOMPILE_OPTIMIZATION_LEVEL3;
#endif

    ID3DBlob* code = nullptr;
    ID3DBlob* errors = nullptr;
    const D3DCompileFn d3dCompile = ResolveD3DCompile(outError);
    if (!d3dCompile)
        return false;

    const std::string sourceName = bundle.canonicalSourcePath.empty() ? (asset.debugName.empty() ? asset.path : asset.debugName) : bundle.canonicalSourcePath.string();
    const HRESULT hr = d3dCompile(bundle.preprocessedSource.data(),
                                  bundle.preprocessedSource.size(),
                                  sourceName.c_str(),
                                  macros.data(),
                                  nullptr,
                                  asset.entryPoint.empty() ? "main" : asset.entryPoint.c_str(),
                                  GetHlslTargetProfile(asset.stage, target).c_str(),
                                  flags,
                                  0u,
                                  &code,
                                  &errors);
    if (FAILED(hr) || !code)
    {
        std::string error = "D3DCompile failed";
        if (errors && errors->GetBufferPointer())
            error.assign(static_cast<const char*>(errors->GetBufferPointer()), errors->GetBufferSize());
        if (errors) errors->Release();
        if (code) code->Release();
        SetError(outError, error);
        return false;
    }

    outCompiled.bytecode.assign(static_cast<const uint8_t*>(code->GetBufferPointer()),
                                static_cast<const uint8_t*>(code->GetBufferPointer()) + code->GetBufferSize());
    outCompiled.sourceText.clear();
    if (errors) errors->Release();
    code->Release();
    return true;
#else
    (void)asset; (void)target; (void)bundle; (void)defines; (void)outCompiled;
    SetError(outError, "DirectX bytecode compilation is only available on Windows builds");
    return false;
#endif
}

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

bool CompileToDxilWithTool(const assets::ShaderAsset& asset,
                           const SourceBundle& bundle,
                           const std::vector<std::string>& defines,
                           assets::CompiledShaderArtifact& outCompiled,
                           std::string* outError)
{
#ifdef _WIN32
    if (asset.sourceLanguage != assets::ShaderSourceLanguage::HLSL)
    {
        SetError(outError, "DirectX12 SM6/DXIL compilation requires HLSL shader sources");
        return false;
    }

    namespace fs = std::filesystem;

    fs::path toolPath;
    if (const char* dxcPath = std::getenv("DXC_PATH"))
    {
        const fs::path candidate = fs::path(dxcPath);
        if (fs::exists(candidate))
            toolPath = candidate;
    }

    if (toolPath.empty())
    {
        if (const char* sdk = std::getenv("VULKAN_SDK"))
        {
            const fs::path bin = fs::path(sdk) / "Bin";
            const fs::path candidate = bin / "dxc.exe";
            if (fs::exists(candidate))
                toolPath = candidate;
        }
    }

    if (toolPath.empty())
    {
        const wchar_t* pathVar = _wgetenv(L"PATH");
        if (pathVar && *pathVar)
        {
            std::wstring pathList(pathVar);
            size_t start = 0u;
            while (start <= pathList.size())
            {
                const size_t end = pathList.find(L';', start);
                const std::wstring segment = pathList.substr(start, end == std::wstring::npos ? std::wstring::npos : end - start);
                if (!segment.empty())
                {
                    const fs::path candidate = fs::path(segment) / "dxc.exe";
                    if (fs::exists(candidate))
                    {
                        toolPath = candidate;
                        break;
                    }
                }

                if (end == std::wstring::npos)
                    break;
                start = end + 1u;
            }
        }
    }

    if (toolPath.empty())
    {
        SetError(outError, "DirectX12 SM6/DXIL compilation requires dxc.exe (set DXC_PATH or install it in PATH/VULKAN_SDK/Bin)");
        return false;
    }

    const fs::path tempDir = fs::temp_directory_path() / "krom_dxc_fallback";
    std::error_code ec;
    fs::create_directories(tempDir, ec);
    const auto unique = Hex64(HashBytes(bundle.preprocessedSource.data(), bundle.preprocessedSource.size())) + "_" + Hex64(HashString(asset.entryPoint));
    const fs::path srcPath = tempDir / (unique + StageToToolExtension(asset.stage, assets::ShaderSourceLanguage::HLSL));
    const fs::path dxilPath = tempDir / (unique + ".dxil");

    {
        std::ofstream src(srcPath, std::ios::binary | std::ios::trunc);
        if (!src)
        {
            SetError(outError, "failed to create temporary HLSL shader source file");
            return false;
        }
        src << BuildShaderSource(bundle, defines);
    }

    const std::string entryPoint = asset.entryPoint.empty() ? "main" : asset.entryPoint;
    const std::string targetProfile = GetHlslTargetProfile(asset.stage, assets::ShaderTargetProfile::DirectX12_SM6);
    const std::wstring toolW = toolPath.wstring();
    const std::wstring srcW = srcPath.wstring();
    const std::wstring dxilW = dxilPath.wstring();
    const std::wstring entryW(entryPoint.begin(), entryPoint.end());
    const std::wstring targetW(targetProfile.begin(), targetProfile.end());

    std::vector<std::wstring> defineStorage;
    defineStorage.reserve(defines.size());
    for (const auto& define : defines)
    {
        std::wstring wide(define.begin(), define.end());
        wide += L"=1";
        defineStorage.push_back(std::move(wide));
    }

    std::vector<const wchar_t*> args;
    args.reserve(16u + defineStorage.size() * 2u);
    args.push_back(toolW.c_str());
    args.push_back(L"-T");
    args.push_back(targetW.c_str());
    args.push_back(L"-E");
    args.push_back(entryW.c_str());
    args.push_back(L"-Fo");
    args.push_back(dxilW.c_str());
    args.push_back(L"-HV");
    args.push_back(L"2021");
    args.push_back(L"-Ges");
#ifndef NDEBUG
    args.push_back(L"-Zi");
    args.push_back(L"-Od");
#else
    args.push_back(L"-O3");
#endif
    for (const auto& define : defineStorage)
    {
        args.push_back(L"-D");
        args.push_back(define.c_str());
    }
    args.push_back(srcW.c_str());
    args.push_back(nullptr);

    const intptr_t rc = _wspawnv(_P_WAIT, toolW.c_str(), args.data());
    if (rc != 0 || !fs::exists(dxilPath))
    {
        SetError(outError, "dxc.exe failed to compile DirectX12 SM6/DXIL artifact");
        fs::remove(srcPath, ec);
        fs::remove(dxilPath, ec);
        return false;
    }

    if (!ReadBinaryFile(dxilPath, outCompiled.bytecode))
    {
        SetError(outError, "failed to read compiled DXIL output");
        fs::remove(srcPath, ec);
        fs::remove(dxilPath, ec);
        return false;
    }

    outCompiled.sourceText.clear();
    fs::remove(srcPath, ec);
    fs::remove(dxilPath, ec);
    return true;
#else
    (void)asset; (void)bundle; (void)defines; (void)outCompiled;
    SetError(outError, "DirectX12 SM6/DXIL compilation is only available on Windows builds");
    return false;
#endif
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

bool CompileBackendArtifact(const assets::ShaderAsset& asset,
                            assets::ShaderTargetProfile target,
                            const SourceBundle& bundle,
                            const std::vector<std::string>& defines,
                            assets::CompiledShaderArtifact& outCompiled,
                            std::string* outError)
{
    switch (target)
    {
    case assets::ShaderTargetProfile::Vulkan_SPIRV:
#if KROM_HAS_SHADERC
        return CompileToSpirv(asset, bundle, defines, outCompiled, outError);
#else
        return CompileToSpirvWithTool(asset, bundle, defines, outCompiled, outError);
#endif
    case assets::ShaderTargetProfile::DirectX11_SM5:
        return CompileToD3DBytecode(asset, target, bundle, defines, outCompiled, outError);
    case assets::ShaderTargetProfile::DirectX12_SM6:
        return CompileToDxilWithTool(asset, bundle, defines, outCompiled, outError);
    case assets::ShaderTargetProfile::OpenGL_GLSL450:
    case assets::ShaderTargetProfile::Null:
    case assets::ShaderTargetProfile::Generic:
    default:
        outCompiled.sourceText = BuildShaderSource(bundle, defines);
        outCompiled.bytecode.clear();
        return true;
    }
}

bool CacheFirstCompile(const assets::ShaderAsset& asset,
                       assets::ShaderTargetProfile target,
                       const std::vector<std::string>& defines,
                       assets::CompiledShaderArtifact& outCompiled,
                       std::string* outError)
{
    const ShaderBinaryFormat binaryFormat = ShaderCompiler::ResolveBinaryFormat(target);
    const ShaderInterfaceLayout interfaceLayout = BuildEngineInterfaceLayout(asset.stage);
    const SourceBundle bundle = BuildSourceBundle(asset, outError);
    if (bundle.preprocessedSource.empty() && asset.sourceCode.empty() && asset.bytecode.empty())
    {
        SetError(outError, "shader asset has neither source nor bytecode");
        return false;
    }
    if (!asset.resolvedPath.empty() && bundle.dependencies.empty())
        return false;

    outCompiled = {};
    outCompiled.target = target;
    outCompiled.stage = asset.stage;
    outCompiled.entryPoint = asset.entryPoint.empty() ? "main" : asset.entryPoint;
    outCompiled.debugName = asset.debugName;
    outCompiled.defines = defines;
    outCompiled.cacheSchemaVersion = kShaderArtifactCacheSchemaVersion;
    outCompiled.dependencies = bundle.dependencies;
    outCompiled.contract.api = ResolveTargetApiNameSpaceSafe(target);
    outCompiled.contract.binaryFormat = binaryFormat;
    outCompiled.contract.stageMask = StageMaskBits(asset.stage);
    outCompiled.contract.interfaceLayout = interfaceLayout;
    outCompiled.contract.pipelineBinding.bindingLayout = BuildEnginePipelineBindingLayout();
    outCompiled.contract.pipelineBinding.runtimeLayout = BuildEngineDescriptorRuntimeLayout();
    outCompiled.contract.pipelineBinding.bindingSignature = DerivePipelineBindingSignatureDesc(outCompiled.contract.pipelineBinding.runtimeLayout);
    outCompiled.contract.pipelineBinding.bindingLayoutHash = HashBindingLayoutDescLocal(outCompiled.contract.pipelineBinding.bindingLayout);
    outCompiled.contract.pipelineBinding.runtimeLayoutHash = HashDescriptorRuntimeLayoutDescLocal(outCompiled.contract.pipelineBinding.runtimeLayout);
    outCompiled.contract.pipelineBinding.bindingSignatureHash = HashPipelineBindingSignatureDesc(outCompiled.contract.pipelineBinding.bindingSignature);
    outCompiled.contract.pipelineBinding.interfaceLayoutHash = interfaceLayout.layoutHash;
    outCompiled.contract.pipelineBinding.bindingSignatureKey = BuildPipelineBindingSignatureKey(outCompiled.contract.pipelineBinding.runtimeLayout,
                                                                                interfaceLayout.layoutHash,
                                                                                outCompiled.contract.pipelineBinding.bindingSignature.staticSamplerPolicy);
    outCompiled.contract.pipelineBinding.computeRuntime = ComputeRuntimeDesc{};
    outCompiled.contract.pipelineBinding.computeRuntimeHash = HashComputeRuntimeDesc(outCompiled.contract.pipelineBinding.computeRuntime);
    outCompiled.cacheKey = BuildArtifactCacheKey(asset, target, binaryFormat, interfaceLayout, bundle, defines);

    const std::filesystem::path cachePath = GetShaderCacheRoot() / (outCompiled.cacheKey + ".bin");
    if (LoadArtifactFromDisk(cachePath, outCompiled.cacheKey, outCompiled))
        return true;

    const std::string legacyCacheKey = BuildArtifactCacheKeyForSchema(asset,
                                                                      target,
                                                                      binaryFormat,
                                                                      interfaceLayout,
                                                                      bundle,
                                                                      defines,
                                                                      kShaderArtifactCacheLegacySchemaVersion);
    if (legacyCacheKey != outCompiled.cacheKey)
    {
        const std::filesystem::path legacyCachePath = GetShaderCacheRoot() / (legacyCacheKey + ".bin");
        assets::CompiledShaderArtifact legacyCompiled{};
        if (LoadArtifactFromDisk(legacyCachePath, legacyCacheKey, legacyCompiled))
        {
            legacyCompiled.cacheSchemaVersion = kShaderArtifactCacheSchemaVersion;
            legacyCompiled.cacheKey = outCompiled.cacheKey;
            SaveArtifactToDisk(cachePath, legacyCompiled);
            outCompiled = std::move(legacyCompiled);
            return true;
        }
    }

    if (!asset.bytecode.empty() && defines.empty())
    {
        outCompiled.bytecode = asset.bytecode;
        outCompiled.sourceText.clear();
    }
    else
    {
        if (!CompileBackendArtifact(asset, target, bundle, defines, outCompiled, outError))
            return false;
    }

    if (!outCompiled.bytecode.empty())
        outCompiled.sourceHash = HashBytes(outCompiled.bytecode.data(), outCompiled.bytecode.size());
    else
        outCompiled.sourceHash = HashBytes(outCompiled.sourceText.data(), outCompiled.sourceText.size());

    outCompiled.contract.pipelineStateKey = HashCombine(outCompiled.contract.pipelineBinding.bindingSignatureKey,
                                                        HashCombine(outCompiled.contract.interfaceLayout.layoutHash,
                                                                    HashCombine(static_cast<uint64_t>(outCompiled.contract.stageMask),
                                                                                HashCombine(static_cast<uint64_t>(outCompiled.contract.binaryFormat),
                                                                                            outCompiled.contract.pipelineBinding.computeRuntimeHash))));
    outCompiled.contract.contractHash = HashCombine(outCompiled.contract.pipelineStateKey,
                                                    HashCombine(static_cast<uint64_t>(outCompiled.contract.api),
                                                                HashString(outCompiled.cacheKey)));

    if (!ShaderCompiler::IsRuntimeConsumable(outCompiled))
    {
        SetError(outError, "compiled shader artifact is empty");
        return false;
    }

    SaveArtifactToDisk(cachePath, outCompiled);
    return true;
}

} // namespace

ShaderTargetApi ResolveTargetApiNameSpaceSafe(assets::ShaderTargetProfile profile) noexcept
{
    switch (profile)
    {
    case assets::ShaderTargetProfile::Null: return ShaderTargetApi::Null;
    case assets::ShaderTargetProfile::DirectX11_SM5: return ShaderTargetApi::DirectX11;
    case assets::ShaderTargetProfile::DirectX12_SM6: return ShaderTargetApi::DirectX12;
    case assets::ShaderTargetProfile::Vulkan_SPIRV: return ShaderTargetApi::Vulkan;
    case assets::ShaderTargetProfile::OpenGL_GLSL450: return ShaderTargetApi::OpenGL;
    default: return ShaderTargetApi::Generic;
    }
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
    if (backend.find("opengl") != std::string::npos || backend == "gl")
        return assets::ShaderTargetProfile::OpenGL_GLSL450;
    if (backend.find("null") != std::string::npos)
        return assets::ShaderTargetProfile::Null;
    return assets::ShaderTargetProfile::Generic;
}

ShaderTargetApi ShaderCompiler::ResolveTargetApi(const IDevice& device)
{
    return ResolveTargetApiNameSpaceSafe(ResolveTargetProfile(device));
}

ShaderBinaryFormat ShaderCompiler::ResolveBinaryFormat(assets::ShaderTargetProfile profile) noexcept
{
    switch (profile)
    {
    case assets::ShaderTargetProfile::Vulkan_SPIRV: return ShaderBinaryFormat::Spirv;
    case assets::ShaderTargetProfile::DirectX11_SM5: return ShaderBinaryFormat::Dxbc;
    case assets::ShaderTargetProfile::DirectX12_SM6: return ShaderBinaryFormat::Dxil;
    case assets::ShaderTargetProfile::OpenGL_GLSL450: return ShaderBinaryFormat::GlslSource;
    case assets::ShaderTargetProfile::Null:
    case assets::ShaderTargetProfile::Generic:
    default:
        return ShaderBinaryFormat::SourceText;
    }
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
    return !shader.bytecode.empty() || !shader.sourceText.empty() || shader.contract.IsValid();
}

bool ShaderCompiler::CompileForTarget(const assets::ShaderAsset& asset,
                                      assets::ShaderTargetProfile target,
                                      assets::CompiledShaderArtifact& outCompiled,
                                      std::string* outError)
{
    return CacheFirstCompile(asset, target, {}, outCompiled, outError);
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
    return CacheFirstCompile(asset, target, VariantFlagsToDefines(flags), outCompiled, outError);
}

} // namespace engine::renderer
