// =============================================================================
// KROM Engine — assets/passthrough.frag
// Passthrough-Tonemap: sampelt uHDRInput (Slot 0).
// RenderSystem bindet hdrSceneColor vor dem Draw an Slot 0.
// =============================================================================
#version 410 core
#extension GL_ARB_shading_language_420pack : enable

layout(binding = 0) uniform sampler2D uHDRInput;

in  vec2 vUV;
layout(location = 0) out vec4 fragColor;

void main()
{
    vec3 hdr = texture(uHDRInput, vUV).rgb;
    // Einfacher Reinhard-Tonemap
    hdr = hdr / (hdr + 1.0);
    fragColor = vec4(hdr, 1.0);
}
