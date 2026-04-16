// =============================================================================
// KROM Engine — assets/triangle_color.opengl.fs.glsl
// Fragment Shader: Dreieck mit Vertex-Farben (OpenGL / GLSL 4.10)
// =============================================================================
#version 410 core

in  vec4 vColor;
layout(location = 0) out vec4 fragColor;

void main()
{
    fragColor = vColor;
}
