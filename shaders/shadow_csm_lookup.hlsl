// =============================================================================
// KROM Engine - shaders/shadow_csm_lookup.hlsl
// Cascade choice of the directional shadow (CSM), shared by the PBR pixel
// shaders and the surface cache GI compute passes (doc/PLAN_GI.md, finding 11).
//
// Needs the PerFrame block of the includer (viewMatrix, nearPlane, farPlane and,
// for KROM_SHADOW_CSM_LOAD, the shadow arrays and shadowTexelSize).
//
// With KROM_SHADOW_CSM_LOAD defined it also offers KromCsmVisibilityLoad: a
// directional shadow lookup without derivatives and samplers (Texture2D
// shadowMap read with Load, one bilinear compare, constant bias). For compute
// shaders; the pixel shaders keep their own filters.
// =============================================================================
#ifndef KROM_SHADOW_CSM_LOOKUP_HLSL
#define KROM_SHADOW_CSM_LOOKUP_HLSL

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

#if defined(KROM_SHADOW_CSM_LOAD)

// Extra normal offset and depth bias of the derivative-free lookup, in shadow
// texels: there is no receiver-plane gradient, so a constant margin replaces it.
static const float kKromCsmLoadNormalTexels = 1.5f;
static const float kKromCsmLoadBiasScale = 2.0f;

// Depth compare of one texel (1 = lit).
float KromCsmLoadCompare(int2 texel, int2 texelMin, int2 texelMax, float cmpDepth)
{
    const int2 t = clamp(texel, texelMin, texelMax);
    return cmpDepth <= shadowMap.Load(int3(t, 0)).r ? 1.0f : 0.0f;
}

// Directional shadow of lights[lightIndex] at positionWS. inside = false when the
// light has no cascades, the point lies outside all of them or outside the
// camera frustum (then 1 is returned and the caller may trace an occlusion ray
// instead). The cascades are fitted to the frustum and the cascade choice
// clamps the view depth to the near plane: a point behind the camera or far
// beside it would land in a cascade whose shadow map need not hold its casters.
float KromCsmVisibilityLoad(float3 positionWS, float3 normalWS, float3 toLight, uint lightIndex, out bool inside)
{
    inside = false;
    const float4 clip = mul(viewProjMatrix, float4(positionWS, 1.0f));
    if (clip.w <= nearPlane || any(abs(clip.xy) > clip.w))
        return 1.0f;
    const float texelSize = max(shadowTexelSize, 1e-8f);
    const float atlasSize = 1.0f / texelSize;
    [loop]
    for (uint shadowIndex = 0u; shadowIndex < shadowLightCount && shadowIndex < 4u; ++shadowIndex)
    {
        if ((uint)(shadowLightMeta[shadowIndex].x + 0.5f) != lightIndex)
            continue;
        const uint firstViewIndex = (uint)(shadowLightExtra[shadowIndex].x + 0.5f);
        const uint viewCount = (uint)(shadowLightExtra[shadowIndex].y + 0.5f);
        if (viewCount == 0u || viewCount >= 6u)
            return 1.0f;
        const float fade = KromShadowDistanceFade(positionWS, shadowLightExtra[shadowIndex].z);
        if (fade >= 0.99f)
            return 1.0f;
        float blend;
        const uint cascade = viewCount > 1u
            ? ChooseCascadeIndexFromViewDepth(positionWS, viewCount, shadowLightExtra[shadowIndex].z,
                                              shadowLightExtra[shadowIndex].w, blend)
            : 0u;
        const float facing = 1.0f - saturate(dot(normalWS, toLight));
        [loop]
        for (uint attempt = 0u; attempt < 2u; ++attempt)
        {
            const uint candidate = cascade + attempt;
            const uint viewIndex = firstViewIndex + candidate;
            if (candidate >= viewCount || viewIndex >= shadowViewCount || viewIndex >= 16u)
                break;
            const float4x4 vp = shadowViewProjArray[viewIndex];
            const float4 rect = shadowViewRect[viewIndex];
            // World size of one texel of this view (orthographic: same everywhere).
            const float3 xRow = float3(vp[0][0], vp[0][1], vp[0][2]);
            const float texelsPerWorld = 0.5f * length(xRow) * rect.z / texelSize;
            const float normalTexels = shadowLightMeta[shadowIndex].z * facing + kKromCsmLoadNormalTexels;
            const float3 p = positionWS + normalWS * (normalTexels / max(texelsPerWorld, 1e-6f));
            const float4 clip = mul(vp, float4(p, 1.0f));
            if (clip.w <= 1e-6f)
                continue;
            const float3 ndc = clip.xyz / clip.w;
            if (abs(ndc.x) >= 1.0f || abs(ndc.y) >= 1.0f || ndc.z <= 0.0f || ndc.z >= 1.0f)
                continue;
            const float3 zRow = float3(vp[2][0], vp[2][1], vp[2][2]);
            const float bias = max(shadowLightMeta[shadowIndex].y * length(zRow) * kKromCsmLoadBiasScale, 1e-6f);
            const float2 localUv = float2(ndc.x * 0.5f + 0.5f, 0.5f - ndc.y * 0.5f);
            const float2 texelPos = (rect.xy + localUv * rect.zw) * atlasSize - 0.5f;
            const int2 texelMin = int2(rect.xy * atlasSize);
            const int2 texelMax = int2((rect.xy + rect.zw) * atlasSize) - 1;
            const int2 base = int2(floor(texelPos));
            const float2 f = frac(texelPos);
            const float cmp = ndc.z - bias;
            const float c00 = KromCsmLoadCompare(base, texelMin, texelMax, cmp);
            const float c10 = KromCsmLoadCompare(base + int2(1, 0), texelMin, texelMax, cmp);
            const float c01 = KromCsmLoadCompare(base + int2(0, 1), texelMin, texelMax, cmp);
            const float c11 = KromCsmLoadCompare(base + int2(1, 1), texelMin, texelMax, cmp);
            const float visibility = lerp(lerp(c00, c10, f.x), lerp(c01, c11, f.x), f.y);
            inside = true;
            return lerp(1.0f, visibility, saturate(shadowLightMeta[shadowIndex].w * (1.0f - fade)));
        }
        return 1.0f;
    }
    return 1.0f;
}

#endif // KROM_SHADOW_CSM_LOAD

#endif // KROM_SHADOW_CSM_LOOKUP_HLSL
