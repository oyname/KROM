#version 410 core
#extension GL_ARB_shading_language_420pack : enable

layout(location = 0) in vec3 aPosition;
layout(location = 1) in vec3 aNormal;
layout(location = 4) in vec2 aTexCoord;

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
    vec4         timeData;
    vec4         ambientColor;
    uint         lightCount;
    uint         shadowCascadeCount;
    float        nearPlane;
    float        farPlane;
    GpuLightData lights[8];
    float        iblPrefilterLevels;
    float        _padFC0;
    float        _padFC1;
    float        _padFC2;
};

layout(std140, binding = 1) uniform PerObject
{
    mat4 worldMatrix;
    mat4 worldMatrixInvT;
    vec4 entityId;
};

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
    vNormalWS   = normalize(mat3(worldMatrixInvT) * aNormal);
    vTexCoord   = vec2(aTexCoord.x, 1.0 - aTexCoord.y);
}
