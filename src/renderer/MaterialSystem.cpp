#include "renderer/MaterialSystem.hpp"
#include "renderer/MaterialCBLayout.hpp"
#include "renderer/MaterialFeatureEval.hpp"
#include "core/Debug.hpp"
#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstring>
#include <type_traits>

namespace engine::renderer {

namespace {

constexpr size_t SemanticIndex(MaterialSemantic semantic) noexcept
{
    return static_cast<size_t>(semantic);
}

MaterialParam MakeFloatParam(const char* name, float v)
{
    MaterialParam p{};
    p.name = name;
    p.type = MaterialParam::Type::Float;
    p.value.f[0] = v;
    return p;
}

MaterialParam MakeVec4Param(const char* name, float x, float y, float z, float w)
{
    MaterialParam p{};
    p.name = name;
    p.type = MaterialParam::Type::Vec4;
    p.value.f[0] = x;
    p.value.f[1] = y;
    p.value.f[2] = z;
    p.value.f[3] = w;
    return p;
}

// Int param for bitfields: avoids the precision loss of casting uint32_t to float.
// Use for materialFeatureMask and similar bit-pattern payloads.
MaterialParam MakeIntParam(const char* name, int32_t v)
{
    MaterialParam p{};
    p.name = name;
    p.type = MaterialParam::Type::Int;
    p.value.i = v;
    return p;
}

bool NameEqualsInsensitive(const std::string& a, const char* b)
{
    if (a.size() != std::strlen(b))
        return false;
    for (size_t i = 0; i < a.size(); ++i)
    {
        char ca = a[i];
        char cb = b[i];
        if (ca >= 'A' && ca <= 'Z') ca = static_cast<char>(ca - 'A' + 'a');
        if (cb >= 'A' && cb <= 'Z') cb = static_cast<char>(cb - 'A' + 'a');
        if (ca != cb) return false;
    }
    return true;
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
    static constexpr uint64_t FNV_PRIME  = 0x100000001B3ull;
    static constexpr uint64_t FNV_OFFSET = 0xCBF29CE484222325ull;

    const uint8_t* p = reinterpret_cast<const uint8_t*>(this);
    uint64_t h = FNV_OFFSET;
    for (size_t i = 0; i < sizeof(PipelineKey); ++i)
    {
        h ^= static_cast<uint64_t>(p[i]);
        h *= FNV_PRIME;
    }
    return h;
}

PipelineKey PipelineKey::From(const PipelineDesc& desc, RenderPassTag pass) noexcept
{
    PipelineKey k{};
    static_assert(std::is_trivially_copyable_v<PipelineKey>, "PipelineKey must be trivially copyable");

    for (const auto& stage : desc.shaderStages)
    {
        if (stage.stage == ShaderStageMask::Vertex)
            k.vertexShader = stage.handle.value;
        else if (stage.stage == ShaderStageMask::Fragment)
            k.fragmentShader = stage.handle.value;
        else if (stage.stage == ShaderStageMask::Compute)
            k.computeShader = stage.handle.value;
    }

    k.fillMode  = static_cast<uint8_t>(desc.rasterizer.fillMode);
    k.cullMode  = static_cast<uint8_t>(desc.rasterizer.cullMode);
    k.frontFace = static_cast<uint8_t>(desc.rasterizer.frontFace);

    k.depthEnable   = desc.depthStencil.depthEnable ? 1u : 0u;
    k.depthWrite    = desc.depthStencil.depthWrite ? 1u : 0u;
    k.depthFunc     = static_cast<uint8_t>(desc.depthStencil.depthFunc);
    k.stencilEnable = desc.depthStencil.stencilEnable ? 1u : 0u;

    const auto& b = desc.blendStates[0];
    k.blendEnable   = b.blendEnable ? 1u : 0u;
    k.srcBlend      = static_cast<uint8_t>(b.srcBlend);
    k.dstBlend      = static_cast<uint8_t>(b.dstBlend);
    k.blendOp       = static_cast<uint8_t>(b.blendOp);
    k.srcBlendAlpha = static_cast<uint8_t>(b.srcBlendAlpha);
    k.dstBlendAlpha = static_cast<uint8_t>(b.dstBlendAlpha);
    k.blendOpAlpha  = static_cast<uint8_t>(b.blendOpAlpha);
    k.writeMask     = b.writeMask;

    k.colorFormat = static_cast<uint8_t>(desc.colorFormat);
    k.depthFormat = static_cast<uint8_t>(desc.depthFormat);
    k.sampleCount = static_cast<uint8_t>(desc.sampleCount);
    k.topology    = static_cast<uint8_t>(desc.topology);

    static constexpr uint32_t PRIME  = 0x01000193u;
    static constexpr uint32_t OFFSET = 0x811C9DC5u;
    uint32_t vh = OFFSET;
    for (const auto& attr : desc.vertexLayout.attributes)
    {
        vh ^= static_cast<uint32_t>(attr.semantic); vh *= PRIME;
        vh ^= static_cast<uint32_t>(attr.format);   vh *= PRIME;
        vh ^= attr.binding;                         vh *= PRIME;
        vh ^= attr.offset;                          vh *= PRIME;
    }
    k.vertexLayoutHash = vh;

    k.shaderContractHash = static_cast<uint32_t>(desc.shaderContractHash ^ (desc.shaderContractHash >> 32u));
    k.pipelineLayoutHash = static_cast<uint32_t>(desc.pipelineLayoutHash ^ (desc.pipelineLayoutHash >> 32u));
    k.pipelineClass = static_cast<uint8_t>(desc.pipelineClass);
    k.passTag = pass;
    return k;
}

SortKey SortKey::ForOpaque(RenderPassTag pass, uint8_t layer, uint32_t pipelineHash, float linearDepth) noexcept
{
    const uint64_t p = static_cast<uint64_t>(pass) & 0xFull;
    const uint64_t l = static_cast<uint64_t>(layer) & 0xFull;
    const uint64_t h = static_cast<uint64_t>(pipelineHash >> 8) & 0xFFFFFFull;
    const uint64_t d = static_cast<uint32_t>(std::max(0.f, linearDepth) * 4294967295.f);

    SortKey sk{};
    sk.value = (p << 60u) | (l << 56u) | (h << 32u) | d;
    return sk;
}

SortKey SortKey::ForTransparent(RenderPassTag pass, uint8_t layer, float linearDepth) noexcept
{
    const uint64_t p = static_cast<uint64_t>(pass) & 0xFull;
    const uint64_t l = static_cast<uint64_t>(layer) & 0xFull;
    const uint32_t rawDepth = static_cast<uint32_t>(std::max(0.f, linearDepth) * 4294967295.f);
    const uint64_t d = static_cast<uint64_t>(UINT32_MAX - rawDepth);

    SortKey sk{};
    sk.value = (p << 60u) | (l << 56u) | d;
    return sk;
}

SortKey SortKey::ForUI(uint8_t layer, uint32_t drawOrder) noexcept
{
    const uint64_t p = static_cast<uint64_t>(RenderPassTag::UI) & 0xFull;
    const uint64_t l = static_cast<uint64_t>(layer) & 0xFull;
    SortKey sk{};
    sk.value = (p << 60u) | (l << 56u) | static_cast<uint64_t>(drawOrder);
    return sk;
}

RenderPassTag MaterialInstance::PassTag() const noexcept
{
    return pipelineKey.passTag;
}

float* MaterialInstance::GetFloatPtr(const std::string& name) noexcept
{
    const uint32_t offset = cbLayout.GetOffset(name);
    if (offset == UINT32_MAX || offset + 4u > cbData.size())
        return nullptr;
    return reinterpret_cast<float*>(cbData.data() + offset);
}

const char* MaterialSystem::SemanticName(MaterialSemantic semantic) noexcept
{
    switch (semantic)
    {
    case MaterialSemantic::BaseColor: return "BaseColor";
    case MaterialSemantic::Normal: return "Normal";
    case MaterialSemantic::Metallic: return "Metallic";
    case MaterialSemantic::Roughness: return "Roughness";
    case MaterialSemantic::Occlusion: return "Occlusion";
    case MaterialSemantic::Emissive: return "Emissive";
    case MaterialSemantic::Opacity: return "Opacity";
    case MaterialSemantic::AlphaCutoff: return "AlphaCutoff";
    case MaterialSemantic::ORM: return "ORM";
    default: return "Unknown";
    }
}

uint32_t MaterialSystem::AllocSlot()
{
    if (!m_freeSlots.empty())
    {
        const uint32_t idx = m_freeSlots.back();
        m_freeSlots.pop_back();
        return idx;
    }
    const uint32_t idx = static_cast<uint32_t>(m_descs.size());
    m_descs.emplace_back();
    m_instances.emplace_back();
    m_generations.push_back(1u);
    return idx;
}

bool MaterialSystem::ValidHandle(MaterialHandle h) const noexcept
{
    if (!h.IsValid()) return false;
    const uint32_t idx = h.Index();
    if (idx >= m_generations.size()) return false;
    return m_generations[idx] == h.Generation();
}

void MaterialSystem::InitializeInstanceFromDesc(MaterialInstance& inst, const MaterialDesc& desc) const noexcept
{
    inst.instanceParams = desc.params;
    inst.semanticValues = desc.semanticValues;
    inst.semanticTextures = desc.semanticTextures;
    inst.featureMask = MaterialFeatureEval::DeriveFeatureMask(desc, inst);
    inst.cbDirty = true;
    inst.layoutDirty = true;
}

std::vector<MaterialParam> MaterialSystem::BuildCanonicalParams(const MaterialDesc& desc, const MaterialInstance& inst) const
{
    // Materialien mit expliziten User-Params (desc.params nicht leer) verwenden
    // ausschliesslich diese. Kanonische PBR-Params werden nur für
    // Semantic-Authored-Materialien (desc.params leer) generiert.
    if (!desc.params.empty())
        return inst.instanceParams;

    std::vector<MaterialParam> params;

    const auto appendOrReplace = [&](const MaterialParam& param)
    {
        auto it = std::find_if(params.begin(), params.end(), [&](const MaterialParam& existing) {
            return existing.name == param.name;
        });
        if (it != params.end())
            *it = param;
        else
            params.push_back(param);
    };

    const auto baseColor = MaterialFeatureEval::ResolveSemanticValue(desc, inst, MaterialSemantic::BaseColor);
    const auto emissive = MaterialFeatureEval::ResolveSemanticValue(desc, inst, MaterialSemantic::Emissive);
    const auto metallic = MaterialFeatureEval::ResolveSemanticValue(desc, inst, MaterialSemantic::Metallic);
    const auto roughness = MaterialFeatureEval::ResolveSemanticValue(desc, inst, MaterialSemantic::Roughness);
    const auto occlusion = MaterialFeatureEval::ResolveSemanticValue(desc, inst, MaterialSemantic::Occlusion);
    const auto opacity = MaterialFeatureEval::ResolveSemanticValue(desc, inst, MaterialSemantic::Opacity);
    const auto alphaCutoff = MaterialFeatureEval::ResolveSemanticValue(desc, inst, MaterialSemantic::AlphaCutoff);
    appendOrReplace(MakeVec4Param("baseColorFactor", baseColor.data[0], baseColor.data[1], baseColor.data[2], baseColor.data[3]));
    appendOrReplace(MakeVec4Param("emissiveFactor", emissive.data[0], emissive.data[1], emissive.data[2], emissive.data[3]));
    appendOrReplace(MakeFloatParam("metallicFactor", metallic.data[0]));
    appendOrReplace(MakeFloatParam("roughnessFactor", roughness.data[0]));
    appendOrReplace(MakeFloatParam("occlusionStrength", occlusion.data[0]));
    appendOrReplace(MakeFloatParam("opacityFactor", opacity.data[0]));
    appendOrReplace(MakeFloatParam("alphaCutoff", alphaCutoff.data[0]));
    // Fix 1: featureMask is a bitfield — store as Int to avoid float precision loss above bit 23.
    // Shader must declare: int materialFeatureMask (HLSL) / int materialFeatureMask (GLSL std140).
    appendOrReplace(MakeIntParam("materialFeatureMask", static_cast<int32_t>(inst.featureMask)));
    appendOrReplace(MakeFloatParam("materialModel", desc.model == MaterialModel::Unlit ? 1.f : 0.f));

    return params;
}

MaterialHandle MaterialSystem::RegisterMaterial(MaterialDesc desc)
{
    NormalizeDesc(desc);
    const uint32_t idx = AllocSlot();
    const std::string name = desc.name;

    m_descs[idx].desc = std::move(desc);
    m_descs[idx].name = name;
    m_descs[idx].isInstance = false;

    const MaterialHandle h = MaterialHandle::Make(idx, m_generations[idx]);
    MaterialInstance& inst = m_instances[idx];
    inst.desc = h;
    InitializeInstanceFromDesc(inst, m_descs[idx].desc);
    inst.pipelineKey = BuildPipelineKey(h);
    inst.pipelineKeyHash = static_cast<uint32_t>(inst.pipelineKey.Hash());

    if (!name.empty())
        m_nameLookup[name] = h;

    Debug::Log("MaterialSystem.cpp: RegisterMaterial '%s' idx=%u passTag=%s", name.c_str(), idx, PassTagName(m_descs[idx].desc.passTag));
    return h;
}

MaterialHandle MaterialSystem::CreateInstance(MaterialHandle base, std::string instanceName)
{
    if (!ValidHandle(base))
    {
        Debug::LogError("MaterialSystem.cpp: CreateInstance - invalid base handle");
        return MaterialHandle::Invalid();
    }

    const uint32_t idx = AllocSlot();
    const uint32_t baseIdx = base.Index();

    m_descs[idx].desc = m_descs[baseIdx].desc;
    m_descs[idx].name = instanceName.empty() ? m_descs[baseIdx].name + "_inst" : std::move(instanceName);
    m_descs[idx].isInstance = true;
    m_descs[idx].baseHandle = base;

    const MaterialHandle h = MaterialHandle::Make(idx, m_generations[idx]);
    MaterialInstance& inst = m_instances[idx];
    inst.desc = h;
    InitializeInstanceFromDesc(inst, m_descs[idx].desc);
    inst.pipelineKey = BuildPipelineKey(base);
    inst.pipelineKeyHash = static_cast<uint32_t>(inst.pipelineKey.Hash());
    return h;
}

const MaterialDesc* MaterialSystem::GetDesc(MaterialHandle h) const noexcept
{
    if (!ValidHandle(h)) return nullptr;
    return &m_descs[h.Index()].desc;
}

MaterialHandle MaterialSystem::FindMaterial(const std::string& name) const noexcept
{
    const auto it = m_nameLookup.find(name);
    return it == m_nameLookup.end() ? MaterialHandle::Invalid() : it->second;
}

MaterialInstance* MaterialSystem::GetInstance(MaterialHandle h) noexcept
{
    if (!ValidHandle(h)) return nullptr;
    return &m_instances[h.Index()];
}

const MaterialInstance* MaterialSystem::GetInstance(MaterialHandle h) const noexcept
{
    if (!ValidHandle(h)) return nullptr;
    return &m_instances[h.Index()];
}

void MaterialSystem::SetFloat(MaterialHandle h, const std::string& name, float v)
{
    if (!ValidHandle(h)) return;
    auto& inst = m_instances[h.Index()];
    for (auto& p : inst.instanceParams)
    {
        if (p.name == name && p.type == MaterialParam::Type::Float)
        {
            p.value.f[0] = v;
            inst.cbDirty = true;
            inst.layoutDirty = true;
            return;
        }
    }
    MaterialParam np{};
    np.name = name; np.type = MaterialParam::Type::Float; np.value.f[0] = v;
    inst.instanceParams.push_back(np);
    inst.cbDirty = true;
    inst.layoutDirty = true;
}

void MaterialSystem::SetVec4(MaterialHandle h, const std::string& name, const math::Vec4& v)
{
    if (!ValidHandle(h)) return;
    auto& inst = m_instances[h.Index()];
    for (auto& p : inst.instanceParams)
    {
        if (p.name == name && p.type == MaterialParam::Type::Vec4)
        {
            p.value.f[0]=v.x; p.value.f[1]=v.y; p.value.f[2]=v.z; p.value.f[3]=v.w;
            inst.cbDirty = true;
            inst.layoutDirty = true;
            return;
        }
    }
    MaterialParam np{};
    np.name = name; np.type = MaterialParam::Type::Vec4;
    np.value.f[0]=v.x; np.value.f[1]=v.y; np.value.f[2]=v.z; np.value.f[3]=v.w;
    inst.instanceParams.push_back(np);
    inst.cbDirty = true;
    inst.layoutDirty = true;
}

void MaterialSystem::SetTexture(MaterialHandle h, const std::string& name, TextureHandle tex)
{
    if (!ValidHandle(h)) return;
    auto& inst = m_instances[h.Index()];
    for (auto& p : inst.instanceParams)
    {
        if (p.name == name && p.type == MaterialParam::Type::Texture)
        {
            p.texture = tex;
            inst.cbDirty = true;
            inst.layoutDirty = true;
            return;
        }
    }
    MaterialParam np{};
    np.name = name; np.type = MaterialParam::Type::Texture; np.texture = tex;
    inst.instanceParams.push_back(np);
    inst.cbDirty = true;
    inst.layoutDirty = true;
}

void MaterialSystem::MarkDirty(MaterialHandle h)
{
    if (!ValidHandle(h)) return;
    auto& inst = m_instances[h.Index()];
    inst.cbDirty = true;
    inst.layoutDirty = true;
}

void MaterialSystem::SetSemanticFloat(MaterialHandle h, MaterialSemantic semantic, float v)
{
    if (!ValidHandle(h)) return;
    auto& inst = m_instances[h.Index()];
    auto& value = inst.semanticValues[SemanticIndex(semantic)];
    value.set = true;
    value.data[0] = v;
    inst.featureMask = MaterialFeatureEval::DeriveFeatureMask(m_descs[h.Index()].desc, inst);
    MarkDirty(h);
}

void MaterialSystem::SetSemanticVec4(MaterialHandle h, MaterialSemantic semantic, const math::Vec4& v)
{
    if (!ValidHandle(h)) return;
    auto& inst = m_instances[h.Index()];
    auto& value = inst.semanticValues[SemanticIndex(semantic)];
    value.set = true;
    value.data = {v.x, v.y, v.z, v.w};
    inst.featureMask = MaterialFeatureEval::DeriveFeatureMask(m_descs[h.Index()].desc, inst);
    MarkDirty(h);
}

void MaterialSystem::SetSemanticTexture(MaterialHandle h, MaterialSemantic semantic, TextureHandle tex, uint32_t samplerIdx)
{
    if (!ValidHandle(h)) return;
    auto& inst = m_instances[h.Index()];
    auto& input = inst.semanticTextures[SemanticIndex(semantic)];
    input.set = true;
    input.texture = tex;
    input.samplerIdx = samplerIdx;
    inst.featureMask = MaterialFeatureEval::DeriveFeatureMask(m_descs[h.Index()].desc, inst);
    MarkDirty(h);
}

void MaterialSystem::ClearSemanticTexture(MaterialHandle h, MaterialSemantic semantic)
{
    if (!ValidHandle(h)) return;
    auto& inst = m_instances[h.Index()];
    auto& input = inst.semanticTextures[SemanticIndex(semantic)];
    input.set = false;
    input.texture = TextureHandle::Invalid();
    input.samplerIdx = 0u;
    inst.featureMask = MaterialFeatureEval::DeriveFeatureMask(m_descs[h.Index()].desc, inst);
    MarkDirty(h);
}

MaterialFeatureFlag MaterialSystem::GetFeatureFlags(MaterialHandle h) const noexcept
{
    if (!ValidHandle(h)) return MaterialFeatureFlag::None;
    const auto& inst = m_instances[h.Index()];
    return static_cast<MaterialFeatureFlag>(inst.featureMask);
}

bool MaterialSystem::HasExplicitSemanticValue(MaterialHandle h, MaterialSemantic semantic) const noexcept
{
    if (!ValidHandle(h)) return false;
    return m_instances[h.Index()].semanticValues[SemanticIndex(semantic)].set;
}

bool MaterialSystem::HasExplicitSemanticTexture(MaterialHandle h, MaterialSemantic semantic) const noexcept
{
    if (!ValidHandle(h)) return false;
    const auto& input = m_instances[h.Index()].semanticTextures[SemanticIndex(semantic)];
    return input.set && input.texture.IsValid();
}

TextureHandle MaterialSystem::GetSemanticTexture(MaterialHandle h, MaterialSemantic semantic) const noexcept
{
    if (!ValidHandle(h)) return TextureHandle::Invalid();
    const auto& input = m_instances[h.Index()].semanticTextures[SemanticIndex(semantic)];
    return (input.set && input.texture.IsValid()) ? input.texture : TextureHandle::Invalid();
}

MaterialSemanticValue MaterialSystem::GetSemanticValue(MaterialHandle h, MaterialSemantic semantic) const noexcept
{
    MaterialSemanticValue empty{};
    if (!ValidHandle(h)) return empty;
    return m_instances[h.Index()].semanticValues[SemanticIndex(semantic)];
}

MaterialSemanticValue MaterialSystem::ResolveSemanticValue(MaterialHandle h, MaterialSemantic semantic) const noexcept
{
    MaterialSemanticValue empty{};
    if (!ValidHandle(h)) return empty;
    return MaterialFeatureEval::ResolveSemanticValue(m_descs[h.Index()].desc, m_instances[h.Index()], semantic);
}

const std::vector<uint8_t>& MaterialSystem::GetCBData(MaterialHandle h)
{
    static std::vector<uint8_t> empty;
    if (!ValidHandle(h)) return empty;
    auto& inst = m_instances[h.Index()];
    const MaterialDesc& desc = m_descs[h.Index()].desc;

    if (inst.layoutDirty)
    {
        inst.featureMask = MaterialFeatureEval::DeriveFeatureMask(desc, inst);
        inst.cbLayout = MaterialCBLayout::Build(BuildCanonicalParams(desc, inst));
        inst.layoutDirty = false;
        inst.cbDirty = true;
    }

    if (inst.cbDirty)
    {
        MaterialCBLayout::BuildCBData(inst, desc);
        inst.cbDirty = false;
    }
    return inst.cbData;
}

const CbLayout& MaterialSystem::GetCBLayout(MaterialHandle h)
{
    static CbLayout empty;
    if (!ValidHandle(h)) return empty;
    GetCBData(h);
    return m_instances[h.Index()].cbLayout;
}

} // namespace engine::renderer
