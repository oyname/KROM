// =============================================================================
// KROM Engine — assets/quad_unlit.vert
// Vertex Shader: texturiertes, unbelichtetes Quad (OpenGL / GLSL 4.10)
//
// Attribute-Locations:
//   location 0 = Position
//   location 1 = Normal
//   location 4 = TexCoord0
//
// Binding-Modell (ShaderBindingModel.hpp):
//   binding 0 (UBO) = PerFrame
//   binding 1 (UBO) = PerObject
// =============================================================================
#version 410 core
#extension GL_ARB_shading_language_420pack : enable

layout(location = 0) in vec3 aPosition;
layout(location = 1) in vec3 aNormal;
layout(location = 4) in vec2 aTexCoord;

layout(std140, binding = 0) uniform PerFrame
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
};

layout(std140, binding = 1) uniform PerObject
{
    mat4 worldMatrix;
    mat4 worldMatrixInvT;
    vec4 entityId;
};

out vec2 vTexCoord;

void main()
{
    vec4 posWS  = worldMatrix * vec4(aPosition, 1.0);
    gl_Position = viewProjMatrix * posWS;
    vTexCoord   = vec2(aTexCoord.x, 1.0 - aTexCoord.y);
}
