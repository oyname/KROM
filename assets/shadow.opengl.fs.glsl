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
    float normalStrength;
    float occlusionStrength;
    float opacityFactor;
    float alphaCutoff;
    int   materialFeatureMask;
    float materialModel;
#ifdef KROM_MATERIAL_GRAPH
    vec4 graphOutputs;
    vec4 graphNodes[16];
    vec4 graphUV[8];
#endif
};

in vec2 vTexCoord;

#define KROM_GRAPH_DECLARE_TIME
#include "pbr_material_graph.opengl.glsl"
#endif

void main()
{
#ifdef KROM_ALPHA_TEST
    vec4 color = texture(albedo, vTexCoord) * baseColorFactor;
#ifdef KROM_MATERIAL_GRAPH
    vec3 graphEmission;
    EvaluateMaterialGraph(vTexCoord, color, graphEmission);
#endif
    float a = color.a * opacityFactor;
    if (a < alphaCutoff) discard;
#endif
}
