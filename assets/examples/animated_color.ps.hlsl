// =============================================================================
// KROM Engine — assets/examples/animated_color.ps.hlsl
// Fragment Shader: Animierter Farb-Effekt (Custom Shader Beispiel)
//
// Effekte:
//   1. Regenbogenfarben — Hue rotiert zeitabhängig (HSV → RGB)
//   2. UV-Wellen        — Sinus-Verzerrung der Texturkoordinaten
//   3. UV-Scrolling     — Textur scrollt diagonal
//   4. Pulsierendes Leuchten — Helligkeit pulsiert
//
// Binding-Modell (wie Standard-Unlit):
//   b0 = PerFrame   (timeParams.x = Laufzeit in Sekunden)
//   b2 = PerMaterial
//   t0 = Albedo Textur (optional)
//   s0 = LinearWrap Sampler
// =============================================================================

// ── Per-Frame Konstanten ─────────────────────────────────────────────────────
struct GpuLightData { float4 p; float4 d; float4 c; float4 x; };

cbuffer PerFrame : register(b0)
{
    float4x4     viewMatrix;
    float4x4     projMatrix;
    float4x4     viewProjMatrix;
    float4x4     invViewProjMatrix;
    float4       cameraPositionWS;
    float4       cameraForwardWS;
    float4       screenSize;
    float4       timeParams;   // x=Zeit(s)  y=DeltaTime  z=FrameCount  w=0
    float4       ambientColor;
    uint         lightCount;
    uint         shadowCascadeCount;
    float        nearPlane;
    float        farPlane;
    GpuLightData lights[7];
    float4x4     shadowViewProj;
    float        iblPrefilterLevels;
    float        shadowBias;
    float        shadowNormalBias;
    float        shadowStrength;
};

// ── Per-Material Konstanten ──────────────────────────────────────────────────
cbuffer PerMaterial : register(b2)
{
    float4 baseColorFactor;   // Multiplikator / Tönung aus dem Material-Editor
    float4 emissiveFactor;
    float  metallicFactor;    // Hier: Geschwindigkeit der Farbrotation (0..1 → 0..4x)
    float  roughnessFactor;   // Hier: Stärke der Wellenverzerrung (0..1)
    float  occlusionStrength;
    float  opacityFactor;
    float  alphaCutoff;
    int    materialFeatureMask;
    float  materialModel;
    float  _pad0;
};

// ── Texturen ─────────────────────────────────────────────────────────────────
Texture2D    tAlbedo : register(t0);
SamplerState sLinear : register(s0);

// ── Input ────────────────────────────────────────────────────────────────────
struct PSInput
{
    float4 positionCS : SV_POSITION;
    float3 positionWS : TEXCOORD1;
    float3 normalWS   : TEXCOORD2;
    float2 texCoord   : TEXCOORD0;
};

// ── Hilfsfunktionen ──────────────────────────────────────────────────────────

// HSV → RGB Konvertierung
// h = Farbton 0..1, s = Sättigung 0..1, v = Helligkeit 0..1
float3 HsvToRgb(float h, float s, float v)
{
    float4 k = float4(1.0, 2.0 / 3.0, 1.0 / 3.0, 3.0);
    float3 p = abs(frac(float3(h, h, h) + k.xyz) * 6.0 - k.www);
    return v * lerp(k.xxx, saturate(p - k.xxx), s);
}

// Sanfte Puls-Funktion: 0..1 pulsierend
float Pulse(float t, float freq)
{
    return 0.5 + 0.5 * sin(t * freq * 6.2832);
}

// ── Hauptfunktion ────────────────────────────────────────────────────────────
float4 main(PSInput IN) : SV_TARGET
{
    float t  = timeParams.x;          // Laufzeit in Sekunden
    float uv_u = IN.texCoord.x;
    float uv_v = IN.texCoord.y;

    // ── 1. UV-Wellenverzerrung ────────────────────────────────────────────────
    //   roughnessFactor steuert die Stärke (0 = keine Verzerrung)
    float waveStrength = roughnessFactor * 0.08;
    uv_u += sin(uv_v * 8.0 + t * 2.0) * waveStrength;
    uv_v += sin(uv_u * 6.0 + t * 1.5) * waveStrength;

    // ── 2. UV-Scrolling diagonal ──────────────────────────────────────────────
    float2 scrolledUV = float2(uv_u + t * 0.1, uv_v + t * 0.07);

    // ── 3. Textur samplen (optional — falls keine Textur: weiß) ───────────────
    float4 texColor = (materialFeatureMask & 1)
        ? tAlbedo.Sample(sLinear, scrolledUV)
        : float4(1.0, 1.0, 1.0, 1.0);

    // ── 4. Regenbogen-Hue ─────────────────────────────────────────────────────
    //   metallicFactor steuert Geschwindigkeit (0=langsam, 1=schnell)
    float speed    = 0.2 + metallicFactor * 0.8;
    float hue      = frac(t * speed + uv_u * 0.3 + uv_v * 0.2);
    float3 rainbow = HsvToRgb(hue, 0.85, 1.0);

    // ── 5. Pulsierendes Leuchten ──────────────────────────────────────────────
    float pulse = 0.75 + 0.25 * Pulse(t, 0.8);

    // ── 6. Alles zusammensetzen ───────────────────────────────────────────────
    //   Regenbogenfarbe × Textur × Material-Tönung × Puls
    float3 finalColor = rainbow * texColor.rgb * baseColorFactor.rgb * pulse;
    float  finalAlpha = texColor.a * baseColorFactor.a * opacityFactor;

    return float4(finalColor, finalAlpha);
}
