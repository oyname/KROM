Texture2D                 normal          : register(t1);
TextureCube               tIBLPrefiltered : register(t6);

SamplerState             sLinear       : register(s0);
SamplerState             sClamp        : register(s1);

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

float4 main(PSInput IN) : SV_Target
{
    const float roughness = clamp(roughnessFactor, 0.04f, 1.0f);
    float3 N = SafeNormalize(IN.normalWS);
    N = SampleNormal(IN.texCoord, N, IN.tangentWS);
    const float3 V = SafeNormalize(cameraPositionWS.xyz - IN.positionWS);
    const float3 R = reflect(-V, N);
    const float3 prefiltered = tIBLPrefiltered.SampleLevel(sClamp, SafeNormalize(R), roughness * iblPrefilterLevels).rgb;
    return float4(prefiltered, 1.0f);
}
