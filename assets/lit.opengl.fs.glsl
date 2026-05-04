#version 410 core
#extension GL_ARB_shading_language_420pack : enable
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

layout(std140, binding = 0) uniform PerFrame
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
    vec2         _shadowPad;
    vec4         shadowLightMeta[4];
    vec4         shadowLightExtra[4];
    vec4         shadowViewRect[16];
    mat4         shadowViewProjArray[16];
    uint         shadowLightCount;
    uint         shadowViewCount;
    vec2         _shadowArrayPad;
};

layout(std140, binding = 2) uniform PerMaterial
{
    vec4  baseColorFactor;
    vec4  emissiveFactor;
    float metallicFactor;
    float roughnessFactor;
    float occlusionStrength;
    float opacityFactor;
    float alphaCutoff;
    int   materialFeatureMask;
    float materialModel;
    float _pad0;
};

uniform sampler2D albedo;
uniform sampler2D emissive;
uniform sampler2DShadow shadowMap;

in vec3 vPositionWS;
in vec3 vNormalWS;
in vec2 vTexCoord;
in vec4 vPositionLightCS;

layout(location = 0) out vec4 fragColor;

vec3 SafeNormalize(vec3 v)
{
    float len2 = dot(v, v);
    return (len2 > 1e-12) ? (v * inversesqrt(len2)) : vec3(0.0, 0.0, 1.0);
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

float CompareShadowAtlas(vec2 uv, float cmpDepth, vec4 atlasRect)
{
    float texelSize = max(shadowTexelSize, 1e-8);
    vec2 minUv = atlasRect.xy + vec2(texelSize * 0.5);
    vec2 maxUv = atlasRect.xy + atlasRect.zw - vec2(texelSize * 0.5);
    return texture(shadowMap, vec3(clamp(uv, minUv, maxUv), cmpDepth));
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

    float visibility = 0.0;
    for (int y = -1; y <= 1; ++y)
    {
        for (int x = -1; x <= 1; ++x)
        {
            vec2 offset = vec2(float(x), float(y)) * shadowTexelSize;
            visibility += CompareShadowAtlas(uv + offset, depth - biasValue, atlasRect);
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

    vec3 N = SafeNormalize(vNormalWS);
    vec3 V = SafeNormalize(cameraPositionWS.xyz - vPositionWS);
    float roughness = clamp(roughnessFactor, 0.04, 1.0);
    float shininess = mix(128.0, 8.0, roughness);
    float specularStrength = clamp(metallicFactor, 0.0, 1.0);

    vec3 lighting = albedo4.rgb * ambientColor.rgb * ambientColor.a;
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

        if (shadowLightCount > 0u && type < 2.5)
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
                            ComputeShadowReceiverCS(vPositionWS, N, shadowViewProjArray[viewIndex], shadowLightMeta[shadowIndex].z),
                            shadowLightMeta[shadowIndex].y,
                            shadowLightMeta[shadowIndex].w,
                            shadowViewRect[viewIndex]);
                        weightAccum += faceWeight;
                    }

                    if (weightAccum > 1e-5)
                        shadowVisibility = visibilityAccum / weightAccum;
                }
                else if (firstViewIndex < shadowViewCount && firstViewIndex < 16u)
                {
                    shadowVisibility = SampleShadowAtlas(
                        ComputeShadowReceiverCS(vPositionWS, N, shadowViewProjArray[firstViewIndex], shadowLightMeta[shadowIndex].z),
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
            firstShadowVisibility = shadowVisibility;
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

    if ((debugFlags & (1u << 13)) != 0u)
    {
        fragColor = vec4(vec3(firstShadowVisibility), 1.0);
        return;
    }

    fragColor = vec4(lighting + emissiveColor, opacity);
}
