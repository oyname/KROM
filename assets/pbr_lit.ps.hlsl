// =============================================================================
// KROM Engine — assets/pbr_lit.ps.hlsl
// Pixel Shader: PBR-Lit (HLSL)
// Targets: DX11 (DXBC), DX12 (DXIL), Vulkan (SPIR-V via DXC -spirv)
// =============================================================================

Texture2D                 albedo          : register(t0);
Texture2D                 normal          : register(t1);
Texture2D                 orm             : register(t2);
Texture2D                 emissive        : register(t3);
Texture2D                 shadowMap       : register(t4);
TextureCube               tIBLIrradiance  : register(t5);
TextureCube               tIBLPrefiltered : register(t6);
Texture2D                 tBRDFLut        : register(t7);

SamplerState             sLinear       : register(s0);
SamplerState             sClamp        : register(s1);
SamplerState             sPoint        : register(s2);
SamplerComparisonState   shadowSampler : register(s3);

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
    float4 baseColorFactor;     // byte  0
    float4 emissiveFactor;      // byte 16
    float  metallicFactor;      // byte 32
    float  roughnessFactor;     // byte 36
    float  normalStrength;      // byte 40
    float  occlusionStrength;   // byte 44
    float  opacityFactor;       // byte 48
    float  alphaCutoff;         // byte 52
    int    materialFeatureMask; // byte 56
    float  materialModel;       // byte 60
    // Row 4 — channel-map constants (zero-default; only meaningful with KROM_CHANNEL_MAP)
    int    occlusionChannel;    // byte 64
    int    roughnessChannel;    // byte 68
    int    metallicChannel;     // byte 72
    float  occlusionBias;       // byte 76
    // Row 5
    float  roughnessBias;       // byte 80
    float  metallicBias;        // byte 84
    float  _pad1;               // byte 88
    float  _pad2;               // byte 92
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
static const float TWO_PI = 6.28318530717958647692f;

float3 SafeNormalize(float3 v)
{
    float len2 = dot(v, v);
    return (len2 > 1e-12f) ? (v * rsqrt(len2)) : float3(0.0f, 0.0f, 1.0f);
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

float3 F_SchlickRoughness(float NoV, float3 F0, float roughness)
{
    float3 inv = max(float3(1.0f - roughness, 1.0f - roughness, 1.0f - roughness), F0);
    return F0 + (inv - F0) * pow(1.0f - NoV, 5.0f);
}


float3 DecodeNormalRGB(float4 encodedNormal)
{
    return encodedNormal.xyz * 2.0f - 1.0f;
}

float3 DecodeNormalBC5(float4 encodedNormal)
{
    float2 xy = encodedNormal.rg * 2.0f - 1.0f;
    float z = sqrt(saturate(1.0f - dot(xy, xy)));
    return float3(xy, z);
}

float SampleChannel(float4 v, int ch)
{
    float4 mask = float4(ch == 0 ? 1.0f : 0.0f,
                         ch == 1 ? 1.0f : 0.0f,
                         ch == 2 ? 1.0f : 0.0f,
                         ch == 3 ? 1.0f : 0.0f);
    return dot(v, mask);
}

float3 SampleDecodedNormal(Texture2D normalTex, SamplerState samp, float2 uv)
{
    const float4 encodedNormal = normalTex.Sample(samp, uv);
#if defined(KROM_NORMALMAP_BC5)
    return DecodeNormalBC5(encodedNormal);
#else
    return DecodeNormalRGB(encodedNormal);
#endif
}

float3 SampleNormal(float2 uv, float3 baseNormalWS, float4 tangentWS, float3 positionWS)
{
#ifdef KROM_NORMAL_MAP
    float3 mapN = SafeNormalize(SampleDecodedNormal(normal, sLinear, uv));
    mapN.xy *= normalStrength;
    mapN.z = sqrt(saturate(1.0f - dot(mapN.xy, mapN.xy)));
    mapN = SafeNormalize(mapN);

    float3 T = tangentWS.xyz;
    float tLen2 = dot(T, T);
    if (tLen2 > 1e-8f)
    {
        T *= rsqrt(tLen2);
        T = T - dot(T, baseNormalWS) * baseNormalWS;
        const float tOrthoLen2 = dot(T, T);
        if (tOrthoLen2 > 1e-8f)
        {
            T *= rsqrt(tOrthoLen2);
            const float handedness = (abs(tangentWS.w) > 0.5f) ? tangentWS.w : 1.0f;
            const float3 B = SafeNormalize(cross(baseNormalWS, T)) * handedness;
            float3 shadedN = SafeNormalize(mapN.x * T + mapN.y * B + mapN.z * baseNormalWS);
            const float NoNg = dot(shadedN, baseNormalWS);
            if (NoNg < 0.0f)
                shadedN = SafeNormalize(shadedN - 2.0f * NoNg * baseNormalWS);
            return shadedN;
        }
    }

    float3 dp1 = ddx(positionWS);
    float3 dp2 = ddy(positionWS);
    float2 duv1 = ddx(uv);
    float2 duv2 = ddy(uv);

    float3 dp2perp = cross(dp2, baseNormalWS);
    float3 dp1perp = cross(baseNormalWS, dp1);

    float3 T_raw = dp2perp * duv1.x + dp1perp * duv2.x;
    float3 B_raw = dp2perp * duv1.y + dp1perp * duv2.y;

    const float derivTLen2 = dot(T_raw, T_raw);
    const float dpLen2 = max(dot(dp1, dp1), dot(dp2, dp2));
    const float uvLen2 = max(dot(duv1, duv1), dot(duv2, duv2));
    const float relRef = dpLen2 * uvLen2;

    if (!(derivTLen2 > relRef * 1e-6f && relRef > 0.0f))
        return baseNormalWS;

    T = T_raw * rsqrt(derivTLen2);
    float3 T_gs = T - dot(T, baseNormalWS) * baseNormalWS;
    float tgs2 = dot(T_gs, T_gs);
    T = (tgs2 > 1e-6f) ? (T_gs * rsqrt(tgs2)) : T;

    float3 B_cross = cross(baseNormalWS, T);
    float handedness = (dot(B_cross, B_raw) >= 0.0f) ? 1.0f : -1.0f;
    float3 B = B_cross * handedness;

    float3 shadedN = SafeNormalize(mapN.x * T + mapN.y * B + mapN.z * baseNormalWS);
    float NoNg = dot(shadedN, baseNormalWS);
    if (NoNg < 0.0f)
        shadedN = SafeNormalize(shadedN - 2.0f * NoNg * baseNormalWS);
    return shadedN;
#else
    return baseNormalWS;
#endif
}

float ApplySpecularAA(float3 shadingNormalWS, float roughness)
{
#ifdef KROM_NORMAL_MAP
    float3 dndx = ddx(shadingNormalWS);
    float3 dndy = ddy(shadingNormalWS);
    float variance = max(dot(dndx, dndx), dot(dndy, dndy));
    variance = min(variance, 0.18f);
    float roughness2 = roughness * roughness;
    return clamp(sqrt(roughness2 + variance), 0.0f, 1.0f);
#else
    return roughness;
#endif
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

    float NoL = saturate(dot(normalWS, lightDirWS));
    float bias = shadowBias + (1.0f - NoL) * shadowNormalBias;

    float visibility = 0.0f;
    [unroll]
    for (int y = -1; y <= 1; ++y)
    {
        [unroll]
        for (int x = -1; x <= 1; ++x)
        {
            float2 offset = float2((float)x, (float)y) * shadowTexelSize;
            visibility += CompareShadowManualBilinear(uv + offset, depth - bias);
        }
    }

    visibility *= (1.0f / 9.0f);
    return lerp(1.0f, visibility, saturate(shadowStrength));
}

float3 EvalSpecularGGX(float3 N, float3 V, float3 L, float3 albedoValue, float metallic, float roughness)
{
    float3 H = SafeNormalize(V + L);
    float NoV = max(dot(N, V), 1e-4f);
    float NoL = max(dot(N, L), 0.0f);
    float NoH = max(dot(N, H), 0.0f);
    float HoV = max(dot(H, V), 0.0f);

    float3 F0 = lerp(float3(0.04f, 0.04f, 0.04f), albedoValue, metallic);
    float3 F = F_Schlick(HoV, F0);
    float D = D_GGX(NoH, roughness);
    float G = G_Smith(NoV, NoL, roughness);

    return (D * G * F) / max(4.0f * NoV * NoL, 1e-4f) * NoL;
}

float3 SamplePrefilteredIBL(float3 R, float roughness)
{
    float3 dRdx = ddx(R);
    float3 dRdy = ddy(R);
    float reflectionVariance = max(dot(dRdx, dRdx), dot(dRdy, dRdy));
    float reflectionMipBias = clamp(0.5f * log2(1.0f + reflectionVariance * 512.0f), 0.0f, 2.0f);
    float lod = roughness * max(iblPrefilterLevels - 1.0f, 0.0f) + reflectionMipBias;
    return tIBLPrefiltered.SampleLevel(sClamp, SafeNormalize(R), lod).rgb;
}

float4 main(PSInput IN) : SV_Target
{
#ifdef KROM_BASECOLOR_MAP
    float4 albedo4 = albedo.Sample(sLinear, IN.texCoord) * baseColorFactor;
#else
    float4 albedo4 = baseColorFactor;
#endif

    float3 albedoValue = albedo4.rgb;
    float opacity = albedo4.a * opacityFactor;
#ifdef KROM_ALPHA_TEST
    clip(opacity - alphaCutoff);
#endif

    float metallic = saturate(metallicFactor);
    float roughness = saturate(roughnessFactor);
    float ao = 1.0f;
#ifdef KROM_ORM_MAP
    float3 ormSample = orm.Sample(sLinear, IN.texCoord).rgb;
    ao        = lerp(1.0f, ormSample.r, saturate(occlusionStrength));
    roughness = saturate(ormSample.g * roughnessFactor);
    metallic  = saturate(ormSample.b * metallicFactor);
#elif defined(KROM_CHANNEL_MAP)
    // channel < 0 means "use scalar constant directly, no texture sampling for this slot"
    float4 maskSample = orm.Sample(sLinear, IN.texCoord);
    ao        = (occlusionChannel >= 0) ? saturate(SampleChannel(maskSample, occlusionChannel) * occlusionStrength + occlusionBias)
                                        : saturate(occlusionStrength);
    roughness = (roughnessChannel >= 0) ? saturate(SampleChannel(maskSample, roughnessChannel) * roughnessFactor + roughnessBias)
                                        : saturate(roughnessFactor);
    metallic  = (metallicChannel  >= 0) ? saturate(SampleChannel(maskSample, metallicChannel)  * metallicFactor  + metallicBias)
                                        : saturate(metallicFactor);
#endif
    roughness = clamp(roughness, 0.04f, 1.0f);

    float3 geomN = SafeNormalize(IN.normalWS);
    float3 N = SampleNormal(IN.texCoord, geomN, IN.tangentWS, IN.positionWS);
    roughness = ApplySpecularAA(N, roughness);
    float3 V = SafeNormalize(cameraPositionWS.xyz - IN.positionWS);
    float NoV = max(dot(N, V), 1e-4f);

    float3 F0 = lerp(float3(0.04f, 0.04f, 0.04f), albedoValue, metallic);
    float3 F_a = F_SchlickRoughness(NoV, F0, roughness);
    float3 kD_a = (1.0f.xxx - F_a) * (1.0f - metallic);

#ifdef KROM_IBL
    float3 R = reflect(-V, N);
    float3 irradiance = tIBLIrradiance.Sample(sClamp, N).rgb;
    float3 prefilt = SamplePrefilteredIBL(R, roughness);
    float2 brdfSamp = tBRDFLut.Sample(sClamp, float2(NoV, roughness)).rg;
    float3 specIBL = prefilt * (F_a * brdfSamp.x + brdfSamp.y);
    float3 diffuseIBL = kD_a * albedoValue * irradiance;
    float3 ambientIBL = (diffuseIBL + specIBL) * ao;
#else
    float3 ambientIBL = ambientColor.rgb * ambientColor.a * albedoValue * ao;
#endif

    float3 lighting = ambientIBL;
    float shadowVisibility = 1.0f;
    if (lightCount > 0u && lights[0].params.w < 0.5f)
        shadowVisibility = SampleDirectionalShadow(IN.positionLightCS, geomN, SafeNormalize(-lights[0].positionWS.xyz));

    [loop]
    for (uint i = 0; i < lightCount; ++i)
    {
        const float type = lights[i].params.w;
        float3 L = float3(0.0f, 1.0f, 0.0f);
        float attenuation = 1.0f;

        if (type < 0.5f)
        {
            L = SafeNormalize(-lights[i].positionWS.xyz);
            attenuation *= shadowVisibility;
        }
        else
        {
            float3 toL = lights[i].positionWS.xyz - IN.positionWS;
            float dist = length(toL);
            L = (dist > 1e-5f) ? (toL / dist) : float3(0.0f, 1.0f, 0.0f);
            attenuation = CalcAttenuation(dist, lights[i].params.z);
            if (type > 1.5f)
                attenuation *= CalcSpotAttenuation(L, SafeNormalize(lights[i].directionWS.xyz),
                                                   lights[i].params.x, lights[i].params.y);
        }

        float3 lightColor = lights[i].colorIntensity.rgb * lights[i].colorIntensity.w * attenuation;
        float NoL_diff = max(dot(N, L), 0.0f);
        float3 diffuse = (kD_a * albedoValue / PI) * NoL_diff;
        float3 specular = EvalSpecularGGX(N, V, L, albedoValue, metallic, roughness);
        lighting += (diffuse + specular) * lightColor;
    }

    float3 emissiveColor = emissiveFactor.rgb;
#ifdef KROM_EMISSIVE_MAP
    emissiveColor *= emissive.Sample(sLinear, IN.texCoord).rgb;
#endif

    return float4(lighting + emissiveColor, opacity);
}
