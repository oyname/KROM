// =============================================================================
// KROM Engine — assets/passthrough.ps.hlsl
// Passthrough-Tonemap / Raw Shadow Debug.
// =============================================================================

Texture2D    uHDRInput : register(t8);
SamplerState uSampler  : register(s1); // Material-selected sampler

cbuffer PerFrame : register(b0)
{
    float4x4     viewMatrix;
    float4x4     projMatrix;
    float4x4     viewProjMatrix;
    float4x4     invViewProjMatrix;
    float4       cameraPositionWS;
    float4       cameraForwardWS;
    float4       screenSize;
    float4       timeData;
    float4       ambientColor;
    uint         lightCount;
    uint         shadowCascadeCount;
    float        nearPlane;
    float        farPlane;
    float4       _lightsPadding[28];
    float4x4     shadowViewProj;
    float        iblPrefilterLevels;
    float        shadowBias;
    float        shadowNormalBias;
    float        shadowStrength;
    float        shadowTexelSize;
};

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
