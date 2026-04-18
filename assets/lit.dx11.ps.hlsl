Texture2D albedo   : register(t0);
Texture2D emissive : register(t3);

SamplerState sLinear : register(s0);

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
    GpuLightData lights[8];
    float        iblPrefilterLevels;
    float        _padFC0;
    float        _padFC1;
    float        _padFC2;
};

cbuffer PerMaterial : register(b2)
{
    float4 baseColorFactor;
    float4 emissiveFactor;
    float  metallicFactor;
    float  roughnessFactor;
    float  occlusionStrength;
    float  opacityFactor;
    float  alphaCutoff;
    int    materialFeatureMask;
    float  materialModel;
    float  _pad0;
};

struct PSInput
{
    float4 positionCS : SV_POSITION;
    float3 positionWS : TEXCOORD1;
    float3 normalWS   : TEXCOORD2;
    float2 texCoord   : TEXCOORD0;
};

float3 SafeNormalize(float3 v)
{
    float len2 = dot(v, v);
    return (len2 > 1e-12f) ? (v * rsqrt(len2)) : float3(0.0f, 0.0f, 1.0f);
}

float CalcAttenuation(float dist, float range)
{
    float ratio = saturate(1.0f - dist / max(range, 0.001f));
    return (ratio * ratio) / max(dist * dist, 1e-4f);
}

float CalcSpotAttenuation(float3 L, float3 spotDir, float cosInner, float cosOuter)
{
    float cosAngle = dot(-L, spotDir);
    return saturate((cosAngle - cosOuter) / max(cosInner - cosOuter, 1e-4f));
}

float4 main(PSInput IN) : SV_Target
{
#ifdef KROM_BASECOLOR_MAP
    float4 albedo4 = albedo.Sample(sLinear, IN.texCoord) * baseColorFactor;
#else
    float4 albedo4 = baseColorFactor;
#endif

    float opacity = albedo4.a * opacityFactor;
#ifdef KROM_ALPHA_TEST
    clip(opacity - alphaCutoff);
#endif

    float3 N = SafeNormalize(IN.normalWS);
    float3 V = SafeNormalize(cameraPositionWS.xyz - IN.positionWS);
    float roughness = clamp(roughnessFactor, 0.04f, 1.0f);
    float shininess = lerp(128.0f, 8.0f, roughness);
    float specularStrength = saturate(metallicFactor);

    float3 lighting = albedo4.rgb * ambientColor.rgb * ambientColor.a;
    [loop] for (uint i = 0; i < lightCount; ++i)
    {
        const float type = lights[i].params.w;
        float3 L = float3(0.0f, 1.0f, 0.0f);
        float attenuation = 1.0f;

        if (type < 0.5f)
        {
            L = SafeNormalize(-lights[i].positionWS.xyz);
        }
        else
        {
            float3 toL = lights[i].positionWS.xyz - IN.positionWS;
            float dist = length(toL);
            L = (dist > 1e-5f) ? (toL / dist) : float3(0.0f, 1.0f, 0.0f);
            attenuation = CalcAttenuation(dist, lights[i].params.z);
            if (type > 1.5f)
                attenuation *= CalcSpotAttenuation(L, SafeNormalize(lights[i].directionWS.xyz), lights[i].params.x, lights[i].params.y);
        }

        float NoL = max(dot(N, L), 0.0f);
        float3 H = SafeNormalize(V + L);
        float spec = pow(max(dot(N, H), 0.0f), shininess) * specularStrength;
        float3 lightColor = lights[i].colorIntensity.rgb * lights[i].colorIntensity.w * attenuation;
        lighting += albedo4.rgb * lightColor * NoL;
        lighting += lightColor * spec;
    }

    float3 emissiveColor = emissiveFactor.rgb;
#ifdef KROM_EMISSIVE_MAP
    emissiveColor *= emissive.Sample(sLinear, IN.texCoord).rgb;
#endif

    return float4(lighting + emissiveColor, opacity);
}
