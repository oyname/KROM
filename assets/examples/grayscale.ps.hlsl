// =============================================================================
// KROM Engine — assets/examples/grayscale.ps.hlsl
// Fragment Shader: Bild schwarz-weiss mit einstellbaren Parametern
//
// Textur im Material-Editor dem "Base Color"-Slot zuweisen!
//
// Parameter (im Material-Editor einstellbar):
//   Metallic   → intensity   0=Originalfarbe  1=vollstaendig S/W
//   Roughness  → contrast    0=flach          1=normal       2=stark
//   Occlusion  → brightness  1=normal  <1=dunkler  >1=heller
// =============================================================================

cbuffer PerMaterial : register(b2)
{
    float4 baseColorFactor;
    float4 emissiveFactor;
    float  metallicFactor;      // intensity:   S/W-Staerke
    float  roughnessFactor;     // contrast:    Kontrast
    float  occlusionStrength;   // brightness:  Helligkeit
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
    // Textur samplen — im Material-Editor dem Base-Color-Slot zuweisen
    float4 color = tAlbedo.Sample(sLinear, IN.texCoord) * baseColorFactor;

    // Luminanz nach ITU-R BT.709
    float luma = dot(color.rgb, float3(0.2126, 0.7152, 0.0722));

    // S/W-Mischung: metallicFactor=0 → Farbe, metallicFactor=1 → S/W
    float3 result = lerp(color.rgb, float3(luma, luma, luma),
                         saturate(metallicFactor));

    // Kontrast (roughnessFactor=1 = normal)
    result = (result - 0.5) * max(roughnessFactor, 0.0) + 0.5;

    // Helligkeit (occlusionStrength=1 = normal)
    result *= occlusionStrength;

    return float4(saturate(result), color.a * opacityFactor);
}
