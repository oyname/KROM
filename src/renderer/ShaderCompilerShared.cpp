// =============================================================================
// KROM Engine - src/renderer/ShaderCompilerShared.cpp
//
// Shared infrastructure for all shader compiler backends.
// Contains: hashing, source bundling, include expansion, artifact cache
// (load/save), and the top-level CacheFirstCompile dispatcher.
//
// This file provides the implementations declared in ShaderCompilerInternal.hpp
// that are consumed by ShaderCompilerD3D, ShaderCompilerDXC,
// ShaderCompilerGLSL, and ShaderCompilerSPIRV.
// =============================================================================
#include "ShaderCompilerInternal.hpp"
#include "renderer/ShaderBindingModel.hpp"
#include "core/Debug.hpp"
#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>
#include <string_view>
#include <vector>
#if KROM_HAS_SHADERC
#   include <shaderc/shaderc.hpp>
#endif

namespace engine::renderer::internal {

// -----------------------------------------------------------------------------
// Constants
// -----------------------------------------------------------------------------
constexpr uint32_t      kShaderArtifactCacheSchemaVersion       = 6u;
constexpr uint32_t      kShaderArtifactCacheLegacySchemaVersion = 4u;
constexpr std::string_view kCacheMagic                          = "KROM_SHADER_CACHE_V1";

// -----------------------------------------------------------------------------
// String / hash utilities
// -----------------------------------------------------------------------------
std::string ToLower(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(),
        [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
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

// -----------------------------------------------------------------------------
// Stage utilities
// -----------------------------------------------------------------------------
static uint32_t StageMaskBits(assets::ShaderStage stage) noexcept
{
    return 1u << static_cast<uint32_t>(stage);
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

// -----------------------------------------------------------------------------
// Engine interface / binding layout builders
// -----------------------------------------------------------------------------
static void AppendEngineBinding(std::vector<ShaderBindingDeclaration>& bindings,
                                const char* name,
                                uint32_t logicalSlot,
                                uint32_t apiBinding,
                                uint32_t space,
                                ShaderBindingClass bindingClass,
                                uint32_t stageMask)
{
    ShaderBindingDeclaration decl{};
    decl.name         = name;
    decl.logicalSlot  = logicalSlot;
    decl.apiBinding   = apiBinding;
    decl.space        = space;
    decl.bindingClass = bindingClass;
    decl.stageMask    = stageMask;
    bindings.push_back(std::move(decl));
}

static ShaderInterfaceLayout BuildEngineInterfaceLayout(assets::ShaderStage stage)
{
    ShaderInterfaceLayout layout{};
    layout.usesEngineBindingModel = true;
    const uint32_t stageMask = StageMaskBits(stage);

    AppendEngineBinding(layout.bindings, "PerFrame",    CBSlots::PerFrame,
        BindingRegisterRanges::CB(CBSlots::PerFrame),   BindingRegisterRanges::RegisterSpace, ShaderBindingClass::ConstantBuffer, stageMask);
    AppendEngineBinding(layout.bindings, "PerObject",   CBSlots::PerObject,
        BindingRegisterRanges::CB(CBSlots::PerObject),  BindingRegisterRanges::RegisterSpace, ShaderBindingClass::ConstantBuffer, stageMask);
    AppendEngineBinding(layout.bindings, "PerMaterial", CBSlots::PerMaterial,
        BindingRegisterRanges::CB(CBSlots::PerMaterial),BindingRegisterRanges::RegisterSpace, ShaderBindingClass::ConstantBuffer, stageMask);
    AppendEngineBinding(layout.bindings, "PerPass",     CBSlots::PerPass,
        BindingRegisterRanges::CB(CBSlots::PerPass),    BindingRegisterRanges::RegisterSpace, ShaderBindingClass::ConstantBuffer, stageMask);

    if (stage == assets::ShaderStage::Fragment || stage == assets::ShaderStage::Compute)
    {
        for (uint32_t i = 0u; i < TexSlots::COUNT; ++i)
            AppendEngineBinding(layout.bindings, "Texture", i,
                BindingRegisterRanges::SRV(i), BindingRegisterRanges::RegisterSpace,
                ShaderBindingClass::ShaderResource, stageMask);
        for (uint32_t i = 0u; i < SamplerSlots::COUNT; ++i)
            AppendEngineBinding(layout.bindings, "Sampler", i,
                BindingRegisterRanges::SMP(i), BindingRegisterRanges::RegisterSpace,
                ShaderBindingClass::Sampler, stageMask);
    }

    if (stage == assets::ShaderStage::Compute)
    {
        for (uint32_t i = 0u; i < UAVSlots::COUNT; ++i)
            AppendEngineBinding(layout.bindings, "UAV", i,
                BindingRegisterRanges::UAV(i), BindingRegisterRanges::RegisterSpace,
                ShaderBindingClass::UnorderedAccess, stageMask);
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

static uint64_t HashBindingLayoutDescLocal(const PipelineBindingLayoutDesc& desc)
{
    return HashPipelineBindingLayoutDesc(desc);
}

static uint64_t HashDescriptorRuntimeLayoutDescLocal(const DescriptorRuntimeLayoutDesc& desc)
{
    return engine::renderer::HashDescriptorRuntimeLayoutDesc(desc);
}

// -----------------------------------------------------------------------------
// File I/O helpers
// -----------------------------------------------------------------------------
static std::string Trim(std::string_view value)
{
    size_t begin = 0u;
    size_t end   = value.size();
    while (begin < end && std::isspace(static_cast<unsigned char>(value[begin])))
        ++begin;
    while (end > begin && std::isspace(static_cast<unsigned char>(value[end - 1u])))
        --end;
    return std::string(value.substr(begin, end - begin));
}

static std::filesystem::path GetShaderCacheRoot()
{
    return std::filesystem::current_path() / ".krom" / "shader_artifacts";
}

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

static bool ReadTextFile(const std::filesystem::path& path, std::string& outText)
{
    std::ifstream in(path, std::ios::binary);
    if (!in)
        return false;
    outText.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    return true;
}

static std::filesystem::path NormalizePath(const std::filesystem::path& path)
{
    std::error_code ec;
    const auto weak = std::filesystem::weakly_canonical(path, ec);
    return ec ? path.lexically_normal() : weak;
}

// -----------------------------------------------------------------------------
// Include expansion
// -----------------------------------------------------------------------------
static bool ParseInclude(std::string_view line, std::string& outInclude)
{
    const std::string trimmed = Trim(line);
    if (trimmed.rfind("#include", 0u) != 0u)
        return false;

    const size_t firstQuote = trimmed.find('"');
    const size_t lastQuote  = trimmed.find_last_of('"');
    if (firstQuote != std::string::npos && lastQuote != std::string::npos && lastQuote > firstQuote)
    {
        outInclude = trimmed.substr(firstQuote + 1u, lastQuote - firstQuote - 1u);
        return !outInclude.empty();
    }

    const size_t firstAngle = trimmed.find('<');
    const size_t lastAngle  = trimmed.find_last_of('>');
    if (firstAngle != std::string::npos && lastAngle != std::string::npos && lastAngle > firstAngle)
    {
        outInclude = trimmed.substr(firstAngle + 1u, lastAngle - firstAngle - 1u);
        return !outInclude.empty();
    }

    return false;
}

static bool ExpandIncludesRecursive(const std::filesystem::path& path,
                                    std::set<std::string>& recursionGuard,
                                    std::set<std::string>& dependencySet,
                                    SourceBundle& outBundle,
                                    std::string* outError)
{
    const std::filesystem::path canonical    = NormalizePath(path);
    const std::string            canonicalKey = canonical.generic_string();

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
        dep.path        = canonicalKey;
        dep.contentHash = HashBytes(source.data(), source.size());
        outBundle.dependencies.push_back(std::move(dep));
    }

    std::istringstream input(source);
    std::string        line;
    const auto         parent = canonical.parent_path();
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

static SourceBundle BuildSourceBundle(const assets::ShaderAsset& asset, std::string* outError)
{
    SourceBundle bundle{};

    if (!asset.resolvedPath.empty())
    {
        bundle.canonicalSourcePath = NormalizePath(asset.resolvedPath);
        std::set<std::string> recursionGuard;
        std::set<std::string> dependencySet;
        if (!ExpandIncludesRecursive(bundle.canonicalSourcePath, recursionGuard, dependencySet, bundle, outError))
            return {};
        std::sort(bundle.dependencies.begin(), bundle.dependencies.end(),
            [](const auto& a, const auto& b) { return a.path < b.path; });
        return bundle;
    }

    bundle.preprocessedSource = asset.sourceCode;
    assets::ShaderDependencyRecord dep{};
    dep.path        = asset.path.empty() ? asset.debugName : asset.path;
    dep.contentHash = HashBytes(asset.sourceCode.data(), asset.sourceCode.size());
    bundle.dependencies.push_back(std::move(dep));
    return bundle;
}

// -----------------------------------------------------------------------------
// Artifact cache key construction
// -----------------------------------------------------------------------------
static std::string GetTargetProfileString(assets::ShaderTargetProfile profile)
{
    return ShaderCompiler::ToString(profile);
}

static std::string GetBinaryFormatString(ShaderBinaryFormat format)
{
    switch (format)
    {
    case ShaderBinaryFormat::SourceText: return "source";
    case ShaderBinaryFormat::Spirv:      return "spirv";
    case ShaderBinaryFormat::Dxbc:       return "dxbc";
    case ShaderBinaryFormat::Dxil:       return "dxil";
    case ShaderBinaryFormat::GlslSource: return "glsl";
    default:                             return "none";
    }
}

static std::string BuildArtifactCacheKeyForSchema(const assets::ShaderAsset& asset,
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

    mix("schema",          schemaVersion);
    mix("stage",           static_cast<uint64_t>(asset.stage));
    mix("source_language", static_cast<uint64_t>(asset.sourceLanguage));
    mix("target",          static_cast<uint64_t>(target));
    mix("binary_format",   static_cast<uint64_t>(binaryFormat));
    mix("interface_layout",interfaceLayout.layoutHash);
    mix("entry_point",     HashString(asset.entryPoint.empty()
                               ? std::string_view("main") : std::string_view(asset.entryPoint)));
    mix("source_path",     HashString(bundle.canonicalSourcePath.generic_string()));
    mix("source_text",     HashBytes(bundle.preprocessedSource.data(), bundle.preprocessedSource.size()));

    for (const auto& dep : bundle.dependencies)
    {
        mix("dep_path", HashString(dep.path));
        mix("dep_hash", dep.contentHash);
    }
    for (const auto& def : defines)
        mix("define", HashString(def));

    std::ostringstream oss;
    oss << "v" << schemaVersion
        << '_' << Hex64(h0) << Hex64(h1)
        << '_' << GetTargetProfileString(target)
        << '_' << GetBinaryFormatString(binaryFormat);
    return oss.str();
}

static std::string BuildArtifactCacheKey(const assets::ShaderAsset& asset,
                                         assets::ShaderTargetProfile target,
                                         ShaderBinaryFormat binaryFormat,
                                         const ShaderInterfaceLayout& interfaceLayout,
                                         const SourceBundle& bundle,
                                         const std::vector<std::string>& defines)
{
    return BuildArtifactCacheKeyForSchema(asset, target, binaryFormat, interfaceLayout, bundle,
                                         defines, kShaderArtifactCacheSchemaVersion);
}

// -----------------------------------------------------------------------------
// Binary serialisation helpers (artifact cache I/O)
// -----------------------------------------------------------------------------
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
        out.assign(bytes.begin() + static_cast<std::ptrdiff_t>(offset),
                   bytes.begin() + static_cast<std::ptrdiff_t>(offset + size));
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

// -----------------------------------------------------------------------------
// Artifact disk cache
// -----------------------------------------------------------------------------
static bool LoadArtifactFromDisk(const std::filesystem::path& cachePath,
                                 const std::string& expectedCacheKey,
                                 assets::CompiledShaderArtifact& outCompiled)
{
    std::vector<uint8_t> fileBytes;
    if (!ReadBinaryFile(cachePath, fileBytes))
        return false;

    BinaryReader reader{ fileBytes };
    std::string  magic;
    if (!reader.ReadString(magic) || magic != kCacheMagic)
        return false;

    uint32_t schemaVersion = 0u;
    if (!reader.Read(schemaVersion) || schemaVersion != kShaderArtifactCacheSchemaVersion)
        return false;

    uint32_t target = 0u, stage = 0u, binaryFormat = 0u, stageMask = 0u;
    uint64_t sourceHash = 0ull, layoutHash = 0ull, contractHash = 0ull;
    uint8_t  usesBindingModel = 0u;
    if (!reader.Read(target)       || !reader.Read(stage)          ||
        !reader.Read(binaryFormat) || !reader.Read(stageMask)      ||
        !reader.Read(sourceHash)   || !reader.Read(layoutHash)     ||
        !reader.Read(contractHash) || !reader.Read(usesBindingModel))
    {
        return false;
    }

    if (!reader.ReadString(outCompiled.entryPoint)  ||
        !reader.ReadString(outCompiled.debugName)   ||
        !reader.ReadString(outCompiled.cacheKey)    ||
        !reader.ReadString(outCompiled.sourceText)  ||
        !reader.ReadBytes (outCompiled.bytecode))
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
    outCompiled.target             = static_cast<assets::ShaderTargetProfile>(target);
    outCompiled.stage              = static_cast<assets::ShaderStage>(stage);
    outCompiled.sourceHash         = sourceHash;

    outCompiled.contract.api          = ResolveTargetApiNameSpaceSafe(outCompiled.target);
    outCompiled.contract.binaryFormat = static_cast<ShaderBinaryFormat>(binaryFormat);
    outCompiled.contract.stageMask    = stageMask;
    outCompiled.contract.interfaceLayout = BuildEngineInterfaceLayout(outCompiled.stage);
    outCompiled.contract.interfaceLayout.layoutHash              = layoutHash;
    outCompiled.contract.interfaceLayout.usesEngineBindingModel  = (usesBindingModel != 0u);

    outCompiled.contract.pipelineBinding.bindingLayout     = BuildEnginePipelineBindingLayout();
    outCompiled.contract.pipelineBinding.runtimeLayout     = BuildEngineDescriptorRuntimeLayout();
    outCompiled.contract.pipelineBinding.bindingSignature  =
        DerivePipelineBindingSignatureDesc(outCompiled.contract.pipelineBinding.runtimeLayout);
    outCompiled.contract.pipelineBinding.bindingLayoutHash =
        HashBindingLayoutDescLocal(outCompiled.contract.pipelineBinding.bindingLayout);
    outCompiled.contract.pipelineBinding.runtimeLayoutHash =
        HashDescriptorRuntimeLayoutDescLocal(outCompiled.contract.pipelineBinding.runtimeLayout);
    outCompiled.contract.pipelineBinding.bindingSignatureHash =
        HashPipelineBindingSignatureDesc(outCompiled.contract.pipelineBinding.bindingSignature);
    outCompiled.contract.pipelineBinding.interfaceLayoutHash = layoutHash;
    outCompiled.contract.pipelineBinding.bindingSignatureKey =
        BuildPipelineBindingSignatureKey(outCompiled.contract.pipelineBinding.runtimeLayout,
                                        layoutHash,
                                        outCompiled.contract.pipelineBinding.bindingSignature.staticSamplerPolicy);
    outCompiled.contract.pipelineBinding.computeRuntime     = ComputeRuntimeDesc{};
    outCompiled.contract.pipelineBinding.computeRuntimeHash =
        HashComputeRuntimeDesc(outCompiled.contract.pipelineBinding.computeRuntime);
    outCompiled.contract.pipelineStateKey =
        HashCombine(outCompiled.contract.pipelineBinding.bindingSignatureKey,
            HashCombine(outCompiled.contract.interfaceLayout.layoutHash,
                HashCombine(static_cast<uint64_t>(outCompiled.contract.stageMask),
                    HashCombine(static_cast<uint64_t>(outCompiled.contract.binaryFormat),
                                outCompiled.contract.pipelineBinding.computeRuntimeHash))));
    outCompiled.contract.contractHash = contractHash;
    return outCompiled.IsValid();
}

static bool SaveArtifactToDisk(const std::filesystem::path& cachePath,
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
        out.write(reinterpret_cast<const char*>(writer.bytes.data()),
                  static_cast<std::streamsize>(writer.bytes.size()));
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

// -----------------------------------------------------------------------------
// Backend dispatch
// -----------------------------------------------------------------------------
static bool CompileBackendArtifact(const assets::ShaderAsset& asset,
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
        outCompiled.sourceText = BuildShaderSource(bundle, defines);
        outCompiled.bytecode.clear();
        return true;
    case assets::ShaderTargetProfile::Null:
    case assets::ShaderTargetProfile::Generic:
    default:
        // Null/Generic: preprocessedSource unverändert durchreichen.
        // BuildShaderSource ist GLSL-spezifisch; für HLSL-Quellen (kein #version)
        // würde es ein leeres Ergebnis liefern.
        outCompiled.sourceText = bundle.preprocessedSource.empty() ? "// null" : bundle.preprocessedSource;
        outCompiled.bytecode.clear();
        return true;
    }
}

// -----------------------------------------------------------------------------
// CacheFirstCompile — top-level entry point called by ShaderCompiler.cpp
// -----------------------------------------------------------------------------
bool CacheFirstCompile(const assets::ShaderAsset& asset,
                       assets::ShaderTargetProfile target,
                       const std::vector<std::string>& defines,
                       assets::CompiledShaderArtifact& outCompiled,
                       std::string* outError)
{
    const ShaderBinaryFormat   binaryFormat    = ShaderCompiler::ResolveBinaryFormat(target);
    const ShaderInterfaceLayout interfaceLayout = BuildEngineInterfaceLayout(asset.stage);
    const SourceBundle          bundle          = BuildSourceBundle(asset, outError);

    if (bundle.preprocessedSource.empty() && asset.sourceCode.empty() && asset.bytecode.empty())
    {
        SetError(outError, "shader asset has neither source nor bytecode");
        return false;
    }
    if (!asset.resolvedPath.empty() && bundle.dependencies.empty())
        return false;

    outCompiled = {};
    outCompiled.target             = target;
    outCompiled.stage              = asset.stage;
    outCompiled.entryPoint         = asset.entryPoint.empty() ? "main" : asset.entryPoint;
    outCompiled.debugName          = asset.debugName;
    outCompiled.defines            = defines;
    outCompiled.cacheSchemaVersion = kShaderArtifactCacheSchemaVersion;
    outCompiled.dependencies       = bundle.dependencies;
    outCompiled.contract.api          = ResolveTargetApiNameSpaceSafe(target);
    outCompiled.contract.binaryFormat = binaryFormat;
    outCompiled.contract.stageMask    = StageMaskBits(asset.stage);
    outCompiled.contract.interfaceLayout = interfaceLayout;

    outCompiled.contract.pipelineBinding.bindingLayout     = BuildEnginePipelineBindingLayout();
    outCompiled.contract.pipelineBinding.runtimeLayout     = BuildEngineDescriptorRuntimeLayout();
    outCompiled.contract.pipelineBinding.bindingSignature  =
        DerivePipelineBindingSignatureDesc(outCompiled.contract.pipelineBinding.runtimeLayout);
    outCompiled.contract.pipelineBinding.bindingLayoutHash =
        HashBindingLayoutDescLocal(outCompiled.contract.pipelineBinding.bindingLayout);
    outCompiled.contract.pipelineBinding.runtimeLayoutHash =
        HashDescriptorRuntimeLayoutDescLocal(outCompiled.contract.pipelineBinding.runtimeLayout);
    outCompiled.contract.pipelineBinding.bindingSignatureHash =
        HashPipelineBindingSignatureDesc(outCompiled.contract.pipelineBinding.bindingSignature);
    outCompiled.contract.pipelineBinding.interfaceLayoutHash = interfaceLayout.layoutHash;
    outCompiled.contract.pipelineBinding.bindingSignatureKey =
        BuildPipelineBindingSignatureKey(outCompiled.contract.pipelineBinding.runtimeLayout,
                                        interfaceLayout.layoutHash,
                                        outCompiled.contract.pipelineBinding.bindingSignature.staticSamplerPolicy);
    outCompiled.contract.pipelineBinding.computeRuntime     = ComputeRuntimeDesc{};
    outCompiled.contract.pipelineBinding.computeRuntimeHash =
        HashComputeRuntimeDesc(outCompiled.contract.pipelineBinding.computeRuntime);

    outCompiled.cacheKey = BuildArtifactCacheKey(asset, target, binaryFormat, interfaceLayout, bundle, defines);

    // --- Cache hit (current schema) ---
    const std::filesystem::path cachePath = GetShaderCacheRoot() / (outCompiled.cacheKey + ".bin");
    if (LoadArtifactFromDisk(cachePath, outCompiled.cacheKey, outCompiled))
        return true;

    // --- Cache hit (legacy schema migration) ---
    const std::string legacyCacheKey = BuildArtifactCacheKeyForSchema(
        asset, target, binaryFormat, interfaceLayout, bundle, defines,
        kShaderArtifactCacheLegacySchemaVersion);
    if (legacyCacheKey != outCompiled.cacheKey)
    {
        const std::filesystem::path legacyPath = GetShaderCacheRoot() / (legacyCacheKey + ".bin");
        assets::CompiledShaderArtifact legacyCompiled{};
        if (LoadArtifactFromDisk(legacyPath, legacyCacheKey, legacyCompiled))
        {
            legacyCompiled.cacheSchemaVersion = kShaderArtifactCacheSchemaVersion;
            legacyCompiled.cacheKey           = outCompiled.cacheKey;
            SaveArtifactToDisk(cachePath, legacyCompiled);
            outCompiled = std::move(legacyCompiled);
            return true;
        }
    }

    // --- Compile ---
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

    outCompiled.contract.pipelineStateKey =
        HashCombine(outCompiled.contract.pipelineBinding.bindingSignatureKey,
            HashCombine(outCompiled.contract.interfaceLayout.layoutHash,
                HashCombine(static_cast<uint64_t>(outCompiled.contract.stageMask),
                    HashCombine(static_cast<uint64_t>(outCompiled.contract.binaryFormat),
                                outCompiled.contract.pipelineBinding.computeRuntimeHash))));
    outCompiled.contract.contractHash =
        HashCombine(outCompiled.contract.pipelineStateKey,
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

} // namespace engine::renderer::internal
