// =============================================================================
// KROM ENGINE — assets/passthrough.ps.hlsl
// Reinhard-Tonemap mit Bloom.
// =============================================================================

Texture2D    uHDRInput     : register(t8);
Texture2D    uBloomTexture : register(t12);
SamplerState uSampler      : register(s1);

cbuffer PerPass : register(b3)
{
    float4 bloomParams; // x=threshold, y=intensity, z=blurRadius
};

struct PSInput
{
    float4 pos : SV_POSITION;
    float2 uv  : TEXCOORD0;
};

float4 main(PSInput IN) : SV_TARGET
{
    float3 v = uHDRInput.Sample(uSampler, IN.uv).rgb;
    v += uBloomTexture.Sample(uSampler, IN.uv).rgb * bloomParams.y;
    v  = v / (v + 1.0f);  // Reinhard
    return float4(v, 1.0f);
}
