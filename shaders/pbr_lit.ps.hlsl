// =============================================================================
// KROM Engine — shaders/pbr_lit.ps.hlsl
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
Texture2D                 gtaoAO          : register(t8); // Screen-space AO for this frame
Texture2D                 contactShadowTex : register(t20); // Contact shadow of the main light (opaque pass)

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
    uint         debugFlags;
    uint         shadowFilterMode;
    float        _shadowPad;
    float4       shadowLightMeta[4];
    float4       shadowLightExtra[4];
    float4       shadowViewRect[16];
    float4x4     shadowViewProjArray[16];
    uint         shadowLightCount;
    uint         shadowViewCount;
    float2       _shadowArrayPad;
    float4       featureParams[8]; // add-on parameter blocks (FrameConstants::featureParams)
};

// GTAO add-on block (krom-gtao, featureParams slot 0, see kGtaoFrameParameterBlock):
// x = radius, y = bias, z = intensity, w = flags (uint bits)
#define gtaoRadius    (featureParams[0].x)
#define gtaoBias      (featureParams[0].y)
#define gtaoIntensity (featureParams[0].z)
#define gtaoEnabled   (asuint(featureParams[0].w))

// Contact shadow add-on block (krom-contact-shadow, featureParams slots 3 and 4,
// see kContactShadowFrameParameterBlock): w = index of the main light in lights[] + 1.
#define contactShadowLightPlusOne (asuint(featureParams[3].w))

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
    float2 uvScale;             // byte 88
    float2 uvOffset;            // byte 96
    float  boxTileSize;
    float  clearcoatFactor;     // 0 = no clear coat layer (pbr_clearcoat.hlsl)
    float  clearcoatRoughness;
    float  specularFactor;      // byte 116; 0.5 = dielectric F0 0.04
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

static const float PI     = 3.14159265358979323846f;
static const float TWO_PI = 6.28318530717958647692f;

// DebugFlags - spiegelt engine::renderer::DebugFlags (RenderWorld.hpp)
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
static const uint DBG_VIEW_CONTACT_SHADOW    = 1u << 22;
static const uint DBG_DISABLE_CONTACT_SHADOW = 1u << 23;
static const uint DBG_VIEW_SHADOW_CASCADE    = 1u << 24;

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

#include "pbr_specular.hlsl"

float3 F_Schlick(float HoV, float3 F0, float F90)
{
    return F0 + (F90 - F0) * pow(1.0f - HoV, 5.0f);
}

float3 F_SchlickRoughness(float NoV, float3 F0, float roughness, float F90)
{
    float3 inv = max(((1.0f - roughness) * F90).xxx, F0);
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

#include "pbr_material_graph.hlsl"

float3 SampleNormal(float2 uv, float3 baseNormalWS, float4 tangentWS, float3 positionWS)
{
    float3 result = baseNormalWS;
#if defined(KROM_NORMAL_MAP) || defined(KROM_MATERIAL_GRAPH)
    float3 mapN;
#ifdef KROM_MATERIAL_GRAPH
    if (KromGraphHasNormal())
        mapN = KromGraphNormalStrength(KromGraphNormal(), normalStrength);
    else
#endif
    {
#ifdef KROM_NORMAL_MAP
        mapN = SafeNormalize(SampleDecodedNormal(normal, sLinear, uv));
        mapN.xy *= normalStrength;
        mapN.z = sqrt(saturate(1.0f - dot(mapN.xy, mapN.xy)));
        mapN = SafeNormalize(mapN);
#else
        return baseNormalWS;
#endif
    }

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

        // Remove the screen-derivative orientation before constructing the UV frame.
        // Otherwise both axes can point backwards and turn bumps into cavities.
        const float frameOrientation = dot(cross(dp1, dp2), baseNormalWS) < 0.0f ? -1.0f : 1.0f;
        T_raw *= frameOrientation;
        B_raw *= frameOrientation;

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


// Geometric specular antialiasing (Tokuyoshi & Kaplanyan 2019/2021, after
// Kaplanyan et al. 2016). The screen-space change of the shading normal is
// treated as a normal distribution inside the pixel footprint and added to the
// GGX alpha^2, so highlights narrower than a pixel stop flickering on
// normal-mapped surfaces and on curved glossy geometry. Per pixel, so it acts
// the same with and without MSAA. roughness is perceptual (alpha = r^2).
// Shading option bit 3; off = roughness unchanged.
static const float kSpecularAAVariance = 0.15915494f; // screen-space filter variance (1 / 2 pi)
static const float kSpecularAAMaxAlpha2 = 0.18f;      // upper bound of the added alpha^2

float ApplySpecularAA(float3 shadingNormalWS, float roughness)
{
    // Derivatives outside the option branch (quad-uniform anyway).
    float3 dndx = ddx(shadingNormalWS);
    float3 dndy = ddy(shadingNormalWS);
    float kernelAlpha2 = min(2.0f * kSpecularAAVariance * (dot(dndx, dndx) + dot(dndy, dndy)),
                             kSpecularAAMaxAlpha2);
    // Degenerate derivatives (NaN) keep the material roughness.
    if (!shadingSpecularAA || !(kernelAlpha2 >= 0.0f))
        return roughness;
    float alpha = roughness * roughness;
    float aaRoughness = sqrt(sqrt(saturate(alpha * alpha + kernelAlpha2)));
    if (!(aaRoughness >= 0.0f))
        return roughness;
    return clamp(aaRoughness, roughness, 1.0f);
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

// Width of the band at the far end of each cascade, as a fraction of the cascade's
// depth range, in which the next cascade is mixed in (both filtered). Must match
// kCascadeBlendBand in addons/shadow/ShadowExtraction.cpp, which extends every
// cascade by this band towards the camera so the next cascade covers it.
static const float kKromCascadeBlendBand = 0.2f;

// blendToNext: 0 outside the band, rising to 1 at the far split of the cascade.
// Fraction of the shadow distance over which directional shadows fade out.
static const float kKromShadowFadeBand = 0.1f;

// 0 inside the shadow distance, rising to 1 at its end (camera view depth).
float KromShadowDistanceFade(float3 positionWS, float maxShadowDistance)
{
    const float viewDepth = -mul(viewMatrix, float4(positionWS, 1.0f)).z;
    const float shadowEnd = max(min(farPlane, maxShadowDistance), 1e-3f);
    return saturate((viewDepth - shadowEnd * (1.0f - kKromShadowFadeBand)) / (shadowEnd * kKromShadowFadeBand));
}

uint ChooseCascadeIndexFromViewDepth(float3 positionWS, uint viewCount, float maxShadowDistance,
                                     float cascadeLambdaValue, out float blendToNext)
{
    blendToNext = 0.0f;
    const uint cascadeCount = min(viewCount, 4u);
    if (cascadeCount <= 1u)
        return 0u;

    const float viewDepth = max(-mul(viewMatrix, float4(positionWS, 1.0f)).z, nearPlane);
    const float camNear = max(nearPlane, 1e-4f);
    const float camFar = min(farPlane, max(maxShadowDistance, camNear + 1.0f));
    const float lambda = saturate(cascadeLambdaValue);
    const float countF = (float)cascadeCount;

    float prevSplit = camNear;
    [loop]
    for (uint splitIndex = 1u; splitIndex < cascadeCount; ++splitIndex)
    {
        const float fi = (float)splitIndex / countF;
        const float splitLog = camNear * pow(camFar / camNear, fi);
        const float splitUniform = camNear + (camFar - camNear) * fi;
        const float splitDepth = lerp(splitUniform, splitLog, lambda);
        if (viewDepth <= splitDepth)
        {
            const float toSplit = (splitDepth - viewDepth) / max(splitDepth - prevSplit, 1e-4f);
            blendToNext = saturate(1.0f - toSplit / kKromCascadeBlendBand);
            return splitIndex - 1u;
        }
        prevSplit = splitDepth;
    }
    return cascadeCount - 1u;
}

#if !defined(KROM_SHADOWS_NONE)

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

// Normal offset (world units) of normalBiasTexels shadow-map texels of this
// view at the receiver. The bias is set in texels like in Unity/Unreal, so one
// value fits every cascade: coarse cascades get a larger offset, fine ones a
// smaller one (a fixed world offset was too large near and too small far away).
float KromShadowNormalOffset(float3 positionWS, float4x4 shadowVP, float4 atlasRect, float normalBiasTexels)
{
    const float4 clip = mul(shadowVP, float4(positionWS, 1.0f));
    const float3 xRow = float3(shadowVP[0][0], shadowVP[0][1], shadowVP[0][2]);
    const float texelsPerWorld = 0.5f * length(xRow) * atlasRect.z /
                                 (max(shadowTexelSize, 1e-8f) * max(abs(clip.w), 1e-6f));
    return normalBiasTexels / max(texelsPerWorld, 1e-6f);
}

// Direction to the light being shaded; set before the shadow lookups.
static float3 gKromShadowToLight = float3(0.0f, 1.0f, 0.0f);

float4 ComputeShadowReceiverCS(float3 positionWS, float3 normalWS, float4x4 shadowVP, float normalBiasTexels,
                               float4 atlasRect)
{
    // As in Unity: less offset where the surface faces the light (1 - NdotL);
    // a floor under a low sun keeps most of it, a wall facing the sun none.
    const float facing = 1.0f - saturate(dot(normalWS, gKromShadowToLight));
    const float offset = KromShadowNormalOffset(positionWS, shadowVP, atlasRect, normalBiasTexels) * facing;
    return mul(shadowVP, float4(positionWS + normalWS * offset, 1.0f));
}

#if !defined(KROM_SHADOWS_2D_ONLY)

uint ChoosePointShadowFace(float3 lightToPoint)
{
    float3 axis = abs(lightToPoint);
    if (axis.x >= axis.y && axis.x >= axis.z)
        return lightToPoint.x >= 0.0f ? 0u : 1u;
    if (axis.y >= axis.z)
        return lightToPoint.y >= 0.0f ? 2u : 3u;
    return lightToPoint.z >= 0.0f ? 4u : 5u;
}

#endif

#include "pbr_shadow_poisson.hlsl"

#include "shadow_receiver.hlsl"

// Receiver-plane depth gradient (depth change per atlas UV) for one shadow view.
// Computed analytically from the screen derivatives of the world position
// (gKromShadowDpdx/Dpdy, taken once at the top of main) instead of ddx/ddy of
// the shadow coordinates: neighbouring pixels may use different cascades, and
// derivatives across that seam (or inside the per-pixel sampling loop) are wrong.
static float2 gKromShadowGradient = float2(0.0f, 0.0f);
static float3 gKromShadowDpdx = float3(0.0f, 0.0f, 0.0f);
static float3 gKromShadowDpdy = float3(0.0f, 0.0f, 0.0f);

float2 KromShadowGradientFor(float4 positionLightCS, float4x4 shadowVP, float4 atlasRect)
{
    const float w = abs(positionLightCS.w) > 1e-6f ? positionLightCS.w : 1e-6f;
    const float3 ndc = positionLightCS.xyz / w;
    const float4 cx = mul(shadowVP, float4(gKromShadowDpdx, 0.0f));
    const float4 cy = mul(shadowVP, float4(gKromShadowDpdy, 0.0f));
    const float3 ndcDx = (cx.xyz - ndc * cx.w) / w;
    const float3 ndcDy = (cy.xyz - ndc * cy.w) / w;
    const float2 dx = float2(0.5f * ndcDx.x * atlasRect.z, -0.5f * ndcDx.y * atlasRect.w);
    const float2 dy = float2(0.5f * ndcDy.x * atlasRect.z, -0.5f * ndcDy.y * atlasRect.w);
    const float2 dz = float2(ndcDx.z, ndcDy.z);
    const float det = dx.x * dy.y - dx.y * dy.x;
    const float scale = max(length(dx) * length(dy), 1e-20f);
    if (abs(det) <= 1e-5f * scale)
        return float2(0.0f, 0.0f);
    return float2(dy.y * dz.x - dx.y * dz.y,
                  dx.x * dz.y - dy.x * dz.x) / det;
}

float SampleShadowAtlas(float4 positionLightCS, float biasValue, float strengthValue, float4 atlasRect, float4x4 shadowVP)
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
            // Set by the caller (gKromShadowGradient), see KromShadowGradientFor.
            float2 dDepth_dUV = gKromShadowGradient;
            // Nur ein winziger Epsilon-Floor gegen reines Z-Fighting — der konstante
            // Bias wird vom Slider gesteuert. Acne faengt der slope-scaled Term
            // (dDepth_dUV) adaptiv ab, daher darf baseBias klein sein. Ein grosser
            // Floor wuerde Peter-Panning (Schatten loest sich vom Objekt) erzwingen.
            float baseBias = KromShadowReceiverBias(biasValue, shadowVP);
            gKromShadowPlaneLimit = KromShadowPlaneLimit(shadowVP, atlasRect.z / max(shadowTexelSize, 1e-8f), depth);
            float visibility = 0.0f;

            if (gKromShadowFilter == 3u)
            {
                visibility = SampleShadowPoisson(atlasUv, depth, dDepth_dUV, baseBias, atlasRect);
            }
            else if (gKromShadowFilter == 4u)
            {
                visibility = SampleShadowPcss(atlasUv, depth, dDepth_dUV, baseBias, atlasRect);
            }
            else if (gKromShadowFilter >= 1u)
            {
                // Tent3x3 = 1, Tent5x5 = 2, Tent7x7 = 5 (ShadowFilter).
                const uint tentQuality = gKromShadowFilter == 5u ? 3u : min(gKromShadowFilter, 2u);
                visibility = SampleShadowTent(atlasUv, depth, dDepth_dUV, baseBias, atlasRect, tentQuality);
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

#endif

#include "pbr_clearcoat.hlsl"

float3 EvalSpecularGGX(float3 N, float3 V, float3 L, float3 albedoValue, float metallic, float roughness, float specularLevel)
{
    float NoL = max(dot(N, L), 0.0f);
    if (NoL <= 0.0f)
        return float3(0.0f, 0.0f, 0.0f);

    float3 Hraw = V + L;
    float hLen2 = dot(Hraw, Hraw);
    if (!(hLen2 > 1e-10f))
        return float3(0.0f, 0.0f, 0.0f);

    float3 H = Hraw * rsqrt(hLen2);
    float NoV = max(dot(N, V), 1e-4f);
    float NoH = max(dot(N, H), 0.0f);
    float HoV = saturate(dot(H, V));

    float3 F0 = KromSpecularF0(albedoValue, metallic, specularLevel);
    float F90 = KromSpecularF90(metallic, specularLevel);
    float3 F = F_Schlick(HoV, F0, F90);
    float D = D_GGX(NoH, roughness);
    if (shadingHeightCorrelatedVisibility)
        return max(D * V_SmithGGXCorrelated(NoV, NoL, roughness) * F * NoL, 0.0f.xxx);
    float G = G_Smith(NoV, NoL, roughness);

    float3 specular = (D * G * F) / max(4.0f * NoV * NoL, 1e-4f) * NoL;
    if (any(specular != specular))
        return float3(0.0f, 0.0f, 0.0f);
    return max(specular, 0.0f.xxx);
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

#include "pbr_box_projection.hlsl"

#include "pbr_local_probe.hlsl"

#include "pbr_ssr_output.hlsl"

// GTAO multi-bounce fit (Jimenez et al. 2016): visibility and albedo to the
// occlusion including interreflections. Equals the input for black albedo and 1 at 1.
float3 GtaoMultiBounce(float visibility, float3 albedo)
{
    const float3 a =  2.0404f * albedo - 0.3324f;
    const float3 b = -4.7951f * albedo + 0.6417f;
    const float3 c =  2.7552f * albedo + 0.6903f;
    const float  x = visibility;
    return max(x.xxx, ((x * a + b) * x + c) * x);
}

// KROM_SSR_OUTPUT: also writes the SSR inputs (pbr_ssr_output.hlsl).
#ifdef KROM_DEPTH_PREPASS
void main(PSInput IN)
#else
float4 KROM_PBR_ENTRY(PSInput IN) : SV_Target
#endif
{
#if !defined(KROM_SHADOWS_NONE)
    gKromShadowPixelPos = IN.positionCS.xy;
#if !defined(KROM_SHADOWS_NONE)
    gKromShadowDpdx = ddx(IN.positionWS);
    gKromShadowDpdy = ddy(IN.positionWS);
#endif
#endif
    float2 uv = KromPbrUV(IN.texCoord, IN.scaledLocalPosition, boxTileSize);
#ifdef KROM_MATERIAL_GRAPH
    EvaluateMaterialGraph(boxTileSize > 0.0f ? uv : IN.meshUV, IN.positionWS);
    if (boxTileSize <= 0.0f)
        uv = KromGraphDirectUV(uv, IN.meshUV, uvScale, uvOffset);
#endif
    // --- Albedo ---
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
    float  opacity     = albedo4.a * opacityFactor;
#ifdef KROM_ALPHA_TEST
    clip(opacity - alphaCutoff);
#endif
#ifndef KROM_DEPTH_PREPASS

    // --- ORM / Channel-Map ---
    float metallic  = saturate(metallicFactor);
    float roughness = saturate(roughnessFactor);
    float specular = saturate(specularFactor);
    float ao        = 1.0f;
#ifdef KROM_ORM_MAP
    float3 ormSample = orm.Sample(sLinear, ormUV).rgb;
    ao        = lerp(1.0f, ormSample.r, saturate(occlusionStrength));
    roughness = saturate(ormSample.g * roughnessFactor);
    metallic  = saturate(ormSample.b * metallicFactor);
#elif defined(KROM_CHANNEL_MAP)
    float4 maskSample = orm.Sample(sLinear, ormUV);
    ao        = (occlusionChannel >= 0) ? saturate(SampleChannel(maskSample, occlusionChannel) * occlusionStrength + occlusionBias)
                                        : saturate(occlusionStrength);
    roughness = (roughnessChannel >= 0) ? saturate(SampleChannel(maskSample, roughnessChannel) * roughnessFactor + roughnessBias)
                                        : saturate(roughnessFactor);
    metallic  = (metallicChannel  >= 0) ? saturate(SampleChannel(maskSample, metallicChannel)  * metallicFactor  + metallicBias)
                                        : saturate(metallicFactor);
#endif
#ifdef KROM_MATERIAL_GRAPH
    metallic  = KromGraphMetallic(metallic);
    roughness = KromGraphRoughness(roughness);
    specular = KromGraphSpecular(specular);
#endif
    roughness = clamp(roughness, 0.04f, 1.0f);

    if (debugFlags & DBG_DISABLE_AO)
        ao = 1.0f;

    // --- Normalen ---
    float3 geomN = SafeNormalize(IN.normalWS);
    // A zero tangent selects the frame derived from the sampled UVs.
    bool deriveNormalFrame = boxTileSize > 0.0f;
#ifdef KROM_MATERIAL_GRAPH
    deriveNormalFrame = deriveNormalFrame || KromGraphNormalUsesDerivedFrame();
#endif
    float4 tangentWS = deriveNormalFrame ? 0.0f : IN.tangentWS;
    float3 N = (debugFlags & DBG_DISABLE_NORMALMAP)
                   ? geomN
                   : SampleNormal(normalUV, geomN, tangentWS, IN.positionWS);
    roughness = ApplySpecularAA(N, roughness);

    // --- View / BRDF-Basisgroessen ---
    float3 V   = SafeNormalize(cameraPositionWS.xyz - IN.positionWS);
    float  NoV = max(dot(N, V), 1e-4f);
    float3 F0  = KromSpecularF0(albedoValue, metallic, specular);
    float F90 = KromSpecularF90(metallic, specular);
    float3 F_a = F_SchlickRoughness(NoV, F0, roughness, F90);
    float3 kD_a = (1.0f.xxx - F_a) * (1.0f - metallic);

    // Clear coat layer (material): own roughness, geometric normal.
    const float coatStrength = saturate(clearcoatFactor);
    const float3 coatN = geomN;
    float coatRoughness = 0.04f;
    float coatFv = 0.0f;
    if (coatStrength > 0.0f)
    {
        coatRoughness = ApplySpecularAA(coatN, clamp(clearcoatRoughness, 0.04f, 1.0f));
        coatFv = KromClearcoatFresnel(max(dot(coatN, V), 1e-4f), coatStrength);
    }

    // Multi-scatter energy compensation for the specular lobes (shading option).
    float3 energyCompensation = float3(1.0f, 1.0f, 1.0f);
#ifdef KROM_IBL
    if (shadingMultiScatter)
        energyCompensation = KromEnergyCompensation(F0, tBRDFLut.SampleLevel(sClamp, float2(NoV, roughness), 0).rg);
#endif

    // --- IBL ---
    float3 iblDiffuse  = float3(0.0f, 0.0f, 0.0f);
    float3 iblSpecular = float3(0.0f, 0.0f, 0.0f);
    float3 iblSpecularWeight = float3(0.0f, 0.0f, 0.0f);
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
            float3 R        = reflect(-V, N);
            float3 prefilt  = SamplePrefilteredIBL(R, roughness);
            if (probeCount != 0u)
                prefilt = localProbes.specular + prefilt * localProbes.remaining;
            float2 brdfSamp = tBRDFLut.Sample(sClamp, float2(NoV, roughness)).rg;
            iblSpecularWeight = (F_a * brdfSamp.x + F90 * brdfSamp.y) * energyCompensation;
            iblSpecular     = prefilt * iblSpecularWeight;
        }
    }
#else
    iblDiffuse = ambientColor.rgb * ambientColor.a * albedoValue;
#endif
    // Clear coat: its own reflection on top, the base IBL attenuated by 1 - Fc.
    float3 coatIBL = float3(0.0f, 0.0f, 0.0f);
#ifdef KROM_IBL
    if (coatStrength > 0.0f && !(debugFlags & DBG_DISABLE_IBL))
    {
        if (!(debugFlags & DBG_DISABLE_IBL_SPEC))
        {
            const float3 coatR = reflect(-V, coatN);
            float3 coatPrefilt = SamplePrefilteredIBL(coatR, coatRoughness);
            if (probeCount != 0u)
            {
                const LocalProbeIBL coatProbes = EvaluateLocalProbes(IN.positionWS, coatN, coatR, coatRoughness,
                                                                     LocalProbeReflectionMipBias(coatR));
                coatPrefilt = coatProbes.specular + coatPrefilt * coatProbes.remaining;
            }
            coatIBL = coatPrefilt * coatFv;
        }
        iblDiffuse *= 1.0f - coatFv;
        iblSpecular *= 1.0f - coatFv;
        iblSpecularWeight *= 1.0f - coatFv;
    }
#endif
    float3 ambientIBL = (iblDiffuse + iblSpecular) * ao;

    // GTAO only modulates indirect light. An unbound AO slot leaves it unchanged.
    // Contact shadow of the main light; 1 without the add-on (the slot is then unbound).
    float contactShadowFactor = 1.0f;
    if (contactShadowLightPlusOne != 0u && !(debugFlags & DBG_DISABLE_CONTACT_SHADOW))
    {
        uint csWidth, csHeight;
        contactShadowTex.GetDimensions(csWidth, csHeight);
        const uint2 csPixel = uint2(IN.positionCS.xy);
        if (all(csPixel < uint2(csWidth, csHeight)))
            contactShadowFactor = contactShadowTex.Load(int3(csPixel, 0)).r;
    }

    float gtaoFactor = 1.0f;
    float3 gtaoDiffuse = float3(1.0f, 1.0f, 1.0f);
    if (gtaoEnabled != 0u && !(debugFlags & DBG_DISABLE_GTAO) && !(debugFlags & DBG_DISABLE_AO))
    {
        uint width, height;
        gtaoAO.GetDimensions(width, height);
        const uint2 pixel = uint2(IN.positionCS.xy);
        const float rawAO = all(pixel < uint2(width, height)) ? gtaoAO.Load(int3(pixel, 0)).r : 1.0f;
        gtaoFactor   = lerp(1.0f, rawAO, saturate(gtaoIntensity));
        // Multi-bounce (Jimenez et al. 2016): bright albedo gets back part of the
        // light the occluders reflect, so diffuse AO darkens less and keeps its tint.
        gtaoDiffuse  = GtaoMultiBounce(gtaoFactor, albedoValue);
        ambientIBL   = iblDiffuse * ao * gtaoDiffuse + iblSpecular * (ao * gtaoFactor);
    }

    // Specular occlusion (krom-probes switch): the specular IBL gets its own
    // factor from AO, roughness and NoV instead of the diffuse AO factor.
    float specularOcclusion = ao * gtaoFactor;
    if (probeSpecularOcclusionEnabled)
    {
        specularOcclusion = ComputeSpecularOcclusion(NoV, ao * gtaoFactor, roughness);
        ambientIBL = iblDiffuse * ao * gtaoDiffuse + iblSpecular * specularOcclusion;
    }

    // --- Direct Lighting ---
    // The coat reflection takes the same specular occlusion as the base; SSR
    // replaces only the base reflection (KromStoreSsrInputs below).
    ambientIBL += coatIBL * specularOcclusion;

    float3 totalDirectDiffuse  = float3(0.0f, 0.0f, 0.0f);
    float3 totalDirectSpecular = float3(0.0f, 0.0f, 0.0f);
    float  firstNoL            = 0.0f;
    float  firstShadowVisibility = 1.0f;
    // DBG_VIEW_SHADOW_CASCADE: first directional shadow that was sampled.
    uint   dbgShadowCascade = 0xFFFFFFFFu;
    float4 dbgShadowPosLCS = float4(0.0f, 0.0f, 0.0f, 1.0f);
    float4 dbgShadowRect = float4(0.0f, 0.0f, 0.0f, 0.0f);
    float  dbgShadowVisibility = 1.0f;

    [loop]
    for (uint i = 0; i < lightCount; ++i)
    {
        const float type = lights[i].params.w;
        float3 L         = float3(0.0f, 1.0f, 0.0f);
        float  atten     = 1.0f;
        float  shadowVisibility = 1.0f;

        if (type < 0.5f)
        {
            L     = SafeNormalize(-lights[i].positionWS.xyz);
        }
        else
        {
            float3 toL = lights[i].positionWS.xyz - IN.positionWS;
            float  dist = length(toL);
            L    = (dist > 1e-5f) ? (toL / dist) : float3(0.0f, 1.0f, 0.0f);
            atten = CalcAttenuation(dist, lights[i].params.z);
            if (type > 1.5f)
                atten *= CalcSpotAttenuation(L, SafeNormalize(lights[i].directionWS.xyz),
                                             lights[i].params.x, lights[i].params.y);
        }

    #if !defined(KROM_SHADOWS_NONE)
        gKromShadowToLight = L;
        if (!(debugFlags & DBG_DISABLE_SHADOWS) && shadowLightCount > 0u && type < 2.5f)
        {
            // Each branch below only picks the shadow view; the filter runs once
            // afterwards. fxc inlines every call, so one call site per filter keeps
            // the DX11 compile time down.
            bool shadowSelected = false;
            uint shadowViewIndexSel = 0u;
            float4 shadowPosLCS = float4(0.0f, 0.0f, 0.0f, 1.0f);
            float shadowBiasSel = 0.0f;
            float shadowStrengthSel = 0.0f;
            uint shadowCascadeSel = 0xFFFFFFFFu;
            // Cascade transition: the next cascade, mixed in with this weight.
            uint shadowViewIndexBlend = 0u;
            float4 shadowPosLCSBlend = float4(0.0f, 0.0f, 0.0f, 1.0f);
            float shadowBlendWeight = 0.0f;
            [loop]
            for (uint shadowIndex = 0u; shadowIndex < shadowLightCount; ++shadowIndex)
            {
                if ((uint)(shadowLightMeta[shadowIndex].x + 0.5f) != i)
                    continue;

                const uint firstViewIndex = (uint)(shadowLightExtra[shadowIndex].x + 0.5f);
                const uint viewCount = (uint)(shadowLightExtra[shadowIndex].y + 0.5f);
            #if !defined(KROM_SHADOWS_2D_ONLY)
                if (viewCount >= 6u)
                {
                    const uint faceIndex = ChoosePointShadowFace(IN.positionWS - lights[i].positionWS.xyz);
                    const uint viewIndex = firstViewIndex + faceIndex;
                    if (viewIndex < shadowViewCount && viewIndex < 16u)
                    {
                        shadowSelected = true;
                        shadowViewIndexSel = viewIndex;
                        shadowPosLCS = ComputeShadowReceiverCS(IN.positionWS, geomN, shadowViewProjArray[viewIndex],
                                                               shadowLightMeta[shadowIndex].z, shadowViewRect[viewIndex]);
                    }
                }
                else
            #endif
                if (type < 0.5f && viewCount > 1u)
                {
                    // Directional CSM: Cascades von der schärfsten (0) zur weitesten (N-1)
                    // durchprobieren und die erste nehmen, in deren Frustum der Fragment liegt.
                    // The filter runs once after the selection loop so fxc does not inline
                    // the whole shadow filter per attempt.
                    float cascadeBlend = 0.0f;
                    uint cascadeIndex = ChooseCascadeIndexFromViewDepth(
                        IN.positionWS, viewCount, shadowLightExtra[shadowIndex].z, shadowLightExtra[shadowIndex].w,
                        cascadeBlend);
                    uint selectedViewIndex = 0xFFFFFFFFu;
                    float4 selectedPosLCS = float4(0.0f, 0.0f, 0.0f, 1.0f);
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
                        if (viewIndex < shadowViewCount && viewIndex < 16u)
                        {
                            float4 posLCS = ComputeShadowReceiverCS(IN.positionWS, geomN,
                                                                    shadowViewProjArray[viewIndex],
                                                                    shadowLightMeta[shadowIndex].z,
                                                                    shadowViewRect[viewIndex]);
                            float3 ndcPos = posLCS.xyz / posLCS.w;
                            if (abs(ndcPos.x) < 1.0f && abs(ndcPos.y) < 1.0f &&
                                ndcPos.z > 0.0f && ndcPos.z < 1.0f)
                            {
                                selectedViewIndex = viewIndex;
                                selectedPosLCS = posLCS;
                                break;
                            }
                        }
                    }

                    if (selectedViewIndex != 0xFFFFFFFFu)
                    {
                        shadowSelected = true;
                        shadowViewIndexSel = selectedViewIndex;
                        shadowCascadeSel = selectedViewIndex - firstViewIndex;
                        // In the last part of a cascade the next one is mixed in
                        // (both filtered), so sharpness changes smoothly.
                        const uint blendView = selectedViewIndex + 1u;
                        if (cascadeBlend > 0.0f && shadowCascadeSel == cascadeIndex &&
                            cascadeIndex + 1u < viewCount && blendView < shadowViewCount && blendView < 16u)
                        {
                            const float4 blendPos = ComputeShadowReceiverCS(IN.positionWS, geomN,
                                shadowViewProjArray[blendView], shadowLightMeta[shadowIndex].z, shadowViewRect[blendView]);
                            const float3 blendNdc = blendPos.xyz / blendPos.w;
                            if (abs(blendNdc.x) < 1.0f && abs(blendNdc.y) < 1.0f &&
                                blendNdc.z > 0.0f && blendNdc.z < 1.0f)
                            {
                                shadowViewIndexBlend = blendView;
                                shadowPosLCSBlend = blendPos;
                                shadowBlendWeight = cascadeBlend;
                            }
                        }
                        shadowPosLCS = selectedPosLCS;
                    }
                }
                else if (firstViewIndex < shadowViewCount && firstViewIndex < 16u)
                {
                    shadowSelected = true;
                    shadowViewIndexSel = firstViewIndex;
                    if (type < 0.5f)
                        shadowCascadeSel = 0u;
                    shadowPosLCS = ComputeShadowReceiverCS(IN.positionWS, geomN,
                                                           shadowViewProjArray[firstViewIndex],
                                                           shadowLightMeta[shadowIndex].z,
                                                           shadowViewRect[firstViewIndex]);
                }
                shadowBiasSel = shadowLightMeta[shadowIndex].y;
                // Directional shadows fade out over the last part of the shadow
                // distance instead of ending in a hard line.
                shadowStrengthSel = shadowLightMeta[shadowIndex].w *
                    (type < 0.5f ? 1.0f - KromShadowDistanceFade(IN.positionWS, shadowLightExtra[shadowIndex].z) : 1.0f);
                KromSelectShadowFilterFromFrame(shadowIndex);
                break;
            }

            if (shadowSelected)
            {
                // One filter call site (fxc inlines it once); two passes only in
                // the cascade transition band.
                const uint shadowSampleCount = shadowBlendWeight > 0.0f ? 2u : 1u;
                const float2 shadowGradientSel = KromShadowGradientFor(shadowPosLCS, shadowViewProjArray[shadowViewIndexSel], shadowViewRect[shadowViewIndexSel]);
                const float2 shadowGradientBlend = KromShadowGradientFor(shadowPosLCSBlend, shadowViewProjArray[shadowViewIndexBlend], shadowViewRect[shadowViewIndexBlend]);
                float blendedVisibility = 0.0f;
                [loop]
                for (uint shadowSample = 0u; shadowSample < shadowSampleCount; ++shadowSample)
                {
                    const uint sampleView = shadowSample == 0u ? shadowViewIndexSel : shadowViewIndexBlend;
                    const float4 samplePos = shadowSample == 0u ? shadowPosLCS : shadowPosLCSBlend;
                    const float sampleWeight = shadowSample == 0u ? 1.0f - shadowBlendWeight : shadowBlendWeight;
                    gKromShadowGradient = shadowSample == 0u ? shadowGradientSel : shadowGradientBlend;
                    KromPreparePcss(shadowViewProjArray[sampleView], shadowViewRect[sampleView], IN.positionWS, L);
                    blendedVisibility += sampleWeight *
                        SampleShadowAtlas(samplePos, shadowBiasSel, shadowStrengthSel,
                                          shadowViewRect[sampleView], shadowViewProjArray[sampleView]);
                }
                shadowVisibility = blendedVisibility;
                if (dbgShadowCascade == 0xFFFFFFFFu && shadowCascadeSel != 0xFFFFFFFFu)
                {
                    dbgShadowCascade = shadowCascadeSel;
                    dbgShadowPosLCS = shadowPosLCS;
                    dbgShadowRect = shadowViewRect[shadowViewIndexSel];
                    dbgShadowVisibility = shadowVisibility;
                }
            }
        }
        // Contact shadows multiply onto the shadow map of the main light only.
        if (i + 1u == contactShadowLightPlusOne && !(debugFlags & DBG_DISABLE_SHADOWS))
            shadowVisibility *= contactShadowFactor;
    #endif
        atten *= shadowVisibility;

        float3 lightColor = lights[i].colorIntensity.rgb * lights[i].colorIntensity.w * atten;
        float  NoL_i      = max(dot(N, L), 0.0f);
        if (i == 0u)
        {
            firstNoL = NoL_i;
            firstShadowVisibility = shadowVisibility;
        }

        // Diffuse weight: view-angle Fresnel for all lights, or per light from the half vector.
        // Burley (shading option) scales the Lambert term per light.
        float3 kD_i = kD_a;
        float  diffuseShape = 1.0f;
        if (shadingPerLightDiffuseFresnel || shadingBurleyDiffuse)
        {
            const float LoH_i = saturate(dot(SafeNormalize(V + L), L));
            if (shadingPerLightDiffuseFresnel)
                kD_i = (1.0f.xxx - F_Schlick(LoH_i, F0, F90)) * (1.0f - metallic);
            if (shadingBurleyDiffuse)
                diffuseShape = KromBurleyDiffuseFactor(NoV, NoL_i, LoH_i, roughness);
        }
        float3 lightDiffuse = (kD_i * albedoValue * (diffuseShape / PI)) * NoL_i * lightColor;
        float3 lightSpecular = EvalSpecularGGX(N, V, L, albedoValue, metallic, roughness, specular) * energyCompensation * lightColor;
        if (coatStrength > 0.0f)
        {
            float coatF;
            const float coatSpecular = KromClearcoatSpecular(coatN, V, L, coatRoughness, coatStrength, coatF);
            lightDiffuse *= 1.0f - coatF;
            lightSpecular = lightSpecular * (1.0f - coatF) + coatSpecular * lightColor;
        }
        totalDirectDiffuse += lightDiffuse;
        totalDirectSpecular += lightSpecular;
    }

    // --- Emissive ---
    float3 emissiveColor = emissiveFactor.rgb;
#ifdef KROM_EMISSIVE_MAP
    emissiveColor *= emissive.Sample(sLinear, uv).rgb;
#endif
#ifdef KROM_MATERIAL_GRAPH
    if (graphOutputs.y >= 0.0)
        emissiveColor = graphEmission * emissiveFactor.rgb;
#endif

    float3 finalColor = ambientIBL + totalDirectDiffuse + totalDirectSpecular + emissiveColor;

    // --- Debug-Views (nur einer sollte aktiv sein; erster gesetzter Bit gewinnt) ---
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
    if (debugFlags & DBG_VIEW_CONTACT_SHADOW) return float4(contactShadowFactor.xxx, 1.0f);
#if !defined(KROM_SHADOWS_NONE)
    if (debugFlags & DBG_VIEW_SHADOW_CASCADE)
        return float4(KromShadowCascadeDebugColor(dbgShadowCascade, dbgShadowPosLCS, dbgShadowRect, dbgShadowVisibility), 1.0f);
#endif
    if (probeDebugView == 1u) return float4(LocalProbeDebugColor(localProbes.firstProbe), 1.0f);
    if (probeDebugView == 2u) return float4((1.0f - localProbes.remaining).xxx, 1.0f);
    if (probeDebugView == 3u) return float4(specularOcclusion.xxx, 1.0f);

#ifdef KROM_SSR_OUTPUT
    // Specular environment light as added above (incl. specular occlusion).
    KromStoreSsrInputs(N, roughness, iblSpecularWeight * specularOcclusion, iblSpecular * specularOcclusion);
#endif
    return float4(finalColor, opacity);
#endif // KROM_DEPTH_PREPASS
}

KROM_PBR_SSR_MAIN(PSInput)
