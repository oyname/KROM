// =============================================================================
// KROM Engine — assets/quad_unlit.opengl.vs.glsl
// Vertex Shader: texturiertes, unbelichtetes Quad (OpenGL / GLSL 4.10)
// =============================================================================
#version 410 core

layout(location = 0) in vec3 aPosition;
layout(location = 1) in vec3 aNormal;
layout(location = 4) in vec2 aTexCoord;

// ---------------------------------------------------------------------------
// UBO 0 – PerFrame
// Byte-Layout muss exakt mit FrameConstants (RenderWorld.hpp) übereinstimmen.
// ---------------------------------------------------------------------------
#include "per_frame.opengl.glsl"

// ---------------------------------------------------------------------------
// UBO 1 – PerObject
// ---------------------------------------------------------------------------
layout(std140) uniform PerObject
{
    mat4 worldMatrix;
    mat4 worldMatrixInvT;
    vec4 entityId;
};

// ---------------------------------------------------------------------------
// UBO 2 – PerMaterial (Deklaration für CB2-Kompatibilität des Bindings)
// ---------------------------------------------------------------------------
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
#ifdef KROM_MATERIAL_GRAPH
    vec4 graphOutputs;
    vec4 graphNodes[16];
    vec4 graphUV[8];
#endif
};

out vec3 vPositionWS;
out vec3 vNormalWS;
out vec2 vTexCoord;

#include "skinning.opengl.glsl"

void main()
{
    mat4 skin = KromSkinMatrix();
    vec4 posWS  = worldMatrix * (skin * vec4(aPosition, 1.0));
    gl_Position = viewProjMatrix * posWS;
    vPositionWS = posWS.xyz;
    // Normale: mat3(worldMatrixInvT) entspricht (M^-1)^T in Column-major,
    // was für Normalen-Transformation korrekt ist.
    vNormalWS   = normalize(mat3(worldMatrixInvT) * (mat3(skin) * aNormal));
    // OpenGL hat invertierte Y-Texturkoordinaten verglichen mit DX.
    vTexCoord   = vec2(aTexCoord.x, 1.0 - aTexCoord.y);
}
