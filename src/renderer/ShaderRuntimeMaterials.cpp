#include "renderer/ShaderRuntime.hpp"
#include "renderer/MaterialSystem.hpp"
#include "renderer/RenderPassRegistry.hpp"
#include "renderer/RenderWorld.hpp"
#include "core/Debug.hpp"
#include <algorithm>
#include <array>

namespace engine::renderer {

namespace {

const ShaderStageMask kGraphicsStages = ShaderStageMask::Vertex | ShaderStageMask::Fragment;
const ShaderStageMask kVulkanPerObjectPushStages = ShaderStageMask::Vertex;

[[nodiscard]] bool IsConstantBufferBindingAligned(const BufferBinding& binding) noexcept
{
    return (binding.offset % kConstantBufferAlignment) == 0u
        && (binding.size % 16u) == 0u;
}

[[nodiscard]] bool ShouldUseVulkanPerObjectPushConstants(const ShaderRuntime& runtime,
                                                         const PerObjectConstants* perObjectConstants) noexcept
{
    return perObjectConstants != nullptr
        && runtime.GetDevice() != nullptr
        && runtime.GetDevice()->GetShaderTargetProfile() == assets::ShaderTargetProfile::Vulkan_SPIRV;
}

[[nodiscard]] VulkanPerObjectPushConstants PackVulkanPerObjectPushConstants(const PerObjectConstants& source) noexcept
{
    VulkanPerObjectPushConstants packed{};
    // PerObjectConstants stores Mat4::Data() in column-major order:
    // [m00,m01,m02,m03, m10,m11,m12,m13, ...].
    // The Vulkan push-constant shader helpers evaluate dot(row, p), so we
    // must repack matrix rows explicitly.
    packed.worldRow0[0] = source.worldMatrix[0u];
    packed.worldRow0[1] = source.worldMatrix[4u];
    packed.worldRow0[2] = source.worldMatrix[8u];
    packed.worldRow0[3] = source.worldMatrix[12u];

    packed.worldRow1[0] = source.worldMatrix[1u];
    packed.worldRow1[1] = source.worldMatrix[5u];
    packed.worldRow1[2] = source.worldMatrix[9u];
    packed.worldRow1[3] = source.worldMatrix[13u];

    packed.worldRow2[0] = source.worldMatrix[2u];
    packed.worldRow2[1] = source.worldMatrix[6u];
    packed.worldRow2[2] = source.worldMatrix[10u];
    packed.worldRow2[3] = source.worldMatrix[14u];

    packed.worldInvTRow0[0] = source.worldMatrixInvT[0u];
    packed.worldInvTRow0[1] = source.worldMatrixInvT[4u];
    packed.worldInvTRow0[2] = source.worldMatrixInvT[8u];
    packed.worldInvTRow0[3] = source.worldMatrixInvT[12u];

    packed.worldInvTRow1[0] = source.worldMatrixInvT[1u];
    packed.worldInvTRow1[1] = source.worldMatrixInvT[5u];
    packed.worldInvTRow1[2] = source.worldMatrixInvT[9u];
    packed.worldInvTRow1[3] = source.worldMatrixInvT[13u];

    packed.worldInvTRow2[0] = source.worldMatrixInvT[2u];
    packed.worldInvTRow2[1] = source.worldMatrixInvT[6u];
    packed.worldInvTRow2[2] = source.worldMatrixInvT[10u];
    packed.worldInvTRow2[3] = source.worldMatrixInvT[14u];

    for (uint32_t i = 0u; i < 4u; ++i)
        packed.entityId[i] = source.entityId[i];
    return packed;
}

VertexLayout BuildShadowVertexLayout(const VertexLayout& source, bool needsTexCoord) noexcept
{
    VertexLayout filtered{};

    for (const auto& attr : source.attributes)
    {
        const bool keepPosition = attr.semantic == VertexSemantic::Position;
        const bool keepTexCoord = needsTexCoord && attr.semantic == VertexSemantic::TexCoord0;
        if (!keepPosition && !keepTexCoord)
            continue;
        filtered.attributes.push_back(attr);
    }

    for (const auto& binding : source.bindings)
    {
        const bool used = std::any_of(filtered.attributes.begin(), filtered.attributes.end(),
            [&](const VertexAttribute& attr) { return attr.binding == binding.binding; });
        if (used)
            filtered.bindings.push_back(binding);
    }

    return filtered;
}

bool HasErrors(const std::vector<ShaderValidationIssue>& issues) noexcept
{
    return std::any_of(issues.begin(), issues.end(), [](const ShaderValidationIssue& issue) {
        return issue.severity == ShaderValidationIssue::Severity::Error;
    });
}

uint32_t ResolveRuntimeSamplerIndex(uint32_t slot,
                                    uint32_t linearWrap,
                                    uint32_t linearClamp,
                                    uint32_t pointClamp,
                                    uint32_t shadowPCF) noexcept
{
    switch (slot)
    {
    case SamplerSlots::LinearWrap:  return linearWrap;
    case SamplerSlots::LinearClamp: return linearClamp;
    case SamplerSlots::PointClamp:  return pointClamp;
    case SamplerSlots::ShadowPCF:   return shadowPCF;
    default:                        return linearWrap;
    }
}

ShaderVariantFlag ResolveNormalEncodingVariantFlag(const ShaderRuntime& runtime,
                                                   const MaterialSystem& materials,
                                                   MaterialHandle material) noexcept
{
    const assets::AssetRegistry* registry = runtime.GetAssetRegistry();
    const MaterialInstance* inst = materials.GetInstance(material);
    if (!registry || !inst)
        return ShaderVariantFlag::None;

    for (uint32_t i = 0u; i < inst->layout.slotCount; ++i)
    {
        const ParameterSlot& slot = inst->layout.slots[i];
        if (slot.type != ParameterType::Texture2D && slot.type != ParameterType::TextureCube)
            continue;
        if (slot.binding != TexSlots::Normal)
            continue;

        const TextureHandle texture = inst->parameters.GetTexture(i);
        if (!texture.IsValid())
            return ShaderVariantFlag::None;

        const assets::TextureAsset* textureAsset = registry->textures.Get(texture);
        if (!textureAsset)
            return ShaderVariantFlag::None;

        return textureAsset->metadata.normalEncoding == assets::NormalEncoding::BC5_XY
            ? ShaderVariantFlag::NormalMapBC5
            : ShaderVariantFlag::None;
    }

    return ShaderVariantFlag::None;
}

ShaderHandle ResolveRuntimeShaderVariant(ShaderRuntime& runtime,
                                         ShaderHandle baseShader,
                                         ShaderPassType pass,
                                         ShaderVariantFlag flags)
{
    if (!baseShader.IsValid())
        return ShaderHandle::Invalid();

    if (runtime.GetAssetRegistry())
    {
        const ShaderHandle variant = runtime.GetOrCreateVariant(baseShader, pass, flags);
        if (variant.IsValid())
            return variant;
    }

    return runtime.PrepareShaderAsset(baseShader);
}

} // namespace

std::vector<ResolvedMaterialBinding> ShaderRuntime::ResolveBindings(const MaterialSystem& materials,
                                                                   MaterialHandle material) const
{
    std::vector<ResolvedMaterialBinding> resolved;
    const MaterialInstance* inst = materials.GetInstance(material);
    const MaterialDesc* desc = materials.GetDesc(material);
    if (!inst || !desc)
        return resolved;

    if (!inst->cbData.empty())
    {
        ResolvedMaterialBinding materialCB{};
        materialCB.kind = ResolvedMaterialBinding::Kind::ConstantBuffer;
        materialCB.name = "PerMaterial";
        materialCB.slot = CBSlots::PerMaterial;
        materialCB.stages = kGraphicsStages;
        resolved.push_back(materialCB);
    }

    for (uint32_t i = 0u; i < inst->layout.slotCount; ++i)
    {
        const ParameterSlot& slot = inst->layout.slots[i];
        switch (slot.type)
        {
        case ParameterType::Texture2D:
        case ParameterType::TextureCube:
        {
            TextureHandle texture = inst->parameters.GetTexture(i);
            if (!texture.IsValid())
                continue;
            ResolvedMaterialBinding binding{};
            binding.kind = ResolvedMaterialBinding::Kind::Texture;
            binding.name = std::string(slot.Name());
            binding.slot = slot.binding;
            binding.stages = slot.stageFlags == ShaderStageMask::None ? ShaderStageMask::Fragment : slot.stageFlags;
            binding.texture = texture;
            resolved.push_back(std::move(binding));
            break;
        }
        case ParameterType::Sampler:
        {
            ResolvedMaterialBinding binding{};
            binding.kind = ResolvedMaterialBinding::Kind::Sampler;
            binding.name = std::string(slot.Name());
            binding.slot = slot.binding;
            binding.stages = slot.stageFlags == ShaderStageMask::None ? ShaderStageMask::Fragment : slot.stageFlags;
            binding.samplerIndex = inst->parameters.GetSampler(i) != 0u ? inst->parameters.GetSampler(i) : m_samplers.linearWrap;
            resolved.push_back(std::move(binding));
            break;
        }
        case ParameterType::StructuredBuffer:
        {
            BufferHandle buffer = inst->parameters.GetBuffer(i);
            if (!buffer.IsValid())
                continue;
            ResolvedMaterialBinding binding{};
            binding.kind = ResolvedMaterialBinding::Kind::Buffer;
            binding.name = std::string(slot.Name());
            binding.slot = slot.binding;
            binding.stages = slot.stageFlags == ShaderStageMask::None ? kGraphicsStages : slot.stageFlags;
            binding.buffer = buffer;
            resolved.push_back(std::move(binding));
            break;
        }
        default:
            break;
        }
    }

    for (const auto& bindingDesc : desc->bindings)
    {
        if (bindingDesc.kind != MaterialBinding::Kind::Sampler)
            continue;

        const bool alreadyResolved = std::any_of(resolved.begin(), resolved.end(),
            [&](const ResolvedMaterialBinding& binding)
            {
                return binding.kind == ResolvedMaterialBinding::Kind::Sampler
                    && binding.slot == bindingDesc.slot;
            });
        if (alreadyResolved)
            continue;

        ResolvedMaterialBinding binding{};
        binding.kind = ResolvedMaterialBinding::Kind::Sampler;
        binding.name = bindingDesc.name;
        binding.slot = bindingDesc.slot;
        binding.stages = bindingDesc.stages == ShaderStageMask::None
            ? ShaderStageMask::Fragment
            : bindingDesc.stages;
        binding.samplerIndex = ResolveRuntimeSamplerIndex(bindingDesc.slot,
                                                         m_samplers.linearWrap,
                                                         m_samplers.linearClamp,
                                                         m_samplers.pointClamp,
                                                         m_samplers.shadowPCF);
        resolved.push_back(std::move(binding));
    }

    if (m_environment.active)
    {
        const std::array<std::pair<const char*, uint32_t>, 3> envSlots{{
            {"IBLIrradiance", TexSlots::IBLIrradiance},
            {"IBLPrefiltered", TexSlots::IBLPrefiltered},
            {"BRDFLUT", TexSlots::BRDFLUT}
        }};
        const std::array<TextureHandle, 3> envTextures{{
            m_environment.irradiance.IsValid() ? m_environment.irradiance : m_fallbackTextures.iblIrradiance,
            m_environment.prefiltered.IsValid() ? m_environment.prefiltered : m_fallbackTextures.iblPrefiltered,
            m_environment.brdfLut.IsValid() ? m_environment.brdfLut : m_fallbackTextures.brdfLut
        }};

        for (size_t i = 0; i < envSlots.size(); ++i)
        {
            if (!envTextures[i].IsValid())
                continue;
            ResolvedMaterialBinding binding{};
            binding.kind = ResolvedMaterialBinding::Kind::Texture;
            binding.name = envSlots[i].first;
            binding.slot = envSlots[i].second;
            binding.stages = ShaderStageMask::Fragment;
            binding.texture = envTextures[i];
            resolved.push_back(std::move(binding));
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
    PipelineDesc pd{};
    if (!desc)
        return pd;

    if (gpuVS.IsValid())
        pd.shaderStages.push_back({ gpuVS, ShaderStageMask::Vertex });
    if (gpuPS.IsValid())
        pd.shaderStages.push_back({ gpuPS, ShaderStageMask::Fragment });
    pd.rasterizer = desc->rasterizer;
    pd.depthStencil = desc->depthStencil;
    pd.blendStates[0] = desc->blend;
    pd.topology = desc->topology;
    pd.vertexLayout = desc->vertexLayout;
    pd.colorFormat = desc->colorFormat;
    pd.depthFormat = desc->depthFormat;
    pd.sampleCount = 1u;
    if (const MaterialInstance* inst = materials.GetInstance(material))
    {
        pd.shaderContractHash = inst->layout.layoutHash;
        pd.pipelineLayoutHash = inst->layout.layoutHash;
    }
    pd.debugName = desc->name + "_Pipeline";
    return pd;
}

PipelineDesc ShaderRuntime::BuildPipelineDescForPass(const MaterialSystem& materials,
                                                     MaterialHandle material,
                                                     ShaderHandle gpuVS,
                                                     ShaderHandle gpuPS,
                                                     RenderPassID pass) const
{
    PipelineDesc pd = BuildPipelineDesc(materials, material, gpuVS, gpuPS);
    if (pass == StandardRenderPasses::Shadow())
    {
        if (const MaterialDesc* desc = materials.GetDesc(material))
            pd.vertexLayout = BuildShadowVertexLayout(desc->vertexLayout, desc->renderPolicy.alphaTest);
        pd.colorFormat = Format::Unknown;
        pd.depthFormat = Format::D32_FLOAT;
        pd.rasterizer.depthBias       = m_shadowDepthBias;
        pd.rasterizer.slopeScaledBias = m_shadowSlopeBias;
    }
    return pd;
}

PipelineHandle ShaderRuntime::ResolvePipelineForPass(const MaterialSystem& materials,
                                                     MaterialHandle material,
                                                     MaterialGpuState& state,
                                                     RenderPassID pass)
{
    // Per-Pass-Cache: verhindert BuildPipelineDescForPass + heap-alloc pro Draw.
    const uint16_t passKey = pass.value;
    const auto cachedIt = state.resolvedPipelines.find(passKey);
    if (cachedIt != state.resolvedPipelines.end())
        return cachedIt->second;

    const MaterialDesc* desc = materials.GetDesc(material);
    if (!desc || !m_device)
        return PipelineHandle::Invalid();

    const bool shadowPass = pass == StandardRenderPasses::Shadow();
    const ShaderVariantFlag baseFlags = materials.BuildShaderVariantFlags(material) |
                                        ResolveNormalEncodingVariantFlag(*this, materials, material);
    const ShaderVariantFlag runtimeFlags = m_environment.active ? (baseFlags | ShaderVariantFlag::IBLMap) : baseFlags;

    ShaderHandle vs = state.vertexShader;
    ShaderHandle ps = state.fragmentShader;

    if (shadowPass)
    {
        const ShaderHandle shadowBase = desc->shadowShader.IsValid() ? desc->shadowShader : desc->vertexShader;
        vs = ResolveRuntimeShaderVariant(*this, shadowBase, ShaderPassType::Shadow, baseFlags | ShaderVariantFlag::ShadowPass);
        ps = ShaderHandle::Invalid();
    }
    else
    {
        if (!vs.IsValid())
            vs = ResolveRuntimeShaderVariant(*this, desc->vertexShader, ShaderPassType::Main, runtimeFlags);
        if (desc->fragmentShader.IsValid() && !ps.IsValid())
            ps = ResolveRuntimeShaderVariant(*this, desc->fragmentShader, ShaderPassType::Main, runtimeFlags);
    }

    if (!vs.IsValid() || (!shadowPass && desc->fragmentShader.IsValid() && !ps.IsValid()))
        return PipelineHandle::Invalid();

    const PipelineDesc pd = BuildPipelineDescForPass(materials, material, vs, ps, pass);
    const PipelineKey key = PipelineKey::From(pd, pass);
    const PipelineHandle handle = m_pipelineCache.GetOrCreate(key, [this, &pd](const PipelineKey&) { return m_device->CreatePipeline(pd); });
    state.resolvedPipelines.emplace(pass.value, handle);
    return handle;
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
        outIssues.push_back({ ShaderValidationIssue::Severity::Error, "material handle invalid" });
        return false;
    }

    if (!desc->vertexShader.IsValid())
        outIssues.push_back({ ShaderValidationIssue::Severity::Error, "vertex shader missing" });
    if (!desc->fragmentShader.IsValid() && desc->renderPass != StandardRenderPasses::Shadow())
        outIssues.push_back({ ShaderValidationIssue::Severity::Error, "fragment shader missing" });
    const bool expectsParameterLayout = !desc->params.empty() || !desc->bindings.empty();
    if (expectsParameterLayout && !inst->layout.IsValid())
        outIssues.push_back({ ShaderValidationIssue::Severity::Error, "shader parameter layout invalid" });

    std::array<bool, TexSlots::COUNT> usedTextureSlots{};
    std::array<bool, SamplerSlots::COUNT> usedSamplerSlots{};
    for (uint32_t i = 0u; i < inst->layout.slotCount; ++i)
    {
        const ParameterSlot& slot = inst->layout.slots[i];
        switch (slot.type)
        {
        case ParameterType::Texture2D:
        case ParameterType::TextureCube:
            if (slot.binding >= TexSlots::COUNT)
            {
                outIssues.push_back({ ShaderValidationIssue::Severity::Error,
                    "texture slot out of range: " + std::string(slot.Name()) });
                break;
            }
            if (usedTextureSlots[slot.binding])
            {
                outIssues.push_back({ ShaderValidationIssue::Severity::Error,
                    "duplicate texture slot: t" + std::to_string(slot.binding) + " (" + std::string(slot.Name()) + ")" });
            }
            usedTextureSlots[slot.binding] = true;
            break;
        case ParameterType::Sampler:
            if (slot.binding >= SamplerSlots::COUNT)
            {
                outIssues.push_back({ ShaderValidationIssue::Severity::Error,
                    "sampler slot out of range: " + std::string(slot.Name()) });
                break;
            }
            if (usedSamplerSlots[slot.binding])
            {
                outIssues.push_back({ ShaderValidationIssue::Severity::Error,
                    "duplicate sampler slot: s" + std::to_string(slot.binding) + " (" + std::string(slot.Name()) + ")" });
            }
            usedSamplerSlots[slot.binding] = true;
            break;
        default:
            break;
        }
    }

    return !HasErrors(outIssues);
}

bool ShaderRuntime::PrepareMaterial(const MaterialSystem& materials, MaterialHandle material)
{
    if (!RequireRenderThread("PrepareMaterial"))
        return false;
    if (!m_device)
        return false;

    auto existingIt = m_materialStates.find(material);
    if (existingIt != m_materialStates.end()
        && !NeedsMaterialRebuild(materials, material, existingIt->second))
    {
        return existingIt->second.valid;
    }

    const MaterialDesc* desc = materials.GetDesc(material);
    if (!desc)
        return false;

    MaterialGpuState next{};
    next.material = material;
    next.environmentRevision = m_environmentRevision;
    next.materialRevision = materials.GetRevision(material);
    next.issues.clear();

    const ShaderVariantFlag baseFlags = materials.BuildShaderVariantFlags(material) |
                                        ResolveNormalEncodingVariantFlag(*this, materials, material);
    const ShaderVariantFlag runtimeFlags = m_environment.active ? (baseFlags | ShaderVariantFlag::IBLMap) : baseFlags;

    next.vertexShader = ResolveRuntimeShaderVariant(*this, desc->vertexShader, ShaderPassType::Main, runtimeFlags);
    next.fragmentShader = desc->fragmentShader.IsValid()
        ? ResolveRuntimeShaderVariant(*this, desc->fragmentShader, ShaderPassType::Main, runtimeFlags)
        : ShaderHandle::Invalid();

    const auto& cbData = const_cast<MaterialSystem&>(materials).GetCBData(material);
    next.bindings = ResolveBindings(materials, material);

    const bool validationOk = ValidateMaterial(materials, material, next.issues);
    if (!validationOk)
    {
        auto it = m_materialStates.find(material);
        if (it != m_materialStates.end())
            RetireMaterialState(it->second);
        m_materialStates[material] = std::move(next);
        return false;
    }

    next.contentHash = HashMaterialState(cbData, next.bindings);

    if (!cbData.empty())
    {
        BufferDesc cbDesc{};
        cbDesc.byteSize = cbData.size();
        cbDesc.type = BufferType::Constant;
        cbDesc.usage = ResourceUsage::ConstantBuffer | ResourceUsage::CopyDest;
        cbDesc.access = MemoryAccess::GpuOnly;
        cbDesc.debugName = desc->name + "_PerMaterialCB";
        next.perMaterialCB = m_device->CreateBuffer(cbDesc);
        if (!next.perMaterialCB.IsValid())
        {
            next.issues.push_back({ ShaderValidationIssue::Severity::Error, "failed to create per-material constant buffer" });
        }
        else
        {
            m_device->UploadBufferData(next.perMaterialCB, cbData.data(), cbData.size());
            next.perMaterialCBSize = static_cast<uint32_t>(cbData.size());
        }
    }

    next.pipeline = ResolvePipelineForPass(materials, material, next, desc->renderPass);
    next.valid = !HasErrors(next.issues)
              && next.vertexShader.IsValid()
              && (!desc->fragmentShader.IsValid() || next.fragmentShader.IsValid())
              && next.pipeline.IsValid();

    auto it = m_materialStates.find(material);
    if (it != m_materialStates.end())
        RetireMaterialState(it->second);
    m_materialStates[material] = std::move(next);
    return m_materialStates[material].valid;
}

bool ShaderRuntime::CommitMaterialRequests(const MaterialSystem& materials,
                                           const std::vector<MaterialHandle>& requests)
{
    bool ok = true;
    for (MaterialHandle material : requests)
        ok = PrepareMaterial(materials, material) && ok;
    return ok;
}

bool ShaderRuntime::PrepareAllMaterials(const MaterialSystem& materials)
{
    std::vector<MaterialHandle> requests;
    if (!CollectMaterialRequests(materials, requests))
        return false;
    return CommitMaterialRequests(materials, requests);
}

const MaterialGpuState* ShaderRuntime::GetMaterialState(MaterialHandle material) const noexcept
{
    auto it = m_materialStates.find(material);
    return it != m_materialStates.end() ? &it->second : nullptr;
}

bool ShaderRuntime::BindMaterial(ICommandList& cmd,
                                 const MaterialSystem& materials,
                                 MaterialHandle material,
                                 BufferHandle perFrameCB,
                                 BufferHandle perObjectCB,
                                 BufferHandle perPassCB,
                                 RenderPassID passOverride)
{
    BufferBinding perObjectBinding{};
    if (perObjectCB.IsValid())
        perObjectBinding = BufferBinding{ perObjectCB, 0u, kConstantBufferAlignment };
    BufferBinding perPassBinding{};
    if (perPassCB.IsValid())
        perPassBinding = BufferBinding{ perPassCB, 0u, kConstantBufferAlignment };
    return BindMaterialWithRange(cmd, materials, material, perFrameCB, perObjectBinding, perPassBinding, nullptr, passOverride);
}

bool ShaderRuntime::BindMaterialWithRange(ICommandList& cmd,
                                          const MaterialSystem& materials,
                                          MaterialHandle material,
                                          BufferHandle perFrameCB,
                                          BufferBinding perObjectBinding,
                                          BufferBinding perPassBinding,
                                          const PerObjectConstants* perObjectConstants,
                                          RenderPassID passOverride)
{
    if (!m_device)
        return false;
    if (perObjectBinding.IsValid() && !IsConstantBufferBindingAligned(perObjectBinding))
        return false;
    if (perPassBinding.IsValid() && !IsConstantBufferBindingAligned(perPassBinding))
        return false;

    auto it = m_materialStates.find(material);
    if (it == m_materialStates.end() || NeedsMaterialRebuild(materials, material, it->second))
    {
        if (!PrepareMaterial(materials, material))
            return false;
        it = m_materialStates.find(material);
        if (it == m_materialStates.end() || !it->second.valid)
            return false;
    }

    MaterialGpuState& state = it->second;
    const PipelineHandle pipeline = ResolvePipelineForPass(materials, material, state, passOverride);
    if (!pipeline.IsValid())
        return false;

    cmd.SetPipeline(pipeline);
    if (perFrameCB.IsValid())
        cmd.SetConstantBuffer(CBSlots::PerFrame, perFrameCB, kGraphicsStages);
    if (ShouldUseVulkanPerObjectPushConstants(*this, perObjectConstants))
    {
        const VulkanPerObjectPushConstants packed = PackVulkanPerObjectPushConstants(*perObjectConstants);
        cmd.SetPushConstants(0u, &packed, static_cast<uint32_t>(sizeof(packed)), kVulkanPerObjectPushStages);
    }
    else if (perObjectBinding.IsValid())
        cmd.SetConstantBufferRange(CBSlots::PerObject, perObjectBinding, kGraphicsStages);
    if (state.perMaterialCB.IsValid())
        cmd.SetConstantBuffer(CBSlots::PerMaterial, state.perMaterialCB, kGraphicsStages);
    if (perPassBinding.IsValid())
        cmd.SetConstantBufferRange(CBSlots::PerPass, perPassBinding, kGraphicsStages);

    for (const auto& binding : state.bindings)
    {
        switch (binding.kind)
        {
        case ResolvedMaterialBinding::Kind::Texture:
            cmd.SetShaderResource(binding.slot, binding.texture, binding.stages);
            break;
        case ResolvedMaterialBinding::Kind::Sampler:
            cmd.SetSampler(binding.slot, binding.samplerIndex, binding.stages);
            break;
        case ResolvedMaterialBinding::Kind::Buffer:
        case ResolvedMaterialBinding::Kind::ConstantBuffer:
            break;
        }
    }

    return true;
}

void ShaderRuntime::UpdatePerObjectBinding(ICommandList& cmd,
                                            BufferBinding perObjectBinding,
                                            const PerObjectConstants* perObjectConstants)
{
    if (ShouldUseVulkanPerObjectPushConstants(*this, perObjectConstants))
    {
        const VulkanPerObjectPushConstants packed = PackVulkanPerObjectPushConstants(*perObjectConstants);
        cmd.SetPushConstants(0u, &packed, static_cast<uint32_t>(sizeof(packed)), kVulkanPerObjectPushStages);
    }
    else if (perObjectBinding.IsValid())
    {
        cmd.SetConstantBufferRange(CBSlots::PerObject, perObjectBinding, kGraphicsStages);
    }
}

void ShaderRuntime::RetireMaterialState(MaterialGpuState& state)
{
    if (!m_device)
    {
        state = {};
        return;
    }

    if (state.perMaterialCB.IsValid())
    {
        if (m_gpuRuntime)
        {
            const uint64_t retirementFence = m_gpuRuntime->GetRetirementFenceForCurrentFrame();
            m_gpuRuntime->ScheduleDestroy(state.perMaterialCB, retirementFence);
        }
        else
        {
            m_device->DestroyBuffer(state.perMaterialCB);
        }
    }
    state = {};
}

void ShaderRuntime::DestroyMaterialState(MaterialGpuState& state)
{
    if (!m_device)
        return;
    if (state.perMaterialCB.IsValid())
        m_device->DestroyBuffer(state.perMaterialCB);
    state = {};
}

} // namespace engine::renderer
