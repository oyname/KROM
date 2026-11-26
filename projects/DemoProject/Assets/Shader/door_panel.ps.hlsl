// =============================================================================
// KROM Engine — Testraum: door_panel.ps.hlsl
// Fragment Shader der Schiebetuer (Custom Shader mit eigener Textur).
//
//   • panelTex (t13, eigene Textur aus der .mat) liefert das Blechmuster.
//   • Warnstreifen laufen diagonal ueber die Tuer, ihre Farbe kommt aus
//     stripeColor, die Geschwindigkeit aus stripeSpeed (eigene Parameter).
//   • Lambert light: the indirect light of the scene (surface cache GI where
//     it is on, the scene's ambient otherwise) plus the first directional
//     light. The door is lit like its surroundings, at any exposure.
//
// Binding-Modell:
//   b0 = PerFrame, b2 = PerMaterial, t13 = panelTex, t21 = GI (pbr_gi.hlsl),
//   s0 = LinearWrap
// =============================================================================

struct GpuLightData
{
    float4 positionWS;      // Richtungslicht: Richtung (Licht → Szene)
    float4 directionWS;
    float4 colorIntensity;  // rgb = Farbe, w = Intensitaet
    float4 params;          // w = Typ (0 = Richtungslicht)
};

cbuffer PerFrame : register(b0)
{
    float4x4     viewMatrix;
    float4x4     projMatrix;
    float4x4     viewProjMatrix;
    float4x4     invViewProjMatrix;
    float4       cameraPositionWS;
    float4       cameraForwardWS;
    float4       screenSize;
    float4       timeParams;   // x = Zeit (s)
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
    float        shadowTexelSize;
    uint         debugFlags;
    uint         shadowFilterMode;
    float        _shadowPad;
    float4       shadowLightMeta[4];
    float4       shadowLightExtra[4];
    float4       shadowViewRect[16];
    float4x4     shadowViewProjArray[16];
    uint         shadowLightCount;
    uint         shadowViewCount;
    uint         shadowBufferLightCount;
    uint         shadowBufferViewCount;
    float4       featureParams[9]; // add-on parameter blocks (slot 8: surface cache GI)
};

#include "pbr_gi.hlsl"

cbuffer PerMaterial : register(b2)
{
    float4 baseColorFactor;
    float4 stripeColor;     // eigener Parameter
    float  stripeSpeed;     // eigener Parameter
    float  stripeWidth;     // eigener Parameter (Streifen pro UV-Einheit)
};

Texture2D    panelTex : register(t13);
SamplerState sLinear  : register(s0);

struct PSInput
{
    float4 positionCS : SV_POSITION;
    float3 positionWS : TEXCOORD1;
    float3 normalWS   : TEXCOORD2;
    float2 texCoord   : TEXCOORD0;
};

float4 main(PSInput IN) : SV_TARGET
{
    const float t = timeParams.x;
    const float3 panel = panelTex.Sample(sLinear, IN.texCoord).rgb * baseColorFactor.rgb;

    // Diagonale Warnstreifen nur am Rand (oben und unten 20 %).
    const float band   = step(IN.texCoord.y, 0.2) + step(0.8, IN.texCoord.y);
    const float stripe = step(0.5, frac((IN.texCoord.x + IN.texCoord.y) * stripeWidth + t * stripeSpeed));
    float3 albedo = lerp(panel, stripeColor.rgb, saturate(band) * stripe);

    // Irradiance E: the scene's ambient (E = pi * ambient, as in the engine
    // shaders), replaced by the surface cache GI where it is applied.
    static const float PI = 3.14159265f;
    const float3 N = normalize(IN.normalWS);
    float3 irradiance = ambientColor.rgb * ambientColor.a * PI;
    float3 giIrradiance;
    float giCoverage;
    if (KromSampleGI(IN.positionCS.xy, IN.positionWS, giIrradiance, giCoverage))
        irradiance = lerp(irradiance, giIrradiance, giStrength);
    const float3 indirect = irradiance;
    if (lightCount > 0 && lights[0].params.w < 0.5)
    {
        const float3 L = normalize(-lights[0].positionWS.xyz);
        irradiance += lights[0].colorIntensity.rgb * lights[0].colorIntensity.w * saturate(dot(N, L));
    }

    // The stripes glow a little so the door stands out in shadow too: as much
    // again as the indirect light gives them. A fixed radiance burnt out to
    // white in a dim scene, and only the bloom around the door still showed
    // the running stripes (a flicker along the frame).
    const float3 glow = stripeColor.rgb * saturate(band) * stripe * (indirect / PI);
    return float4(albedo * irradiance / PI + glow, 1.0);
}
