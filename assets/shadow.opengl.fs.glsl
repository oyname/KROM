// =============================================================================
// KROM Engine — assets/shadow.opengl.fs.glsl
// Fragment Shader: Shadow-Pass (OpenGL / GLSL 4.10)
// Kein Farboutput — nur Tiefenpuffer wird beschrieben.
// Mit KROM_ALPHA_TEST: Alpha-Discard für Cutout-Materialien (z.B. Blätter).
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
    float occlusionStrength;
    float opacityFactor;
    float alphaCutoff;
    int   materialFeatureMask;
    float materialModel;
    float _pad0;
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
