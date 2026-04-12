#include "renderer/ShaderRuntime.hpp"
#include <array>
#include "core/Debug.hpp"
#include <algorithm>
#include <cstring>
#include <unordered_set>
#include <thread>

namespace engine::renderer {


namespace {

uint64_t FoldContractHash(const assets::CompiledShaderArtifact* artifact, uint64_t salt)
{
    if (!artifact)
        return 0ull;
    const uint64_t key = artifact->contract.pipelineStateKey != 0ull
        ? artifact->contract.pipelineStateKey
        : artifact->contract.contractHash;
    return key != 0ull ? (key ^ salt) : 0ull;
}

uint64_t FoldPipelineBindingHash(const assets::CompiledShaderArtifact* artifact, uint64_t salt)
{
    if (!artifact)
        return 0ull;

    const uint64_t rootKey = artifact->contract.pipelineBinding.bindingSignatureKey != 0ull
        ? artifact->contract.pipelineBinding.bindingSignatureKey
        : artifact->contract.interfaceLayout.layoutHash;
    return rootKey != 0ull ? (rootKey ^ salt) : 0ull;
}

} // namespace


bool ShaderRuntime::IsRenderThread() const noexcept
{
    return m_renderThreadId == std::this_thread::get_id();
}

bool ShaderRuntime::RequireRenderThread(const char* opName) const noexcept
{
    if (IsRenderThread())
        return true;

    Debug::LogError("ShaderRuntime.cpp: %s must run on render thread", opName ? opName : "operation");
    return false;
}

namespace {
const ShaderStageMask kGraphicsStages = ShaderStageMask::Vertex | ShaderStageMask::Fragment;

uint32_t ToContractStageBits(ShaderStageMask mask) noexcept
{
    return static_cast<uint32_t>(mask);
}

uint64_t HashContractBindings(const ShaderInterfaceLayout& layout) noexcept
{
    uint64_t h = 1469598103934665603ull;
    for (const auto& binding : layout.bindings)
    {
        h ^= static_cast<uint64_t>(binding.logicalSlot) + (h << 1u);
        h ^= static_cast<uint64_t>(binding.apiBinding) + (h << 1u);
        h ^= static_cast<uint64_t>(binding.space) + (h << 1u);
        h ^= static_cast<uint64_t>(binding.bindingClass) + (h << 1u);
        h ^= static_cast<uint64_t>(binding.stageMask) + (h << 1u);
        for (char c : binding.name)
        {
            h ^= static_cast<uint64_t>(static_cast<unsigned char>(c));
            h *= 1099511628211ull;
        }
    }
    return h;
}

std::string ToLower(std::string s)
{
    for (char& ch : s)
        if (ch >= "A"[0] && ch <= "Z"[0]) ch = static_cast<char>(ch - "A"[0] + "a"[0]);
    return s;
}

uint32_t ResolveTextureSlotByName(const std::string& name)
{
    const std::string lower = ToLower(name);
    if (lower.find("albedo") != std::string::npos || lower.find("basecolor") != std::string::npos || lower.find("diffuse") != std::string::npos)
        return TexSlots::Albedo;
    if (lower.find("normal") != std::string::npos)
        return TexSlots::Normal;
    if (lower.find("orm") != std::string::npos || lower.find("rough") != std::string::npos || lower.find("metal") != std::string::npos || lower.find("occlusion") != std::string::npos)
        return TexSlots::ORM;
    if (lower.find("emissive") != std::string::npos)
        return TexSlots::Emissive;
    if (lower.find("shadow") != std::string::npos)
        return TexSlots::ShadowMap;
    if (lower.find("history") != std::string::npos)
        return TexSlots::HistoryBuffer;
    if (lower.find("bloom") != std::string::npos)
        return TexSlots::BloomTexture;
    return TexSlots::PassSRV0;
}

uint32_t ResolveSamplerSlotByName(const std::string& name)
{
    const std::string lower = ToLower(name);
    if (lower.find("shadow") != std::string::npos || lower.find("pcf") != std::string::npos)
        return SamplerSlots::ShadowPCF;
    if (lower.find("point") != std::string::npos)
        return SamplerSlots::PointClamp;
    if (lower.find("clamp") != std::string::npos)
        return SamplerSlots::LinearClamp;
    return SamplerSlots::LinearWrap;
}

} // namespace

bool ShaderRuntime::Initialize(IDevice& device)
{
    m_device = &device;
    m_renderThreadId = std::this_thread::get_id();
    m_shaderAssets.clear();
    m_materialStates.clear();
    m_pipelineCache.Clear();
    m_variantCache.SetUploadFunction(
        [this](const assets::ShaderAsset&, const assets::CompiledShaderArtifact& artifact) -> ShaderHandle
        {
            if (!m_device)
                return ShaderHandle::Invalid();

            if (!artifact.bytecode.empty())
            {
                return m_device->CreateShaderFromBytecode(
                    artifact.bytecode.data(),
                    artifact.bytecode.size(),
                    ToStageMask(artifact.stage),
                    artifact.debugName);
            }

            if (!artifact.sourceText.empty())
            {
                std::string source;
                for (const auto& d : artifact.defines)
                    source += "#define " + d + " 1\n";
                source += artifact.sourceText;

                return m_device->CreateShaderFromSource(
                    source,
                    ToStageMask(artifact.stage),
                    artifact.entryPoint,
                    artifact.debugName);
            }

            return ShaderHandle::Invalid();
        });
    CreateDefaultSamplers();
    CreateFallbackTextures();
    return true;
}

void ShaderRuntime::Shutdown()
{
    if (!IsRenderThread())
    {
        Debug::LogWarning("ShaderRuntime.cpp: Shutdown not on render thread after WaitIdle - continuing cleanup");
    }
    if (m_device)
    {
        for (auto& [_, state] : m_materialStates)
            DestroyMaterialState(state);
        for (auto& [_, status] : m_shaderAssets)
        {
            if (status.gpuHandle.IsValid())
                m_device->DestroyShader(status.gpuHandle);
        }
        m_pipelineCache.ForEach([&](const PipelineKey&, PipelineHandle handle) {
            if (handle.IsValid())
                m_device->DestroyPipeline(handle);
        });
        if (m_fallbackTextures.white.IsValid()) m_device->DestroyTexture(m_fallbackTextures.white);
        if (m_fallbackTextures.black.IsValid()) m_device->DestroyTexture(m_fallbackTextures.black);
        if (m_fallbackTextures.gray.IsValid()) m_device->DestroyTexture(m_fallbackTextures.gray);
        if (m_fallbackTextures.neutralNormal.IsValid()) m_device->DestroyTexture(m_fallbackTextures.neutralNormal);
    }
    m_materialStates.clear();
    m_shaderAssets.clear();
    m_variantCache.Clear();
    m_pipelineCache.Clear();
    m_device = nullptr;
    m_renderThreadId = std::thread::id{};
}

ShaderStageMask ShaderRuntime::ToStageMask(assets::ShaderStage stage) noexcept
{
    switch (stage)
    {
    case assets::ShaderStage::Vertex: return ShaderStageMask::Vertex;
    case assets::ShaderStage::Fragment: return ShaderStageMask::Fragment;
    case assets::ShaderStage::Compute: return ShaderStageMask::Compute;
    case assets::ShaderStage::Geometry: return ShaderStageMask::Geometry;
    case assets::ShaderStage::Hull: return ShaderStageMask::Hull;
    case assets::ShaderStage::Domain: return ShaderStageMask::Domain;
    default: return ShaderStageMask::None;
    }
}

uint64_t ShaderRuntime::HashBytes(const void* data, size_t size) noexcept
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

const assets::CompiledShaderArtifact* ShaderRuntime::FindCompiledArtifact(const assets::ShaderAsset& shaderAsset) const noexcept
{
    if (!m_device) return nullptr;
    const auto target = ShaderCompiler::ResolveTargetProfile(*m_device);
    auto it = std::find_if(shaderAsset.compiledArtifacts.begin(), shaderAsset.compiledArtifacts.end(), [&](const assets::CompiledShaderArtifact& artifact) {
        return artifact.target == target && artifact.stage == shaderAsset.stage && artifact.IsValid();
    });
    if (it != shaderAsset.compiledArtifacts.end())
        return &(*it);

    it = std::find_if(shaderAsset.compiledArtifacts.begin(), shaderAsset.compiledArtifacts.end(), [&](const assets::CompiledShaderArtifact& artifact) {
        return artifact.target == assets::ShaderTargetProfile::Generic && artifact.stage == shaderAsset.stage && artifact.IsValid();
    });
    return it != shaderAsset.compiledArtifacts.end() ? &(*it) : nullptr;
}

uint64_t ShaderRuntime::HashMaterialState(const std::vector<uint8_t>& cbData,
                                          const std::vector<ResolvedMaterialBinding>& bindings) noexcept
{
    uint64_t h = HashBytes(cbData.data(), cbData.size());
    for (const auto& binding : bindings)
    {
        h ^= HashBytes(binding.name.data(), binding.name.size());
        h ^= static_cast<uint64_t>(binding.slot) << 7u;
        h ^= static_cast<uint64_t>(binding.texture.value) << 13u;
        h ^= static_cast<uint64_t>(binding.samplerIndex) << 17u;
        h ^= static_cast<uint64_t>(binding.kind == ResolvedMaterialBinding::Kind::Texture ? 0xAAu : binding.kind == ResolvedMaterialBinding::Kind::Sampler ? 0xBBu : 0xCCu);
        h *= 1099511628211ull;
    }
    return h;
}

TextureHandle ShaderRuntime::ResolveFallbackTexture(MaterialSemantic semantic) const noexcept
{
    switch (semantic)
    {
    case MaterialSemantic::BaseColor:   return m_fallbackTextures.white;
    case MaterialSemantic::Normal:      return m_fallbackTextures.neutralNormal;
    case MaterialSemantic::Metallic:    return m_fallbackTextures.black;
    case MaterialSemantic::Roughness:   return m_fallbackTextures.gray;
    case MaterialSemantic::Occlusion:   return m_fallbackTextures.white;
    case MaterialSemantic::Emissive:    return m_fallbackTextures.black;
    case MaterialSemantic::Opacity:     return m_fallbackTextures.white;
    case MaterialSemantic::AlphaCutoff: return m_fallbackTextures.white;
    // ORM fallback: gray encodes Occlusion=1 (R), Roughness=0.5 (G), Metallic=0 (B) — neutral PBR defaults.
    case MaterialSemantic::ORM:         return m_fallbackTextures.gray;
    default:                            return m_fallbackTextures.white;
    }
}

void ShaderRuntime::CreateFallbackTextures()
{
    if (!m_device) return;

    auto create1x1 = [&](const char* name, const std::array<uint8_t, 4>& rgba) -> TextureHandle
    {
        TextureDesc td{};
        td.width = 1u;
        td.height = 1u;
        td.format = Format::RGBA8_UNORM;
        td.usage = ResourceUsage::ShaderResource | ResourceUsage::CopyDest;
        td.initialState = ResourceState::ShaderRead;
        td.debugName = name;
        TextureHandle tex = m_device->CreateTexture(td);
        if (tex.IsValid())
            m_device->UploadTextureData(tex, rgba.data(), rgba.size(), 0u, 0u);
        return tex;
    };

    m_fallbackTextures.white = create1x1("Fallback_White", {255u, 255u, 255u, 255u});
    m_fallbackTextures.black = create1x1("Fallback_Black", {0u, 0u, 0u, 255u});
    m_fallbackTextures.gray = create1x1("Fallback_Gray", {128u, 128u, 128u, 255u});
    m_fallbackTextures.neutralNormal = create1x1("Fallback_NeutralNormal", {128u, 128u, 255u, 255u});
}

void ShaderRuntime::CreateDefaultSamplers()
{
    if (!m_device) return;

    SamplerDesc linearWrap;
    linearWrap.addressU = linearWrap.addressV = linearWrap.addressW = WrapMode::Repeat;
    linearWrap.minFilter = linearWrap.magFilter = linearWrap.mipFilter = FilterMode::Linear;
    m_samplers.linearWrap = m_device->CreateSampler(linearWrap);

    SamplerDesc linearClamp = linearWrap;
    linearClamp.addressU = linearClamp.addressV = linearClamp.addressW = WrapMode::Clamp;
    m_samplers.linearClamp = m_device->CreateSampler(linearClamp);

    SamplerDesc pointClamp = linearClamp;
    pointClamp.minFilter = pointClamp.magFilter = pointClamp.mipFilter = FilterMode::Nearest;
    m_samplers.pointClamp = m_device->CreateSampler(pointClamp);

    SamplerDesc shadow = linearClamp;
    shadow.compareFunc = CompareFunc::LessEqual;
    m_samplers.shadowPCF = m_device->CreateSampler(shadow);
}


ShaderHandle ShaderRuntime::GetOrCreateVariant(ShaderHandle shaderAssetHandle,
                                               ShaderPassType pass,
                                               ShaderVariantFlag flags)
{
    if (!m_device || !m_assets || !shaderAssetHandle.IsValid())
        return ShaderHandle::Invalid();

    auto* shaderAsset = m_assets->shaders.Get(shaderAssetHandle);
    if (!shaderAsset)
        return ShaderHandle::Invalid();

    const ShaderVariantKey key{ shaderAssetHandle, pass, flags };
    const assets::ShaderTargetProfile target = ShaderCompiler::ResolveTargetProfile(*m_device);
    return m_variantCache.GetOrCreate(*shaderAsset, target, key);
}

bool ShaderRuntime::CollectShaderRequests(const MaterialSystem& materials,
                                        std::vector<ShaderHandle>& outRequests) const
{
    outRequests.clear();
    outRequests.reserve(materials.DescCount() * 2u);

    for (uint32_t i = 0; i < materials.DescCount(); ++i)
    {
        const MaterialHandle material = MaterialHandle::Make(i, 1u);
        const MaterialDesc* desc = materials.GetDesc(material);
        if (!desc)
            continue;
        if (desc->vertexShader.IsValid())
            outRequests.push_back(desc->vertexShader);
        if (desc->fragmentShader.IsValid())
            outRequests.push_back(desc->fragmentShader);
    }

    std::sort(outRequests.begin(), outRequests.end(), [](const ShaderHandle& a, const ShaderHandle& b) {
        return a.value < b.value;
    });
    outRequests.erase(std::unique(outRequests.begin(), outRequests.end(), [](const ShaderHandle& a, const ShaderHandle& b) {
        return a == b;
    }), outRequests.end());
    return true;
}

bool ShaderRuntime::CollectMaterialRequests(const MaterialSystem& materials,
                                          std::vector<MaterialHandle>& outRequests) const
{
    outRequests.clear();
    outRequests.reserve(materials.DescCount());

    for (uint32_t i = 0; i < materials.DescCount(); ++i)
    {
        const MaterialHandle material = MaterialHandle::Make(i, 1u);
        if (materials.GetDesc(material))
            outRequests.push_back(material);
    }

    std::sort(outRequests.begin(), outRequests.end(), [](const MaterialHandle& a, const MaterialHandle& b) {
        return a.value < b.value;
    });
    outRequests.erase(std::unique(outRequests.begin(), outRequests.end(), [](const MaterialHandle& a, const MaterialHandle& b) {
        return a == b;
    }), outRequests.end());
    return true;
}

ShaderHandle ShaderRuntime::PrepareShaderAsset(ShaderHandle shaderAssetHandle)
{
    if (!RequireRenderThread("PrepareShaderAsset"))
        return ShaderHandle::Invalid();
    if (!m_device || !m_assets || !shaderAssetHandle.IsValid())
        return ShaderHandle::Invalid();

    if (auto it = m_shaderAssets.find(shaderAssetHandle); it != m_shaderAssets.end() && it->second.gpuHandle.IsValid())
        return it->second.gpuHandle;

    const assets::ShaderAsset* shaderAsset = m_assets->shaders.Get(shaderAssetHandle);
    if (!shaderAsset)
    {
        Debug::LogError("ShaderRuntime.cpp: missing shader asset %u", shaderAssetHandle.value);
        return ShaderHandle::Invalid();
    }

    const ShaderStageMask stageMask = ToStageMask(shaderAsset->stage);
    ShaderHandle gpuHandle = ShaderHandle::Invalid();
    bool fromBytecode = false;
    bool fromCompiledArtifact = false;
    uint64_t compiledHash = 0ull;
    const auto target = ShaderCompiler::ResolveTargetProfile(*m_device);

    if (const auto* compiled = FindCompiledArtifact(*shaderAsset))
    {
        fromCompiledArtifact = true;
        compiledHash = compiled->sourceHash;
        if (!compiled->bytecode.empty())
        {
            gpuHandle = m_device->CreateShaderFromBytecode(compiled->bytecode.data(), compiled->bytecode.size(), stageMask, compiled->debugName.empty() ? shaderAsset->debugName : compiled->debugName);
            fromBytecode = true;
        }
        else
        {
            if (target == assets::ShaderTargetProfile::Vulkan_SPIRV)
            {
                Debug::LogError("ShaderRuntime.cpp: Vulkan requires SPIR-V bytecode for '%s'",
                                (compiled->debugName.empty() ? shaderAsset->debugName : compiled->debugName).c_str());
            }
            else
            {
                gpuHandle = m_device->CreateShaderFromSource(compiled->sourceText, stageMask, compiled->entryPoint, compiled->debugName.empty() ? shaderAsset->debugName : compiled->debugName);
            }
        }
    }
    else if (!shaderAsset->bytecode.empty())
    {
        gpuHandle = m_device->CreateShaderFromBytecode(shaderAsset->bytecode.data(), shaderAsset->bytecode.size(), stageMask, shaderAsset->debugName);
        fromBytecode = true;
        compiledHash = HashBytes(shaderAsset->bytecode.data(), shaderAsset->bytecode.size());
    }
    else
    {
        if (target == assets::ShaderTargetProfile::Vulkan_SPIRV)
        {
            Debug::LogError("ShaderRuntime.cpp: no SPIR-V bytecode available for shader '%s'", shaderAsset->debugName.c_str());
        }
        else
        {
            gpuHandle = m_device->CreateShaderFromSource(shaderAsset->sourceCode, stageMask, shaderAsset->entryPoint, shaderAsset->debugName);
            compiledHash = HashBytes(shaderAsset->sourceCode.data(), shaderAsset->sourceCode.size());
        }
    }

    ShaderAssetStatus status{};
    status.assetHandle = shaderAssetHandle;
    status.gpuHandle = gpuHandle;
    status.stage = stageMask;
    status.target = target;
    if (const auto* compiled = FindCompiledArtifact(*shaderAsset))
        status.contract = compiled->contract;
    status.compiledHash = compiledHash;
    status.loaded = gpuHandle.IsValid();
    status.fromBytecode = fromBytecode;
    status.fromCompiledArtifact = fromCompiledArtifact;
    m_shaderAssets[shaderAssetHandle] = status;
    return gpuHandle;
}

bool ShaderRuntime::CommitShaderRequests(const std::vector<ShaderHandle>& requests)
{
    if (!RequireRenderThread("CommitShaderRequests"))
        return false;

    bool ok = true;
    for (ShaderHandle handle : requests)
        ok = PrepareShaderAsset(handle).IsValid() && ok;
    return ok;
}

bool ShaderRuntime::PrepareAllShaderAssets()
{
    if (!m_assets) return true;
    std::vector<ShaderHandle> requests;
    m_assets->shaders.ForEach([&](ShaderHandle handle, assets::ShaderAsset&) {
        requests.push_back(handle);
    });
    std::sort(requests.begin(), requests.end(), [](const ShaderHandle& a, const ShaderHandle& b) { return a.value < b.value; });
    requests.erase(std::unique(requests.begin(), requests.end(), [](const ShaderHandle& a, const ShaderHandle& b) { return a == b; }), requests.end());
    return CommitShaderRequests(requests);
}

std::vector<ResolvedMaterialBinding> ShaderRuntime::ResolveBindings(const MaterialSystem& materials,
                                                                    MaterialHandle material) const
{
    std::vector<ResolvedMaterialBinding> resolved;
    const MaterialDesc* desc = materials.GetDesc(material);
    const MaterialInstance* inst = materials.GetInstance(material);
    if (!desc || !inst) return resolved;

    ResolvedMaterialBinding materialCB{};
    materialCB.kind = ResolvedMaterialBinding::Kind::ConstantBuffer;
    materialCB.name = "PerMaterial";
    materialCB.slot = CBSlots::PerMaterial;
    materialCB.stages = kGraphicsStages;
    resolved.push_back(materialCB);

    const auto pushTexture = [&](const char* name, uint32_t slot, TextureHandle texture, uint32_t /*samplerIdx*/ = 0u)
    {
        if (!texture.IsValid())
            return;
        ResolvedMaterialBinding tex{};
        tex.kind = ResolvedMaterialBinding::Kind::Texture;
        tex.name = name;
        tex.slot = slot;
        tex.stages = ShaderStageMask::Fragment;
        tex.texture = texture;
        resolved.push_back(tex);
    };

    const auto baseColorTex = materials.GetSemanticTexture(material, MaterialSemantic::BaseColor);
    const auto normalTex    = materials.GetSemanticTexture(material, MaterialSemantic::Normal);
    const auto emissiveTex  = materials.GetSemanticTexture(material, MaterialSemantic::Emissive);
    const auto ormExplicit  = materials.GetSemanticTexture(material, MaterialSemantic::ORM);
    const auto metallicTex  = materials.GetSemanticTexture(material, MaterialSemantic::Metallic);
    const auto roughnessTex = materials.GetSemanticTexture(material, MaterialSemantic::Roughness);
    const auto occlusionTex = materials.GetSemanticTexture(material, MaterialSemantic::Occlusion);

    // Resolve the ORM carrier for slot t2.
    // Priority: explicit ORM semantic > Metallic > Roughness > Occlusion (individual single-channel).
    // Explicit ORM is the preferred GLTF-style path; the individual fallback is the legacy path.
    TextureHandle ormTex = TextureHandle::Invalid();
    uint32_t ormSamplerIdx = 0u;
    if (ormExplicit.IsValid())
    {
        ormTex = ormExplicit;
        ormSamplerIdx = inst->semanticTextures[static_cast<size_t>(MaterialSemantic::ORM)].samplerIdx;
    }
    else
    {
        const auto tryTakeOrm = [&](MaterialSemantic semantic, TextureHandle tex)
        {
            if (!tex.IsValid() || ormTex.IsValid())
                return;
            ormTex = tex;
            ormSamplerIdx = inst->semanticTextures[static_cast<size_t>(semantic)].samplerIdx;
        };
        tryTakeOrm(MaterialSemantic::Metallic,   metallicTex);
        tryTakeOrm(MaterialSemantic::Roughness,  roughnessTex);
        tryTakeOrm(MaterialSemantic::Occlusion,  occlusionTex);
    }

    const TextureHandle ormFallback = ResolveFallbackTexture(MaterialSemantic::ORM);
    pushTexture("BaseColor", TexSlots::Albedo,
                baseColorTex.IsValid() ? baseColorTex : ResolveFallbackTexture(MaterialSemantic::BaseColor),
                inst->semanticTextures[static_cast<size_t>(MaterialSemantic::BaseColor)].samplerIdx);
    pushTexture("Normal", TexSlots::Normal,
                normalTex.IsValid() ? normalTex : ResolveFallbackTexture(MaterialSemantic::Normal),
                inst->semanticTextures[static_cast<size_t>(MaterialSemantic::Normal)].samplerIdx);
    pushTexture("ORM", TexSlots::ORM,
                ormTex.IsValid() ? ormTex : ormFallback,
                ormSamplerIdx);
    pushTexture("Emissive", TexSlots::Emissive,
                emissiveTex.IsValid() ? emissiveTex : ResolveFallbackTexture(MaterialSemantic::Emissive),
                inst->semanticTextures[static_cast<size_t>(MaterialSemantic::Emissive)].samplerIdx);

    const auto& params = inst->instanceParams.empty() ? desc->params : inst->instanceParams;
    for (const auto& param : params)
    {
        if (param.type == MaterialParam::Type::Texture)
        {
            const uint32_t slot = ResolveTextureSlotByName(param.name);
            if (slot == TexSlots::Albedo || slot == TexSlots::Normal || slot == TexSlots::ORM || slot == TexSlots::Emissive)
                continue;

            ResolvedMaterialBinding binding{};
            binding.kind = ResolvedMaterialBinding::Kind::Texture;
            binding.name = param.name;
            binding.slot = slot;
            binding.stages = ShaderStageMask::Fragment;
            binding.texture = param.texture;
            resolved.push_back(binding);
        }
        else if (param.type == MaterialParam::Type::Sampler)
        {
            ResolvedMaterialBinding binding{};
            binding.kind = ResolvedMaterialBinding::Kind::Sampler;
            binding.name = param.name;
            binding.slot = ResolveSamplerSlotByName(param.name);
            binding.stages = ShaderStageMask::Fragment;
            binding.samplerIndex = param.samplerIdx;
            resolved.push_back(binding);
        }
    }
    return resolved;
}

PipelineDesc ShaderRuntime::BuildPipelineDesc(const MaterialSystem& materials,
                                              MaterialHandle material,
                                              ShaderHandle gpuVS,
                                              ShaderHandle gpuPS) const
{
    const MaterialDesc* desc = materials.GetDesc(material);
    return BuildPipelineDescForPass(materials, material, gpuVS, gpuPS,
                                    desc ? desc->passTag : RenderPassTag::Opaque);
}

PipelineDesc ShaderRuntime::BuildPipelineDescForPass(const MaterialSystem& materials,
                                                     MaterialHandle material,
                                                     ShaderHandle gpuVS,
                                                     ShaderHandle gpuPS,
                                                     RenderPassTag pass) const
{
    PipelineDesc pd;
    const MaterialDesc* desc = materials.GetDesc(material);
    if (!desc) return pd;

    if (gpuVS.IsValid()) pd.shaderStages.push_back({gpuVS, ShaderStageMask::Vertex});
    if (gpuPS.IsValid()) pd.shaderStages.push_back({gpuPS, ShaderStageMask::Fragment});
    pd.vertexLayout = desc->vertexLayout;
    pd.rasterizer = desc->rasterizer;
    pd.blendStates[0] = desc->blend;
    pd.depthStencil = desc->depthStencil;
    pd.topology = desc->topology;
    pd.colorFormat = (pass == RenderPassTag::Shadow) ? Format::Unknown : desc->colorFormat;
    pd.depthFormat = (pass == RenderPassTag::Shadow) ? Format::D32_FLOAT : desc->depthFormat;
    pd.pipelineClass = PipelineClass::Graphics;
    if (m_assets)
    {
        uint64_t contractHash = 0ull;
        uint64_t pipelineBindingHash = 0ull;

        const auto* vsAsset = m_assets->shaders.Get((pass == RenderPassTag::Shadow && desc->shadowShader.IsValid()) ? desc->shadowShader : desc->vertexShader);
        const auto* vsArtifact = vsAsset ? FindCompiledArtifact(*vsAsset) : nullptr;
        contractHash ^= FoldContractHash(vsArtifact, 0x9E3779B185EBCA87ull);
        pipelineBindingHash ^= FoldPipelineBindingHash(vsArtifact, 0x517CC1B727220A95ull);

        if (gpuPS.IsValid())
        {
            const auto* psAsset = m_assets->shaders.Get(desc->fragmentShader);
            const auto* psArtifact = psAsset ? FindCompiledArtifact(*psAsset) : nullptr;
            contractHash ^= FoldContractHash(psArtifact, 0xC2B2AE3D27D4EB4Full);
            pipelineBindingHash ^= FoldPipelineBindingHash(psArtifact, 0x165667B19E3779F9ull);
        }

        pd.shaderContractHash = contractHash;
        pd.pipelineLayoutHash = pipelineBindingHash != 0ull ? pipelineBindingHash : contractHash;
    }
    pd.debugName = desc->name + ((pass == RenderPassTag::Shadow) ? "_ShadowPipeline" : "_Pipeline");
    return pd;
}

PipelineHandle ShaderRuntime::ResolvePipelineForPass(const MaterialSystem& materials,
                                                     MaterialHandle material,
                                                     const MaterialGpuState& state,
                                                     RenderPassTag pass)
{
    const MaterialDesc* desc = materials.GetDesc(material);
    if (!desc)
        return PipelineHandle::Invalid();

    if (pass != RenderPassTag::Shadow || desc->passTag == RenderPassTag::Shadow)
        return state.pipeline;

    ShaderHandle gpuVS = desc->shadowShader.IsValid() ? desc->shadowShader : desc->vertexShader;
    if (m_assets && gpuVS.IsValid())
        gpuVS = PrepareShaderAsset(gpuVS);
    if (!gpuVS.IsValid())
        return PipelineHandle::Invalid();

    const PipelineDesc pipelineDesc = BuildPipelineDescForPass(materials, material, gpuVS, ShaderHandle::Invalid(), pass);
    const PipelineKey pipelineKey = PipelineKey::From(pipelineDesc, pass);
    return m_pipelineCache.GetOrCreate(pipelineKey, [&](const PipelineKey&) {
        return m_device->CreatePipeline(pipelineDesc);
    });
}

bool ShaderRuntime::ValidateMaterial(const MaterialSystem& materials,
                                     MaterialHandle material,
                                     std::vector<ShaderValidationIssue>& outIssues) const
{
    outIssues.clear();
    const MaterialDesc* desc = materials.GetDesc(material);
    const MaterialInstance* inst = materials.GetInstance(material);
    if (!desc || !inst)
    {
        outIssues.push_back({ShaderValidationIssue::Severity::Error, "invalid material handle"});
        return false;
    }

    const bool isShadowPass = desc->passTag == RenderPassTag::Shadow;
    const ShaderHandle vertexShader = (isShadowPass && desc->shadowShader.IsValid())
        ? desc->shadowShader
        : desc->vertexShader;

    if (!vertexShader.IsValid())
        outIssues.push_back({ShaderValidationIssue::Severity::Error, "missing vertex shader"});
    if (!desc->fragmentShader.IsValid() && !isShadowPass)
        outIssues.push_back({ShaderValidationIssue::Severity::Error, "missing fragment shader"});

    {
        // Validate ORM slot t2 contract: if the explicit ORM semantic is set, it is the sole carrier
        // and individual M/R/O textures are ignored for slot t2 (no conflict possible).
        // If no explicit ORM is set, all individual textures that are present must share the same
        // handle — otherwise the shader would see an undefined ORM composition.
        const TextureHandle ormExplicitHandle = materials.GetSemanticTexture(material, MaterialSemantic::ORM);
        if (!ormExplicitHandle.IsValid())
        {
            const TextureHandle metallic  = materials.GetSemanticTexture(material, MaterialSemantic::Metallic);
            const TextureHandle roughness = materials.GetSemanticTexture(material, MaterialSemantic::Roughness);
            const TextureHandle occlusion = materials.GetSemanticTexture(material, MaterialSemantic::Occlusion);

            TextureHandle ormCarrier = TextureHandle::Invalid();
            const auto validateOrmCarrier = [&](TextureHandle candidate, const char* label)
            {
                if (!candidate.IsValid())
                    return;
                if (!ormCarrier.IsValid())
                {
                    ormCarrier = candidate;
                    return;
                }
                if (ormCarrier != candidate)
                {
                    outIssues.push_back({ShaderValidationIssue::Severity::Error,
                                         std::string("packed ORM contract violated: '") + label +
                                         "' uses a different texture handle. "
                                         "Either use MaterialSemantic::ORM for a pre-packed texture, "
                                         "or ensure Metallic/Roughness/Occlusion all reference the same handle."});
                }
            };

            validateOrmCarrier(metallic,  "Metallic");
            validateOrmCarrier(roughness, "Roughness");
            validateOrmCarrier(occlusion, "Occlusion");
        }
    }

    std::unordered_set<uint32_t> textureSlots;
    std::unordered_set<uint32_t> samplerSlots;
    for (const auto& binding : ResolveBindings(materials, material))
    {
        if (binding.kind == ResolvedMaterialBinding::Kind::Texture)
        {
            if (!binding.texture.IsValid())
                outIssues.push_back({ShaderValidationIssue::Severity::Warning, "texture binding '" + binding.name + "' has invalid texture handle"});
            if (!textureSlots.insert(binding.slot).second)
                outIssues.push_back({ShaderValidationIssue::Severity::Error, "duplicate texture slot " + std::to_string(binding.slot)});
        }
        if (binding.kind == ResolvedMaterialBinding::Kind::Sampler)
        {
            if (!samplerSlots.insert(binding.slot).second)
                outIssues.push_back({ShaderValidationIssue::Severity::Error, "duplicate sampler slot " + std::to_string(binding.slot)});
        }
    }

    const auto& cbData = const_cast<MaterialSystem&>(materials).GetCBData(material);
    if (!cbData.empty() && (cbData.size() % 16u) != 0u)
        outIssues.push_back({ShaderValidationIssue::Severity::Error, "per-material constant buffer is not 16-byte aligned"});

    const auto validateShaderContract = [&](ShaderHandle shaderHandle, ShaderStageMask expectedStage, const char* role)
    {
        if (!m_assets || !shaderHandle.IsValid())
            return;
        const auto* shaderAsset = m_assets->shaders.Get(shaderHandle);
        if (!shaderAsset)
            return;
        const auto* artifact = FindCompiledArtifact(*shaderAsset);
        if (!artifact)
        {
            outIssues.push_back({ShaderValidationIssue::Severity::Warning, std::string(role) + " has no compiled artifact contract"});
            return;
        }
        if (!artifact->contract.interfaceLayout.usesEngineBindingModel)
            outIssues.push_back({ShaderValidationIssue::Severity::Error, std::string(role) + " does not use the engine binding contract"});
        if ((artifact->contract.stageMask & ToContractStageBits(expectedStage)) == 0u)
            outIssues.push_back({ShaderValidationIssue::Severity::Error, std::string(role) + " contract stage mismatch"});
    };

    validateShaderContract(vertexShader, ShaderStageMask::Vertex, "vertex shader");
    if (!isShadowPass)
        validateShaderContract(desc->fragmentShader, ShaderStageMask::Fragment, "fragment shader");

    return std::none_of(outIssues.begin(), outIssues.end(), [](const ShaderValidationIssue& issue) {
        return issue.severity == ShaderValidationIssue::Severity::Error;
    });
}

bool ShaderRuntime::PrepareMaterial(const MaterialSystem& materials, MaterialHandle material)
{
    if (!RequireRenderThread("PrepareMaterial"))
        return false;
    if (!m_device || !material.IsValid())
        return false;

    std::vector<ShaderValidationIssue> issues;
    const bool valid = ValidateMaterial(materials, material, issues);
    const MaterialDesc* desc = materials.GetDesc(material);
    if (!desc)
        return false;

    const bool isShadowPass = desc->passTag == RenderPassTag::Shadow;
    const ShaderHandle sourceVS = (isShadowPass && desc->shadowShader.IsValid())
        ? desc->shadowShader
        : desc->vertexShader;

    ShaderHandle gpuVS = sourceVS;
    ShaderHandle gpuPS = desc->fragmentShader;
    if (m_assets)
    {
        gpuVS = PrepareShaderAsset(sourceVS);
        gpuPS = desc->fragmentShader.IsValid() ? PrepareShaderAsset(desc->fragmentShader) : ShaderHandle::Invalid();
    }

    if (!gpuVS.IsValid() || (!gpuPS.IsValid() && !isShadowPass))
        return false;

    MaterialGpuState next{};
    next.material = material;
    next.vertexShader = gpuVS;
    next.fragmentShader = gpuPS;
    next.bindings = ResolveBindings(materials, material);
    next.issues = issues;
    next.valid = valid;

    const auto& cbData = const_cast<MaterialSystem&>(materials).GetCBData(material);
    next.contentHash = HashMaterialState(cbData, next.bindings);

    const PipelineDesc pipelineDesc = BuildPipelineDesc(materials, material, gpuVS, gpuPS);
    PipelineKey pipelineKey = PipelineKey::From(pipelineDesc, desc->passTag);
    next.pipeline = m_pipelineCache.GetOrCreate(pipelineKey, [&](const PipelineKey&) {
        return m_device->CreatePipeline(pipelineDesc);
    });

    if (!cbData.empty())
    {
        BufferDesc cbDesc{};
        cbDesc.byteSize = cbData.size();
        cbDesc.type = BufferType::Constant;
        cbDesc.usage = ResourceUsage::ConstantBuffer | ResourceUsage::CopyDest;
        cbDesc.access = MemoryAccess::GpuOnly;
        cbDesc.debugName = desc->name + "_PerMaterialCB";
        next.perMaterialCB = m_device->CreateBuffer(cbDesc);
        next.perMaterialCBSize = static_cast<uint32_t>(cbData.size());
        m_device->UploadBufferData(next.perMaterialCB, cbData.data(), cbData.size());
    }

    auto it = m_materialStates.find(material);
    if (it != m_materialStates.end())
        DestroyMaterialState(it->second);
    m_materialStates[material] = next;
    return next.valid && next.pipeline.IsValid();
}

bool ShaderRuntime::CommitMaterialRequests(const MaterialSystem& materials,
                                         const std::vector<MaterialHandle>& requests)
{
    if (!RequireRenderThread("CommitMaterialRequests"))
        return false;

    bool ok = true;
    for (MaterialHandle handle : requests)
        ok = PrepareMaterial(materials, handle) && ok;
    return ok;
}

bool ShaderRuntime::PrepareAllMaterials(const MaterialSystem& materials)
{
    std::vector<MaterialHandle> requests;
    const bool collected = CollectMaterialRequests(materials, requests);
    return collected && CommitMaterialRequests(materials, requests);
}

const MaterialGpuState* ShaderRuntime::GetMaterialState(MaterialHandle material) const noexcept
{
    if (auto it = m_materialStates.find(material); it != m_materialStates.end())
        return &it->second;
    return nullptr;
}

const ShaderAssetStatus* ShaderRuntime::GetShaderStatus(ShaderHandle shaderAssetHandle) const noexcept
{
    if (auto it = m_shaderAssets.find(shaderAssetHandle); it != m_shaderAssets.end())
        return &it->second;
    return nullptr;
}

bool ShaderRuntime::BindMaterial(ICommandList& cmd,
                                 const MaterialSystem& materials,
                                 MaterialHandle material,
                                 BufferHandle perFrameCB,
                                 BufferHandle perObjectCB,
                                 BufferHandle perPassCB,
                                 RenderPassTag passOverride)
{
    if (!RequireRenderThread("BindMaterial"))
        return false;
    auto* state = const_cast<MaterialGpuState*>(GetMaterialState(material));
    if (!state || !state->valid)
    {
        if (!PrepareMaterial(materials, material))
            return false;
        state = const_cast<MaterialGpuState*>(GetMaterialState(material));
        if (!state || !state->valid) return false;
    }

    const PipelineHandle pipeline = ResolvePipelineForPass(materials, material, *state, passOverride);
    if (!pipeline.IsValid())
        return false;

    cmd.SetPipeline(pipeline);
    if (perFrameCB.IsValid())
        cmd.SetConstantBuffer(CBSlots::PerFrame, perFrameCB, kGraphicsStages);
    if (perObjectCB.IsValid())
        cmd.SetConstantBuffer(CBSlots::PerObject, perObjectCB, kGraphicsStages);
    if (state->perMaterialCB.IsValid())
        cmd.SetConstantBuffer(CBSlots::PerMaterial, state->perMaterialCB, kGraphicsStages);
    if (perPassCB.IsValid())
        cmd.SetConstantBuffer(CBSlots::PerPass, perPassCB, kGraphicsStages);

    for (const auto& binding : state->bindings)
    {
        switch (binding.kind)
        {
        case ResolvedMaterialBinding::Kind::Texture:
            if (binding.texture.IsValid())
                cmd.SetShaderResource(binding.slot, binding.texture, binding.stages);
            break;
        case ResolvedMaterialBinding::Kind::Sampler:
        {
            uint32_t sampler = binding.samplerIndex;
            if (sampler == 0u)
            {
                switch (binding.slot)
                {
                case SamplerSlots::LinearClamp: sampler = m_samplers.linearClamp; break;
                case SamplerSlots::PointClamp: sampler = m_samplers.pointClamp; break;
                case SamplerSlots::ShadowPCF: sampler = m_samplers.shadowPCF; break;
                default: sampler = m_samplers.linearWrap; break;
                }
            }
            cmd.SetSampler(binding.slot, sampler, binding.stages);
            break;
        }
        case ResolvedMaterialBinding::Kind::ConstantBuffer:
            break;
        }
    }
    return true;
}

bool ShaderRuntime::BindMaterialWithRange(ICommandList& cmd,
                                          const MaterialSystem& materials,
                                          MaterialHandle material,
                                          BufferHandle   perFrameCB,
                                          BufferBinding  perObjectBinding,
                                          BufferBinding  perPassBinding,
                                          RenderPassTag  passOverride)
{
    if (!RequireRenderThread("BindMaterialWithRange"))
        return false;
    auto* state = const_cast<MaterialGpuState*>(GetMaterialState(material));
    if (!state || !state->valid)
    {
        if (!PrepareMaterial(materials, material))
            return false;
        state = const_cast<MaterialGpuState*>(GetMaterialState(material));
        if (!state || !state->valid) return false;
    }

    const PipelineHandle pipeline = ResolvePipelineForPass(materials, material, *state, passOverride);
    if (!pipeline.IsValid())
        return false;

    cmd.SetPipeline(pipeline);
    if (perFrameCB.IsValid())
        cmd.SetConstantBuffer(CBSlots::PerFrame, perFrameCB, kGraphicsStages);
    if (perObjectBinding.IsValid())
        cmd.SetConstantBufferRange(CBSlots::PerObject, perObjectBinding, kGraphicsStages);
    if (state->perMaterialCB.IsValid())
        cmd.SetConstantBuffer(CBSlots::PerMaterial, state->perMaterialCB, kGraphicsStages);
    if (perPassBinding.IsValid())
        cmd.SetConstantBufferRange(CBSlots::PerPass, perPassBinding, kGraphicsStages);

    for (const auto& binding : state->bindings)
    {
        switch (binding.kind)
        {
        case ResolvedMaterialBinding::Kind::Texture:
            if (binding.texture.IsValid())
                cmd.SetShaderResource(binding.slot, binding.texture, binding.stages);
            break;
        case ResolvedMaterialBinding::Kind::Sampler:
        {
            uint32_t sampler = binding.samplerIndex;
            if (sampler == 0u)
            {
                switch (binding.slot)
                {
                case SamplerSlots::LinearClamp: sampler = m_samplers.linearClamp; break;
                case SamplerSlots::PointClamp:  sampler = m_samplers.pointClamp;  break;
                case SamplerSlots::ShadowPCF:   sampler = m_samplers.shadowPCF;   break;
                default: sampler = m_samplers.linearWrap; break;
                }
            }
            cmd.SetSampler(binding.slot, sampler, binding.stages);
            break;
        }
        case ResolvedMaterialBinding::Kind::ConstantBuffer:
            break;
        }
    }
    return true;
}

void ShaderRuntime::DestroyMaterialState(MaterialGpuState& state)
{
    if (!m_device) return;
    if (state.perMaterialCB.IsValid())
        m_device->DestroyBuffer(state.perMaterialCB);
    state.perMaterialCB = BufferHandle::Invalid();
    state.perMaterialCBSize = 0u;
}

} // namespace engine::renderer
