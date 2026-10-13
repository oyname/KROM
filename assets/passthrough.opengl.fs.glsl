#version 410 core

uniform sampler2D uHDRInput;
uniform sampler2D uBloomTexture;

layout(std140) uniform PerPass
{
    vec4 bloomParams;
};

in vec2 vTexCoord;
layout(location = 0) out vec4 fragColor;

void main()
{
    vec2 uv = vec2(vTexCoord.x, 1.0 - vTexCoord.y);
    vec3 v = texture(uHDRInput, uv).rgb;
    v += texture(uBloomTexture, uv).rgb * bloomParams.y;
    v = v / (v + vec3(1.0));
    fragColor = vec4(v, 1.0);
}
