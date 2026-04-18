#include "PbrMaterial.hpp"
#include <stdexcept>

namespace engine::renderer::pbr {

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

PbrMaterial::PbrMaterial(MaterialSystem& materials, MaterialHandle handle) noexcept
    : m_materials(&materials),
      m_handle(handle),
      m_instance(materials.GetInstance(handle))
{
    CacheSlots();
}

PbrShaderAssetSet PbrMaterial::DefaultShaderAssetSet(PbrShaderBackend backend) noexcept
{
    switch (backend) {
    case PbrShaderBackend::DX11:
        return {"pbr_lit.dx11.vs.hlsl", "pbr_lit.dx11.ps.hlsl", "pbr_lit.dx11.vs.hlsl", StandardRenderPasses::Opaque()};
    case PbrShaderBackend::OpenGL:
        return {"pbr_lit.opengl.vs.glsl", "pbr_lit.opengl.fs.glsl", "pbr_lit.opengl.vs.glsl", StandardRenderPasses::Opaque()};
    case PbrShaderBackend::Vulkan:
        return {"pbr_lit.vulkan.vs.glsl", "pbr_lit.vulkan.fs.glsl", "pbr_lit.vulkan.vs.glsl", StandardRenderPasses::Opaque()};
    default:
        return {};
    }
}

void PbrMaterial::ApplyDefaultShaderAssetSet(PbrMaterialCreateInfo& info, PbrShaderBackend backend) noexcept
{
    const PbrShaderAssetSet set = DefaultShaderAssetSet(backend);
    info.renderPass = set.renderPass;
}

MaterialDesc PbrMaterial::BuildDesc(const PbrMaterialCreateInfo& info)
{
    MaterialDesc desc{};
    desc.name = info.name;
    desc.renderPass = info.renderPass;
    desc.vertexShader = info.vertexShader;
    desc.fragmentShader = info.fragmentShader;
    desc.shadowShader = info.shadowShader;
    desc.vertexLayout = info.vertexLayout;
    desc.colorFormat = info.colorFormat;
    desc.depthFormat = info.depthFormat;

    desc.renderPolicy.cullMode = info.cullMode;
    desc.renderPolicy.castShadows = info.castShadows;
    desc.renderPolicy.receiveShadows = info.receiveShadows;
    desc.renderPolicy.doubleSided = info.doubleSided;
    desc.renderPolicy.alphaCutoff = info.alphaCutoff;
    desc.doubleSided = info.doubleSided;
    desc.castShadows = info.castShadows;
    desc.alphaCutoff = info.alphaCutoff;

    ShaderVariantFlag flags = ShaderVariantFlag::PBRMetalRough;
    if (info.enableBaseColorMap) flags = flags | ShaderVariantFlag::BaseColorMap;
    if (info.enableNormalMap) flags = flags | ShaderVariantFlag::NormalMap;
    if (info.enableORMMap) flags = flags | ShaderVariantFlag::ORMMap;
    if (info.enableEmissiveMap) flags = flags | ShaderVariantFlag::EmissiveMap;
    if (info.enableIBL) flags = flags | ShaderVariantFlag::IBLMap;
    if (info.doubleSided) flags = flags | ShaderVariantFlag::DoubleSided;
    desc.permutationFlags = static_cast<uint64_t>(flags);

    desc.params = {
        MakeVec4Param("baseColorFactor", info.baseColorFactor),
        MakeVec4Param("emissiveFactor", info.emissiveFactor),
        MakeFloatParam("metallicFactor", info.metallicFactor),
        MakeFloatParam("roughnessFactor", info.roughnessFactor),
        MakeFloatParam("occlusionStrength", info.occlusionStrength),
        MakeFloatParam("opacityFactor", info.opacityFactor),
        MakeFloatParam("alphaCutoff", info.alphaCutoff),
        MakeIntParam("materialFeatureMask", info.materialFeatureMask),
        MakeFloatParam("materialModel", info.materialModel),
        MakeTextureParam("albedo"),
        MakeTextureParam("normal"),
        MakeTextureParam("orm"),
        MakeTextureParam("emissive"),
        MakeSamplerParam("sLinearWrap", SamplerSlots::LinearWrap),
        MakeSamplerParam("sLinearClamp", SamplerSlots::LinearClamp),
    };

    desc.bindings = {
        { TexSlots::Albedo,   0u, ShaderStageMask::Fragment, MaterialBinding::Kind::Texture, "albedo" },
        { TexSlots::Normal,   0u, ShaderStageMask::Fragment, MaterialBinding::Kind::Texture, "normal" },
        { TexSlots::ORM,      0u, ShaderStageMask::Fragment, MaterialBinding::Kind::Texture, "orm" },
        { TexSlots::Emissive, 0u, ShaderStageMask::Fragment, MaterialBinding::Kind::Texture, "emissive" },
        { SamplerSlots::LinearWrap,  0u, ShaderStageMask::Fragment, MaterialBinding::Kind::Sampler, "sLinearWrap" },
        { SamplerSlots::LinearClamp, 0u, ShaderStageMask::Fragment, MaterialBinding::Kind::Sampler, "sLinearClamp" },
    };

    return desc;
}

MaterialHandle PbrMaterial::Register(MaterialSystem& materials, const PbrMaterialCreateInfo& info)
{
    return materials.RegisterMaterial(BuildDesc(info));
}

PbrMaterial PbrMaterial::Create(MaterialSystem& materials, const PbrMaterialCreateInfo& info) noexcept
{
    return PbrMaterial(materials, Register(materials, info));
}

bool PbrMaterial::IsValid() const noexcept
{
    return m_materials != nullptr && m_instance != nullptr && m_handle.IsValid();
}

bool PbrMaterial::SetBaseColorFactor(const math::Vec4& value) noexcept
{
    if (!IsValid())
        return false;
    m_materials->SetVec4(m_handle, "baseColorFactor", value);
    return true;
}

bool PbrMaterial::SetEmissiveFactor(const math::Vec4& value) noexcept
{
    if (!IsValid())
        return false;
    m_materials->SetVec4(m_handle, "emissiveFactor", value);
    return true;
}

bool PbrMaterial::SetMetallicFactor(float value) noexcept
{
    if (!IsValid())
        return false;
    m_materials->SetFloat(m_handle, "metallicFactor", value);
    return true;
}

bool PbrMaterial::SetRoughnessFactor(float value) noexcept
{
    if (!IsValid())
        return false;
    m_materials->SetFloat(m_handle, "roughnessFactor", value);
    return true;
}

bool PbrMaterial::SetOcclusionStrength(float value) noexcept
{
    if (!IsValid())
        return false;
    m_materials->SetFloat(m_handle, "occlusionStrength", value);
    return true;
}

bool PbrMaterial::SetOpacityFactor(float value) noexcept
{
    if (!IsValid())
        return false;
    m_materials->SetFloat(m_handle, "opacityFactor", value);
    return true;
}

bool PbrMaterial::SetAlphaCutoff(float value) noexcept
{
    if (!IsValid())
        return false;
    m_materials->SetFloat(m_handle, "alphaCutoff", value);
    return true;
}

bool PbrMaterial::SetAlbedo(TextureHandle texture) noexcept
{
    return SetTextureAtSlot(m_albedoSlot, "albedo", texture);
}

bool PbrMaterial::SetNormal(TextureHandle texture) noexcept
{
    return SetTextureAtSlot(m_normalSlot, "normal", texture);
}

bool PbrMaterial::SetORM(TextureHandle texture) noexcept
{
    return SetTextureAtSlot(m_ormSlot, "orm", texture);
}

bool PbrMaterial::SetEmissive(TextureHandle texture) noexcept
{
    return SetTextureAtSlot(m_emissiveSlot, "emissive", texture);
}

MaterialInstance& PbrMaterial::Raw()
{
    if (!m_instance)
        throw std::runtime_error("PbrMaterial::Raw called on invalid material");
    return *m_instance;
}

const MaterialInstance& PbrMaterial::Raw() const
{
    if (!m_instance)
        throw std::runtime_error("PbrMaterial::Raw called on invalid material");
    return *m_instance;
}

bool PbrMaterial::SetTextureAtSlot(int32_t slotIndex, const char* slotName, TextureHandle texture) noexcept
{
    if (!IsValid() || slotIndex < 0)
        return false;

    const uint32_t resolvedSlot = static_cast<uint32_t>(slotIndex);
    m_materials->SetTexture(m_handle, slotName, texture);
    (void)m_instance->parameters.SetTexture(resolvedSlot, texture);
    return true;
}

void PbrMaterial::CacheSlots() noexcept
{
    m_albedoSlot = static_cast<int32_t>(TexSlots::Albedo);
    m_normalSlot = static_cast<int32_t>(TexSlots::Normal);
    m_ormSlot = static_cast<int32_t>(TexSlots::ORM);
    m_emissiveSlot = static_cast<int32_t>(TexSlots::Emissive);
}

} // namespace engine::renderer::pbr
