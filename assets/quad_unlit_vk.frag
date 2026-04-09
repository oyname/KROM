#version 450
layout(set = 0, binding = 16) uniform texture2D uAlbedoTex;
layout(set = 0, binding = 32) uniform sampler uLinearWrap;
layout(location = 0) in vec2 outUV;
layout(location = 0) out vec4 outColor;
void main()
{
    outColor = texture(sampler2D(uAlbedoTex, uLinearWrap), outUV);
}
