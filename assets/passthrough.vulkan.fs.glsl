#version 450
layout(set = 0, binding = 24) uniform texture2D uHDRInputTex;
layout(set = 0, binding = 33) uniform sampler uLinearClamp;
layout(location = 0) in vec2 outUV;
layout(location = 0) out vec4 outColor;
void main()
{
    vec3 hdr = texture(sampler2D(uHDRInputTex, uLinearClamp), outUV).rgb;
    hdr = hdr / (hdr + vec3(1.0));
    outColor = vec4(hdr, 1.0);
}
