// =============================================================================
// KROM Engine - src/renderer/MaterialSystem.cpp
// =============================================================================
#include "renderer/MaterialSystem.hpp"
#include "core/Debug.hpp"
#include <cstring>
#include <cassert>
#include <cmath>

namespace engine::renderer {

// =============================================================================
// CbLayout
// =============================================================================

// HLSL cbuffer-Packing (vereinfacht):
//   - float  = 4 Bytes
//   - float2 = 8 Bytes (4-Byte-aligned)
//   - float3 = 12 Bytes (bleibt im selben 16-Byte-Register falls möglich)
//   - float4 = 16 Bytes (16-Byte-aligned)
// Wir runden jeden Parameter auf die nächste 16-Byte-Grenze auf (konservativ).
CbLayout CbLayout::Build(const std::vector<MaterialParam>& params) noexcept
{
    CbLayout layout;
    uint32_t offset = 0u;

    for (const auto& p : params)
    {
        if (p.type == MaterialParam::Type::Texture ||
            p.type == MaterialParam::Type::Sampler)
            continue; // Textures/Samplers landen nicht im CB

        CbFieldDesc field;
        field.name       = p.name;
        field.offset     = offset;
        field.arrayCount = 1u;
        field.type       = p.type;

        uint32_t fieldSize = 16u; // default: float4-aligned
        switch (p.type) {
        case MaterialParam::Type::Float: fieldSize = 4u;  break;
        case MaterialParam::Type::Vec2:  fieldSize = 8u;  break;
        case MaterialParam::Type::Vec3:  fieldSize = 12u; break;
        case MaterialParam::Type::Vec4:  fieldSize = 16u; break;
        case MaterialParam::Type::Int:   fieldSize = 4u;  break;
        case MaterialParam::Type::Bool:  fieldSize = 4u;  break;
        default:                         fieldSize = 16u; break;
        }
        field.size = fieldSize;

        // HLSL-Packing: Feld darf 16-Byte-Boundary nicht überschreiten
        // Wenn aktueller Offset + fieldSize die nächste 16-Byte-Grenze überschreitet:
        // Padding einfügen
        const uint32_t boundaryOffset = (offset / 16u + 1u) * 16u;
        if (fieldSize > 4u && (offset % 16u) + fieldSize > 16u)
        {
            // Padding zum nächsten Register
            offset = boundaryOffset;
            field.offset = offset;
        }

        layout.fields.push_back(field);
        offset += fieldSize;
    }

    // Gesamtgröße auf 16-Byte-Grenze aufrunden (CB muss 16-Byte-aligned sein)
    layout.totalSize = (offset + 15u) & ~15u;
    if (layout.totalSize == 0u) layout.totalSize = 16u; // Mindestgröße

    return layout;
}


// =============================================================================
// ShaderVariantKey
// =============================================================================

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

// =============================================================================
// PipelineKey
// =============================================================================

bool PipelineKey::operator==(const PipelineKey& o) const noexcept
{
    return std::memcmp(this, &o, sizeof(PipelineKey)) == 0;
}

uint64_t PipelineKey::Hash() const noexcept
{
    // FNV-1a über die rohen Bytes
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
    // Explizit null-initialisieren: alle Padding-Bytes auf 0.
    // PipelineKey::operator== benutzt memcmp über die rohen Bytes -
    // uninitialisierte Padding-Felder würden zufällige Cache-Misses verursachen.
    PipelineKey k{};
    static_assert(std::is_trivially_copyable_v<PipelineKey>,
        "PipelineKey muss trivially copyable sein für memcmp/Hash");

    // Shaders
    for (const auto& stage : desc.shaderStages)
    {
        if (stage.stage == ShaderStageMask::Vertex)
            k.vertexShader   = stage.handle.value;
        else if (stage.stage == ShaderStageMask::Fragment)
            k.fragmentShader = stage.handle.value;
        else if (stage.stage == ShaderStageMask::Compute)
            k.computeShader  = stage.handle.value;
    }

    // Rasterizer
    k.fillMode  = static_cast<uint8_t>(desc.rasterizer.fillMode);
    k.cullMode  = static_cast<uint8_t>(desc.rasterizer.cullMode);
    k.frontFace = static_cast<uint8_t>(desc.rasterizer.frontFace);

    // Depth/Stencil
    k.depthEnable   = desc.depthStencil.depthEnable  ? 1u : 0u;
    k.depthWrite    = desc.depthStencil.depthWrite   ? 1u : 0u;
    k.depthFunc     = static_cast<uint8_t>(desc.depthStencil.depthFunc);
    k.stencilEnable = desc.depthStencil.stencilEnable ? 1u : 0u;

    // Blend (RT0)
    const auto& b   = desc.blendStates[0];
    k.blendEnable   = b.blendEnable ? 1u : 0u;
    k.srcBlend      = static_cast<uint8_t>(b.srcBlend);
    k.dstBlend      = static_cast<uint8_t>(b.dstBlend);
    k.blendOp       = static_cast<uint8_t>(b.blendOp);
    k.srcBlendAlpha = static_cast<uint8_t>(b.srcBlendAlpha);
    k.dstBlendAlpha = static_cast<uint8_t>(b.dstBlendAlpha);
    k.blendOpAlpha  = static_cast<uint8_t>(b.blendOpAlpha);
    k.writeMask     = b.writeMask;

    // Formate
    k.colorFormat = static_cast<uint8_t>(desc.colorFormat);
    k.depthFormat = static_cast<uint8_t>(desc.depthFormat);
    k.sampleCount = static_cast<uint8_t>(desc.sampleCount);
    k.topology    = static_cast<uint8_t>(desc.topology);

    // VertexLayout-Hash (FNV-1a über Attribute-Bytes)
    {
        static constexpr uint32_t PRIME  = 0x01000193u;
        static constexpr uint32_t OFFSET = 0x811C9DC5u;
        uint32_t h = OFFSET;
        for (const auto& attr : desc.vertexLayout.attributes)
        {
            h ^= static_cast<uint32_t>(attr.semantic); h *= PRIME;
            h ^= static_cast<uint32_t>(attr.format);   h *= PRIME;
            h ^= attr.binding;                          h *= PRIME;
            h ^= attr.offset;                           h *= PRIME;
        }
        k.vertexLayoutHash = h;
    }

    k.shaderContractHash = static_cast<uint32_t>(desc.shaderContractHash ^ (desc.shaderContractHash >> 32u));
    k.pipelineLayoutHash = static_cast<uint32_t>(desc.pipelineLayoutHash ^ (desc.pipelineLayoutHash >> 32u));
    k.pipelineClass = static_cast<uint8_t>(desc.pipelineClass);
    k.passTag = pass;
    return k;
}

// =============================================================================
// SortKey
// =============================================================================

SortKey SortKey::ForOpaque(RenderPassTag pass,
                             uint8_t layer,
                             uint32_t pipelineHash,
                             float linearDepth) noexcept
{
    // [63..60] pass (4) | [59..56] layer (4) | [55..32] pipeHash (24) | [31..0] depth
    const uint64_t p = static_cast<uint64_t>(pass)  & 0xFull;
    const uint64_t l = static_cast<uint64_t>(layer) & 0xFull;
    const uint64_t h = static_cast<uint64_t>(pipelineHash >> 8) & 0xFFFFFFull; // top 24 bits
    // Tiefe: als uint32 kodiert, front-to-back (kleiner = näher = besser)
    const uint64_t d = static_cast<uint32_t>(std::max(0.f, linearDepth) * 4294967295.f);

    SortKey sk;
    sk.value = (p << 60u) | (l << 56u) | (h << 32u) | d;
    return sk;
}

SortKey SortKey::ForTransparent(RenderPassTag pass,
                                  uint8_t layer,
                                  float linearDepth) noexcept
{
    const uint64_t p = static_cast<uint64_t>(pass)  & 0xFull;
    const uint64_t l = static_cast<uint64_t>(layer) & 0xFull;
    // Transparent: back-to-front → Tiefe invertieren
    const uint32_t rawDepth = static_cast<uint32_t>(std::max(0.f, linearDepth) * 4294967295.f);
    const uint64_t d = static_cast<uint64_t>(UINT32_MAX - rawDepth);

    SortKey sk;
    sk.value = (p << 60u) | (l << 56u) | d;
    return sk;
}

SortKey SortKey::ForUI(uint8_t layer, uint32_t drawOrder) noexcept
{
    const uint64_t p = static_cast<uint64_t>(RenderPassTag::UI) & 0xFull;
    const uint64_t l = static_cast<uint64_t>(layer)             & 0xFull;
    SortKey sk;
    sk.value = (p << 60u) | (l << 56u) | static_cast<uint64_t>(drawOrder);
    return sk;
}

// =============================================================================
// MaterialInstance
// =============================================================================

RenderPassTag MaterialInstance::PassTag() const noexcept
{
    return pipelineKey.passTag;
}

// =============================================================================
// MaterialSystem
// =============================================================================

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

MaterialHandle MaterialSystem::RegisterMaterial(MaterialDesc desc)
{
    const uint32_t idx = AllocSlot();
    const std::string name = desc.name;

    m_descs[idx].desc       = std::move(desc);
    m_descs[idx].name       = name;
    m_descs[idx].isInstance = false;

    // Initiale Instance anlegen (selbstreferenzierend)
    const MaterialHandle h = MaterialHandle::Make(idx, m_generations[idx]);

    // PipelineKey vorberechnen
    MaterialInstance& inst = m_instances[idx];
    inst.desc              = h;
    inst.pipelineKey       = BuildPipelineKey(h);
    inst.pipelineKeyHash   = static_cast<uint32_t>(inst.pipelineKey.Hash());
    inst.cbDirty           = true;
    inst.layoutDirty       = true;

    Debug::Log("MaterialSystem.cpp: RegisterMaterial '%s' idx=%u passTag=%s",
        name.c_str(), idx, PassTagName(m_descs[idx].desc.passTag));

    if (!name.empty())
        m_nameLookup[name] = h;

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

    m_descs[idx].desc       = m_descs[baseIdx].desc; // Kopie
    m_descs[idx].name       = instanceName.empty()
                              ? m_descs[baseIdx].name + "_inst"
                              : std::move(instanceName);
    m_descs[idx].isInstance = true;
    m_descs[idx].baseHandle = base;

    const MaterialHandle h = MaterialHandle::Make(idx, m_generations[idx]);
    MaterialInstance& inst = m_instances[idx];
    inst.desc              = h;
    inst.pipelineKey       = BuildPipelineKey(base);
    inst.pipelineKeyHash   = static_cast<uint32_t>(inst.pipelineKey.Hash());
    inst.instanceParams    = m_descs[baseIdx].desc.params;
    inst.cbDirty           = true;
    inst.layoutDirty       = true;

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
    if (it == m_nameLookup.end())
        return MaterialHandle::Invalid();
    return it->second;
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

PipelineKey MaterialSystem::BuildPipelineKey(MaterialHandle h) const noexcept
{
    if (!ValidHandle(h)) return PipelineKey{};
    const MaterialDesc& d = m_descs[h.Index()].desc;

    const bool isShadowPass = d.passTag == RenderPassTag::Shadow;
    const ShaderHandle pipelineVS = (isShadowPass && d.shadowShader.IsValid()) ? d.shadowShader : d.vertexShader;

    PipelineDesc pd;
    if (pipelineVS.IsValid())
        pd.shaderStages.push_back({ pipelineVS, ShaderStageMask::Vertex });
    if (!isShadowPass && d.fragmentShader.IsValid())
        pd.shaderStages.push_back({ d.fragmentShader, ShaderStageMask::Fragment });
    pd.rasterizer   = d.rasterizer;
    pd.depthStencil = d.depthStencil;
    pd.blendStates[0] = d.blend;
    pd.topology     = d.topology;
    pd.vertexLayout = d.vertexLayout;
    pd.colorFormat  = isShadowPass ? Format::Unknown : d.colorFormat;
    pd.depthFormat  = d.depthFormat;

    return PipelineKey::From(pd, d.passTag);
}

void MaterialSystem::SetFloat(MaterialHandle h, const std::string& name, float v)
{
    if (!ValidHandle(h)) return;
    auto& inst = m_instances[h.Index()];
    for (auto& p : inst.instanceParams)
    {
        if (p.name == name && p.type == MaterialParam::Type::Float)
        { p.value.f[0] = v; inst.cbDirty = true; return; }
    }
    // Neu anlegen falls nicht vorhanden
    MaterialParam np;
    np.name = name; np.type = MaterialParam::Type::Float; np.value.f[0] = v;
    inst.instanceParams.push_back(np);
    inst.cbDirty    = true;
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
            p.value.f[0]=v.x; p.value.f[1]=v.y;
            p.value.f[2]=v.z; p.value.f[3]=v.w;
            inst.cbDirty = true;
            return;
        }
    }
    MaterialParam np;
    np.name = name; np.type = MaterialParam::Type::Vec4;
    np.value.f[0]=v.x; np.value.f[1]=v.y;
    np.value.f[2]=v.z; np.value.f[3]=v.w;
    inst.instanceParams.push_back(np);
    inst.cbDirty    = true;
    inst.layoutDirty = true;
}

void MaterialSystem::SetTexture(MaterialHandle h, const std::string& name, TextureHandle tex)
{
    if (!ValidHandle(h)) return;
    auto& inst = m_instances[h.Index()];
    for (auto& p : inst.instanceParams)
    {
        if (p.name == name && p.type == MaterialParam::Type::Texture)
        { p.texture = tex; inst.cbDirty = true; return; }
    }
    MaterialParam np;
    np.name = name; np.type = MaterialParam::Type::Texture; np.texture = tex;
    inst.instanceParams.push_back(np);
    inst.cbDirty    = true;
    inst.layoutDirty = true;
}

void MaterialSystem::MarkDirty(MaterialHandle h)
{
    if (ValidHandle(h)) m_instances[h.Index()].cbDirty = true;
}

const std::vector<uint8_t>& MaterialSystem::GetCBData(MaterialHandle h)
{
    static std::vector<uint8_t> empty;
    if (!ValidHandle(h)) return empty;
    auto& inst = m_instances[h.Index()];
    const MaterialDesc& desc = m_descs[h.Index()].desc;

    if (inst.layoutDirty)
    {
        const auto& params = inst.instanceParams.empty() ? desc.params : inst.instanceParams;
        inst.cbLayout   = CbLayout::Build(params);
        inst.layoutDirty = false;
        inst.cbDirty    = true; // Layout hat sich geändert → CB neu bauen
    }

    if (inst.cbDirty)
    {
        BuildCBData(inst, desc);
        inst.cbDirty = false;
    }
    return inst.cbData;
}

const CbLayout& MaterialSystem::GetCBLayout(MaterialHandle h)
{
    static CbLayout empty;
    if (!ValidHandle(h)) return empty;
    // GetCBData stellt sicher dass Layout aktuell ist
    GetCBData(h);
    return m_instances[h.Index()].cbLayout;
}

float* MaterialInstance::GetFloatPtr(const std::string& name) noexcept
{
    const uint32_t offset = cbLayout.GetOffset(name);
    if (offset == UINT32_MAX || offset + 4u > cbData.size()) return nullptr;
    return reinterpret_cast<float*>(cbData.data() + offset);
}

void MaterialSystem::BuildCBData(MaterialInstance& inst, const MaterialDesc& desc)
{
    // Nutzt CbLayout für korrektes HLSL-Packing (Name→Offset-Map).
    // Texture/Sampler-Parameter werden nicht im CB gepackt.
    const CbLayout& layout = inst.cbLayout;
    inst.cbData.assign(layout.totalSize, 0u);

    const auto& params = inst.instanceParams.empty() ? desc.params : inst.instanceParams;
    for (const auto& p : params)
    {
        if (p.type == MaterialParam::Type::Texture ||
            p.type == MaterialParam::Type::Sampler) continue;

        const uint32_t offset = layout.GetOffset(p.name);
        if (offset == UINT32_MAX) continue; // nicht im Layout → überspringen

        // In-Place schreiben mit korrektem Offset
        float* dst = reinterpret_cast<float*>(inst.cbData.data() + offset);
        switch (p.type) {
        case MaterialParam::Type::Float:
            dst[0] = p.value.f[0];
            break;
        case MaterialParam::Type::Vec2:
            dst[0] = p.value.f[0]; dst[1] = p.value.f[1];
            break;
        case MaterialParam::Type::Vec3:
            dst[0] = p.value.f[0]; dst[1] = p.value.f[1]; dst[2] = p.value.f[2];
            break;
        case MaterialParam::Type::Vec4:
            dst[0] = p.value.f[0]; dst[1] = p.value.f[1];
            dst[2] = p.value.f[2]; dst[3] = p.value.f[3];
            break;
        case MaterialParam::Type::Int:
            std::memcpy(dst, &p.value.i, 4u);
            break;
        case MaterialParam::Type::Bool: {
            const uint32_t bval = p.value.b ? 1u : 0u;
            std::memcpy(dst, &bval, 4u);
            break;
        }
        default: break;
        }
    }
}

} // namespace engine::renderer
