// =============================================================================
// KROM Engine — assets/shadow_pbr.ps.hlsl
// Pixel Shader: Shadow-Pass für PBR-Materialien (KROM_ALPHA_TEST)
// Sampelt die Albedo-Textur (UV bereits durch shadow_pbr.vs.hlsl transformiert)
// und verwirft Fragmente mit Opacity < alphaCutoff.
// =============================================================================

#ifdef KROM_ALPHA_TEST

Texture2D    albedo  : register(t0);
SamplerState sLinear : register(s0);

cbuffer PerMaterial : register(b2)
{
    float4 baseColorFactor;
    float4 emissiveFactor;
    float  metallicFactor;
    float  roughnessFactor;
    float  normalStrength;
    float  occlusionStrength;
    float  opacityFactor;
    float  alphaCutoff;
    int    materialFeatureMask;
    float  materialModel;
    int    occlusionChannel;
    int    roughnessChannel;
    int    metallicChannel;
    float  occlusionBias;
    float  roughnessBias;
    float  metallicBias;
    float2 uvScale;
    float2 uvOffset;
};

struct PSInput
{
    float4 positionCS : SV_POSITION;
    float2 texCoord   : TEXCOORD0;
};

void main(PSInput IN)
{
    float4 c = albedo.Sample(sLinear, IN.texCoord) * baseColorFactor;
    float opacity = c.a * opacityFactor;
    clip(opacity - alphaCutoff);
}

#else

void main() {}

#endif
