#version 450

struct GpuLightData
{
    vec4 positionWS;
    vec4 directionWS;
    vec4 colorIntensity;
    vec4 params;
};

layout(set = 0, binding = 0, std140) uniform PerFrame
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
    GpuLightData lights[8];
} perFrame;

layout(set = 0, binding = 1, std140) uniform PerObject
{
    mat4 worldMatrix;
    mat4 worldMatrixInvT;
    vec4 entityId;
} perObject;

layout(set = 0, binding = 2, std140) uniform PerMaterial
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
} perMaterial;

layout(location = 0) in vec3 inPos;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec4 inTangent;
layout(location = 4) in vec2 inUV;

layout(location = 0) out vec3 outPositionWS;
layout(location = 1) out vec3 outNormalWS;
layout(location = 2) out vec4 outTangentWS;
layout(location = 3) out vec2 outUV;

void main()
{
    vec4 posWS = perObject.worldMatrix * vec4(inPos, 1.0);
    vec3 N = normalize(mat3(perObject.worldMatrixInvT) * inNormal);
    vec3 T = normalize(mat3(perObject.worldMatrixInvT) * inTangent.xyz);
    T = normalize(T - N * dot(N, T));
    gl_Position = perFrame.viewProjMatrix * posWS;
    outPositionWS = posWS.xyz;
    outNormalWS = N;
    outTangentWS = vec4(T, inTangent.w);
    outUV = inUV;
}
