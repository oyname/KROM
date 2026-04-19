#include "UnlitMaterial.hpp"
#include <stdexcept>

namespace engine::renderer::unlit {
namespace {

MaterialParam MakeFloatParam(const char* name, float value)
{
    MaterialParam p{};
    p.name = name;
    p.type = MaterialParam::Type::Float;
    p.value.f[0] = value;
    return p;
}

MaterialParam MakeVec4Param(const char* name, const math::Vec4& value)
{
    MaterialParam p{};
    p.name = name;
    p.type = MaterialParam::Type::Vec4;
    p.value.f[0] = value.x;
    p.value.f[1] = value.y;
    p.value.f[2] = value.z;
    p.value.f[3] = value.w;
    return p;
}

MaterialParam MakeIntParam(const char* name, int32_t value)
{
    MaterialParam p{};
    p.name = name;
    p.type = MaterialParam::Type::Int;
    p.value.i = value;
    return p;
}

MaterialParam MakeTextureParam(const char* name)
{
    MaterialParam p{};
    p.name = name;
    p.type = MaterialParam::Type::Texture;
    return p;
}

MaterialParam MakeSamplerParam(const char* name, uint32_t samplerIdx)
{
    MaterialParam p{};
    p.name = name;
    p.type = MaterialParam::Type::Sampler;
    p.samplerIdx = samplerIdx;
    return p;
}

} // namespace

UnlitMaterial::UnlitMaterial(MaterialSystem& materials, MaterialHandle handle) noexcept
    : m_materials(&materials),
      m_handle(handle),
      m_instance(materials.GetInstance(handle))
{
    CacheSlots();
}

UnlitShaderAssetSet UnlitMaterial::DefaultShaderAssetSet() noexcept
{
    return {"quad_unlit.vs.hlsl", "quad_unlit.ps.hlsl", StandardRenderPasses::Opaque()};
}

MaterialDesc UnlitMaterial::BuildDesc(const UnlitMaterialCreateInfo& info)
{
    MaterialDesc desc{};
    desc.name           = info.name;
    desc.renderPass     = info.renderPass;
    desc.vertexShader   = info.vertexShader;
    desc.fragmentShader = info.fragmentShader;
    desc.vertexLayout   = info.vertexLayout;
    desc.colorFormat    = info.colorFormat;
    desc.depthFormat    = info.depthFormat;
    desc.rasterizer.frontFace = info.frontFace;

    desc.renderPolicy.cullMode       = info.cullMode;
    desc.renderPolicy.castShadows    = info.castShadows;
    desc.renderPolicy.receiveShadows = false;
    desc.renderPolicy.alphaTest      = info.alphaTest;
    desc.renderPolicy.alphaCutoff    = info.alphaCutoff;
    desc.renderPolicy.doubleSided    = info.doubleSided;
    desc.doubleSided  = info.doubleSided;
    desc.castShadows  = info.castShadows;
    desc.alphaCutoff  = info.alphaCutoff;

    ShaderVariantFlag flags = ShaderVariantFlag::Unlit;
    if (info.enableBaseColorMap) flags = flags | ShaderVariantFlag::BaseColorMap;
    if (info.enableEmissiveMap)  flags = flags | ShaderVariantFlag::EmissiveMap;
    if (info.alphaTest)          flags = flags | ShaderVariantFlag::AlphaTest;
    if (info.doubleSided)        flags = flags | ShaderVariantFlag::DoubleSided;
    desc.permutationFlags = static_cast<uint64_t>(flags);

    int32_t materialFeatureMask = 0;
    if (info.enableBaseColorMap) materialFeatureMask |= 2;
    if (info.enableEmissiveMap)  materialFeatureMask |= (1 << 11);

    desc.params = {
        MakeVec4Param("baseColorFactor",    info.baseColorFactor),
        MakeVec4Param("emissiveFactor",     info.emissiveFactor),
        MakeFloatParam("metallicFactor",    0.0f),
        MakeFloatParam("roughnessFactor",   1.0f),
        MakeFloatParam("occlusionStrength", 1.0f),
        MakeFloatParam("opacityFactor",     info.opacityFactor),
        MakeFloatParam("alphaCutoff",       info.alphaCutoff),
        MakeIntParam("materialFeatureMask", materialFeatureMask),
        MakeFloatParam("materialModel",     0.0f),
        MakeTextureParam("albedo"),
        MakeTextureParam("emissive"),
        MakeSamplerParam("sLinearWrap", SamplerSlots::LinearWrap),
    };

    desc.bindings = {
        { TexSlots::Albedo,   0u, ShaderStageMask::Fragment, MaterialBinding::Kind::Texture, "albedo" },
        { TexSlots::Emissive, 0u, ShaderStageMask::Fragment, MaterialBinding::Kind::Texture, "emissive" },
        { SamplerSlots::LinearWrap, 0u, ShaderStageMask::Fragment, MaterialBinding::Kind::Sampler, "sLinearWrap" },
    };

    return desc;
}

MaterialHandle UnlitMaterial::Register(MaterialSystem& materials,
                                        const UnlitMaterialCreateInfo& info)
{
    return materials.RegisterMaterial(BuildDesc(info));
}

UnlitMaterial UnlitMaterial::Create(MaterialSystem& materials,
                                     const UnlitMaterialCreateInfo& info) noexcept
{
    return UnlitMaterial(materials, Register(materials, info));
}

bool UnlitMaterial::IsValid() const noexcept
{
    return m_materials != nullptr && m_instance != nullptr && m_handle.IsValid();
}

bool UnlitMaterial::SetBaseColorFactor(const math::Vec4& value) noexcept
{
    if (!IsValid()) return false;
    m_materials->SetVec4(m_handle, "baseColorFactor", value);
    return true;
}

bool UnlitMaterial::SetEmissiveFactor(const math::Vec4& value) noexcept
{
    if (!IsValid()) return false;
    m_materials->SetVec4(m_handle, "emissiveFactor", value);
    return true;
}

bool UnlitMaterial::SetOpacityFactor(float value) noexcept
{
    if (!IsValid()) return false;
    m_materials->SetFloat(m_handle, "opacityFactor", value);
    return true;
}

bool UnlitMaterial::SetAlphaCutoff(float value) noexcept
{
    if (!IsValid()) return false;
    m_materials->SetFloat(m_handle, "alphaCutoff", value);
    return true;
}

bool UnlitMaterial::SetAlbedo(TextureHandle texture) noexcept
{
    return SetTextureAtSlot(m_albedoSlot, "albedo", texture);
}

bool UnlitMaterial::SetEmissive(TextureHandle texture) noexcept
{
    return SetTextureAtSlot(m_emissiveSlot, "emissive", texture);
}

MaterialInstance& UnlitMaterial::Raw()
{
    if (!m_instance)
        throw std::runtime_error("UnlitMaterial::Raw called on invalid material");
    return *m_instance;
}

const MaterialInstance& UnlitMaterial::Raw() const
{
    if (!m_instance)
        throw std::runtime_error("UnlitMaterial::Raw called on invalid material");
    return *m_instance;
}

bool UnlitMaterial::SetTextureAtSlot(int32_t slotIndex, const char* slotName,
                                      TextureHandle texture) noexcept
{
    if (!IsValid() || slotIndex < 0)
        return false;
    m_materials->SetTexture(m_handle, slotName, texture);
    (void)m_instance->parameters.SetTexture(static_cast<uint32_t>(slotIndex), texture);
    return true;
}

void UnlitMaterial::CacheSlots() noexcept
{
    m_albedoSlot   = static_cast<int32_t>(TexSlots::Albedo);
    m_emissiveSlot = static_cast<int32_t>(TexSlots::Emissive);
}

} // namespace engine::renderer::unlit
