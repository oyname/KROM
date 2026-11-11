// =============================================================================
// KROM Engine — assets/shadow.opengl.vs.glsl
// Vertex Shader: Shadow-Pass Tiefenrendering (OpenGL / GLSL 4.10)
// Gibt nur gl_Position aus (depth-only).
// =============================================================================
#version 410 core

layout(location = 0) in vec3 aPosition;
#ifdef KROM_ALPHA_TEST
layout(location = 4) in vec2 aTexCoord;
out vec2 vTexCoord;
out vec3 vPositionWS;
#endif

#include "per_frame.opengl.glsl"

layout(std140) uniform PerObject
{
    mat4 worldMatrix;
    mat4 worldMatrixInvT;
    vec4 entityId;
};

#include "skinning.opengl.glsl"

void main()
{
    vec4 posWS = worldMatrix * (KromSkinMatrix() * vec4(aPosition, 1.0));
    gl_Position = viewProjMatrix * posWS;
#ifdef KROM_ALPHA_TEST
    vPositionWS = posWS.xyz;
    vTexCoord = aTexCoord;
#endif
}
