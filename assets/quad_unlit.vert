// =============================================================================
// KROM Engine — assets/quad_unlit.vert
// Vertex Shader: texturiertes, unbelichtetes Quad (OpenGL / GLSL 4.10)
//
// Attribute-Locations aus VertexSemantic enum (OpenGLCommandList.cpp):
//   location 0 = VertexSemantic::Position
//   location 4 = VertexSemantic::TexCoord0
//
// Binding-Modell (ShaderBindingModel.hpp):
//   binding 0 (UBO) = PerFrame
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

out vec2 vTexCoord;

void main()
{
    gl_Position = viewProjMatrix * vec4(aPosition, 1.0);
    // Bitmap-Upload/UV-Konvention ist im OpenGL-Pfad vertikal invertiert.
    vTexCoord = vec2(aTexCoord.x, 1.0 - aTexCoord.y);
}
