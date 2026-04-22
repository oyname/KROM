#version 410 core
#extension GL_ARB_shading_language_420pack : enable

layout(std140, binding = 0) uniform PerFrame
{
    mat4  viewMatrix;
    mat4  projMatrix;
    mat4  viewProjMatrix;
    mat4  invViewProjMatrix;
    vec4  cameraPositionWS;
    vec4  cameraForwardWS;
    vec4  screenSize;
    vec4  timeData;
    vec4  ambientColor;
    uint  lightCount;
    uint  shadowCascadeCount;
    float nearPlane;
    float farPlane;
    vec4  _lightsPadding[28];
    mat4  shadowViewProj;
    float iblPrefilterLevels;
    float shadowBias;
    float shadowNormalBias;
    float shadowStrength;
    float shadowTexelSize;
};

layout(binding = 8) uniform sampler2D uHDRInput;

in vec2 vTexCoord;
layout(location = 0) out vec4 fragColor;

void main()
{
    vec3 v = texture(uHDRInput, vTexCoord).rgb;
    v = v / (v + vec3(1.0));
    fragColor = vec4(v, 1.0);
}
