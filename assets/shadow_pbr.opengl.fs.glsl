// =============================================================================
// KROM Engine — assets/shadow_pbr.opengl.fs.glsl
// Fragment Shader: Shadow-Pass für PBR-Materialien (OpenGL / GLSL 4.10)
// Verwirft Fragmente mit Opacity < alphaCutoff (Alpha-Test / Cutout).
// =============================================================================
#version 410 core

#ifdef KROM_ALPHA_TEST
uniform sampler2D albedo;

layout(std140) uniform PerMaterial
{
    vec4  baseColorFactor;
    vec4  emissiveFactor;
    float metallicFactor;
    float roughnessFactor;
    float normalStrength;
    float occlusionStrength;
    float opacityFactor;
    float alphaCutoff;
    int   materialFeatureMask;
    float materialModel;
    int   occlusionChannel;
    int   roughnessChannel;
    int   metallicChannel;
    float occlusionBias;
    float roughnessBias;
    float metallicBias;
    vec2  uvScale;
    vec2  uvOffset;
};

in vec2 vTexCoord;
#endif

void main()
{
#ifdef KROM_ALPHA_TEST
    float a = texture(albedo, vTexCoord).a * baseColorFactor.a * opacityFactor;
    if (a < alphaCutoff) discard;
#endif
}
