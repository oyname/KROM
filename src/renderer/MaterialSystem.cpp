#include "renderer/MaterialSystem.hpp"
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

CbLayout CbLayout::Build(const std::vector<MaterialParam>& params) noexcept
{
    CbLayout layout;
    uint32_t offset = 0u;

    for (const auto& p : params)
    {
        if (p.type == MaterialParam::Type::Texture || p.type == MaterialParam::Type::Sampler)
            continue;

        CbFieldDesc field{};
        field.name = p.name;
        field.offset = offset;
        field.arrayCount = 1u;
        field.type = p.type;

        uint32_t fieldSize = 16u;
        switch (p.type)
        {
        case MaterialParam::Type::Float: fieldSize = 4u; break;
        case MaterialParam::Type::Vec2:  fieldSize = 8u; break;
        // std140: vec3 has 16-byte alignment and occupies 16 bytes (4 bytes of padding appended).
        // BuildCBData writes only 12 bytes of data; the trailing 4 bytes remain zero-initialised.
        case MaterialParam::Type::Vec3:  fieldSize = 16u; break;
        case MaterialParam::Type::Vec4:  fieldSize = 16u; break;
        case MaterialParam::Type::Int:   fieldSize = 4u; break;
        case MaterialParam::Type::Bool:  fieldSize = 4u; break;
        default: break;
        }
        field.size = fieldSize;

        const uint32_t boundaryOffset = (offset / 16u + 1u) * 16u;
        if (fieldSize > 4u && (offset % 16u) + fieldSize > 16u)
        {
            offset = boundaryOffset;
            field.offset = offset;
        }

        layout.fields.push_back(field);
        offset += fieldSize;
    }

    layout.totalSize = (offset + 15u) & ~15u;
    if (layout.totalSize == 0u)
        layout.totalSize = 16u;
    return layout;
}

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

void MaterialSystem::NormalizeDesc(MaterialDesc& desc) const noexcept
{
    // Fix 5: Direct shortcut fields (doubleSided, castShadows) are merged ONE-WAY into
    // renderPolicy. After NormalizeDesc, renderPolicy is the single source of truth.
    // Code that reads desc.doubleSided/castShadows after registration reads stale shortcuts —
    // always use renderPolicy post-registration.
    desc.renderPolicy.doubleSided = desc.renderPolicy.doubleSided || desc.doubleSided;
    desc.renderPolicy.castShadows = desc.renderPolicy.castShadows && desc.castShadows;
    // Sync alphaCutoff back so the shortcut field stays readable (read-only after this point).
    desc.alphaCutoff = desc.renderPolicy.alphaCutoff;

    switch (desc.renderPolicy.blendMode)
    {
    case MaterialBlendMode::Opaque:
        desc.blend.blendEnable = false;
        break;
    case MaterialBlendMode::AlphaBlend:
        desc.blend.blendEnable = true;
        desc.blend.srcBlend = BlendFactor::SrcAlpha;
        desc.blend.dstBlend = BlendFactor::InvSrcAlpha;
        desc.blend.srcBlendAlpha = BlendFactor::One;
        desc.blend.dstBlendAlpha = BlendFactor::InvSrcAlpha;
        break;
    case MaterialBlendMode::Additive:
        desc.blend.blendEnable = true;
        desc.blend.srcBlend = BlendFactor::One;
        desc.blend.dstBlend = BlendFactor::One;
        desc.blend.srcBlendAlpha = BlendFactor::One;
        desc.blend.dstBlendAlpha = BlendFactor::One;
        break;
    }

    if (desc.renderPolicy.doubleSided)
        desc.rasterizer.cullMode = CullMode::None;

    // Legacy named-parameter migration into semantic authoring state.
    // Missing semantics stay missing here. Runtime defaults are resolved later and
    // must not be marked as explicit material data.
    for (const auto& param : desc.params)
    {
        if (param.type == MaterialParam::Type::Texture)
        {
            if (NameEqualsInsensitive(param.name, "albedo") || NameEqualsInsensitive(param.name, "basecolor") || NameEqualsInsensitive(param.name, "diffuse"))
            {
                auto& t = desc.semanticTextures[SemanticIndex(MaterialSemantic::BaseColor)];
                if (!t.set) { t.set = true; t.texture = param.texture; }
            }
            else if (NameEqualsInsensitive(param.name, "normal"))
            {
                auto& t = desc.semanticTextures[SemanticIndex(MaterialSemantic::Normal)];
                if (!t.set) { t.set = true; t.texture = param.texture; }
            }
            // Fix 3c: "orm" param name maps to the explicit ORM semantic (not Metallic).
            // Individual "metallic" and "roughness" params remain as single-channel semantic textures.
            else if (NameEqualsInsensitive(param.name, "orm"))
            {
                auto& t = desc.semanticTextures[SemanticIndex(MaterialSemantic::ORM)];
                if (!t.set) { t.set = true; t.texture = param.texture; }
            }
            else if (NameEqualsInsensitive(param.name, "metallic"))
            {
                auto& t = desc.semanticTextures[SemanticIndex(MaterialSemantic::Metallic)];
                if (!t.set) { t.set = true; t.texture = param.texture; }
            }
            else if (NameEqualsInsensitive(param.name, "roughness"))
            {
                auto& t = desc.semanticTextures[SemanticIndex(MaterialSemantic::Roughness)];
                if (!t.set) { t.set = true; t.texture = param.texture; }
            }
            else if (NameEqualsInsensitive(param.name, "occlusion") || NameEqualsInsensitive(param.name, "ao"))
            {
                auto& t = desc.semanticTextures[SemanticIndex(MaterialSemantic::Occlusion)];
                if (!t.set) { t.set = true; t.texture = param.texture; }
            }
            else if (NameEqualsInsensitive(param.name, "emissive"))
            {
                auto& t = desc.semanticTextures[SemanticIndex(MaterialSemantic::Emissive)];
                if (!t.set) { t.set = true; t.texture = param.texture; }
            }
            else if (NameEqualsInsensitive(param.name, "opacity"))
            {
                auto& t = desc.semanticTextures[SemanticIndex(MaterialSemantic::Opacity)];
                if (!t.set) { t.set = true; t.texture = param.texture; }
            }
        }
        else if (param.type == MaterialParam::Type::Float)
        {
            if (NameEqualsInsensitive(param.name, "metallic"))
            {
                auto& v = desc.semanticValues[SemanticIndex(MaterialSemantic::Metallic)];
                v.set = true; v.data[0] = param.value.f[0];
            }
            else if (NameEqualsInsensitive(param.name, "roughness"))
            {
                auto& v = desc.semanticValues[SemanticIndex(MaterialSemantic::Roughness)];
                v.set = true; v.data[0] = param.value.f[0];
            }
            else if (NameEqualsInsensitive(param.name, "occlusion") || NameEqualsInsensitive(param.name, "ao"))
            {
                auto& v = desc.semanticValues[SemanticIndex(MaterialSemantic::Occlusion)];
                v.set = true; v.data[0] = param.value.f[0];
            }
            else if (NameEqualsInsensitive(param.name, "opacity"))
            {
                auto& v = desc.semanticValues[SemanticIndex(MaterialSemantic::Opacity)];
                v.set = true; v.data[0] = param.value.f[0];
            }
            else if (NameEqualsInsensitive(param.name, "alphacutoff"))
            {
                auto& v = desc.semanticValues[SemanticIndex(MaterialSemantic::AlphaCutoff)];
                v.set = true; v.data[0] = param.value.f[0];
            }
        }
        else if (param.type == MaterialParam::Type::Vec4)
        {
            if (NameEqualsInsensitive(param.name, "albedo") || NameEqualsInsensitive(param.name, "basecolor"))
            {
                auto& v = desc.semanticValues[SemanticIndex(MaterialSemantic::BaseColor)];
                v.set = true;
                v.data = {param.value.f[0], param.value.f[1], param.value.f[2], param.value.f[3]};
            }
            else if (NameEqualsInsensitive(param.name, "emissive"))
            {
                auto& v = desc.semanticValues[SemanticIndex(MaterialSemantic::Emissive)];
                v.set = true;
                v.data = {param.value.f[0], param.value.f[1], param.value.f[2], param.value.f[3]};
            }
        }
    }
}

MaterialSemanticValue MaterialSystem::DefaultSemanticValue(MaterialSemantic semantic,
                                                        float alphaCutoff) noexcept
{
    MaterialSemanticValue value{};
    switch (semantic)
    {
    case MaterialSemantic::BaseColor:   value.data = {1.f, 1.f, 1.f, 1.f}; break;
    case MaterialSemantic::Normal:      value.data = {0.5f, 0.5f, 1.f, 1.f}; break;
    case MaterialSemantic::Metallic:    value.data = {0.f, 0.f, 0.f, 0.f}; break;
    case MaterialSemantic::Roughness:   value.data = {0.5f, 0.f, 0.f, 0.f}; break;
    case MaterialSemantic::Occlusion:   value.data = {1.f, 0.f, 0.f, 0.f}; break;
    case MaterialSemantic::Emissive:    value.data = {0.f, 0.f, 0.f, 0.f}; break;
    case MaterialSemantic::Opacity:     value.data = {1.f, 0.f, 0.f, 0.f}; break;
    case MaterialSemantic::AlphaCutoff: value.data = {alphaCutoff, 0.f, 0.f, 0.f}; break;
    // ORM has no meaningful scalar default; individual M/R/O defaults apply when no texture is bound.
    case MaterialSemantic::ORM:         value.data = {0.f, 0.f, 0.f, 0.f}; break;
    default: break;
    }
    return value;
}

MaterialSemanticValue MaterialSystem::ResolveSemanticValue(const MaterialDesc& desc,
                                                           const MaterialInstance& inst,
                                                           MaterialSemantic semantic) noexcept
{
    const MaterialSemanticValue& explicitValue = inst.semanticValues[SemanticIndex(semantic)];
    if (explicitValue.set)
        return explicitValue;
    return DefaultSemanticValue(semantic, desc.renderPolicy.alphaCutoff);
}

void MaterialSystem::InitializeInstanceFromDesc(MaterialInstance& inst, const MaterialDesc& desc) const noexcept
{
    inst.instanceParams = desc.params;
    inst.semanticValues = desc.semanticValues;
    inst.semanticTextures = desc.semanticTextures;
    inst.featureMask = DeriveFeatureMask(desc, inst);
    inst.cbDirty = true;
    inst.layoutDirty = true;
}

uint32_t MaterialSystem::DeriveFeatureMask(const MaterialDesc& desc, const MaterialInstance& inst) const noexcept
{
    MaterialFeatureFlag flags = MaterialFeatureFlag::None;
    if (desc.model == MaterialModel::Unlit)
        flags |= MaterialFeatureFlag::Unlit;
    else
        flags |= MaterialFeatureFlag::PBRMetalRough;

    if (desc.renderPolicy.alphaTest)
        flags |= MaterialFeatureFlag::AlphaTest;
    if (desc.renderPolicy.doubleSided)
        flags |= MaterialFeatureFlag::DoubleSided;
    if (desc.renderPolicy.castShadows)
        flags |= MaterialFeatureFlag::CastShadows;
    if (desc.renderPolicy.receiveShadows)
        flags |= MaterialFeatureFlag::ReceiveShadows;

    // Fix 6: Texture takes priority over Value. When a texture is present, the value bit is not
    // emitted — the shader samples the texture and ignores the CB value for that semantic.
    // This rule is enforced here so the shader never needs to handle the ambiguous "both set" case.
    const auto addTextureValueFlags = [&](MaterialSemantic semantic,
                                          MaterialFeatureFlag valueBit,
                                          MaterialFeatureFlag texBit)
    {
        const auto& value = inst.semanticValues[SemanticIndex(semantic)];
        const auto& tex = inst.semanticTextures[SemanticIndex(semantic)];
        const bool hasTexture = tex.set && tex.texture.IsValid();
        if (hasTexture)
            flags |= texBit;              // texture present → only texture bit
        else if (value.set)
            flags |= valueBit;            // no texture, explicit value → value bit only
    };

    // Fix 3d: Explicit ORM semantic has priority over individual M/R/O texture semantics.
    // If ORM is set, emit ORMTexture and skip MetallicTexture/RoughnessTexture/OcclusionTexture.
    const auto& ormTex = inst.semanticTextures[SemanticIndex(MaterialSemantic::ORM)];
    const bool hasExplicitORM = ormTex.set && ormTex.texture.IsValid();

    if (hasExplicitORM)
    {
        flags |= MaterialFeatureFlag::ORMTexture;
        // Individual M/R/O value flags still apply (scalar fallbacks in CB, not replaced by texture).
        addTextureValueFlags(MaterialSemantic::Metallic,   MaterialFeatureFlag::MetallicValue,   MaterialFeatureFlag::MetallicTexture);
        addTextureValueFlags(MaterialSemantic::Roughness,  MaterialFeatureFlag::RoughnessValue,  MaterialFeatureFlag::RoughnessTexture);
        addTextureValueFlags(MaterialSemantic::Occlusion,  MaterialFeatureFlag::OcclusionValue,  MaterialFeatureFlag::OcclusionTexture);
        // When explicit ORM is set, any individual M/R/O *texture* flags are suppressed —
        // the ORM channel is already the carrier. Value flags remain valid.
        flags = static_cast<MaterialFeatureFlag>(
            static_cast<uint32_t>(flags) &
            ~(static_cast<uint32_t>(MaterialFeatureFlag::MetallicTexture) |
              static_cast<uint32_t>(MaterialFeatureFlag::RoughnessTexture) |
              static_cast<uint32_t>(MaterialFeatureFlag::OcclusionTexture)));
    }
    else
    {
        addTextureValueFlags(MaterialSemantic::Metallic,   MaterialFeatureFlag::MetallicValue,   MaterialFeatureFlag::MetallicTexture);
        addTextureValueFlags(MaterialSemantic::Roughness,  MaterialFeatureFlag::RoughnessValue,  MaterialFeatureFlag::RoughnessTexture);
        addTextureValueFlags(MaterialSemantic::Occlusion,  MaterialFeatureFlag::OcclusionValue,  MaterialFeatureFlag::OcclusionTexture);
    }

    addTextureValueFlags(MaterialSemantic::BaseColor, MaterialFeatureFlag::BaseColorValue, MaterialFeatureFlag::BaseColorTexture);
    addTextureValueFlags(MaterialSemantic::Emissive,  MaterialFeatureFlag::EmissiveValue,  MaterialFeatureFlag::EmissiveTexture);
    addTextureValueFlags(MaterialSemantic::Opacity,   MaterialFeatureFlag::OpacityValue,   MaterialFeatureFlag::OpacityTexture);

    const auto& normalTex = inst.semanticTextures[SemanticIndex(MaterialSemantic::Normal)];
    if (normalTex.set && normalTex.texture.IsValid())
        flags |= MaterialFeatureFlag::NormalTexture;

    return static_cast<uint32_t>(flags);
}

std::vector<MaterialParam> MaterialSystem::BuildCanonicalParams(const MaterialDesc& desc, const MaterialInstance& inst) const
{
    std::vector<MaterialParam> params = inst.instanceParams;

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

    const auto baseColor = ResolveSemanticValue(desc, inst, MaterialSemantic::BaseColor);
    const auto emissive = ResolveSemanticValue(desc, inst, MaterialSemantic::Emissive);
    const auto metallic = ResolveSemanticValue(desc, inst, MaterialSemantic::Metallic);
    const auto roughness = ResolveSemanticValue(desc, inst, MaterialSemantic::Roughness);
    const auto occlusion = ResolveSemanticValue(desc, inst, MaterialSemantic::Occlusion);
    const auto opacity = ResolveSemanticValue(desc, inst, MaterialSemantic::Opacity);
    const auto alphaCutoff = ResolveSemanticValue(desc, inst, MaterialSemantic::AlphaCutoff);
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
    pd.rasterizer = d.rasterizer;
    pd.depthStencil = d.depthStencil;
    pd.blendStates[0] = d.blend;
    pd.topology = d.topology;
    pd.vertexLayout = d.vertexLayout;
    pd.colorFormat = isShadowPass ? Format::Unknown : d.colorFormat;
    pd.depthFormat = d.depthFormat;
    pd.shaderContractHash = static_cast<uint64_t>(BuildShaderVariantFlags(h));
    // Fix 4: pipelineLayoutHash encodes what descriptor layout this material requires.
    // The engine binding layout is fixed (CBSlots, TexSlots, SamplerSlots constants), so we
    // combine a stable engine magic constant with pass and model info. This ensures materials
    // on different passes or with different models produce distinct PipelineKey hashes even
    // when their shader variant flags happen to be identical.
    pd.pipelineLayoutHash = pd.shaderContractHash
                          ^ (static_cast<uint64_t>(d.passTag) << 32u)
                          ^ (static_cast<uint64_t>(static_cast<uint8_t>(d.model)) << 40u)
                          ^ 0x4B524F4D45474E45ull; // "KROMEGNE" sentinel

    return PipelineKey::From(pd, d.passTag);
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
    inst.featureMask = DeriveFeatureMask(m_descs[h.Index()].desc, inst);
    MarkDirty(h);
}

void MaterialSystem::SetSemanticVec4(MaterialHandle h, MaterialSemantic semantic, const math::Vec4& v)
{
    if (!ValidHandle(h)) return;
    auto& inst = m_instances[h.Index()];
    auto& value = inst.semanticValues[SemanticIndex(semantic)];
    value.set = true;
    value.data = {v.x, v.y, v.z, v.w};
    inst.featureMask = DeriveFeatureMask(m_descs[h.Index()].desc, inst);
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
    inst.featureMask = DeriveFeatureMask(m_descs[h.Index()].desc, inst);
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
    inst.featureMask = DeriveFeatureMask(m_descs[h.Index()].desc, inst);
    MarkDirty(h);
}

MaterialFeatureFlag MaterialSystem::GetFeatureFlags(MaterialHandle h) const noexcept
{
    if (!ValidHandle(h)) return MaterialFeatureFlag::None;
    const auto& inst = m_instances[h.Index()];
    return static_cast<MaterialFeatureFlag>(inst.featureMask);
}

ShaderVariantFlag MaterialSystem::BuildShaderVariantFlags(MaterialHandle h) const noexcept
{
    if (!ValidHandle(h)) return ShaderVariantFlag::None;
    const MaterialDesc& desc = m_descs[h.Index()].desc;
    const MaterialInstance& inst = m_instances[h.Index()];

    ShaderVariantFlag flags = ShaderVariantFlag::None;
    if (desc.model == MaterialModel::Unlit)
        flags = flags | ShaderVariantFlag::Unlit;
    else
        flags = flags | ShaderVariantFlag::PBRMetalRough;
    if (desc.renderPolicy.alphaTest)
        flags = flags | ShaderVariantFlag::AlphaTest;
    if (desc.renderPolicy.doubleSided)
        flags = flags | ShaderVariantFlag::DoubleSided;

    const auto testTex = [&](MaterialSemantic s) {
        const auto& t = inst.semanticTextures[SemanticIndex(s)];
        return t.set && t.texture.IsValid();
    };

    if (testTex(MaterialSemantic::BaseColor)) flags = flags | ShaderVariantFlag::BaseColorMap;
    if (testTex(MaterialSemantic::Normal))    flags = flags | ShaderVariantFlag::NormalMap;
    if (testTex(MaterialSemantic::Emissive))  flags = flags | ShaderVariantFlag::EmissiveMap;
    if (testTex(MaterialSemantic::Opacity))   flags = flags | ShaderVariantFlag::OpacityMap;

    // Fix 3e: ORM texture path — explicit ORM semantic or any individual M/R/O texture
    // both produce a single ORMMap variant flag. The shader sees one flag, one slot (t2).
    // This avoids combinatorial MetallicMap×RoughnessMap×OcclusionMap permutations.
    const bool hasORM = testTex(MaterialSemantic::ORM)
                     || testTex(MaterialSemantic::Metallic)
                     || testTex(MaterialSemantic::Roughness)
                     || testTex(MaterialSemantic::Occlusion);
    if (hasORM)
        flags = flags | ShaderVariantFlag::ORMMap;

    return flags;
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
    return ResolveSemanticValue(m_descs[h.Index()].desc, m_instances[h.Index()], semantic);
}

const std::vector<uint8_t>& MaterialSystem::GetCBData(MaterialHandle h)
{
    static std::vector<uint8_t> empty;
    if (!ValidHandle(h)) return empty;
    auto& inst = m_instances[h.Index()];
    const MaterialDesc& desc = m_descs[h.Index()].desc;

    if (inst.layoutDirty)
    {
        inst.featureMask = DeriveFeatureMask(desc, inst);
        inst.cbLayout = CbLayout::Build(BuildCanonicalParams(desc, inst));
        inst.layoutDirty = false;
        inst.cbDirty = true;
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
    GetCBData(h);
    return m_instances[h.Index()].cbLayout;
}

void MaterialSystem::BuildCBData(MaterialInstance& inst, const MaterialDesc& desc)
{
    const std::vector<MaterialParam> params = BuildCanonicalParams(desc, inst);
    const CbLayout& layout = inst.cbLayout;
    inst.cbData.assign(layout.totalSize, 0u);

    for (const auto& p : params)
    {
        if (p.type == MaterialParam::Type::Texture || p.type == MaterialParam::Type::Sampler)
            continue;

        const uint32_t offset = layout.GetOffset(p.name);
        if (offset == UINT32_MAX)
            continue;

        float* dst = reinterpret_cast<float*>(inst.cbData.data() + offset);
        switch (p.type)
        {
        case MaterialParam::Type::Float: dst[0] = p.value.f[0]; break;
        case MaterialParam::Type::Vec2:  dst[0] = p.value.f[0]; dst[1] = p.value.f[1]; break;
        case MaterialParam::Type::Vec3:  dst[0] = p.value.f[0]; dst[1] = p.value.f[1]; dst[2] = p.value.f[2]; break;
        case MaterialParam::Type::Vec4:  dst[0] = p.value.f[0]; dst[1] = p.value.f[1]; dst[2] = p.value.f[2]; dst[3] = p.value.f[3]; break;
        case MaterialParam::Type::Int:   std::memcpy(dst, &p.value.i, 4u); break;
        case MaterialParam::Type::Bool:  { const uint32_t b = p.value.b ? 1u : 0u; std::memcpy(dst, &b, 4u); break; }
        default: break;
        }
    }
}

} // namespace engine::renderer
