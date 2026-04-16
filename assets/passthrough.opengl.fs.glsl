#version 410 core
#extension GL_ARB_shading_language_420pack : enable

layout(binding = 8) uniform sampler2D uHDRInput;

in vec2 vTexCoord;
layout(location = 0) out vec4 fragColor;

void main()
{
    vec3 hdr = texture(uHDRInput, vTexCoord).rgb;
    hdr = hdr / (hdr + vec3(1.0));
    fragColor = vec4(hdr, 1.0);
}
