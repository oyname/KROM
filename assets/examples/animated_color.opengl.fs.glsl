// =============================================================================
// KROM Engine — assets/examples/animated_color.opengl.fs.glsl
// Fragment Shader: Animierter Farb-Effekt (OpenGL / GLSL 4.10)
//
// Effekte:
//   1. Regenbogenfarben — Hue rotiert zeitabhängig (HSV → RGB)
//   2. UV-Wellen        — Sinus-Verzerrung der Texturkoordinaten
//   3. UV-Scrolling     — Textur scrollt diagonal
//   4. Pulsierendes Leuchten — Helligkeit pulsiert
//
// Parameter ueber Material-Editor steuerbar:
//   metallicFactor   → Rotationsgeschwindigkeit der Farben (0=langsam, 1=schnell)
//   roughnessFactor  → Staerke der Wellenverzerrung     (0=keine, 1=stark)
//   baseColorFactor  → Farbtoenung / Multiplikator
// =============================================================================
#version 410 core

struct GpuLightData { vec4 p; vec4 d; vec4 c; vec4 x; };

layout(std140) uniform PerFrame
{
    mat4         viewMatrix;
    mat4         projMatrix;
    mat4         viewProjMatrix;
    mat4         invViewProjMatrix;
    vec4         cameraPositionWS;
    vec4         cameraForwardWS;
    vec4         screenSize;
    vec4         timeParams;
    vec4         ambientColor;
    uint         lightCount;
    uint         shadowCascadeCount;
    float        nearPlane;
    float        farPlane;
    GpuLightData lights[7];
    mat4         shadowViewProj;
    float        iblPrefilterLevels;
    float        shadowBias;
    float        shadowNormalBias;
    float        shadowStrength;
};

layout(std140) uniform PerMaterial
{
    vec4  baseColorFactor;
    vec4  emissiveFactor;
    float metallicFactor;
    float roughnessFactor;
    float occlusionStrength;
    float opacityFactor;
    float alphaCutoff;
    int   materialFeatureMask;
    float materialModel;
    float _pad0;
};

uniform sampler2D tAlbedo;

in  vec3 vPositionWS;
in  vec3 vNormalWS;
in  vec2 vTexCoord;
out vec4 FragColor;

// ── HSV → RGB ─────────────────────────────────────────────────────────────────
vec3 HsvToRgb(float h, float s, float v)
{
    vec4 k = vec4(1.0, 2.0 / 3.0, 1.0 / 3.0, 3.0);
    vec3 p = abs(fract(vec3(h) + k.xyz) * 6.0 - k.www);
    return v * mix(k.xxx, clamp(p - k.xxx, 0.0, 1.0), s);
}

// ── Puls 0..1 ─────────────────────────────────────────────────────────────────
float Pulse(float t, float freq)
{
    return 0.5 + 0.5 * sin(t * freq * 6.2832);
}

void main()
{
    float t   = timeParams.x;
    float u   = vTexCoord.x;
    float v   = vTexCoord.y;

    // 1. UV-Wellenverzerrung
    float waveStrength = roughnessFactor * 0.08;
    u += sin(v * 8.0 + t * 2.0) * waveStrength;
    v += sin(u * 6.0 + t * 1.5) * waveStrength;

    // 2. UV-Scrolling
    vec2 scrolledUV = vec2(u + t * 0.1, v + t * 0.07);

    // 3. Textur (optional)
    vec4 texColor = ((materialFeatureMask & 1) != 0)
        ? texture(tAlbedo, scrolledUV)
        : vec4(1.0);

    // 4. Regenbogen-Hue
    float speed   = 0.2 + metallicFactor * 0.8;
    float hue     = fract(t * speed + u * 0.3 + v * 0.2);
    vec3  rainbow = HsvToRgb(hue, 0.85, 1.0);

    // 5. Puls
    float pulse = 0.75 + 0.25 * Pulse(t, 0.8);

    // 6. Zusammensetzen
    vec3  finalColor = rainbow * texColor.rgb * baseColorFactor.rgb * pulse;
    float finalAlpha = texColor.a * baseColorFactor.a * opacityFactor;

    FragColor = vec4(finalColor, finalAlpha);
}
