// =============================================================================
// KROM Engine — assets/quad_unlit.opengl.fs.glsl
// Fragment Shader: texturiertes, unbelichtetes Quad (OpenGL / GLSL 4.10)
// =============================================================================
#version 410 core
#extension GL_ARB_shading_language_420pack : enable

// ---------------------------------------------------------------------------
// UBO 2 – PerMaterial
// ---------------------------------------------------------------------------
layout(std140, binding = 2) uniform PerMaterial
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
};

// ---------------------------------------------------------------------------
// Texturen & Sampler (binding = TexSlots::Albedo = 0)
// ---------------------------------------------------------------------------
layout(binding = 0) uniform sampler2D uAlbedo;

in  vec3 vPositionWS;
in  vec3 vNormalWS;
in  vec2 vTexCoord;

layout(location = 0) out vec4 fragColor;

void main()
{
    // MaterialFeatureFlag::BaseColorTexture = 1u << 1 = 2
    // uAlbedo ist immer valide gebunden (ShaderRuntime: Fallback White-Textur).
#ifdef KROM_BASECOLOR_MAP
    vec4 albedo = texture(uAlbedo, vTexCoord) * baseColorFactor;
#else
    vec4 albedo;
    if ((materialFeatureMask & 2) != 0)
        albedo = texture(uAlbedo, vTexCoord) * baseColorFactor;
    else
        albedo = baseColorFactor;
#endif

#ifdef KROM_ALPHA_TEST
    if (albedo.a < alphaCutoff) discard;
#endif

    albedo.a *= opacityFactor;
    fragColor = albedo;
}
