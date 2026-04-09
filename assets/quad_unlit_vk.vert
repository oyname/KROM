#version 450

layout(set = 0, binding = 0, std140) uniform PerFrame
{
    mat4 viewMatrix;
    mat4 projMatrix;
    mat4 viewProjMatrix;
    mat4 invViewProjMatrix;
    vec4 cameraPositionWS;
    vec4 cameraForwardWS;
    vec4 screenSize;
    vec4 timeParams;
    vec4 ambientColor;
    uint lightCount;
    uint shadowEnabled;
    float nearPlane;
    float farPlane;
} perFrame;

layout(set = 0, binding = 1, std140) uniform PerObject
{
    mat4 worldMatrix;
    mat4 worldMatrixInvT;
    vec4 entityId;
} perObject;

layout(location = 0) in vec3 inPos;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec2 inUV;
layout(location = 0) out vec2 outUV;

void main()
{
    vec4 posWS = perObject.worldMatrix * vec4(inPos, 1.0);
    gl_Position = perFrame.viewProjMatrix * posWS;
    outUV = inUV;
}
