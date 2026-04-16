// =============================================================================
// KROM Engine — assets/triangle_color.vulkan.fs.glsl
// Fragment Shader: Dreieck mit Vertex-Farben (Vulkan / GLSL 4.50)
// =============================================================================
#version 450

layout(location = 0) in  vec4 vColor;
layout(location = 0) out vec4 fragColor;

void main()
{
    fragColor = vColor;
}
