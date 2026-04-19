// =============================================================================
// KROM Engine — assets/pbr_lit.ps.hlsl
// Pixel Shader: PBR-Lit (HLSL)
// Targets: DX11 (DXBC), DX12 (DXIL), Vulkan (SPIR-V via DXC -spirv)
//
// Farb-/Lichtvertrag:
//   - BaseColor / Emissive werden ueber Hardware-sRGB gelesen.
//   - ORM / Normal / BRDF LUT / HDR-IBL bleiben linear.
//   - Dieser Pass schreibt lineares HDR in den Scene-Color-Render-Target.
//   - Tonemap + finale sRGB-Ausgabe passieren spaeter im Present-/Tonemap-Pass.
//
// IBL-Vertrag (ShaderBindingModel.hpp TexSlots):
//   t5  = diffuse irradiance (2D equirect, bereits vorkonvolviert)
//   t6  = specular prefiltered environment (2D equirect, Mip-Kette = roughness)
//   t7  = BRDF integration LUT (RG in linear space)
// =============================================================================

Texture2D albedo          : register(t0);
Texture2D normal          : register(t1);
Texture2D orm             : register(t2);
Texture2D emissive        : register(t3);
Texture2D tIBLIrradiance  : register(t5);
Texture2D tIBLPrefiltered : register(t6);
Texture2D tBRDFLut        : register(t7);

SamplerState sLinear : register(s0);
SamplerState sClamp  : register(s1);

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
    float4 positionCS : SV_Position;
    float3 positionWS : TEXCOORD1;
    float3 normalWS   : TEXCOORD2;
    float4 tangentWS  : TEXCOORD3;
    float2 uv         : TEXCOORD0;
};

static const float PI     = 3.14159265358979323846f;
static const float TWO_PI = 6.28318530717958647692f;

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

float3 EvalBRDF_GGX(float3 N, float3 V, float3 L, float3 baseAlbedo, float metallic, float roughness)
{
    float3 H = normalize(V + L);
    float NoV = max(dot(N, V), 1e-4f);
    float NoL = max(dot(N, L), 0.0f);
    float NoH = max(dot(N, H), 0.0f);
    float HoV = max(dot(H, V), 0.0f);

    float3 F0 = lerp(float3(0.04f, 0.04f, 0.04f), baseAlbedo, metallic);
    float3 F = F_Schlick(HoV, F0);
    float D = D_GGX(NoH, roughness);
    float G = G_Smith(NoV, NoL, roughness);

    float3 spec = (D * G * F) / max(4.0f * NoV * NoL, 1e-4f);
    float3 kD = (1.0f - F) * (1.0f - metallic);
    float3 diff = kD * baseAlbedo / PI;
    return (diff + spec) * NoL;
}

float2 SphereUV(float3 dir)
{
    dir = normalize(dir);
    float u = atan2(dir.z, dir.x) / TWO_PI + 0.5f;
    float v = acos(clamp(dir.y, -1.0f, 1.0f)) / PI;
    return float2(frac(u), clamp(v, 0.0f, 1.0f));
}

float3 SafeNormalize(float3 v)
{
    float len2 = dot(v, v);
    return (len2 > 1e-12f) ? (v * rsqrt(len2)) : float3(0.0f, 0.0f, 1.0f);
}

float3x3 BuildTBN(float3 N, float4 tangentWS)
{
    float3 T = tangentWS.xyz;
    float handedness = tangentWS.w;
    if (dot(T, T) <= 1e-10f)
    {
        float3 up = (abs(N.y) < 0.999f) ? float3(0.0f, 1.0f, 0.0f) : float3(1.0f, 0.0f, 0.0f);
        T = normalize(cross(up, N));
    }
    else
    {
        T = normalize(T - N * dot(N, T));
    }

    float3 B = normalize(cross(N, T)) * handedness;
    return float3x3(T, B, N);
}

float3 SampleNormal(float2 uv, float3 baseNormalWS, float4 tangentWS)
{
#if defined(KROM_NORMAL_MAP)
    float3 mapN = normal.Sample(sLinear, uv).xyz * 2.0f - 1.0f;
    float3x3 tbn = BuildTBN(baseNormalWS, tangentWS);
    return SafeNormalize(mul(mapN, tbn));
#else
    return baseNormalWS;
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

float3 SamplePrefilteredIBL(float3 R, float roughness)
{
    return tIBLPrefiltered.SampleLevel(sClamp, SphereUV(R), roughness * iblPrefilterLevels).rgb;
}

float4 main(PSInput IN) : SV_Target
{
#ifdef KROM_BASECOLOR_MAP
    float4 albedo4 = albedo.Sample(sLinear, IN.uv) * baseColorFactor;
#else
    float4 albedo4 = baseColorFactor;
#endif

    float3 baseAlbedo = albedo4.rgb;
    float opacity = albedo4.a * opacityFactor;
#ifdef KROM_ALPHA_TEST
    if (opacity < alphaCutoff)
        discard;
#endif

#ifdef KROM_UNLIT
    float3 emissiveColor = emissiveFactor.rgb;
#ifdef KROM_EMISSIVE_MAP
    emissiveColor *= emissive.Sample(sLinear, IN.uv).rgb;
#endif
    return float4(baseAlbedo + emissiveColor, opacity);
#endif

    float metallic  = saturate(metallicFactor);
    float roughness = saturate(roughnessFactor);
    float ao = 1.0f;
#ifdef KROM_ORM_MAP
    float3 ormSample = orm.Sample(sLinear, IN.uv).rgb;
    ao        = lerp(1.0f, ormSample.r, saturate(occlusionStrength));
    roughness = saturate(ormSample.g * roughnessFactor);
    metallic  = saturate(ormSample.b * metallicFactor);
#endif
    roughness = clamp(roughness, 0.04f, 1.0f);

    float3 N = SafeNormalize(IN.normalWS);
    N = SampleNormal(IN.uv, N, IN.tangentWS);
    float3 V = SafeNormalize(cameraPositionWS.xyz - IN.positionWS);
    float NoV = max(dot(N, V), 1e-4f);
    float3 F0 = lerp(float3(0.04f, 0.04f, 0.04f), baseAlbedo, metallic);
    float3 F_a  = F_SchlickRoughness(NoV, F0, roughness);
    float3 kD_a = (1.0f.xxx - F_a) * (1.0f - metallic);

#ifdef KROM_IBL
    float3 R = reflect(-V, N);
    float3 irradiance = tIBLIrradiance.Sample(sClamp, SphereUV(N)).rgb;
    float3 prefilt    = SamplePrefilteredIBL(R, roughness);
    float2 brdfSamp   = tBRDFLut.Sample(sClamp, float2(NoV, roughness)).rg;
    float3 specIBL    = prefilt * (F_a * brdfSamp.x + brdfSamp.y);
    float3 indirect   = (kD_a * baseAlbedo * irradiance + specIBL) * ao;
#else
    float3 indirect = kD_a * baseAlbedo * ao;
#endif
    indirect *= ambientColor.rgb * ambientColor.a;

    float3 Lo = indirect;
    [loop] for (uint i = 0; i < lightCount; ++i)
    {
        const float type = lights[i].params.w;
        const float4 c = lights[i].colorIntensity;
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
                attenuation *= CalcSpotAttenuation(L, SafeNormalize(lights[i].directionWS.xyz),
                                                   lights[i].params.x, lights[i].params.y);
        }

        Lo += EvalBRDF_GGX(N, V, L, baseAlbedo, metallic, roughness) * (c.rgb * c.a * attenuation);
    }

    float3 emissiveColor = emissiveFactor.rgb;
#ifdef KROM_EMISSIVE_MAP
    emissiveColor *= emissive.Sample(sLinear, IN.uv).rgb;
#endif

    return float4(Lo + emissiveColor, opacity);
}
