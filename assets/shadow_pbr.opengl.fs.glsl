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
    float  boxTileSize;
#ifdef KROM_MATERIAL_GRAPH
    vec4 graphOutputs;
    vec4 graphNodes[16];
    vec4 graphUV[8];
#endif
};

in vec2 vTexCoord;
in vec3 vPositionWS;
in vec2 vMeshUV;
in vec3 vScaledLocalPosition;

#include "pbr_box_projection.opengl.glsl"
#define KROM_GRAPH_DECLARE_TIME
#include "pbr_material_graph.opengl.glsl"
#endif

void main()
{
#ifdef KROM_ALPHA_TEST
    vec2 uv = KromPbrUV(vTexCoord, vScaledLocalPosition, boxTileSize);
#ifdef KROM_MATERIAL_GRAPH
    EvaluateMaterialGraph(boxTileSize > 0.0 ? uv : vMeshUV, vPositionWS);
    if (boxTileSize <= 0.0)
        uv = KromGraphDirectUV(uv, vMeshUV, uvScale, uvOffset);
#endif
    vec4 color = texture(albedo, uv) * baseColorFactor;
#ifdef KROM_MATERIAL_GRAPH
    vec3 graphEmission;
    ApplyMaterialGraph(color, graphEmission);
#endif
    float a = color.a * opacityFactor;
    if (a < alphaCutoff) discard;
#endif
}
