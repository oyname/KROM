#include "renderer/MaterialFeatureEval.hpp"
#include <cstring>

namespace engine::renderer {

namespace {

constexpr size_t SemanticIndex(MaterialSemantic semantic) noexcept
{
    return static_cast<size_t>(semantic);
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

void MaterialFeatureEval::NormalizeDesc(MaterialDesc& desc) noexcept
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

MaterialSemanticValue MaterialFeatureEval::DefaultSemanticValue(MaterialSemantic semantic,
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

MaterialSemanticValue MaterialFeatureEval::ResolveSemanticValue(const MaterialDesc& desc,
                                                                const MaterialInstance& inst,
                                                                MaterialSemantic semantic) noexcept
{
    const MaterialSemanticValue& explicitValue = inst.semanticValues[SemanticIndex(semantic)];
    if (explicitValue.set)
        return explicitValue;
    return DefaultSemanticValue(semantic, desc.renderPolicy.alphaCutoff);
}

uint32_t MaterialFeatureEval::DeriveFeatureMask(const MaterialDesc& desc, const MaterialInstance& inst) noexcept
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
            flags |= texBit;
        else if (value.set)
            flags |= valueBit;
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

ShaderVariantFlag MaterialFeatureEval::BuildShaderVariantFlags(const MaterialDesc& desc,
                                                               const MaterialInstance& inst) noexcept
{
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

PipelineKey MaterialFeatureEval::BuildPipelineKey(const MaterialDesc& d,
                                                  const MaterialInstance& inst) noexcept
{
    (void)inst;

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
    pd.shaderContractHash = static_cast<uint64_t>(BuildShaderVariantFlags(d, inst));
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

} // namespace engine::renderer
