Texture2D                 normal          : register(t1);

SamplerState             sLinear       : register(s0);

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
    float  _pad0;
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

static const float PI = 3.14159265358979323846f;

float3 SafeNormalize(float3 v)
{
    float len2 = dot(v, v);
    return (len2 > 1e-12f) ? (v * rsqrt(len2)) : float3(0.0f, 0.0f, 1.0f);
}

float3 DecodeNormalRGB(float4 encodedNormal)
{
    return encodedNormal.xyz * 2.0f - 1.0f;
}

float3x3 BuildTBN(float3 N, float4 tangentWS)
{
    float3 T = tangentWS.xyz;
    float handedness = (abs(tangentWS.w) > 0.5f) ? tangentWS.w : 1.0f;
    if (dot(T, T) <= 1e-10f)
    {
        float3 up = (abs(N.y) < 0.999f) ? float3(0.0f, 1.0f, 0.0f) : float3(1.0f, 0.0f, 0.0f);
        T = SafeNormalize(cross(up, N));
    }
    else
    {
        T = SafeNormalize(T - N * dot(N, T));
    }
    float3 B = SafeNormalize(cross(N, T)) * handedness;
    return float3x3(T, B, N);
}

float3 SampleNormal(float2 uv, float3 baseNormalWS, float4 tangentWS)
{
#ifdef KROM_NORMAL_MAP
    float3 mapN = SafeNormalize(DecodeNormalRGB(normal.Sample(sLinear, uv)));
    mapN.xy *= normalStrength;
    mapN.z = sqrt(saturate(1.0f - dot(mapN.xy, mapN.xy)));
    mapN = SafeNormalize(mapN);
    float3x3 tbn = BuildTBN(baseNormalWS, tangentWS);
    return SafeNormalize(mul(mapN, tbn));
#else
    return baseNormalWS;
#endif
}

float D_GGX(float NoH, float roughness)
{
    float a = roughness * roughness;
    float a2 = a * a;
    float d = (NoH * a2 - NoH) * NoH + 1.0f;
    return a2 / max(PI * d * d, 1e-6f);
}

float G_SchlickGGX(float NoV, float roughness)
{
    float r = roughness + 1.0f;
    float k = (r * r) / 8.0f;
    return NoV / max(NoV * (1.0f - k) + k, 1e-6f);
}

float G_Smith(float NoV, float NoL, float roughness)
{
    return G_SchlickGGX(NoV, roughness) * G_SchlickGGX(NoL, roughness);
}

float3 F_Schlick(float HoV, float3 F0)
{
    return F0 + (1.0f - F0) * pow(1.0f - HoV, 5.0f);
}

float4 main(PSInput IN) : SV_Target
{
    if (lightCount == 0u)
        return float4(0.0f, 0.0f, 0.0f, 1.0f);

    const float3 albedoValue = baseColorFactor.rgb;
    const float metallic = saturate(metallicFactor);
    const float roughness = clamp(roughnessFactor, 0.04f, 1.0f);

    float3 N = SafeNormalize(IN.normalWS);
    N = SampleNormal(IN.texCoord, N, IN.tangentWS);
    const float3 V = SafeNormalize(cameraPositionWS.xyz - IN.positionWS);
    const float3 L = SafeNormalize(-lights[0].positionWS.xyz);
    const float3 H = SafeNormalize(V + L);

    const float NoV = max(dot(N, V), 1e-4f);
    const float NoL = max(dot(N, L), 0.0f);
    const float NoH = max(dot(N, H), 0.0f);
    const float HoV = max(dot(H, V), 0.0f);

    const float3 F0 = lerp(float3(0.04f, 0.04f, 0.04f), albedoValue, metallic);
    const float3 F = F_Schlick(HoV, F0);
    const float D = D_GGX(NoH, roughness);
    const float G = G_Smith(NoV, NoL, roughness);
    const float3 spec = (D * G * F) / max(4.0f * NoV * NoL, 1e-4f);
    const float3 lightColor = lights[0].colorIntensity.rgb * lights[0].colorIntensity.w;
    return float4(spec * lightColor * NoL, 1.0f);
}
