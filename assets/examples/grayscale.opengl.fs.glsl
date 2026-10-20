// =============================================================================
// KROM Engine - grayscale OpenGL fragment shader
// =============================================================================
#version 410 core

layout(std140) uniform PerMaterial
{
    vec4  baseColorFactor;
    vec4  emissiveFactor;
    float metallicFactor;    // S/W-Staerke
    float roughnessFactor;   // Kontrast
    float occlusionStrength; // Helligkeit
    float opacityFactor;
    float alphaCutoff;
    int   materialFeatureMask;
    float materialModel;
    float _pad0;
};

uniform sampler2D tAlbedo;

in vec3 vPositionWS;
in vec3 vNormalWS;
in vec2 vTexCoord;
out vec4 FragColor;

void main()
{
    vec4 texColor = ((materialFeatureMask & 2) != 0)
        ? texture(tAlbedo, vTexCoord)
        : vec4(1.0);

    vec4 color = texColor * baseColorFactor;
    float luma = dot(color.rgb, vec3(0.2126, 0.7152, 0.0722));
    vec3 result = mix(color.rgb, vec3(luma), clamp(metallicFactor, 0.0, 1.0));
    result = (result - 0.5) * max(roughnessFactor, 0.0) + 0.5;
    result *= occlusionStrength;

    FragColor = vec4(clamp(result, 0.0, 1.0), color.a * opacityFactor);
}
