// =============================================================================
// KROM Engine — assets/quad_unlit.vulkan.fs.glsl
// Fragment Shader: unbelichtetes Quad (Vulkan / GLSL 4.50 / SPIR-V)
//
// Binding-Modell (BindingRegisterRanges):
//   set=0, binding= 2  → PerMaterial (CB2)
//   set=0, binding=16  → tAlbedo     (TexSlots::Albedo,   SRVBase+0)
//   set=0, binding=32  → sLinearWrap (SamplerSlots::LinearWrap, SamplerBase+0)
// =============================================================================
#version 450

// ---------------------------------------------------------------------------
// UBO 2 – PerMaterial
// ---------------------------------------------------------------------------
layout(set = 0, binding = 2, std140) uniform PerMaterial
{
    vec4  baseColorFactor;
    vec4  emissiveFactor;
    float metallicFactor;
    float roughnessFactor;
    float occlusionStrength;
    float opacityFactor;
    float alphaCutoff;
    int   materialFeatureMask;
    float materialModel;
    float _pad0;
} perMaterial;

// ---------------------------------------------------------------------------
// Texturen & Sampler
// ---------------------------------------------------------------------------
layout(set = 0, binding = 16) uniform texture2D tAlbedo;
layout(set = 0, binding = 19) uniform texture2D emissive;
layout(set = 0, binding = 32) uniform sampler   sLinearWrap;

layout(location = 0) in  vec3 outPositionWS;
layout(location = 1) in  vec3 outNormalWS;
layout(location = 2) in  vec2 outUV;

layout(location = 0) out vec4 fragColor;

void main()
{
    // MaterialFeatureFlag::BaseColorTexture = 1u << 1 = 2
    // tAlbedo ist immer valide gebunden (ShaderRuntime: Fallback White-Textur).
#ifdef KROM_BASECOLOR_MAP
    vec4 albedo = texture(sampler2D(tAlbedo, sLinearWrap), outUV)
                * perMaterial.baseColorFactor;
#else
    vec4 albedo;
    if ((perMaterial.materialFeatureMask & 2) != 0)
        albedo = texture(sampler2D(tAlbedo, sLinearWrap), outUV)
               * perMaterial.baseColorFactor;
    else
        albedo = perMaterial.baseColorFactor;
#endif

#ifdef KROM_ALPHA_TEST
    if (albedo.a < perMaterial.alphaCutoff) discard;
#endif

    albedo.a *= perMaterial.opacityFactor;

    vec3 emissiveColor = perMaterial.emissiveFactor.rgb;
#ifdef KROM_EMISSIVE_MAP
    emissiveColor *= texture(sampler2D(emissive, sLinearWrap), outUV).rgb;
#endif

    fragColor  = vec4(albedo.rgb + emissiveColor, albedo.a);
}
