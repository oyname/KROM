// =============================================================================
// KROM Engine — assets/passthrough.ps.hlsl
// Tonemap pass: HDR + bloom, exposure, then a selectable tonemap operator.
// Output is LINEAR: the sRGB render target / backbuffer applies the sRGB curve.
// The operators below therefore end in linear values; none adds its own gamma.
// =============================================================================

Texture2D    uHDRInput     : register(t8);
Texture2D    uBloomTexture : register(t12);
SamplerState uSampler      : register(s1);

cbuffer PerPass : register(b3)
{
    float4 bloomParams;    // x=threshold, y=intensity, z=blurRadius
    float4 tonemapParams;  // x=exposure scale (2^EV), y=operator, zw unused
};

// Operator ids (addons/forward/ForwardTonemap.hpp, TonemapOperator).
static const int KROM_TONEMAP_REINHARD     = 0;
static const int KROM_TONEMAP_ACES_FITTED  = 1;
static const int KROM_TONEMAP_AGX          = 2;
static const int KROM_TONEMAP_PBR_NEUTRAL  = 3;

struct PSInput
{
    float4 pos : SV_POSITION;
    float2 uv  : TEXCOORD0;
};

float3 TonemapReinhard(float3 v)
{
    return v / (v + 1.0f);
}

// ACES fitted (Stephen Hill): sRGB -> AP1-ish, RRT+ODT fit, back to sRGB. Linear out.
float3 RRTAndODTFit(float3 v)
{
    float3 a = v * (v + 0.0245786f) - 0.000090537f;
    float3 b = v * (0.983729f * v + 0.4329510f) + 0.238081f;
    return a / b;
}

float3 TonemapAcesFitted(float3 color)
{
    static const float3x3 inputMat = {
        0.59719f, 0.35458f, 0.04823f,
        0.07600f, 0.90834f, 0.01566f,
        0.02840f, 0.13383f, 0.83777f };
    static const float3x3 outputMat = {
         1.60475f, -0.53108f, -0.07367f,
        -0.10208f,  1.10813f, -0.00605f,
        -0.00327f, -0.07276f,  1.07602f };
    color = mul(inputMat, color);
    color = RRTAndODTFit(color);
    color = mul(outputMat, color);
    return saturate(color);
}

// AgX (base look, minimal fit by B. Wrensch). The curve produces display-encoded
// values; the last step undoes the display gamma so the pass stays linear.
float3 AgxContrastApprox(float3 x)
{
    float3 x2 = x * x;
    float3 x4 = x2 * x2;
    return 15.5f * x4 * x2 - 40.14f * x4 * x + 31.96f * x4
         - 6.868f * x2 * x + 0.4298f * x2 + 0.1191f * x - 0.00232f;
}

float3 TonemapAgx(float3 color)
{
    static const float3x3 agxInset = {
        0.842479062253094f, 0.0784335999999992f, 0.0792237451477643f,
        0.0423282422610123f, 0.878468636469772f, 0.0791661274605434f,
        0.0423756549057051f, 0.0784336f, 0.879142973793104f };
    static const float3x3 agxOutset = {
         1.19687900512017f,  -0.0980208811401368f, -0.0990297440797205f,
        -0.0528968517574562f, 1.15190312990417f,   -0.0989611768448433f,
        -0.0529716355144438f, -0.0980434501171241f, 1.15107367264116f };
    const float minEv = -12.47393f;
    const float maxEv = 4.026069f;

    // Rows as written; the GLSL reference builds the same matrices column by column.
    color = mul(agxInset, color);
    color = clamp(log2(max(color, 1e-10f)), minEv, maxEv);
    color = (color - minEv) / (maxEv - minEv);
    color = AgxContrastApprox(color);
    color = mul(agxOutset, color);
    // Display encoding (gamma 2.2) -> linear for the sRGB target.
    return pow(saturate(color), 2.2f);
}

// Khronos PBR Neutral (reference implementation). Works in linear, returns linear.
float3 TonemapPbrNeutral(float3 color)
{
    const float startCompression = 0.8f - 0.04f;
    const float desaturation = 0.15f;

    float x = min(color.r, min(color.g, color.b));
    float offset = x < 0.08f ? x - 6.25f * x * x : 0.04f;
    color -= offset;

    float peak = max(color.r, max(color.g, color.b));
    if (peak < startCompression)
        return color;

    const float d = 1.0f - startCompression;
    float newPeak = 1.0f - d * d / (peak + d - startCompression);
    color *= newPeak / peak;

    float g = 1.0f - 1.0f / (desaturation * (peak - newPeak) + 1.0f);
    return lerp(color, newPeak.xxx, g);
}

float4 main(PSInput IN) : SV_TARGET
{
    float3 v = uHDRInput.Sample(uSampler, IN.uv).rgb;
    v += uBloomTexture.Sample(uSampler, IN.uv).rgb * bloomParams.y;
    v *= max(tonemapParams.x, 0.0f);

    const int op = (int)(tonemapParams.y + 0.5f);
    if (op == KROM_TONEMAP_ACES_FITTED)
        v = TonemapAcesFitted(v);
    else if (op == KROM_TONEMAP_AGX)
        v = TonemapAgx(v);
    else if (op == KROM_TONEMAP_PBR_NEUTRAL)
        v = TonemapPbrNeutral(v);
    else
        v = TonemapReinhard(v);
    return float4(v, 1.0f);
}
