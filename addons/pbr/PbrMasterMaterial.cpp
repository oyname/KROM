#include "PbrMasterMaterial.hpp"
#include "PbrInstanceBuilder.hpp"
#include "PbrSlotTable.hpp"
#include "renderer/MaterialDomain.hpp"
#include "renderer/MaterialSystem.hpp"
#include "renderer/runtime/MaterialRuntimeBridge.hpp"
#include "renderer/ShaderBindingModel.hpp"
#include "core/Debug.hpp"

namespace engine::renderer::pbr {

namespace {

using engine::renderer::pbr::detail::kSlots;
using engine::renderer::pbr::detail::SlotDef;


MaterialParam MakeFloatParam(const char* name, float value)
{
    MaterialParam p{};
    p.name = name;
    p.type = MaterialParam::Type::Float;
    p.value.f[0] = value;
    return p;
}

MaterialParam MakeVec4Param(const char* name, math::Vec4 v)
{
    MaterialParam p{};
    p.name = name;
    p.type = MaterialParam::Type::Vec4;
    p.value.f[0] = v.x; p.value.f[1] = v.y;
    p.value.f[2] = v.z; p.value.f[3] = v.w;
    return p;
}

MaterialParam MakeVec2Param(const char* name, math::Vec2 v)
{
    MaterialParam p{};
    p.name = name;
    p.type = MaterialParam::Type::Vec2;
    p.value.f[0] = v.x;
    p.value.f[1] = v.y;
    return p;
}

MaterialParam MakeIntParam(const char* name, int32_t v)
{
    MaterialParam p{};
    p.name = name;
    p.type = MaterialParam::Type::Int;
    p.value.i = v;
    return p;
}

MaterialParam MakeTextureParam(const char* name)
{
    MaterialParam p{};
    p.name = name;
    p.type = MaterialParam::Type::Texture;
    return p;
}

MaterialParam MakeSamplerParam(const char* name, uint32_t idx)
{
    MaterialParam p{};
    p.name = name;
    p.type = MaterialParam::Type::Sampler;
    p.samplerIdx = idx;
    return p;
}

} // namespace

// =============================================================================

MaterialHandle PbrInstanceBuilder::Build()
{
    const uint64_t flags = ComputeFlags();

    MaterialHandle base = m_master.GetOrRegisterPermutation(flags);
    if (!base.IsValid())
        return MaterialHandle{};

    MaterialSystem* ms = m_master.GetMaterialSystem();
    MaterialHandle inst = ms->CreateInstance(base, m_name);
    if (!inst.IsValid())
        return MaterialHandle{};

    for (auto& [id, value] : m_slots)
        m_master.SetSlotValue(inst, id, value);

    if (m_alphaTest)
        ms->SetFloat(inst, "alphaCutoff", m_alphaCutoff);

    return inst;
}

uint64_t PbrInstanceBuilder::ComputeFlags() const noexcept
{
    ShaderVariantFlag flags = ShaderVariantFlag::PBRMetalRough;

    for (const detail::SlotDef& s : detail::kSlots)
    {
        if (s.variantFlag == ShaderVariantFlag::None) continue;
        auto it = m_slots.find(s.id);
        if (it != m_slots.end() && it->second.HasTexture())
            flags = flags | s.variantFlag;
    }

    if (m_ibl)         flags = flags | ShaderVariantFlag::IBLMap;
    if (m_doubleSided) flags = flags | ShaderVariantFlag::DoubleSided;
    if (m_alphaTest)   flags = flags | ShaderVariantFlag::AlphaTest;

    return static_cast<uint64_t>(flags);
}

// =============================================================================

PbrMasterMaterial PbrMasterMaterial::Create(MaterialSystem& materials, Config config)
{
    PbrMasterMaterial m;
    m.m_materials = &materials;
    m.m_config    = std::move(config);
    m.m_slotDescs = BuildSlotDescs();
    return m;
}

PbrInstanceBuilder PbrMasterMaterial::CreateInstance(std::string name) noexcept
{
    return PbrInstanceBuilder(*this, std::move(name));
}

MaterialHandle PbrMasterMaterial::GetOrRegisterPermutation(uint64_t flagBits)
{
    auto it = m_permCache.find(flagBits);
    if (it != m_permCache.end())
        return it->second;

    MaterialDesc desc{};
    const bool isAlphaTest = (flagBits & static_cast<uint64_t>(ShaderVariantFlag::AlphaTest)) != 0;
    desc.domain         = MaterialDomain::Mesh;
    desc.surface        = isAlphaTest ? MaterialSurfaceType::Cutout : MaterialSurfaceType::Opaque;
    desc.features       = static_cast<MaterialFeatureFlags>(flagBits);
    desc.renderPolicy.cull.mode      = m_config.cullMode;
    desc.renderPolicy.castShadows    = m_config.castShadows;
    desc.renderPolicy.receiveShadows = m_config.receiveShadows;
    desc.renderPolicy.alphaTest      = isAlphaTest;
    desc.renderPolicy.cull.doubleSided = (flagBits & static_cast<uint64_t>(MaterialFeatureFlags::DoubleSided)) != 0;

    // CB params with safe defaults — instances override individual values.
    // Layout must exactly match the PerMaterial cbuffer in all shader files:
    //   float4 baseColorFactor  @ 0
    //   float4 emissiveFactor   @ 16
    //   float  metallicFactor   @ 32
    //   float  roughnessFactor  @ 36
    //   float  normalStrength   @ 40
    //   float  occlusionStrength@ 44
    //   float  opacityFactor    @ 48
    //   float  alphaCutoff      @ 52
    //   int    materialFeatureMask @ 56
    //   float  materialModel    @ 60
    //   int    occlusionChannel @ 64  (-1 = constant mode)
    //   int    roughnessChannel @ 68  (-1 = constant mode)
    //   int    metallicChannel  @ 72  (-1 = constant mode)
    //   float  occlusionBias    @ 76
    //   float  roughnessBias    @ 80
    //   float  metallicBias     @ 84
    //   (float _pad1            @ 88  — shader-side only, zero-filled)
    //   (float _pad2            @ 92  — shader-side only, zero-filled)
    desc.parameters = {
        MakeVec4Param("baseColorFactor",    {1.f, 1.f, 1.f, 1.f}),
        MakeVec4Param("emissiveFactor",     {0.f, 0.f, 0.f, 0.f}),
        MakeFloatParam("metallicFactor",    0.0f),
        MakeFloatParam("roughnessFactor",   0.5f),
        MakeFloatParam("normalStrength",    1.0f),
        MakeFloatParam("occlusionStrength", 1.0f),
        MakeFloatParam("opacityFactor",     1.0f),
        MakeFloatParam("alphaCutoff",       0.5f),
        MakeIntParam("materialFeatureMask", 0),
        MakeFloatParam("materialModel",     0.0f),
        MakeIntParam("occlusionChannel",    -1),   // -1 = constant
        MakeIntParam("roughnessChannel",    -1),
        MakeIntParam("metallicChannel",     -1),
        MakeFloatParam("occlusionBias",     0.0f),
        MakeFloatParam("roughnessBias",     0.0f),
        MakeFloatParam("metallicBias",      0.0f),
        MakeVec2Param("uvScale",            {1.f, 1.f}),
        MakeVec2Param("uvOffset",           {0.f, 0.f}),
        MakeTextureParam("albedo"),
        MakeTextureParam("normal"),
        MakeTextureParam("orm"),
        MakeTextureParam("emissive"),
        MakeSamplerParam("sLinearWrap",  SamplerSlots::LinearWrap),
    };

    desc.textureSlots = {
        { "albedo",   MaterialTextureSemantic::BaseColor,                  {}, {}, SamplerSlots::LinearWrap },
        { "normal",   MaterialTextureSemantic::Normal,                     {}, {}, SamplerSlots::LinearWrap },
        { "orm",      MaterialTextureSemantic::MetallicRoughnessOcclusion, {}, {}, SamplerSlots::LinearWrap },
        { "emissive", MaterialTextureSemantic::Emissive,                   {}, {}, SamplerSlots::LinearWrap },
    };

    desc.name = "PBR_Perm_" + std::to_string(flagBits);
    desc.materialGraph = "template://krom/pbr-lit";
    MaterialRuntimeDesc runtime{};
    runtime.renderPass = m_config.renderPass;
    runtime.vertexShader = m_config.vs;
    runtime.fragmentShader = m_config.fs;
    runtime.shadowShader         = m_config.shadow;
    runtime.shadowFragmentShader = m_config.shadowFs;
    runtime.vertexLayout = m_config.vertexLayout;
    runtime.colorFormat = m_config.colorFormat;
    runtime.depthFormat = m_config.depthFormat;
    runtime.rasterizer.frontFace = m_config.frontFace;
    MaterialHandle h = MaterialRuntimeBridge::RegisterMaterial(*m_materials, std::move(desc), runtime);
    if (h.IsValid())
        m_permCache[flagBits] = h;
    else
        Debug::LogError("PbrMasterMaterial: failed to register permutation flags=0x%llx", flagBits);

    return h;
}

void PbrMasterMaterial::SetSlotValue(MaterialHandle instance,
                                      const std::string& slotId,
                                      const PbrSlotValue& value) noexcept
{
    if (!m_materials) return;

    for (const SlotDef& s : kSlots)
    {
        if (slotId != s.id) continue;

        if (value.HasTexture() && s.texParam)
        {
            m_materials->SetTexture(instance, s.texParam, value.texture);
            if (s.scaleParam)   m_materials->SetFloat(instance, s.scaleParam, value.scale);
            if (s.channelParam) m_materials->SetInt  (instance, s.channelParam,
                                                      static_cast<int32_t>(value.channel));
            if (s.biasParam)    m_materials->SetFloat(instance, s.biasParam, value.bias);
        }
        else
        {
            if (s.constantVec4Param)  m_materials->SetVec4 (instance, s.constantVec4Param,  value.constant);
            if (s.constantFloatParam) m_materials->SetFloat(instance, s.constantFloatParam, value.constant.x);
            if (s.channelParam)       m_materials->SetInt  (instance, s.channelParam, -1);
            if (s.biasParam)          m_materials->SetFloat(instance, s.biasParam, 0.0f);
        }
        return;
    }
}

std::vector<PbrSlotDesc> PbrMasterMaterial::BuildSlotDescs()
{
    std::vector<PbrSlotDesc> descs;
    descs.reserve(std::size(kSlots));
    for (const SlotDef& s : kSlots)
        descs.push_back({ s.id, s.displayName, s.dataType,
                          s.acceptsTexture, s.defaultValue, s.minValue, s.maxValue });
    return descs;
}

} // namespace engine::renderer::pbr
