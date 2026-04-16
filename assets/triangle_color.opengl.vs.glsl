// =============================================================================
// KROM Engine — assets/triangle_color.opengl.vs.glsl
// Vertex Shader: Dreieck mit Vertex-Farben (OpenGL / GLSL 4.10)
//
// Location-Mapping entspricht VertexSemantic-Enum:
//   0 = Position, 6 = Color0
// =============================================================================
#version 410 core
#extension GL_ARB_shading_language_420pack : enable

layout(location = 0) in vec3 aPosition;
layout(location = 6) in vec4 aColor;

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

out vec4 vColor;

void main()
{
    vec4 posWS  = worldMatrix * vec4(aPosition, 1.0);
    gl_Position = viewProjMatrix * posWS;
    vColor      = aColor;
}
