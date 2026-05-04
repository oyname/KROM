// =============================================================================
// KROM Engine — assets/shadow.opengl.vs.glsl
// Vertex Shader: Shadow-Pass Tiefenrendering (OpenGL / GLSL 4.10)
// Gibt nur gl_Position aus (depth-only).
// =============================================================================
#version 410 core
#extension GL_ARB_shading_language_420pack : enable

layout(location = 0) in vec3 aPosition;
#ifdef KROM_ALPHA_TEST
layout(location = 4) in vec2 aTexCoord;
out vec2 vTexCoord;
#endif

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
    GpuLightData lights[7];
    mat4         shadowViewProj;
    float        iblPrefilterLevels;
    float        shadowBias;
    float        shadowNormalBias;
    float        shadowStrength;
    float        shadowTexelSize;
    uint         debugFlags;
    vec2         _shadowPad;
    vec4         shadowLightMeta[4];
    vec4         shadowLightExtra[4];
    vec4         shadowViewRect[16];
    mat4         shadowViewProjArray[16];
    uint         shadowLightCount;
    uint         shadowViewCount;
    vec2         _shadowArrayPad;
};

layout(std140, binding = 1) uniform PerObject
{
    mat4 worldMatrix;
    mat4 worldMatrixInvT;
    vec4 entityId;
};

void main()
{
    gl_Position = shadowViewProj * worldMatrix * vec4(aPosition, 1.0);
#ifdef KROM_ALPHA_TEST
    vTexCoord = vec2(aTexCoord.x, 1.0 - aTexCoord.y);
#endif
}
