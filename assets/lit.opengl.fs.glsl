#version 410 core
// =============================================================================
// KROM Engine - assets/lit.opengl.fs.glsl
// Fragment Shader: Lit (OpenGL / GLSL 4.10)
// Separater OpenGL-Shadow-Pfad.
// =============================================================================

struct GpuLightData
{
    vec4 positionWS;
    vec4 directionWS;
    vec4 colorIntensity;
    vec4 params;
};

layout(std140) uniform PerFrame
{
    mat4         viewMatrix;
    mat4         projMatrix;
    mat4         viewProjMatrix;
    mat4         invViewProjMatrix;
    vec4         cameraPositionWS;
    vec4         cameraForwardWS;
    vec4         screenSize;
    vec4         timeData;
    vec4         ambientColor;
    uint         lightCount;
    uint         shadowCascadeCount;
    float        nearPlane;
    float        farPlane;
    GpuLightData lights[7];
    mat4         shadowViewProj;
    float        iblPrefilterLevels;
    float        shadowBias;
    float        shadowNormalBias;
    float        shadowStrength;
    float        shadowTexelSize;
    uint         debugFlags;
    uint         shadowFilterMode;
    float        _shadowPad;
    vec4         shadowLightMeta[4];
    vec4         shadowLightExtra[4];
    vec4         shadowViewRect[16];
    mat4         shadowViewProjArray[16];
    uint         shadowLightCount;
    uint         shadowViewCount;
    vec2         _shadowArrayPad;
};

layout(std140) uniform PerMaterial
{
    vec4  baseColorFactor;
    vec4  emissiveFactor;
    float metallicFactor;
    float roughnessFactor;
    float normalStrength;
    float occlusionStrength;
    float opacityFactor;
    float alphaCutoff;
    int   materialFeatureMask;
    float materialModel;
};

uniform sampler2D albedo;
uniform sampler2D normalMap;
uniform sampler2D emissive;
uniform sampler2D shadowMapRaw;

in vec3 vPositionWS;
in vec3 vNormalWS;
in vec4 vTangentWS;
in vec2 vTexCoord;

layout(location = 0) out vec4 fragColor;

vec3 SafeNormalize(vec3 v)
{
    float len2 = dot(v, v);
    return (len2 > 1e-12) ? (v * inversesqrt(len2)) : vec3(0.0, 0.0, 1.0);
}

vec3 SampleNormal(vec2 uv, vec3 baseNormalWS, vec4 tangentWS, vec3 positionWS)
{
#ifdef KROM_NORMAL_MAP
    vec3 mapN = SafeNormalize(texture(normalMap, uv).xyz * 2.0 - 1.0);
    mapN.xy *= normalStrength;
    mapN.z = sqrt(clamp(1.0 - dot(mapN.xy, mapN.xy), 0.0, 1.0));
    mapN = SafeNormalize(mapN);

    vec3 T = tangentWS.xyz;
    float tLen2 = dot(T, T);
    if (tLen2 > 1e-8)
    {
        T *= inversesqrt(tLen2);
        T = T - dot(T, baseNormalWS) * baseNormalWS;
        float tOrthoLen2 = dot(T, T);
        if (tOrthoLen2 > 1e-8)
        {
            T *= inversesqrt(tOrthoLen2);
            float handedness = (abs(tangentWS.w) > 0.5) ? tangentWS.w : 1.0;
            vec3 B = SafeNormalize(cross(baseNormalWS, T)) * handedness;
            vec3 shadedN = SafeNormalize(mapN.x * T + mapN.y * B + mapN.z * baseNormalWS);
            float NoNg = dot(shadedN, baseNormalWS);
            if (NoNg < 0.0)
                shadedN = SafeNormalize(shadedN - 2.0 * NoNg * baseNormalWS);
            return shadedN;
        }
    }

    vec3 dp1 = dFdx(positionWS);
    vec3 dp2 = dFdy(positionWS);
    vec2 duv1 = dFdx(uv);
    vec2 duv2 = dFdy(uv);
    vec3 dp2perp = cross(dp2, baseNormalWS);
    vec3 dp1perp = cross(baseNormalWS, dp1);
    vec3 Traw = dp2perp * duv1.x + dp1perp * duv2.x;
    vec3 Braw = dp2perp * duv1.y + dp1perp * duv2.y;
    float tRawLen2 = dot(Traw, Traw);
    float refLen2 = max(dot(dp1, dp1), dot(dp2, dp2)) * max(dot(duv1, duv1), dot(duv2, duv2));
    if (!(tRawLen2 > refLen2 * 1e-6 && refLen2 > 0.0))
        return baseNormalWS;

    T = Traw * inversesqrt(tRawLen2);
    T = SafeNormalize(T - dot(T, baseNormalWS) * baseNormalWS);
    float handedness = (dot(cross(baseNormalWS, T), Braw) >= 0.0) ? 1.0 : -1.0;
    vec3 B = cross(baseNormalWS, T) * handedness;
    vec3 shadedN = SafeNormalize(mapN.x * T + mapN.y * B + mapN.z * baseNormalWS);
    float NoNg = dot(shadedN, baseNormalWS);
    if (NoNg < 0.0)
        shadedN = SafeNormalize(shadedN - 2.0 * NoNg * baseNormalWS);
    return shadedN;
#else
    return baseNormalWS;
#endif
}

float CalcAttenuation(float dist, float range)
{
    float ratio = clamp(1.0 - dist / max(range, 0.001), 0.0, 1.0);
    return (ratio * ratio) / max(dist * dist, 1e-4);
}

float CalcSpotAttenuation(vec3 L, vec3 spotDir, float cosInner, float cosOuter)
{
    float cosAngle = dot(-L, spotDir);
    return clamp((cosAngle - cosOuter) / max(cosInner - cosOuter, 1e-4), 0.0, 1.0);
}

uint ChooseCascadeIndexFromViewDepth(vec3 positionWS, uint viewCount, float maxShadowDistance, float cascadeLambdaValue)
{
    uint cascadeCount = min(viewCount, 4u);
    if (cascadeCount <= 1u)
        return 0u;

    float viewDepth = max(-(viewMatrix * vec4(positionWS, 1.0)).z, nearPlane);
    float camNear = max(nearPlane, 1e-4);
    float camFar = min(farPlane, max(maxShadowDistance, camNear + 1.0));
    float lambda = clamp(cascadeLambdaValue, 0.0, 1.0);
    float countF = float(cascadeCount);

    for (uint splitIndex = 1u; splitIndex < cascadeCount; ++splitIndex)
    {
        float fi = float(splitIndex) / countF;
        float splitLog = camNear * pow(camFar / camNear, fi);
        float splitUniform = camNear + (camFar - camNear) * fi;
        float splitDepth = mix(splitUniform, splitLog, lambda);
        if (viewDepth <= splitDepth)
            return splitIndex - 1u;
    }
    return cascadeCount - 1u;
}

float CompareShadowAtlas(vec2 uv, float cmpDepth, vec4 atlasRect)
{
    float texelSize = max(shadowTexelSize, 1e-8);
    vec2 minUv = atlasRect.xy + vec2(texelSize * 0.5);
    vec2 maxUv = atlasRect.xy + atlasRect.zw - vec2(texelSize * 0.5);
    float sampledDepth = texture(shadowMapRaw, clamp(uv, minUv, maxUv)).r;
    return (cmpDepth <= sampledDepth) ? 1.0 : 0.0;
}

vec4 ComputeShadowReceiverCS(vec3 positionWS, vec3 normalWS, mat4 shadowVP, float normalBiasValue)
{
    vec3 offsetPositionWS = positionWS + normalWS * normalBiasValue;
    return shadowVP * vec4(offsetPositionWS, 1.0);
}

uint ChoosePointShadowFace(vec3 lightToPoint)
{
    vec3 axis = abs(lightToPoint);
    if (axis.x >= axis.y && axis.x >= axis.z)
        return lightToPoint.x >= 0.0 ? 0u : 1u;
    if (axis.y >= axis.z)
        return lightToPoint.y >= 0.0 ? 2u : 3u;
    return lightToPoint.z >= 0.0 ? 4u : 5u;
}

void ChoosePointShadowFaces(vec3 lightToPoint,
                            out uint faceX,
                            out uint faceY,
                            out uint faceZ,
                            out float weightX,
                            out float weightY,
                            out float weightZ)
{
    vec3 axis = abs(lightToPoint);
    float eps = 1e-6;
    float sum = max(axis.x + axis.y + axis.z, eps);
    faceX = lightToPoint.x >= 0.0 ? 0u : 1u;
    faceY = lightToPoint.y >= 0.0 ? 2u : 3u;
    faceZ = lightToPoint.z >= 0.0 ? 4u : 5u;
    weightX = axis.x / sum;
    weightY = axis.y / sum;
    weightZ = axis.z / sum;
}

float SampleShadowAtlas(vec4 positionLightCS, float biasValue, float strengthValue, vec4 atlasRect)
{
    if (shadowCascadeCount == 0u || strengthValue <= 0.0)
        return 1.0;
    if (positionLightCS.w <= 1e-6)
        return 1.0;

    vec3 posNDC = positionLightCS.xyz / positionLightCS.w;
    vec2 localUv = posNDC.xy * 0.5 + 0.5;
    float depth = posNDC.z * 0.5 + 0.5;

    if (localUv.x < 0.0 || localUv.x > 1.0 || localUv.y < 0.0 || localUv.y > 1.0)
        return 1.0;
    if (depth <= 0.0 || depth >= 1.0)
        return 1.0;

    vec2 uv = atlasRect.xy + localUv * atlasRect.zw;

    // Slope-scaled Receiver-Plane Depth Bias (analog zum HLSL/DX-Pfad):
    // winziger konstanter Bias ohne Acne → kein Peter-Panning.
    float ddxDepth = dFdx(depth);
    float ddyDepth = dFdy(depth);
    float ddxUvX = dFdx(uv.x);
    float ddyUvY = dFdy(uv.y);
    vec2 dDepth_dUV = vec2(
        abs(ddxUvX) > 1e-7 ? ddxDepth / ddxUvX : 0.0,
        abs(ddyUvY) > 1e-7 ? ddyDepth / ddyUvY : 0.0);
    dDepth_dUV = clamp(dDepth_dUV, -1.0, 1.0);
    float baseBias = max(biasValue, 0.00005);

    float visibility = 0.0;
    for (int y = -1; y <= 1; ++y)
    {
        for (int x = -1; x <= 1; ++x)
        {
            vec2 offset = vec2(float(x), float(y)) * shadowTexelSize;
            float depthRef = depth + dot(dDepth_dUV, offset) - baseBias;
            visibility += CompareShadowAtlas(uv + offset, depthRef, atlasRect);
        }
    }
    visibility *= (1.0 / 9.0);
    return mix(1.0, visibility, clamp(strengthValue, 0.0, 1.0));
}

void main()
{
#ifdef KROM_BASECOLOR_MAP
    vec4 albedo4 = texture(albedo, vTexCoord) * baseColorFactor;
#else
    vec4 albedo4 = baseColorFactor;
#endif

    float opacity = albedo4.a * opacityFactor;
#ifdef KROM_ALPHA_TEST
    if (opacity < alphaCutoff) discard;
#endif

    vec3 geomN = SafeNormalize(vNormalWS);
    vec3 N = SampleNormal(vTexCoord, geomN, vTangentWS, vPositionWS);
    vec3 V = SafeNormalize(cameraPositionWS.xyz - vPositionWS);
    float roughness = clamp(roughnessFactor, 0.04, 1.0);
    float shininess = mix(128.0, 8.0, roughness);
    float specularStrength = clamp(metallicFactor, 0.0, 1.0);

    vec3 lighting = albedo4.rgb * ambientColor.rgb * ambientColor.a;
    float firstNoL = 0.0;
    float firstShadowVisibility = 1.0;

    for (uint i = 0u; i < lightCount; ++i)
    {
        float type = lights[i].params.w;
        vec3 L = vec3(0.0, 1.0, 0.0);
        float attenuation = 1.0;
        float shadowVisibility = 1.0;

        if (type < 0.5)
        {
            L = SafeNormalize(-lights[i].positionWS.xyz);
        }
        else
        {
            vec3 toLight = lights[i].positionWS.xyz - vPositionWS;
            float dist = length(toLight);
            L = (dist > 1e-5) ? (toLight / dist) : vec3(0.0, 1.0, 0.0);
            attenuation = CalcAttenuation(dist, lights[i].params.z);
            if (type > 1.5)
                attenuation *= CalcSpotAttenuation(L, SafeNormalize(lights[i].directionWS.xyz),
                                                   lights[i].params.x, lights[i].params.y);
        }

        if ((debugFlags & (1u << 2)) == 0u && shadowLightCount > 0u && type < 2.5)
        {
            for (uint shadowIndex = 0u; shadowIndex < shadowLightCount; ++shadowIndex)
            {
                if (uint(shadowLightMeta[shadowIndex].x + 0.5) != i)
                    continue;
                uint firstViewIndex = uint(shadowLightExtra[shadowIndex].x + 0.5);
                uint viewCount = uint(shadowLightExtra[shadowIndex].y + 0.5);
                if (viewCount >= 6u)
                {
                    uint faceX = 0u;
                    uint faceY = 0u;
                    uint faceZ = 0u;
                    float weightX = 0.0;
                    float weightY = 0.0;
                    float weightZ = 0.0;
                    ChoosePointShadowFaces(vPositionWS - lights[i].positionWS.xyz,
                                           faceX, faceY, faceZ, weightX, weightY, weightZ);

                    float visibilityAccum = 0.0;
                    float weightAccum = 0.0;
                    uint faceIndices[3] = uint[3](faceX, faceY, faceZ);
                    float faceWeights[3] = float[3](weightX, weightY, weightZ);
                    for (uint blendIndex = 0u; blendIndex < 3u; ++blendIndex)
                    {
                        float faceWeight = faceWeights[blendIndex];
                        uint viewIndex = firstViewIndex + faceIndices[blendIndex];
                        if (faceWeight <= 1e-4 || viewIndex >= shadowViewCount || viewIndex >= 16u)
                            continue;

                        visibilityAccum += faceWeight * SampleShadowAtlas(
                            ComputeShadowReceiverCS(vPositionWS, geomN, shadowViewProjArray[viewIndex], shadowLightMeta[shadowIndex].z),
                            shadowLightMeta[shadowIndex].y,
                            shadowLightMeta[shadowIndex].w,
                            shadowViewRect[viewIndex]);
                        weightAccum += faceWeight;
                    }

                    if (weightAccum > 1e-5)
                        shadowVisibility = visibilityAccum / weightAccum;
                }
                else if (type < 0.5 && viewCount > 1u)
                {
                    // Directional CSM: try cascades from tightest (0) to widest (N-1)
                    uint cascadeIndex = ChooseCascadeIndexFromViewDepth(
                        vPositionWS, viewCount, shadowLightExtra[shadowIndex].z, shadowLightExtra[shadowIndex].w);
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

                        uint viewIndex = firstViewIndex + candidate;
                        if (viewIndex < shadowViewCount && viewIndex < 16u)
                        {
                            vec4 posLCS = ComputeShadowReceiverCS(vPositionWS, geomN,
                                                                   shadowViewProjArray[viewIndex],
                                                                   shadowLightMeta[shadowIndex].z);
                            vec3 ndcPos = posLCS.xyz / posLCS.w;
                            if (abs(ndcPos.x) < 1.0 && abs(ndcPos.y) < 1.0 &&
                                ndcPos.z >= 0.0 && ndcPos.z < 1.0)
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
                    // Spot or single-cascade directional
                    shadowVisibility = SampleShadowAtlas(
                        ComputeShadowReceiverCS(vPositionWS, geomN, shadowViewProjArray[firstViewIndex], shadowLightMeta[shadowIndex].z),
                        shadowLightMeta[shadowIndex].y,
                        shadowLightMeta[shadowIndex].w,
                        shadowViewRect[firstViewIndex]);
                }
                break;
            }
        }
        attenuation *= shadowVisibility;

        float NoL = max(dot(N, L), 0.0);
        if (i == 0u)
        {
            firstNoL = NoL;
            firstShadowVisibility = shadowVisibility;
        }
        vec3 H = SafeNormalize(V + L);
        float spec = pow(max(dot(N, H), 0.0), shininess) * specularStrength;
        vec3 lightColor = lights[i].colorIntensity.rgb * lights[i].colorIntensity.w * attenuation;
        lighting += albedo4.rgb * lightColor * NoL;
        lighting += lightColor * spec;
    }

    vec3 emissiveColor = emissiveFactor.rgb;
#ifdef KROM_EMISSIVE_MAP
    emissiveColor *= texture(emissive, vTexCoord).rgb;
#endif

    if ((debugFlags & (1u << 8)) != 0u)
    {
        fragColor = vec4(N * 0.5 + 0.5, 1.0);
        return;
    }
    if ((debugFlags & (1u << 9)) != 0u)
    {
        fragColor = vec4(vec3(firstNoL), 1.0);
        return;
    }
    if ((debugFlags & (1u << 13)) != 0u)
    {
        fragColor = vec4(vec3(firstShadowVisibility), 1.0);
        return;
    }

    fragColor = vec4(lighting + emissiveColor, opacity);
}
