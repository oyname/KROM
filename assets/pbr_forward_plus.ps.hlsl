// =============================================================================
// KROM Engine - assets/pbr_forward_plus.ps.hlsl
// Pixel Shader: PBR-Lit Forward+ (HLSL)
// =============================================================================

struct GpuLightData
{
    float4 positionWS;
    float4 directionWS;
    float4 colorIntensity;
    float4 params;
};

struct GpuShadowLightData
{
    float4 meta;
    float4 extra;
};

struct GpuShadowViewData
{
    float4 rect;
    float4x4 viewProj;
};

Texture2D                 albedo          : register(t0);
Texture2D                 normal          : register(t1);
Texture2D                 orm             : register(t2);
Texture2D                 emissive        : register(t3);
Texture2D                 shadowMap       : register(t4);
TextureCube               tIBLIrradiance  : register(t5);
TextureCube               tIBLPrefiltered : register(t6);
Texture2D                 tBRDFLut        : register(t7);
Texture2D                 gtaoAO          : register(t8); // Screen-space AO (GTAO Option B, previous frame)
// Vulkan: BufSRV-Range beginnt bei binding 64 (BindingRegisterRanges::BufSRVBase).
// DX11 nutzt weiterhin die t-Register.
#ifdef __spirv__
[[vk::binding(74)]] StructuredBuffer<GpuLightData> forwardPlusLights;
[[vk::binding(75)]] StructuredBuffer<uint4>        forwardPlusTileHeaders;
[[vk::binding(76)]] StructuredBuffer<uint>         forwardPlusLightIndices;
[[vk::binding(78)]] StructuredBuffer<GpuShadowLightData> shadowLights;
[[vk::binding(79)]] StructuredBuffer<GpuShadowViewData>  shadowViews;
#else
StructuredBuffer<GpuLightData> forwardPlusLights      : register(t10);
StructuredBuffer<uint4>        forwardPlusTileHeaders  : register(t11);
StructuredBuffer<uint>         forwardPlusLightIndices : register(t12);
StructuredBuffer<GpuShadowLightData> shadowLights      : register(t14);
StructuredBuffer<GpuShadowViewData>  shadowViews       : register(t15);
#endif

SamplerState             sLinear       : register(s0);
SamplerState             sClamp        : register(s1);
SamplerState             sPoint        : register(s2);
SamplerComparisonState   shadowSampler : register(s3);

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
    uint         debugFlags;
    uint         shadowFilterMode;
    float        _shadowPad;
    float4       shadowLightMeta[4];
    float4       shadowLightExtra[4];
    float4       shadowViewRect[16];
    float4x4     shadowViewProjArray[16];
    uint         shadowLightCount;
    uint         shadowViewCount;
    uint         shadowBufferLightCount;
    uint         shadowBufferViewCount;
    float4       featureParams[8]; // add-on parameter blocks (FrameConstants::featureParams)
};

// GTAO add-on block (krom-gtao, featureParams slot 0, see kGtaoFrameParameterBlock):
// x = radius, y = bias, z = intensity, w = flags (uint bits)
#define gtaoRadius    (featureParams[0].x)
#define gtaoBias      (featureParams[0].y)
#define gtaoIntensity (featureParams[0].z)
#define gtaoEnabled   (asuint(featureParams[0].w))
// w bits: 1 = sample the AO history, 2 = reproject it (kGtaoFlag*).
// featureParams[3..5] (kGtaoReprojectionFrameParameterBlock): rows x, y, w of the
// view-projection the AO history was rendered with.
#define gtaoReproject ((gtaoEnabled & 2u) != 0u)
#define gtaoPrevRowX  (featureParams[3])
#define gtaoPrevRowY  (featureParams[4])
#define gtaoPrevRowW  (featureParams[5])

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
    float2 uvScale;
    float2 uvOffset;
    float  boxTileSize;
#ifdef KROM_MATERIAL_GRAPH
    float4 graphOutputs;
    float4 graphNodes[16];
    float4 graphUV[8];
#endif
};

struct PSInput
{
    float4 positionCS      : SV_POSITION;
    float3 positionWS      : TEXCOORD1;
    float3 normalWS        : TEXCOORD2;
    float4 tangentWS       : TEXCOORD3;
    float2 texCoord        : TEXCOORD0;
    float3 scaledLocalPosition : TEXCOORD4;
    float2 meshUV : TEXCOORD5;
};

static const float PI = 3.14159265358979323846f;
static const uint DBG_DISABLE_IBL_SPEC  = 1u << 0;
static const uint DBG_DISABLE_IBL       = 1u << 1;
static const uint DBG_DISABLE_SHADOWS   = 1u << 2;
static const uint DBG_DISABLE_AO        = 1u << 3;
static const uint DBG_DISABLE_NORMALMAP = 1u << 4;
static const uint DBG_VIEW_NORMALS      = 1u << 8;
static const uint DBG_VIEW_NOL          = 1u << 9;
static const uint DBG_VIEW_ROUGHNESS    = 1u << 10;
static const uint DBG_VIEW_METALLIC     = 1u << 11;
static const uint DBG_VIEW_AO           = 1u << 12;
static const uint DBG_VIEW_SHADOW       = 1u << 13;
static const uint DBG_VIEW_DIRECT_DIFF  = 1u << 14;
static const uint DBG_VIEW_DIRECT_SPEC  = 1u << 15;
static const uint DBG_VIEW_IBL_DIFF     = 1u << 16;
static const uint DBG_VIEW_IBL_SPEC     = 1u << 17;
static const uint DBG_VIEW_FRESNEL_F0   = 1u << 18;
static const uint DBG_VIEW_GTAO         = 1u << 20;
static const uint DBG_DISABLE_GTAO      = 1u << 21;
static const uint FORWARD_PLUS_TILE_SIZE = 16u;

float3 SafeNormalize(float3 v)
{
    float len2 = dot(v, v);
    return (len2 > 1e-12f) ? (v * rsqrt(len2)) : float3(0.0f, 0.0f, 1.0f);
}

#include "pbr_shading_options.hlsl"

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
    float3 result = baseNormalWS;
#ifdef KROM_NORMAL_MAP
    float3 mapN = SafeNormalize(SampleDecodedNormal(normal, sLinear, uv));
    mapN.xy *= normalStrength;
    mapN.z = sqrt(saturate(1.0f - dot(mapN.xy, mapN.xy)));
    mapN = SafeNormalize(mapN);

    float3 T = tangentWS.xyz;
    float tLen2 = dot(T, T);
    bool usedTangentFrame = false;

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
            result = shadedN;
            usedTangentFrame = true;
        }
    }

    if (!usedTangentFrame)
    {
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

        if (derivTLen2 > relRef * 1e-6f && relRef > 0.0f)
        {
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
            result = shadedN;
        }
    }
#endif
    return result;
}


float ApplySpecularAA(float3 shadingNormalWS, float roughness)
{
#ifdef KROM_NORMAL_MAP
    float3 dndx = ddx(shadingNormalWS);
    float3 dndy = ddy(shadingNormalWS);
    float variance = max(dot(dndx, dndx), dot(dndy, dndy));
    if (!(variance >= 0.0f))
        return roughness;
    variance = min(variance, 0.18f);
    float roughness2 = roughness * roughness;
    float aaRoughness = sqrt(max(roughness2 + variance, 0.0f));
    if (!(aaRoughness >= 0.0f))
        return roughness;
    return clamp(aaRoughness, 0.0f, 1.0f);
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

uint ChooseCascadeIndexFromViewDepth(float3 positionWS, uint viewCount, float maxShadowDistance, float cascadeLambdaValue)
{
    const uint cascadeCount = min(viewCount, 4u);
    if (cascadeCount <= 1u)
        return 0u;

    const float viewDepth = max(-mul(viewMatrix, float4(positionWS, 1.0f)).z, nearPlane);
    const float camNear = max(nearPlane, 1e-4f);
    const float camFar = min(farPlane, max(maxShadowDistance, camNear + 1.0f));
    const float lambda = saturate(cascadeLambdaValue);
    const float countF = (float)cascadeCount;

    [loop]
    for (uint splitIndex = 1u; splitIndex < cascadeCount; ++splitIndex)
    {
        const float fi = (float)splitIndex / countF;
        const float splitLog = camNear * pow(camFar / camNear, fi);
        const float splitUniform = camNear + (camFar - camNear) * fi;
        const float splitDepth = lerp(splitUniform, splitLog, lambda);
        if (viewDepth <= splitDepth)
            return splitIndex - 1u;
    }
    return cascadeCount - 1u;
}

float CompareShadowManual(float2 uv, float cmpDepth)
{
    float sampledDepth = shadowMap.SampleLevel(sPoint, uv, 0).r;
    return (cmpDepth <= sampledDepth) ? 1.0f : 0.0f;
}

float CompareShadowManualBilinear(float2 uv, float cmpDepth, float4 atlasRect)
{
    float texelSize = max(shadowTexelSize, 1e-8f);
    float invTexelSize = 1.0f / texelSize;
    float halfTexel = texelSize * 0.5f;
    float2 minUv = atlasRect.xy + halfTexel.xx;
    float2 maxUv = atlasRect.xy + atlasRect.zw - halfTexel.xx;
    uv = clamp(uv, minUv, maxUv);
    float2 texelPos = uv * invTexelSize - 0.5f;
    float2 baseTexel = floor(texelPos);
    float2 fracTexel = frac(texelPos);
    float2 uv00 = (baseTexel + float2(0.5f, 0.5f)) * texelSize;
    float2 uv10 = uv00 + float2(texelSize, 0.0f);
    float2 uv01 = uv00 + float2(0.0f, texelSize);
    float2 uv11 = uv00 + float2(texelSize, texelSize);
    uv00 = clamp(uv00, minUv, maxUv);
    uv10 = clamp(uv10, minUv, maxUv);
    uv01 = clamp(uv01, minUv, maxUv);
    uv11 = clamp(uv11, minUv, maxUv);
    float c00 = CompareShadowManual(uv00, cmpDepth);
    float c10 = CompareShadowManual(uv10, cmpDepth);
    float c01 = CompareShadowManual(uv01, cmpDepth);
    float c11 = CompareShadowManual(uv11, cmpDepth);
    float cx0 = lerp(c00, c10, fracTexel.x);
    float cx1 = lerp(c01, c11, fracTexel.x);
    return lerp(cx0, cx1, fracTexel.y);
}

float4 ComputeShadowReceiverCS(float3 positionWS, float3 normalWS, float4x4 shadowVP, float normalBiasValue)
{
    float3 offsetPositionWS = positionWS + normalWS * normalBiasValue;
    return mul(shadowVP, float4(offsetPositionWS, 1.0f));
}

uint ChoosePointShadowFace(float3 lightToPoint)
{
    const float3 axis = abs(lightToPoint);
    if (axis.x >= axis.y && axis.x >= axis.z)
        return lightToPoint.x >= 0.0f ? 0u : 1u;
    if (axis.y >= axis.z)
        return lightToPoint.y >= 0.0f ? 2u : 3u;
    return lightToPoint.z >= 0.0f ? 4u : 5u;
}

#include "pbr_shadow_poisson.hlsl"

float SampleShadowAtlas(float4 positionLightCS, float biasValue, float strengthValue, float4 atlasRect)
{
    float result = 1.0f;

    if (shadowCascadeCount != 0u && strengthValue > 0.0f && positionLightCS.w > 1e-6f)
    {
        float3 posNDC = positionLightCS.xyz / positionLightCS.w;
        float2 localUv = float2(posNDC.x * 0.5f + 0.5f, 0.5f - posNDC.y * 0.5f);
        float depth = posNDC.z;

        const bool insideAtlas =
            localUv.x >= 0.0f && localUv.x <= 1.0f &&
            localUv.y >= 0.0f && localUv.y <= 1.0f &&
            depth > 0.0f && depth < 1.0f;

        if (insideAtlas)
        {
            float2 atlasUv = atlasRect.xy + localUv * atlasRect.zw;
            float ddxDepth = ddx(depth);
            float ddyDepth = ddy(depth);
            float ddxUvX = ddx(atlasUv.x);
            float ddyUvY = ddy(atlasUv.y);
            float2 dDepth_dUV = float2(
                abs(ddxUvX) > 1e-7f ? ddxDepth / ddxUvX : 0.0f,
                abs(ddyUvY) > 1e-7f ? ddyDepth / ddyUvY : 0.0f
            );
            dDepth_dUV = clamp(dDepth_dUV, -1.0f, 1.0f);
            // Nur ein winziger Epsilon-Floor gegen reines Z-Fighting — der konstante
            // Bias wird vom Slider gesteuert. Acne faengt der slope-scaled Term
            // (dDepth_dUV) adaptiv ab; ein grosser Floor wuerde Peter-Panning erzwingen.
            float baseBias = max(biasValue, 0.00005f);
            float visibility = 0.0f;

            if (shadowFilterMode == 1u)
            {
                [unroll]
                for (int y = 0; y <= 1; ++y)
                {
                    [unroll]
                    for (int x = 0; x <= 1; ++x)
                    {
                        float2 offset = float2((float)x - 0.5f, (float)y - 0.5f) * shadowTexelSize;
                        float depthRef = depth + dot(dDepth_dUV, offset) - baseBias;
                        visibility += CompareShadowManualBilinear(atlasUv + offset, depthRef, atlasRect);
                    }
                }
                visibility *= 0.25f;
            }
            else if (shadowFilterMode == 3u)
            {
                visibility = SampleShadowPoisson(atlasUv, depth, dDepth_dUV, baseBias, atlasRect);
            }
            else if (shadowFilterMode == 4u)
            {
                visibility = SampleShadowPcss(atlasUv, depth, dDepth_dUV, baseBias, atlasRect);
            }
            else if (shadowFilterMode >= 2u)
            {
                [unroll]
                for (int y = -1; y <= 1; ++y)
                {
                    [unroll]
                    for (int x = -1; x <= 1; ++x)
                    {
                        float2 offset = float2((float)x, (float)y) * shadowTexelSize;
                        float depthRef = depth + dot(dDepth_dUV, offset) - baseBias;
                        visibility += CompareShadowManualBilinear(atlasUv + offset, depthRef, atlasRect);
                    }
                }
                visibility *= (1.0f / 9.0f);
            }
            else
            {
                float depthRef = depth - baseBias;
                visibility = CompareShadowManualBilinear(atlasUv, depthRef, atlasRect);
            }

            result = lerp(1.0f, visibility, saturate(strengthValue));
        }
    }

    return result;
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
    if (shadingHeightCorrelatedVisibility)
        return max(D * V_SmithGGXCorrelated(NoV, NoL, roughness) * F * NoL, 0.0f.xxx);
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

uint2 ComputeTileCoord(float2 pixelPos)
{
    uint tileCountX = max(1u, (uint(screenSize.x) + FORWARD_PLUS_TILE_SIZE - 1u) / FORWARD_PLUS_TILE_SIZE);
    uint tileCountY = max(1u, (uint(screenSize.y) + FORWARD_PLUS_TILE_SIZE - 1u) / FORWARD_PLUS_TILE_SIZE);
    uint tileX = min(tileCountX - 1u, uint(pixelPos.x) / FORWARD_PLUS_TILE_SIZE);
    uint tileY = min(tileCountY - 1u, uint(pixelPos.y) / FORWARD_PLUS_TILE_SIZE);
    return uint2(tileX, tileY);
}

#include "pbr_box_projection.hlsl"

#include "pbr_local_probe.hlsl"

#include "pbr_material_graph.hlsl"

float4 main(PSInput IN) : SV_Target
{
    gKromShadowPixelPos = IN.positionCS.xy;
    float2 uv = KromPbrUV(IN.texCoord, IN.scaledLocalPosition, boxTileSize);
#ifdef KROM_MATERIAL_GRAPH
    EvaluateMaterialGraph(boxTileSize > 0.0f ? uv : IN.meshUV, IN.positionWS);
    if (boxTileSize <= 0.0f)
        uv = KromGraphDirectUV(uv, IN.meshUV, uvScale, uvOffset);
#endif
#ifdef KROM_BASECOLOR_MAP
    float4 albedo4 = albedo.Sample(sLinear, uv) * baseColorFactor;
#else
    float4 albedo4 = baseColorFactor;
#endif
#ifdef KROM_MATERIAL_GRAPH
    float3 graphEmission;
    ApplyMaterialGraph(albedo4, graphEmission);
    // A UV node linked to the normal or ORM texture node moves those maps too.
    const float2 ormUV = KromGraphOrmUV(uv);
    const float2 normalUV = KromGraphNormalUV(uv);
#else
    const float2 ormUV = uv;
    const float2 normalUV = uv;
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
    float3 ormSample = orm.Sample(sLinear, ormUV).rgb;
    ao = lerp(1.0f, ormSample.r, saturate(occlusionStrength));
    roughness = saturate(ormSample.g * roughnessFactor);
    metallic = saturate(ormSample.b * metallicFactor);
#elif defined(KROM_CHANNEL_MAP)
    float4 maskSample = orm.Sample(sLinear, ormUV);
    ao = (occlusionChannel >= 0) ? saturate(SampleChannel(maskSample, occlusionChannel) * occlusionStrength + occlusionBias)
                                 : saturate(occlusionStrength);
    roughness = (roughnessChannel >= 0) ? saturate(SampleChannel(maskSample, roughnessChannel) * roughnessFactor + roughnessBias)
                                        : saturate(roughnessFactor);
    metallic = (metallicChannel >= 0) ? saturate(SampleChannel(maskSample, metallicChannel) * metallicFactor + metallicBias)
                                      : saturate(metallicFactor);
#endif
#ifdef KROM_MATERIAL_GRAPH
    metallic = KromGraphMetallic(metallic);
    roughness = KromGraphRoughness(roughness);
#endif
    roughness = clamp(roughness, 0.04f, 1.0f);
    if (debugFlags & DBG_DISABLE_AO)
        ao = 1.0f;

    float3 geomN = SafeNormalize(IN.normalWS);
    // A zero tangent selects the frame derived from the sampled UVs.
    bool deriveNormalFrame = boxTileSize > 0.0f;
#ifdef KROM_MATERIAL_GRAPH
    deriveNormalFrame = deriveNormalFrame || KromGraphNormalUsesDerivedFrame();
#endif
    float4 tangentWS = deriveNormalFrame ? 0.0f : IN.tangentWS;
    float3 N = (debugFlags & DBG_DISABLE_NORMALMAP) ? geomN : SampleNormal(normalUV, geomN, tangentWS, IN.positionWS);
    roughness = ApplySpecularAA(N, roughness);

    float3 V = SafeNormalize(cameraPositionWS.xyz - IN.positionWS);
    float NoV = max(dot(N, V), 1e-4f);
    float3 F0 = lerp(float3(0.04f, 0.04f, 0.04f), albedoValue, metallic);
    float3 F_a = F_SchlickRoughness(NoV, F0, roughness);
    float3 kD_a = (1.0f.xxx - F_a) * (1.0f - metallic);

    // Multi-scatter energy compensation for the specular lobes (shading option).
    float3 energyCompensation = float3(1.0f, 1.0f, 1.0f);
#ifdef KROM_IBL
    if (shadingMultiScatter)
        energyCompensation = KromEnergyCompensation(F0, tBRDFLut.SampleLevel(sClamp, float2(NoV, roughness), 0).rg);
#endif

    float3 iblDiffuse = float3(0.0f, 0.0f, 0.0f);
    float3 iblSpecular = float3(0.0f, 0.0f, 0.0f);
LocalProbeIBL localProbes;
    localProbes.irradiance = float3(0.0f, 0.0f, 0.0f);
    localProbes.specular = float3(0.0f, 0.0f, 0.0f);
    localProbes.remaining = 1.0f;
    localProbes.firstProbe = -1;
#ifdef KROM_IBL
    if (debugFlags & DBG_DISABLE_IBL)
    {
        iblDiffuse = ambientColor.rgb * ambientColor.a * albedoValue;
    }
    else
    {
        // Local probes: blended in priority order, the rest from the global IBL.
        float3 irradiance = tIBLIrradiance.Sample(sClamp, N).rgb;
        if (probeCount != 0u)
        {
            const float3 probeR = reflect(-V, N);
            localProbes = EvaluateLocalProbes(IN.positionWS, N, probeR, roughness,
                                              LocalProbeReflectionMipBias(probeR));
            irradiance = localProbes.irradiance + irradiance * localProbes.remaining;
        }
        // Lambert: irradiance E times albedo / pi, as for the direct light.
        iblDiffuse = kD_a * albedoValue * irradiance / PI;
        if (!(debugFlags & DBG_DISABLE_IBL_SPEC))
        {
            float3 R = reflect(-V, N);
            float3 prefilt = SamplePrefilteredIBL(R, roughness);
            if (probeCount != 0u)
                prefilt = localProbes.specular + prefilt * localProbes.remaining;
            float2 brdfSamp = tBRDFLut.Sample(sClamp, float2(NoV, roughness)).rg;
            iblSpecular = prefilt * (F_a * brdfSamp.x + brdfSamp.y) * energyCompensation;
        }
    }
#else
    iblDiffuse = ambientColor.rgb * ambientColor.a * albedoValue;
#endif
    float3 ambientIBL = (iblDiffuse + iblSpecular) * ao;

    // Screen-space ambient occlusion (GTAO Option B): attenuate ONLY the indirect
    // ambient term so direct lighting and emissive stay untouched. Sampled from
    // the previous frame's AO history: at this pixel's screen UV, or with
    // reprojection where the surface point was in that frame.
    float gtaoFactor = 1.0f;
    if (gtaoEnabled != 0u && !(debugFlags & DBG_DISABLE_GTAO) && !(debugFlags & DBG_DISABLE_AO))
    {
        float2 aoUV  = IN.positionCS.xy / max(screenSize.xy, float2(1.0f, 1.0f));
        if (gtaoReproject)
        {
            // Where this surface point was on screen when the AO history was rendered.
            // Outside that frame (or behind its camera) there is no history: keep
            // the same screen position.
            const float4 historyP = float4(IN.positionWS, 1.0f);
            const float  historyW = dot(gtaoPrevRowW, historyP);
            if (historyW > 1e-4f)
            {
                const float2 historyNdc = float2(dot(gtaoPrevRowX, historyP), dot(gtaoPrevRowY, historyP)) / historyW;
                const float2 historyUV  = float2(historyNdc.x * 0.5f + 0.5f, 0.5f - historyNdc.y * 0.5f);
                if (all(historyUV >= 0.0f.xx) && all(historyUV <= 1.0f.xx))
                    aoUV = historyUV;
            }
        }
        float  rawAO = gtaoAO.SampleLevel(sClamp, aoUV, 0).r;
        gtaoFactor   = lerp(1.0f, rawAO, saturate(gtaoIntensity));
        ambientIBL  *= gtaoFactor;
    }

    // Specular occlusion (krom-probes switch): the specular IBL gets its own
    // factor from AO, roughness and NoV instead of the diffuse AO factor.
    float specularOcclusion = ao * gtaoFactor;
    if (probeSpecularOcclusionEnabled)
    {
        specularOcclusion = ComputeSpecularOcclusion(NoV, ao * gtaoFactor, roughness);
        ambientIBL = iblDiffuse * (ao * gtaoFactor) + iblSpecular * specularOcclusion;
    }

    float3 totalDirectDiffuse = float3(0.0f, 0.0f, 0.0f);
    float3 totalDirectSpecular = float3(0.0f, 0.0f, 0.0f);
    float firstNoL = 0.0f;
    float firstShadowVisibility = 1.0f;

    uint tileCountX = max(1u, (uint(screenSize.x) + FORWARD_PLUS_TILE_SIZE - 1u) / FORWARD_PLUS_TILE_SIZE);
    uint2 tileCoord = ComputeTileCoord(IN.positionCS.xy);
    uint tileIndex = tileCoord.y * tileCountX + tileCoord.x;
    uint4 tileHeader = forwardPlusTileHeaders[tileIndex];
    // tileHeader.z = reserved0 = tatsächlich gespeicherte Einträge (Compute-Pfad setzt via InterlockedMax).
    // Nicht tileHeader.y (count) nutzen: der AtomicAdd-Zähler überläuft fpMaxLightsPerTile wenn mehr
    // Lichter das Tile beanspruchen als Platz ist. tileHeader.y begrenzt auch nicht korrekt auf
    // lightCount aus dem PerFrame-CB (max 7 Inline-Lichter ≠ Forward+-Puffer-Größe).
    uint storedLightCount = tileHeader.z;

    [loop]
    for (uint localLightIndex = 0u; localLightIndex < storedLightCount; ++localLightIndex)
    {
        uint lightIndex = forwardPlusLightIndices[tileHeader.x + localLightIndex];
        const GpuLightData light = forwardPlusLights[lightIndex];
        const float type = light.params.w;
        float3 L = float3(0.0f, 1.0f, 0.0f);
        float atten = 1.0f;
        float shadowVisibility = 1.0f;

        if (type < 0.5f)
        {
            L = SafeNormalize(-light.positionWS.xyz);
        }
        else
        {
            float3 toL = light.positionWS.xyz - IN.positionWS;
            float dist = length(toL);
            L = (dist > 1e-5f) ? (toL / dist) : float3(0.0f, 1.0f, 0.0f);
            atten = CalcAttenuation(dist, light.params.z);
            if (type > 1.5f)
                atten *= CalcSpotAttenuation(L, SafeNormalize(light.directionWS.xyz), light.params.x, light.params.y);
        }

        if (!(debugFlags & DBG_DISABLE_SHADOWS) && shadowBufferLightCount > 0u && type < 2.5f)
        {
            [loop]
            for (uint shadowIndex = 0u; shadowIndex < shadowBufferLightCount; ++shadowIndex)
            {
                const GpuShadowLightData shadowLight = shadowLights[shadowIndex];
                const float4 shadowMeta = shadowLight.meta;
                const float4 shadowExtra = shadowLight.extra;
                if ((uint)(shadowMeta.x + 0.5f) != lightIndex)
                    continue;

                const uint firstViewIndex = (uint)(shadowExtra.x + 0.5f);
                const uint viewCount = (uint)(shadowExtra.y + 0.5f);
                if (viewCount >= 6u)
                {
                    const float3 receiverWS = IN.positionWS + geomN * shadowMeta.z;
                    const uint faceIndex = ChoosePointShadowFace(receiverWS - light.positionWS.xyz);
                    const uint viewIndex = firstViewIndex + faceIndex;
                    if (viewIndex < shadowBufferViewCount)
                    {
                        const GpuShadowViewData shadowView = shadowViews[viewIndex];
                        KromPreparePcss(shadowView.viewProj, shadowView.rect, IN.positionWS, L);
                        shadowVisibility = SampleShadowAtlas(
                            mul(shadowView.viewProj, float4(receiverWS, 1.0f)),
                            shadowMeta.y,
                            shadowMeta.w,
                            shadowView.rect);
                    }
                }
                else if (type < 0.5f && viewCount > 1u)
                {
                    // Directional CSM: try cascades from tightest (0) to widest (N-1)
                    const uint cascadeIndex = ChooseCascadeIndexFromViewDepth(
                        IN.positionWS, viewCount, shadowExtra.z, shadowExtra.w);
                    [unroll]
                    for (uint attempt = 0u; attempt < 3u; ++attempt)
                    {
                        uint candidate = cascadeIndex;
                        if (attempt == 1u)
                        {
                            if (cascadeIndex == 0u) continue;
                            candidate = cascadeIndex - 1u;
                        }
                        else if (attempt == 2u)
                        {
                            candidate = cascadeIndex + 1u;
                            if (candidate >= viewCount) continue;
                        }

                        const uint viewIndex = firstViewIndex + candidate;
                        if (viewIndex < shadowBufferViewCount)
                        {
                            const GpuShadowViewData shadowView = shadowViews[viewIndex];
                            const float4 posLCS = ComputeShadowReceiverCS(
                                IN.positionWS, geomN,
                                shadowView.viewProj,
                                shadowMeta.z);
                            const float3 ndcPos = posLCS.xyz / posLCS.w;
                            if (abs(ndcPos.x) < 1.0f && abs(ndcPos.y) < 1.0f &&
                                ndcPos.z >= 0.0f && ndcPos.z < 1.0f)
                            {
                                KromPreparePcss(shadowView.viewProj, shadowView.rect, IN.positionWS, L);
                                shadowVisibility = SampleShadowAtlas(
                                    posLCS,
                                    shadowMeta.y,
                                    shadowMeta.w,
                                    shadowView.rect);
                                break;
                            }
                        }
                    }
                }
                else if (firstViewIndex < shadowBufferViewCount)
                {
                    const GpuShadowViewData shadowView = shadowViews[firstViewIndex];
                    // Spot or single-cascade directional
                    KromPreparePcss(shadowView.viewProj, shadowView.rect, IN.positionWS, L);
                    shadowVisibility = SampleShadowAtlas(
                        ComputeShadowReceiverCS(IN.positionWS, geomN, shadowView.viewProj, shadowMeta.z),
                        shadowMeta.y,
                        shadowMeta.w,
                        shadowView.rect);
                }
                break;
            }
        }

        atten *= shadowVisibility;
        float3 lightColor = light.colorIntensity.rgb * light.colorIntensity.w * atten;
        float NoL_i = max(dot(N, L), 0.0f);
        if (localLightIndex == 0u)
        {
            firstNoL = NoL_i;
            firstShadowVisibility = shadowVisibility;
        }

        // Diffuse weight: view-angle Fresnel for all lights, or per light from the half vector.
        float3 kD_i = kD_a;
        if (shadingPerLightDiffuseFresnel)
            kD_i = (1.0f.xxx - F_Schlick(saturate(dot(SafeNormalize(V + L), V)), F0)) * (1.0f - metallic);
        totalDirectDiffuse += (kD_i * albedoValue / PI) * NoL_i * lightColor;
        totalDirectSpecular += EvalSpecularGGX(N, V, L, albedoValue, metallic, roughness) * energyCompensation * lightColor;
    }

    float3 emissiveColor = emissiveFactor.rgb;
#ifdef KROM_EMISSIVE_MAP
    emissiveColor *= emissive.Sample(sLinear, uv).rgb;
#endif
#ifdef KROM_MATERIAL_GRAPH
    if (graphOutputs.y >= 0.0)
        emissiveColor = graphEmission * emissiveFactor.rgb;
#endif

    float3 finalColor = ambientIBL + totalDirectDiffuse + totalDirectSpecular + emissiveColor;
    if (debugFlags & DBG_VIEW_NORMALS)     return float4(N * 0.5f + 0.5f, 1.0f);
    if (debugFlags & DBG_VIEW_NOL)         return float4(firstNoL.xxx, 1.0f);
    if (debugFlags & DBG_VIEW_ROUGHNESS)   return float4(roughness.xxx, 1.0f);
    if (debugFlags & DBG_VIEW_METALLIC)    return float4(metallic.xxx, 1.0f);
    if (debugFlags & DBG_VIEW_AO)          return float4(ao.xxx, 1.0f);
    if (debugFlags & DBG_VIEW_SHADOW)      return float4(firstShadowVisibility.xxx, 1.0f);
    if (debugFlags & DBG_VIEW_DIRECT_DIFF) return float4(totalDirectDiffuse, 1.0f);
    if (debugFlags & DBG_VIEW_DIRECT_SPEC) return float4(totalDirectSpecular, 1.0f);
    if (debugFlags & DBG_VIEW_IBL_DIFF)    return float4(iblDiffuse, 1.0f);
    if (debugFlags & DBG_VIEW_IBL_SPEC)    return float4(iblSpecular, 1.0f);
    if (debugFlags & DBG_VIEW_FRESNEL_F0)  return float4(F0, 1.0f);
    if (debugFlags & DBG_VIEW_GTAO)        return float4(gtaoFactor.xxx, 1.0f);
    if (probeDebugView == 1u) return float4(LocalProbeDebugColor(localProbes.firstProbe), 1.0f);
    if (probeDebugView == 2u) return float4((1.0f - localProbes.remaining).xxx, 1.0f);
    if (probeDebugView == 3u) return float4(specularOcclusion.xxx, 1.0f);
    return float4(finalColor, opacity);
}
