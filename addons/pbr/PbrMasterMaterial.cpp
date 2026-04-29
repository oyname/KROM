#include "PbrMasterMaterial.hpp"
#include "PbrInstanceBuilder.hpp"
#include "PbrSlotTable.hpp"
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
    desc.renderPass     = m_config.renderPass;
    desc.vertexShader   = m_config.vs;
    desc.fragmentShader = m_config.fs;
    desc.shadowShader   = m_config.shadow;
    desc.vertexLayout   = m_config.vertexLayout;
    desc.colorFormat    = m_config.colorFormat;
    desc.depthFormat    = m_config.depthFormat;
    desc.permutationFlags = flagBits;

    desc.renderPolicy.cullMode       = m_config.cullMode;
    desc.renderPolicy.castShadows    = m_config.castShadows;
    desc.renderPolicy.receiveShadows = m_config.receiveShadows;
    desc.castShadows = m_config.castShadows;
    desc.doubleSided = (flagBits & static_cast<uint64_t>(ShaderVariantFlag::DoubleSided)) != 0;

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
    desc.params = {
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
        MakeTextureParam("albedo"),
        MakeTextureParam("normal"),
        MakeTextureParam("orm"),
        MakeTextureParam("emissive"),
        MakeSamplerParam("sLinearWrap",  SamplerSlots::LinearWrap),
    };

    desc.bindings = {
        { TexSlots::Albedo,   0u, ShaderStageMask::Fragment, MaterialBinding::Kind::Texture, "albedo"  },
        { TexSlots::Normal,   0u, ShaderStageMask::Fragment, MaterialBinding::Kind::Texture, "normal"  },
        { TexSlots::ORM,      0u, ShaderStageMask::Fragment, MaterialBinding::Kind::Texture, "orm"     },
        { TexSlots::Emissive, 0u, ShaderStageMask::Fragment, MaterialBinding::Kind::Texture, "emissive"},
        { SamplerSlots::LinearWrap,  0u, ShaderStageMask::Fragment, MaterialBinding::Kind::Sampler, "sLinearWrap"  },
        { SamplerSlots::LinearClamp, 0u, ShaderStageMask::Fragment, MaterialBinding::Kind::Sampler, "sLinearClamp" },
    };

    desc.name = "PBR_Perm_" + std::to_string(flagBits);
    MaterialHandle h = m_materials->RegisterMaterial(std::move(desc));
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
