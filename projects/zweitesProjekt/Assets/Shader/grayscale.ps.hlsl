// =============================================================================
// KROM Engine — assets/examples/grayscale.ps.hlsl
// Fragment Shader: Bild schwarz-weiss mit einstellbarer Intensitaet
//
// Parameter (automatisch per Reflection im Material-Editor sichtbar):
//   intensity   — 0.0 = Originalfarbe, 1.0 = vollstaendig S/W
//   brightness  — Helligkeit des Ergebnisses (+/-)
//   contrast    — Kontrast des Ergebnisses (1.0 = normal)
//
// Texturen:
//   t0 (albedo) — Eingabebild (Base Color Textur)
// =============================================================================

cbuffer PerMaterial : register(b2)
{
    float4 baseColorFactor;   // Farbtoenung
    float4 emissiveFactor;
    float  intensity;         // S/W-Staerke: 0=Farbe, 1=S/W  [Editor: Slider]
    float  brightness;        // Helligkeit: -1..+1            [Editor: Slider]
    float  contrast;          // Kontrast: 0..2               [Editor: Slider]
    float  opacityFactor;
    float  alphaCutoff;
    int    materialFeatureMask;
    float  materialModel;
    float  _pad0;
};

Texture2D    tAlbedo : register(t0);
SamplerState sLinear : register(s0);

struct PSInput
{
    float4 positionCS : SV_POSITION;
    float3 positionWS : TEXCOORD1;
    float3 normalWS   : TEXCOORD2;
    float2 texCoord   : TEXCOORD0;
};

float4 main(PSInput IN) : SV_TARGET
{
    // Textur samplen
    float4 texColor = (materialFeatureMask & 1)
        ? tAlbedo.Sample(sLinear, IN.texCoord)
        : float4(1.0, 1.0, 1.0, 1.0);

    float4 color = texColor * baseColorFactor;

    // Luminanz nach ITU-R BT.709 (wie menschliches Auge gewichtet)
    float luma = dot(color.rgb, float3(0.2126, 0.7152, 0.0722));

    // S/W-Mischung: intensity=0 → Original, intensity=1 → vollstaendig S/W
    float3 result = lerp(color.rgb, float3(luma, luma, luma), saturate(intensity));

    // Helligkeit (+/-)
    result += brightness;

    // Kontrast: um 0.5 skalieren
    result = (result - 0.5) * max(contrast, 0.0) + 0.5;

    return float4(saturate(result), color.a * opacityFactor);
}
