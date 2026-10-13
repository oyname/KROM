Texture2D    uHDRInput : register(t8);
SamplerState uSampler  : register(s1);

struct PSInput
{
    float4 pos : SV_POSITION;
    float2 uv  : TEXCOORD0;
};

float4 main(PSInput IN) : SV_TARGET
{
    float3 v = uHDRInput.Sample(uSampler, IN.uv).rgb;
    return float4(saturate(v), 1.0f);
}
