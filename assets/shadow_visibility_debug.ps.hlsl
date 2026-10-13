Texture2D                 normal    : register(t1);
Texture2D                 shadowMap : register(t4);

SamplerState             sLinear       : register(s0);
SamplerState             sPoint        : register(s2);

struct GpuLightData
{
    float4 positionWS;
    float4 directionWS;
    float4 colorIntensity;
    float4 params;
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
    float4       timeData;
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
};

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
    float  _pad1;
    float  _pad2;
};

struct PSInput
{
    float4 positionCS      : SV_POSITION;
    float3 positionWS      : TEXCOORD1;
    float3 normalWS        : TEXCOORD2;
    float4 tangentWS       : TEXCOORD3;
    float2 texCoord        : TEXCOORD0;
    float4 positionLightCS : TEXCOORD4;
};

float3 SafeNormalize(float3 v)
{
    float len2 = dot(v, v);
    return (len2 > 1e-12f) ? (v * rsqrt(len2)) : float3(0.0f, 0.0f, 1.0f);
}

float CompareShadowManual(float2 uv, float cmpDepth)
{
    float sampledDepth = shadowMap.SampleLevel(sPoint, uv, 0).r;
    return (cmpDepth <= sampledDepth) ? 1.0f : 0.0f;
}

float CompareShadowManualBilinear(float2 uv, float cmpDepth)
{
    float texelSize = max(shadowTexelSize, 1e-8f);
    float invTexelSize = 1.0f / texelSize;

    float2 texelPos = uv * invTexelSize - 0.5f;
    float2 baseTexel = floor(texelPos);
    float2 fracTexel = frac(texelPos);

    float2 uv00 = (baseTexel + float2(0.5f, 0.5f)) * texelSize;
    float2 uv10 = uv00 + float2(texelSize, 0.0f);
    float2 uv01 = uv00 + float2(0.0f, texelSize);
    float2 uv11 = uv00 + float2(texelSize, texelSize);

    float c00 = CompareShadowManual(uv00, cmpDepth);
    float c10 = CompareShadowManual(uv10, cmpDepth);
    float c01 = CompareShadowManual(uv01, cmpDepth);
    float c11 = CompareShadowManual(uv11, cmpDepth);

    float cx0 = lerp(c00, c10, fracTexel.x);
    float cx1 = lerp(c01, c11, fracTexel.x);
    return lerp(cx0, cx1, fracTexel.y);
}

float SampleDirectionalShadow(float4 positionLightCS, float3 normalWS, float3 lightDirWS)
{
    if (shadowCascadeCount == 0u || shadowStrength <= 0.0f)
        return 1.0f;
    if (positionLightCS.w <= 1e-6f)
        return 1.0f;

    float3 posNDC = positionLightCS.xyz / positionLightCS.w;
    float2 uv = float2(posNDC.x * 0.5f + 0.5f, 0.5f - posNDC.y * 0.5f);
    float depth = posNDC.z;

    if (uv.x < 0.0f || uv.x > 1.0f || uv.y < 0.0f || uv.y > 1.0f)
        return 1.0f;
    if (depth <= 0.0f || depth >= 1.0f)
        return 1.0f;

    float ddx_d  = ddx(depth);
    float ddy_d  = ddy(depth);
    float ddx_ux = ddx(uv.x);
    float ddy_uy = ddy(uv.y);
    float2 dDepth_dUV = float2(
        abs(ddx_ux) > 1e-7f ? ddx_d / ddx_ux : 0.0f,
        abs(ddy_uy) > 1e-7f ? ddy_d / ddy_uy : 0.0f
    );
    dDepth_dUV = clamp(dDepth_dUV, -1.0f, 1.0f);

    static const float2 kPoissonDisk[8] = {
        float2(-0.94201624f, -0.39906216f),
        float2( 0.94558609f, -0.76890725f),
        float2(-0.09418410f, -0.92938870f),
        float2( 0.34495938f,  0.29387760f),
        float2(-0.91588581f,  0.45771432f),
        float2(-0.81544232f, -0.87912464f),
        float2(-0.38277543f,  0.27676845f),
        float2( 0.97484398f,  0.75648379f),
    };

    float filterRadius = shadowTexelSize * 2.0f;
    float visibility = 0.0f;
    [unroll]
    for (int i = 0; i < 8; ++i)
    {
        float2 offset   = kPoissonDisk[i] * filterRadius;
        float  cmpDepth = depth + dot(dDepth_dUV, offset) - shadowBias;
        visibility += CompareShadowManualBilinear(uv + offset, cmpDepth);
    }

    visibility *= (1.0f / 8.0f);
    return lerp(1.0f, visibility, saturate(shadowStrength));
}

float4 main(PSInput IN) : SV_Target
{
    float3 N = SafeNormalize(IN.normalWS);
    float NoL = 0.0f;
    float shadowVisibility = 1.0f;
    bool hasDirectional = (lightCount > 0u && lights[0].params.w < 0.5f);
    if (hasDirectional)
    {
        float3 L = SafeNormalize(-lights[0].positionWS.xyz);
        NoL = saturate(dot(N, L));
        shadowVisibility = SampleDirectionalShadow(IN.positionLightCS, N, L);
    }

    float shadowAmount = saturate(1.0f - shadowVisibility);
    float3 litColor = float3(0.12f, 0.55f, 0.18f);
    float3 shadowColor = float3(1.0f, 0.18f, 0.05f);
    float3 debugColor = lerp(litColor, shadowColor, pow(shadowAmount, 0.5f));
    debugColor *= lerp(0.35f, 1.0f, NoL);
    return float4(debugColor, 1.0f);
}
