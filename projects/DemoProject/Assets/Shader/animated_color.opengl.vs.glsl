// =============================================================================
// KROM Engine — assets/examples/animated_color.opengl.vs.glsl
// Vertex Shader: Animierter Farb-Effekt (OpenGL / GLSL 4.10)
// =============================================================================
#version 410 core

layout(location = 0) in vec3 aPosition;
layout(location = 1) in vec3 aNormal;
layout(location = 4) in vec2 aTexCoord;

struct GpuLightData { vec4 p; vec4 d; vec4 c; vec4 x; };

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

out vec3 vPositionWS;
out vec3 vNormalWS;
out vec2 vTexCoord;

void main()
{
    vec4 posWS  = worldMatrix * vec4(aPosition, 1.0);
    gl_Position = viewProjMatrix * posWS;
    vPositionWS = posWS.xyz;
    vNormalWS   = normalize(mat3(worldMatrixInvT) * aNormal);
    vTexCoord   = vec2(aTexCoord.x, 1.0 - aTexCoord.y);
}
