// =============================================================================
// KROM Engine — Testraum: door_panel.opengl.vs.glsl
// Vertex Shader der Schiebetuer (OpenGL / GLSL 4.10), Gegenstueck zu
// door_panel.vs.hlsl. Transformiert Position, Normal, UV.
//
// Die UV bleibt unveraendert (wie unter HLSL), damit die Warnstreifen dieselbe
// Lage haben; nur das Textur-Sampling im Fragment-Shader dreht V.
// =============================================================================
#version 410 core

layout(location = 0) in vec3 aPosition;
layout(location = 1) in vec3 aNormal;
layout(location = 4) in vec2 aTexCoord;

struct GpuLightData { vec4 positionWS; vec4 directionWS; vec4 colorIntensity; vec4 params; };

layout(std140) uniform PerFrame
{
    mat4         viewMatrix;
    mat4         projMatrix;
    mat4         viewProjMatrix;
    mat4         invViewProjMatrix;
    vec4         cameraPositionWS;
    vec4         cameraForwardWS;
    vec4         screenSize;
    vec4         timeParams;   // x=Zeit(s)  y=Delta  z=Frame  w=0
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

layout(std140) uniform PerObject
{
    mat4 worldMatrix;
    mat4 worldMatrixInvT;
    vec4 entityId;
};

out vec3 vPositionWS;
out vec3 vNormalWS;
out vec2 vTexCoord;

void main()
{
    vec4 posWS  = worldMatrix * vec4(aPosition, 1.0);
    gl_Position = viewProjMatrix * posWS;
    vPositionWS = posWS.xyz;
    vNormalWS   = normalize(mat3(worldMatrixInvT) * aNormal);
    vTexCoord   = aTexCoord;
}
