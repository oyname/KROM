// =============================================================================
// KROM Engine — assets/shadow_pbr.opengl.vs.glsl
// Vertex Shader: Shadow-Pass für PBR-Materialien (OpenGL / GLSL 4.10)
// Transformiert UV mit uvScale/uvOffset aus dem PBR-PerMaterial-Block.
// =============================================================================
#version 410 core

layout(location = 0) in vec3 aPosition;
#ifdef KROM_ALPHA_TEST
layout(location = 4) in vec2 aTexCoord;
out vec2 vTexCoord;
out vec2 vMeshUV;
out vec3 vPositionWS;
out vec3 vScaledLocalPosition;
#endif

#include "per_frame.opengl.glsl"

layout(std140) uniform PerObject
{
    mat4 worldMatrix;
    mat4 worldMatrixInvT;
    vec4 entityId;
};

#ifdef KROM_ALPHA_TEST
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
#endif

#include "skinning.opengl.glsl"

void main()
{
    vec4 posWS = worldMatrix * (KromSkinMatrix() * vec4(aPosition, 1.0));
    gl_Position = viewProjMatrix * posWS;
#ifdef KROM_ALPHA_TEST
    vPositionWS = posWS.xyz;
    vec2 uv = aTexCoord;
    vMeshUV = uv;
    vTexCoord = uv * uvScale + uvOffset;
    vScaledLocalPosition = boxTileSize > 0.0
        ? aPosition * vec3(length(worldMatrix[0].xyz), length(worldMatrix[1].xyz), length(worldMatrix[2].xyz))
        : vec3(0.0);
#endif
}
