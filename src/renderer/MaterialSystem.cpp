#include "renderer/MaterialSystem.hpp"
#include "renderer/RenderPassRegistry.hpp"
#include "renderer/MaterialCBLayout.hpp"
#include "core/Debug.hpp"
#include <algorithm>
#include <cassert>
#include <cstring>
#include <type_traits>

namespace engine::renderer {

namespace {

uint32_t HashVertexLayout(const VertexLayout& layout) noexcept
{
    static constexpr uint32_t kPrime = 0x01000193u;
    static constexpr uint32_t kOffset = 0x811C9DC5u;
    uint32_t hash = kOffset;
    for (const auto& attr : layout.attributes)
    {
        hash ^= static_cast<uint32_t>(attr.semantic); hash *= kPrime;
        hash ^= static_cast<uint32_t>(attr.format); hash *= kPrime;
        hash ^= attr.binding; hash *= kPrime;
        hash ^= attr.offset; hash *= kPrime;
    }
    for (const auto& binding : layout.bindings)
    {
        hash ^= binding.binding; hash *= kPrime;
        hash ^= binding.stride; hash *= kPrime;
        hash ^= static_cast<uint32_t>(binding.inputRate); hash *= kPrime;
    }
    return hash;
}

uint32_t Align16(uint32_t v) noexcept
{
    return (v + 15u) & ~15u;
}

uint32_t ParameterByteSize(ParameterType type) noexcept
{
    switch (type)
    {
    case ParameterType::Float: return 4u;
    case ParameterType::Vec2: return 8u;
    case ParameterType::Vec3: return 12u;
    case ParameterType::Vec4: return 16u;
    case ParameterType::Int: return 4u;
    case ParameterType::Bool: return 4u;
    default: return 0u;
    }
}

bool IsResourceType(ParameterType type) noexcept
{
    switch (type)
    {
    case ParameterType::Texture2D:
    case ParameterType::TextureCube:
    case ParameterType::Sampler:
    case ParameterType::StructuredBuffer:
        return true;
    default:
        return false;
    }
}

uint32_t NormalizeBindingSlot(MaterialBinding::Kind kind, uint32_t slot) noexcept
{
    switch (kind)
    {
    case MaterialBinding::Kind::Texture:
    case MaterialBinding::Kind::Buffer:
        if (slot >= BindingRegisterRanges::ShaderResourceBase)
            return slot - BindingRegisterRanges::ShaderResourceBase;
        return slot;
    case MaterialBinding::Kind::Sampler:
        if (slot >= BindingRegisterRanges::SamplerBase)
            return slot - BindingRegisterRanges::SamplerBase;
        return slot;
    case MaterialBinding::Kind::ConstantBuffer:
        if (slot >= BindingRegisterRanges::ConstantBufferBase)
            return slot - BindingRegisterRanges::ConstantBufferBase;
        return slot;
    }
    return slot;
}

uint32_t InferTextureSlot(std::string_view name, uint32_t fallback) noexcept
{
    if (name == "albedo" || name == "albedoMap" || name == "baseColorMap" || name == "baseColor")
        return TexSlots::Albedo;
    if (name == "normal" || name == "normalMap")
        return TexSlots::Normal;
    if (name == "orm" || name == "ormMap" || name == "metallicRoughnessMap")
        return TexSlots::ORM;
    if (name == "emissive" || name == "emissiveMap")
        return TexSlots::Emissive;
    if (name == "shadowMap")
        return TexSlots::ShadowMap;
    if (name == "tIBLIrradiance")
        return TexSlots::IBLIrradiance;
    if (name == "tIBLPrefiltered")
        return TexSlots::IBLPrefiltered;
    if (name == "tBRDFLut" || name == "brdfLut")
        return TexSlots::BRDFLUT;
    return fallback;
}

uint32_t InferSamplerSlot(std::string_view name, uint32_t fallback) noexcept
{
    if (name == "sLinear" || name == "sLinearWrap" || name == "linearWrapSampler")
        return SamplerSlots::LinearWrap;
    if (name == "sClamp" || name == "sLinearClamp" || name == "linearClampSampler" || name == "linearclamp")
        return SamplerSlots::LinearClamp;
    if (name == "pointClampSampler")
        return SamplerSlots::PointClamp;
    if (name == "shadowSampler" || name == "sShadow")
        return SamplerSlots::ShadowPCF;
    return fallback;
}

ParameterType ToLayoutType(MaterialParam::Type type) noexcept
{
    switch (type)
    {
    case MaterialParam::Type::Float: return ParameterType::Float;
    case MaterialParam::Type::Vec2: return ParameterType::Vec2;
    case MaterialParam::Type::Vec3: return ParameterType::Vec3;
    case MaterialParam::Type::Vec4: return ParameterType::Vec4;
    case MaterialParam::Type::Int: return ParameterType::Int;
    case MaterialParam::Type::Bool: return ParameterType::Bool;
    case MaterialParam::Type::Texture: return ParameterType::Texture2D;
    case MaterialParam::Type::Sampler: return ParameterType::Sampler;
    case MaterialParam::Type::Buffer: return ParameterType::StructuredBuffer;
    }
    return ParameterType::Unknown;
}

} // namespace

ShaderVariantKey ShaderVariantKey::Normalized() const noexcept
{
    ShaderVariantKey k = *this;
    if (k.pass == ShaderPassType::Shadow || k.pass == ShaderPassType::Depth)
    {
        const auto shadowMask = ShaderVariantFlag::Skinned |
                                ShaderVariantFlag::AlphaTest |
                                ShaderVariantFlag::ShadowPass;
        k.flags = k.flags & shadowMask;
    }
    return k;
}

uint64_t ShaderVariantKey::Hash() const noexcept
{
    uint64_t h = 14695981039346656037ull;
    const auto mix = [&h](uint64_t v)
    {
        h ^= v;
        h *= 1099511628211ull;
    };

    mix(static_cast<uint64_t>(baseShader.value));
    mix(static_cast<uint64_t>(static_cast<uint8_t>(pass)));
    mix(static_cast<uint64_t>(static_cast<uint32_t>(flags)));
    return h;
}

bool PipelineKey::operator==(const PipelineKey& o) const noexcept
{
    return std::memcmp(this, &o, sizeof(PipelineKey)) == 0;
}

uint64_t PipelineKey::Hash() const noexcept
{
    static constexpr uint64_t kPrime = 1099511628211ull;
    static constexpr uint64_t kOffset = 14695981039346656037ull;
    const auto* bytes = reinterpret_cast<const uint8_t*>(this);
    uint64_t hash = kOffset;
    for (size_t i = 0; i < sizeof(PipelineKey); ++i)
    {
        hash ^= static_cast<uint64_t>(bytes[i]);
        hash *= kPrime;
    }
    return hash;
}

PipelineKey PipelineKey::From(const PipelineDesc& desc, RenderPassID pass) noexcept
{
    PipelineKey key{};
    static_assert(std::is_trivially_copyable_v<PipelineKey>, "PipelineKey must be trivially copyable");

    for (const auto& stage : desc.shaderStages)
    {
        if (stage.stage == ShaderStageMask::Vertex)
            key.vertexShader = stage.handle.value;
        else if (stage.stage == ShaderStageMask::Fragment)
            key.fragmentShader = stage.handle.value;
        else if (stage.stage == ShaderStageMask::Compute)
            key.computeShader = stage.handle.value;
    }

    key.fillMode = static_cast<uint8_t>(desc.rasterizer.fillMode);
    key.cullMode = static_cast<uint8_t>(desc.rasterizer.cullMode);
    key.frontFace = static_cast<uint8_t>(desc.rasterizer.frontFace);
    key.depthEnable = desc.depthStencil.depthEnable ? 1u : 0u;
    key.depthWrite = desc.depthStencil.depthWrite ? 1u : 0u;
    key.depthFunc = static_cast<uint8_t>(desc.depthStencil.depthFunc);
    key.stencilEnable = desc.depthStencil.stencilEnable ? 1u : 0u;

    const auto& blend = desc.blendStates[0];
    key.blendEnable = blend.blendEnable ? 1u : 0u;
    key.srcBlend = static_cast<uint8_t>(blend.srcBlend);
    key.dstBlend = static_cast<uint8_t>(blend.dstBlend);
    key.blendOp = static_cast<uint8_t>(blend.blendOp);
    key.srcBlendAlpha = static_cast<uint8_t>(blend.srcBlendAlpha);
    key.dstBlendAlpha = static_cast<uint8_t>(blend.dstBlendAlpha);
    key.blendOpAlpha = static_cast<uint8_t>(blend.blendOpAlpha);
    key.writeMask = blend.writeMask;

    key.colorFormat = static_cast<uint8_t>(desc.colorFormat);
    key.depthFormat = static_cast<uint8_t>(desc.depthFormat);
    key.sampleCount = static_cast<uint8_t>(desc.sampleCount);
    key.topology = static_cast<uint8_t>(desc.topology);
    key.vertexLayoutHash = HashVertexLayout(desc.vertexLayout);
    key.shaderContractHash = static_cast<uint32_t>(desc.shaderContractHash);
    key.pipelineLayoutHash = static_cast<uint32_t>(desc.pipelineLayoutHash);
    key.renderPassId = pass.value;
    return key;
}

SortKey SortKey::ForFrontToBack(RenderPassID pass, uint8_t layer, uint32_t pipelineHash, float linearDepth) noexcept
{
    const uint32_t depthKey = static_cast<uint32_t>(std::clamp(linearDepth, 0.0f, 1.0f) * 65535.0f);
    SortKey key{};
    key.value = (static_cast<uint64_t>(pass.value) << 48u) |
                (static_cast<uint64_t>(layer) << 40u) |
                (static_cast<uint64_t>(pipelineHash & 0x00FFFFFFu) << 16u) |
                static_cast<uint64_t>(depthKey);
    return key;
}

SortKey SortKey::ForBackToFront(RenderPassID pass, uint8_t layer, float linearDepth) noexcept
{
    const uint32_t depthKey = 65535u - static_cast<uint32_t>(std::clamp(linearDepth, 0.0f, 1.0f) * 65535.0f);
    SortKey key{};
    key.value = (static_cast<uint64_t>(pass.value) << 48u) |
                (static_cast<uint64_t>(layer) << 40u) |
                static_cast<uint64_t>(depthKey);
    return key;
}

SortKey SortKey::ForSubmissionOrder(RenderPassID pass, uint8_t layer, uint32_t drawOrder) noexcept
{
    SortKey key{};
    key.value = (static_cast<uint64_t>(pass.value) << 48u) |
                (static_cast<uint64_t>(layer) << 40u) |
                static_cast<uint64_t>(drawOrder);
    return key;
}

float* MaterialInstance::GetFloatPtr(const std::string& name) noexcept
{
    for (auto& param : instanceParams)
    {
        if (param.name == name && param.type == MaterialParam::Type::Float)
            return &param.value.f[0];
    }
    return nullptr;
}

uint32_t MaterialSystem::AllocSlot()
{
    if (!m_freeSlots.empty())
    {
        const uint32_t index = m_freeSlots.back();
        m_freeSlots.pop_back();
        return index;
    }

    const uint32_t index = static_cast<uint32_t>(m_descs.size());
    m_descs.emplace_back();
    m_instances.emplace_back();
    m_generations.push_back(1u);
    return index;
}

bool MaterialSystem::ValidHandle(MaterialHandle h) const noexcept
{
    if (!h.IsValid())
        return false;
    const uint32_t index = h.Index();
    return index < m_generations.size() && m_generations[index] == h.Generation();
}

void MaterialSystem::NormalizeDesc(MaterialDesc& desc) const noexcept
{
    MaterialFeatureEval::NormalizeDesc(desc);
}

ShaderParameterLayout MaterialSystem::BuildLayoutFromDesc(const MaterialDesc& desc) const noexcept
{
    if (desc.parameterLayout.IsValid())
        return desc.parameterLayout;

    ShaderParameterLayout layout{};
    uint32_t cbOffset = 0u;
    uint32_t nextTextureSlot = 0u;
    uint32_t nextSamplerSlot = 0u;
    uint32_t nextBufferSlot = 0u;

    for (const auto& param : desc.params)
    {
        ParameterSlot slot{};
        slot.SetName(param.name);
        slot.type = ToLayoutType(param.type);
        slot.set = 0u;
        slot.stageFlags = (param.type == MaterialParam::Type::Texture || param.type == MaterialParam::Type::Sampler)
                        ? ShaderStageMask::Fragment
                        : ShaderStageMask::Vertex | ShaderStageMask::Fragment;
        slot.elementCount = 1u;

        if (IsResourceType(slot.type))
        {
            auto it = std::find_if(desc.bindings.begin(), desc.bindings.end(), [&](const MaterialBinding& binding) {
                return binding.name == param.name;
            });

            if (it != desc.bindings.end())
            {
                slot.binding = NormalizeBindingSlot(it->kind, it->slot);
                slot.stageFlags = it->stages == ShaderStageMask::None ? slot.stageFlags : it->stages;
            }
            else
            {
                switch (slot.type)
                {
                case ParameterType::Texture2D:
                case ParameterType::TextureCube:
                    slot.binding = InferTextureSlot(param.name, nextTextureSlot++);
                    break;
                case ParameterType::Sampler:
                    slot.binding = InferSamplerSlot(param.name, nextSamplerSlot++);
                    break;
                case ParameterType::StructuredBuffer:
                    slot.binding = nextBufferSlot++;
                    break;
                default:
                    break;
                }
            }
        }
        else
        {
            const uint32_t byteSize = ParameterByteSize(slot.type);
            const uint32_t rowOffset = cbOffset & 15u;
            if (byteSize == 16u || (rowOffset + byteSize) > 16u)
                cbOffset = Align16(cbOffset);

            slot.binding = CBSlots::PerMaterial;
            slot.byteOffset = cbOffset;
            slot.byteSize = byteSize;
            cbOffset += byteSize;
        }

        if (!layout.AddSlot(slot))
            break;
    }

    return layout;
}

void MaterialSystem::InitializeInstanceFromDesc(MaterialInstance& inst, const MaterialDesc& desc) const noexcept
{
    inst.renderPass = desc.renderPass;
    inst.shader = desc.fragmentShader.IsValid() ? desc.fragmentShader : desc.vertexShader;
    inst.shaderSourceCode = "reflected-layout-pending";
    inst.layout = BuildLayoutFromDesc(desc);
    inst.parameters.Reset(inst.layout);
    inst.instanceParams = desc.params;
    inst.layoutDirty = true;
    inst.cbDirty = true;
}

void MaterialSystem::SyncBlobFromParams(MaterialInstance& inst) const
{
    for (uint32_t slotIndex = 0u; slotIndex < inst.layout.slotCount; ++slotIndex)
    {
        const ParameterSlot& slot = inst.layout.slots[slotIndex];
        auto it = std::find_if(inst.instanceParams.begin(), inst.instanceParams.end(), [&](const MaterialParam& param) {
            return param.name == slot.Name();
        });
        if (it == inst.instanceParams.end())
            continue;

        switch (slot.type)
        {
        case ParameterType::Float:
        case ParameterType::Vec2:
        case ParameterType::Vec3:
        case ParameterType::Vec4:
            std::memcpy(inst.cbData.data() + slot.byteOffset, it->value.f, std::min<uint32_t>(slot.byteSize, 16u));
            break;
        case ParameterType::Int:
            std::memcpy(inst.cbData.data() + slot.byteOffset, &it->value.i, 4u);
            break;
        case ParameterType::Bool:
        {
            const uint32_t value = it->value.b ? 1u : 0u;
            std::memcpy(inst.cbData.data() + slot.byteOffset, &value, 4u);
            break;
        }
        case ParameterType::Texture2D:
        case ParameterType::TextureCube:
(void)inst.parameters.SetTexture(slotIndex, it->texture);
            break;
        case ParameterType::Sampler:
(void)inst.parameters.SetSampler(slotIndex, it->samplerIdx);
            break;
        case ParameterType::StructuredBuffer:
(void)inst.parameters.SetBuffer(slotIndex, it->buffer);
            break;
        default:
            break;
        }
    }
}

MaterialHandle MaterialSystem::RegisterMaterial(MaterialDesc desc)
{
    NormalizeDesc(desc);

    const uint32_t index = AllocSlot();
    const MaterialHandle handle = MaterialHandle::Make(index, m_generations[index]);
    const std::string name = desc.name.empty() ? ("Material_" + std::to_string(index)) : desc.name;

    desc.name = name;
    if (!desc.parameterLayout.IsValid())
        desc.parameterLayout = BuildLayoutFromDesc(desc);

    m_descs[index].desc = desc;
    m_descs[index].name = name;
    m_descs[index].isInstance = false;
    m_descs[index].baseHandle = MaterialHandle::Invalid();
    m_nameLookup[name] = handle;

    MaterialInstance& inst = m_instances[index];
    inst.desc = handle;
    InitializeInstanceFromDesc(inst, desc);

    const std::string_view passName = RenderPassName(desc.renderPass);
    Debug::Log("MaterialSystem.cpp: RegisterMaterial '%s' idx=%u renderPass=%s", name.c_str(), index, passName.data());
    return handle;
}

MaterialHandle MaterialSystem::CreateInstance(MaterialHandle base, std::string instanceName)
{
    if (!ValidHandle(base))
    {
        Debug::LogError("MaterialSystem.cpp: CreateInstance - invalid base handle");
        return MaterialHandle::Invalid();
    }

    const MaterialDesc baseDesc = m_descs[base.Index()].desc;
    MaterialDesc desc = baseDesc;
    const uint32_t index = AllocSlot();
    const MaterialHandle handle = MaterialHandle::Make(index, m_generations[index]);

    desc.name = instanceName.empty() ? (baseDesc.name + "_Instance_" + std::to_string(index)) : std::move(instanceName);
    m_descs[index].desc = desc;
    m_descs[index].name = desc.name;
    m_descs[index].isInstance = true;
    m_descs[index].baseHandle = base;
    m_nameLookup[desc.name] = handle;

    MaterialInstance& inst = m_instances[index];
    inst.desc = handle;
    InitializeInstanceFromDesc(inst, desc);
    return handle;
}

const MaterialDesc* MaterialSystem::GetDesc(MaterialHandle h) const noexcept
{
    return ValidHandle(h) ? &m_descs[h.Index()].desc : nullptr;
}

MaterialHandle MaterialSystem::FindMaterial(const std::string& name) const noexcept
{
    auto it = m_nameLookup.find(name);
    return it != m_nameLookup.end() ? it->second : MaterialHandle::Invalid();
}

MaterialInstance* MaterialSystem::GetInstance(MaterialHandle h) noexcept
{
    return ValidHandle(h) ? &m_instances[h.Index()] : nullptr;
}

const MaterialInstance* MaterialSystem::GetInstance(MaterialHandle h) const noexcept
{
    return ValidHandle(h) ? &m_instances[h.Index()] : nullptr;
}

PipelineKey MaterialSystem::BuildPipelineKey(MaterialHandle h) const noexcept
{
    if (!ValidHandle(h))
        return PipelineKey{};
    return MaterialFeatureEval::BuildPipelineKey(m_descs[h.Index()].desc, m_instances[h.Index()]);
}

ShaderVariantFlag MaterialSystem::BuildShaderVariantFlags(MaterialHandle h) const noexcept
{
    if (!ValidHandle(h))
        return ShaderVariantFlag::None;
    return MaterialFeatureEval::BuildShaderVariantFlags(m_descs[h.Index()].desc, m_instances[h.Index()]);
}

void MaterialSystem::SetFloat(MaterialHandle h, const std::string& name, float v)
{
    MaterialInstance* inst = GetInstance(h);
    if (!inst)
        return;
    for (auto& param : inst->instanceParams)
    {
        if (param.name == name && param.type == MaterialParam::Type::Float)
        {
            param.value.f[0] = v;
            MarkDirty(h);
            return;
        }
    }
}

void MaterialSystem::SetVec4(MaterialHandle h, const std::string& name, const math::Vec4& v)
{
    MaterialInstance* inst = GetInstance(h);
    if (!inst)
        return;
    for (auto& param : inst->instanceParams)
    {
        if (param.name == name && param.type == MaterialParam::Type::Vec4)
        {
            param.value.f[0] = v.x;
            param.value.f[1] = v.y;
            param.value.f[2] = v.z;
            param.value.f[3] = v.w;
            MarkDirty(h);
            return;
        }
    }
}

void MaterialSystem::SetTexture(MaterialHandle h, const std::string& name, TextureHandle tex)
{
    MaterialInstance* inst = GetInstance(h);
    if (!inst)
        return;
    for (auto& param : inst->instanceParams)
    {
        if (param.name == name && param.type == MaterialParam::Type::Texture)
        {
            param.texture = tex;
            MarkDirty(h);
            return;
        }
    }
}

void MaterialSystem::MarkDirty(MaterialHandle h)
{
    MaterialInstance* inst = GetInstance(h);
    if (!inst)
        return;
    inst->cbDirty = true;
    ++inst->revision;
}

uint64_t MaterialSystem::GetRevision(MaterialHandle h) const noexcept
{
    const MaterialInstance* inst = GetInstance(h);
    return inst ? inst->revision : 0ull;
}

const std::vector<uint8_t>& MaterialSystem::GetCBData(MaterialHandle h)
{
    static const std::vector<uint8_t> kEmpty{};
    MaterialInstance* inst = GetInstance(h);
    if (!inst)
        return kEmpty;

    if (inst->cbDirty || inst->layoutDirty)
    {
        MaterialCBLayout::BuildCBData(*inst);
        if (inst->cbData.empty() && inst->cbLayout.totalSize > 0u)
            inst->cbData.assign(inst->cbLayout.totalSize, 0u);
        SyncBlobFromParams(*inst);
        inst->cbDirty = false;
        inst->layoutDirty = false;
    }

    return inst->cbData;
}

const CbLayout& MaterialSystem::GetCBLayout(MaterialHandle h)
{
    static const CbLayout kEmpty{};
    MaterialInstance* inst = GetInstance(h);
    if (!inst)
        return kEmpty;
    GetCBData(h);
    return inst->cbLayout;
}

} // namespace engine::renderer
