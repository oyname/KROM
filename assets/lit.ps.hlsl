// =============================================================================
// KROM Engine - assets/lit.ps.hlsl
// Pixel Shader: Lit (HLSL)
// Gemeinsamer Shadow-Vertrag fuer DX11 / Vulkan / spaeter DX12.
// =============================================================================

Texture2D                 albedo      : register(t0);
Texture2D                 normalMap   : register(t1);
Texture2D                 emissive    : register(t3);
Texture2D                 shadowMap   : register(t4);

SamplerState             sLinear      : register(s0);
SamplerState             sPoint       : register(s2);
SamplerComparisonState   shadowSampler: register(s3);

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
};

struct PSInput
{
    float4 positionCS      : SV_POSITION;
    float3 positionWS      : TEXCOORD1;
    float3 normalWS        : TEXCOORD2;
    float2 texCoord        : TEXCOORD0;
    float4 positionLightCS : TEXCOORD3;
    float4 tangentWS       : TEXCOORD4;
};

static const uint DBG_DISABLE_SHADOWS = 1u << 2;
static const uint DBG_VIEW_NORMALS    = 1u << 8;
static const uint DBG_VIEW_NOL        = 1u << 9;
static const uint DBG_VIEW_SHADOW     = 1u << 13;

float3 SafeNormalize(float3 v)
{
    float len2 = dot(v, v);
    return (len2 > 1e-12f) ? (v * rsqrt(len2)) : float3(0.0f, 0.0f, 1.0f);
}

float3 SampleNormal(float2 uv, float3 baseNormalWS, float4 tangentWS, float3 positionWS)
{
    float3 result = baseNormalWS;
#ifdef KROM_NORMAL_MAP
    float3 mapN = normalMap.Sample(sLinear, uv).xyz * 2.0f - 1.0f;
    mapN = SafeNormalize(mapN);
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
        const float3 dp1 = ddx(positionWS);
        const float3 dp2 = ddy(positionWS);
        const float2 duv1 = ddx(uv);
        const float2 duv2 = ddy(uv);
        const float3 dp2perp = cross(dp2, baseNormalWS);
        const float3 dp1perp = cross(baseNormalWS, dp1);
        const float3 Traw = dp2perp * duv1.x + dp1perp * duv2.x;
        const float3 Braw = dp2perp * duv1.y + dp1perp * duv2.y;
        const float tRawLen2 = dot(Traw, Traw);
        const float refLen2 = max(dot(dp1, dp1), dot(dp2, dp2)) * max(dot(duv1, duv1), dot(duv2, duv2));
        if (tRawLen2 > refLen2 * 1e-6f && refLen2 > 0.0f)
        {
            T = Traw * rsqrt(tRawLen2);
            T = SafeNormalize(T - dot(T, baseNormalWS) * baseNormalWS);
            const float handedness = (dot(cross(baseNormalWS, T), Braw) >= 0.0f) ? 1.0f : -1.0f;
            const float3 B = cross(baseNormalWS, T) * handedness;
            float3 shadedN = SafeNormalize(mapN.x * T + mapN.y * B + mapN.z * baseNormalWS);
            const float NoNg = dot(shadedN, baseNormalWS);
            if (NoNg < 0.0f)
                shadedN = SafeNormalize(shadedN - 2.0f * NoNg * baseNormalWS);
            result = shadedN;
        }
    }
#endif
    return result;
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
    float3 axis = abs(lightToPoint);
    if (axis.x >= axis.y && axis.x >= axis.z)
        return lightToPoint.x >= 0.0f ? 0u : 1u;
    if (axis.y >= axis.z)
        return lightToPoint.y >= 0.0f ? 2u : 3u;
    return lightToPoint.z >= 0.0f ? 4u : 5u;
}

void ChoosePointShadowFaces(float3 lightToPoint,
                            out uint faceX,
                            out uint faceY,
                            out uint faceZ,
                            out float weightX,
                            out float weightY,
                            out float weightZ)
{
    const float3 axis = abs(lightToPoint);
    const float eps = 1e-6f;
    const float sum = max(axis.x + axis.y + axis.z, eps);
    faceX = lightToPoint.x >= 0.0f ? 0u : 1u;
    faceY = lightToPoint.y >= 0.0f ? 2u : 3u;
    faceZ = lightToPoint.z >= 0.0f ? 4u : 5u;
    weightX = axis.x / sum;
    weightY = axis.y / sum;
    weightZ = axis.z / sum;
}

float SampleShadowAtlas(float4 positionLightCS, float biasValue, float strengthValue, float4 atlasRect)
{
    float result = 1.0f;

    if (shadowCascadeCount != 0u && strengthValue > 0.0f && positionLightCS.w > 1e-6f)
    {
        float3 posNDC = positionLightCS.xyz / positionLightCS.w;
        // DX11/Vulkan: render target V=0 oben, NDC Y=+1 = oben -> V=(1-y)/2.
        // Mit Y-Flip in shadowViewProj: posNDC.y = -clip_y, daher 0.5 - posNDC.y*0.5 = (1+clip_y)/2 = korrekte V.
        float2 localUv = float2(posNDC.x * 0.5f + 0.5f, 0.5f - posNDC.y * 0.5f);
        float depth = posNDC.z;

        const bool insideAtlas =
            localUv.x >= 0.0f && localUv.x <= 1.0f &&
            localUv.y >= 0.0f && localUv.y <= 1.0f &&
            depth > 0.0f && depth < 1.0f;

        if (insideAtlas)
        {
            float2 uv = atlasRect.xy + localUv * atlasRect.zw;

            // Slope-scaled Receiver-Plane Depth Bias (analog zum OpenGL-Pfad):
            // Verhindert Shadow Acne auf schrägen Flächen ohne übermäßiges Peter-Panning.
            float ddxDepth = ddx(depth);
            float ddyDepth = ddy(depth);
            float ddxUvX   = ddx(uv.x);
            float ddyUvY   = ddy(uv.y);
            float2 dDepth_dUV = float2(
                abs(ddxUvX) > 1e-7f ? ddxDepth / ddxUvX : 0.0f,
                abs(ddyUvY) > 1e-7f ? ddyDepth / ddyUvY : 0.0f);
            dDepth_dUV = clamp(dDepth_dUV, -1.0f, 1.0f);
            float baseBias = max(biasValue, 0.00005f);

            float visibility = 0.0f;

            [loop]
            for (int y = -1; y <= 1; ++y)
            {
                [loop]
                for (int x = -1; x <= 1; ++x)
                {
                    float2 offset = float2((float)x, (float)y) * shadowTexelSize;
                    float depthRef = depth + dot(dDepth_dUV, offset) - baseBias;
                    visibility += CompareShadowManualBilinear(uv + offset, depthRef, atlasRect);
                }
            }

            visibility *= (1.0f / 9.0f);
            result = lerp(1.0f, visibility, saturate(strengthValue));
        }
    }

    return result;
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

    float3 geomN = SafeNormalize(IN.normalWS);
    float3 N = SampleNormal(IN.texCoord, geomN, IN.tangentWS, IN.positionWS);
    float3 V = SafeNormalize(cameraPositionWS.xyz - IN.positionWS);
    float roughness = clamp(roughnessFactor, 0.04f, 1.0f);
    float shininess = lerp(128.0f, 8.0f, roughness);
    float specularStrength = saturate(metallicFactor);

    float3 lighting = albedo4.rgb * ambientColor.rgb * ambientColor.a;

    float firstNoL = 0.0f;
    float firstShadowVisibility = 1.0f;

    [loop]
    for (uint i = 0; i < lightCount; ++i)
    {
        const float type = lights[i].params.w;
        float3 L = float3(0.0f, 1.0f, 0.0f);
        float attenuation = 1.0f;
        float shadowVisibility = 1.0f;

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

        if (!(debugFlags & DBG_DISABLE_SHADOWS) && shadowLightCount > 0u && type < 2.5f)
        {
            [loop]
            for (uint shadowIndex = 0u; shadowIndex < shadowLightCount; ++shadowIndex)
            {
                if ((uint)(shadowLightMeta[shadowIndex].x + 0.5f) != i)
                    continue;
                const uint firstViewIndex = (uint)(shadowLightExtra[shadowIndex].x + 0.5f);
                const uint viewCount = (uint)(shadowLightExtra[shadowIndex].y + 0.5f);
                if (viewCount >= 6u)
                {
                    uint faceX = 0u;
                    uint faceY = 0u;
                    uint faceZ = 0u;
                    float weightX = 0.0f;
                    float weightY = 0.0f;
                    float weightZ = 0.0f;
                    ChoosePointShadowFaces(IN.positionWS - lights[i].positionWS.xyz,
                                           faceX, faceY, faceZ, weightX, weightY, weightZ);

                    float visibilityAccum = 0.0f;
                    float weightAccum = 0.0f;
                    const uint faceIndices[3] = { faceX, faceY, faceZ };
                    const float faceWeights[3] = { weightX, weightY, weightZ };
                    [loop]
                    for (uint blendIndex = 0u; blendIndex < 3u; ++blendIndex)
                    {
                        const float faceWeight = faceWeights[blendIndex];
                        const uint viewIndex = firstViewIndex + faceIndices[blendIndex];
                        if (faceWeight <= 1e-4f || viewIndex >= shadowViewCount || viewIndex >= 16u)
                            continue;

                        visibilityAccum += faceWeight * SampleShadowAtlas(
                            ComputeShadowReceiverCS(IN.positionWS, geomN, shadowViewProjArray[viewIndex], shadowLightMeta[shadowIndex].z),
                            shadowLightMeta[shadowIndex].y,
                            shadowLightMeta[shadowIndex].w,
                            shadowViewRect[viewIndex]);
                        weightAccum += faceWeight;
                    }

                    if (weightAccum > 1e-5f)
                        shadowVisibility = visibilityAccum / weightAccum;
                }
                else if (type < 0.5f && viewCount > 1u)
                {
                    // Directional CSM: Cascades von der schärfsten (0) zur weitesten (N-1)
                    // durchprobieren und die erste nehmen, in deren Frustum der Fragment liegt.
                    const uint cascadeIndex = ChooseCascadeIndexFromViewDepth(
                        IN.positionWS, viewCount, shadowLightExtra[shadowIndex].z, shadowLightExtra[shadowIndex].w);
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
                            float4 posLCS = ComputeShadowReceiverCS(IN.positionWS, geomN, shadowViewProjArray[viewIndex], shadowLightMeta[shadowIndex].z);
                            float3 ndcPos = posLCS.xyz / posLCS.w;
                            if (abs(ndcPos.x) < 1.0f && abs(ndcPos.y) < 1.0f &&
                                ndcPos.z > 0.0f && ndcPos.z < 1.0f)
                            {
                                shadowVisibility = SampleShadowAtlas(posLCS,
                                    shadowLightMeta[shadowIndex].y,
                                    shadowLightMeta[shadowIndex].w,
                                    shadowViewRect[viewIndex]);
                                break;
                            }
                        }
                    }
                }
                else if (firstViewIndex < shadowViewCount && firstViewIndex < 16u)
                {
                    shadowVisibility = SampleShadowAtlas(
                        ComputeShadowReceiverCS(IN.positionWS, geomN, shadowViewProjArray[firstViewIndex], shadowLightMeta[shadowIndex].z),
                        shadowLightMeta[shadowIndex].y,
                        shadowLightMeta[shadowIndex].w,
                        shadowViewRect[firstViewIndex]);
                }
                break;
            }
        }
        attenuation *= shadowVisibility;

        float NoL = max(dot(N, L), 0.0f);
        if (i == 0u)
        {
            firstNoL = NoL;
            firstShadowVisibility = shadowVisibility;
        }
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

    if (debugFlags & DBG_VIEW_NORMALS) return float4(N * 0.5f + 0.5f, 1.0f);
    if (debugFlags & DBG_VIEW_NOL)     return float4(firstNoL.xxx, 1.0f);
    if (debugFlags & DBG_VIEW_SHADOW)  return float4(firstShadowVisibility.xxx, 1.0f);

    return float4(lighting + emissiveColor, opacity);
}
