// =============================================================================
// KROM Engine — assets/quad_unlit.frag
// Fragment Shader: texturiertes, unbelichtetes Quad (OpenGL / GLSL 4.10)
//
// Binding-Modell (ShaderBindingModel.hpp):
//   binding 0 = Albedo / BaseColor (TexSlots::Albedo = 0)
// =============================================================================
#version 410 core
#extension GL_ARB_shading_language_420pack : enable

layout(binding = 0) uniform sampler2D uAlbedo;

in  vec2 vTexCoord;
layout(location = 0) out vec4 fragColor;

void main()
{
    fragColor = texture(uAlbedo, vTexCoord);
}
