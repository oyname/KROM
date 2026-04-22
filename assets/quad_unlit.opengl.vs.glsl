// =============================================================================
// KROM Engine — assets/quad_unlit.opengl.vs.glsl
// Vertex Shader: texturiertes, unbelichtetes Quad (OpenGL / GLSL 4.10)
// =============================================================================
#version 410 core
#extension GL_ARB_shading_language_420pack : enable

layout(location = 0) in vec3 aPosition;
layout(location = 1) in vec3 aNormal;
layout(location = 4) in vec2 aTexCoord;

// ---------------------------------------------------------------------------
// UBO 0 – PerFrame
// Byte-Layout muss exakt mit FrameConstants (RenderWorld.hpp) übereinstimmen.
// ---------------------------------------------------------------------------
struct GpuLightData
{
    vec4 positionWS;
    vec4 directionWS;
    vec4 colorIntensity;
    vec4 params;
};

layout(std140, binding = 0) uniform PerFrame
{
    mat4         viewMatrix;
    mat4         projMatrix;
    mat4         viewProjMatrix;
    mat4         invViewProjMatrix;
    vec4         cameraPositionWS;
    vec4         cameraForwardWS;
    vec4         screenSize;
    vec4         timeParams;
    vec4         ambientColor;
    uint         lightCount;
    uint         shadowCascadeCount;
    float        nearPlane;
    float        farPlane;
    GpuLightData lights[7];
    mat4         shadowViewProj;
    float        iblPrefilterLevels;
    float        shadowBias;
    float        shadowNormalBias;
    float        shadowStrength;
};

// ---------------------------------------------------------------------------
// UBO 1 – PerObject
// ---------------------------------------------------------------------------
layout(std140, binding = 1) uniform PerObject
{
    mat4 worldMatrix;
    mat4 worldMatrixInvT;
    vec4 entityId;
};

// ---------------------------------------------------------------------------
// UBO 2 – PerMaterial (Deklaration für CB2-Kompatibilität des Bindings)
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

out vec3 vPositionWS;
out vec3 vNormalWS;
out vec2 vTexCoord;

void main()
{
    vec4 posWS  = worldMatrix * vec4(aPosition, 1.0);
    gl_Position = viewProjMatrix * posWS;
    vPositionWS = posWS.xyz;
    // Normale: mat3(worldMatrixInvT) entspricht (M^-1)^T in Column-major,
    // was für Normalen-Transformation korrekt ist.
    vNormalWS   = normalize(mat3(worldMatrixInvT) * aNormal);
    // OpenGL hat invertierte Y-Texturkoordinaten verglichen mit DX.
    vTexCoord   = vec2(aTexCoord.x, 1.0 - aTexCoord.y);
}
