#version 410 core

uniform sampler2D uHDRInput;

in vec2 vTexCoord;
layout(location = 0) out vec4 fragColor;

void main()
{
    vec2 uv = vec2(vTexCoord.x, 1.0 - vTexCoord.y);
    vec3 v = texture(uHDRInput, uv).rgb;
    fragColor = vec4(clamp(v, vec3(0.0), vec3(1.0)), 1.0);
}
