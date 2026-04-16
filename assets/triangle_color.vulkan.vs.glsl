// =============================================================================
// KROM Engine — assets/triangle_color.vulkan.vs.glsl
// Vertex Shader: Dreieck mit Vertex-Farben (Vulkan / GLSL 4.50)
//
// Location-Mapping:
//   0 = Position (vec3)
//   6 = Color0   (vec4)  — entspricht VertexSemantic::Color0 = 6
// =============================================================================
#version 450

layout(location = 0) in vec3 aPosition;
layout(location = 6) in vec4 aColor;

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

layout(location = 0) out vec4 vColor;

void main()
{
    vec4 posWS  = perObject.worldMatrix * vec4(aPosition, 1.0);
    gl_Position = perFrame.viewProjMatrix * posWS;
    vColor      = aColor;
}
