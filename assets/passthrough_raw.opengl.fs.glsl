#version 410 core
#extension GL_ARB_shading_language_420pack : enable

layout(binding = 8) uniform sampler2D uHDRInput;

in vec2 vTexCoord;
layout(location = 0) out vec4 fragColor;

void main()
{
    vec3 v = texture(uHDRInput, vTexCoord).rgb;
    fragColor = vec4(clamp(v, vec3(0.0), vec3(1.0)), 1.0);
}
