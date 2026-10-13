Texture2D                 albedo         : register(t0);
Texture2D                 normal         : register(t1);
TextureCube               tIBLIrradiance : register(t5);

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
    T = SafeNormalize(T - N * dot(N, T));
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
#ifdef KROM_BASECOLOR_MAP
    float4 albedo4 = albedo.Sample(sLinear, IN.texCoord) * baseColorFactor;
#else
    float4 albedo4 = baseColorFactor;
#endif

    float3 geomN = SafeNormalize(IN.normalWS);
    float3 N = SampleNormal(IN.texCoord, geomN, IN.tangentWS);
    float3 irradiance = tIBLIrradiance.Sample(sClamp, SafeNormalize(N)).rgb;
    float3 debugColor = irradiance * 4.0f;
    debugColor = debugColor / (debugColor + 1.0f.xxx);
    debugColor = pow(saturate(debugColor), 1.0f / 2.2f);
    return float4(debugColor, 1.0f);
}
