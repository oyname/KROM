// =============================================================================
// KROM Engine — assets/passthrough.ps.hlsl
// Passthrough-Tonemap / Raw Shadow Debug.
// =============================================================================

Texture2D    uHDRInput : register(t8);
SamplerState uSampler  : register(s1); // Material-selected sampler

struct PSInput
{
    float4 pos : SV_POSITION;
    float2 uv  : TEXCOORD0;
};

float4 main(PSInput IN) : SV_TARGET
{
    float3 v = uHDRInput.Sample(uSampler, IN.uv).rgb;
    v = v / (v + 1.0f);
    return float4(v, 1.0f);
}
