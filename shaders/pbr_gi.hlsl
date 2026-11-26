// =============================================================================
// KROM Engine - shaders/pbr_gi.hlsl
// Surface cache GI in the PBR shaders (doc/PLAN_GI.md, package 4).
//
// The surface cache add-on resolves its radiance cache into a full- or half-resolution
// texture (shaders/surface_cache_apply.hlsl) on TexSlots::GlobalIllumination:
// rgb = indirect diffuse irradiance E, a = view depth and surface normal of
// the texel (gi_texel.hlsl; negative where the far field filled in, 0 for the sky). KromSampleGI upsamples it at the
// shaded pixel with bilinear weights times a depth weight.
//
// The irradiance replaces the probe/IBL irradiance of the diffuse term; it is
// never added to it (the cache already holds sky, probes and bounced light).
// Material AO and GTAO act on it as before. The environment reflection keeps
// coming from IBL, probes and SSR, but is dimmed where the GI is darker than
// the probe/IBL irradiance (KromGiSpecularScale).
//
// Needs the PerFrame block (featureParams, screenSize, camera) in front.
// =============================================================================
#ifndef KROM_PBR_GI_HLSL
#define KROM_PBR_GI_HLSL

#include "gi_texel.hlsl"

Texture2D giIrradianceTex : register(t21);

// Surface cache block (krom-surface-cache, featureParams slot 8, see
// kSurfaceCacheFrameParameterBlock): x = flags, y = strength, z = debug view,
// w = depth tolerance (relative to the view depth).
#define giFlags          (asuint(featureParams[8].x))
#define giStrength       (featureParams[8].y)
#define giDebugView      (asuint(featureParams[8].z))
#define giDepthTolerance (featureParams[8].w)

static const uint GI_FLAG_ENABLED = 1u;

// Irradiance of the GI texture at this pixel. False where it does not apply:
// switched off, the transparent pass (the white fallback is bound there, not
// a half-resolution texture) or only sky around the pixel. The caller then
// keeps the probe/IBL irradiance. coverage: share of the samples
// the cache served (1) rather than the far field (0). normalWS: the normal of
// the geometry (not the normal map), either side.
bool KromSampleGI(float2 pixelPos, float3 positionWS, float3 normalWS, out float3 irradiance, out float coverage)
{
    irradiance = float3(0.0f, 0.0f, 0.0f);
    coverage = 0.0f;
    if ((giFlags & GI_FLAG_ENABLED) == 0u)
        return false;
    uint w, h;
    giIrradianceTex.GetDimensions(w, h);
    const uint2 full = uint2(screenSize.xy);
    const uint2 expected = (full + 1u) / 2u;
    const bool fullResolution = w == full.x && h == full.y;
    if ((!fullResolution && (w != expected.x || h != expected.y)) || w == 0u || h == 0u)
        return false;

    const float depth = max(dot(positionWS - cameraPositionWS.xyz, normalize(cameraForwardWS.xyz)), 1e-4f);
    const float tolerance = max(giDepthTolerance, 1e-3f) * depth;
    // The texel normals face the camera; so does this one.
    float3 surfaceN = normalize(normalWS);
    if (dot(surfaceN, cameraPositionWS.xyz - positionWS) < 0.0f)
        surfaceN = -surfaceN;
    if (fullResolution)
    {
        // The texture holds this pixel. Only where its texel belongs to another
        // surface (an MSAA edge: the resolve saw sample 0) the best of the
        // eight neighbours takes over. Another surface: the depth differs (a
        // silhouette) or the normal does (two surfaces that touch, where the
        // depth is the same on both). The misfit is the depth difference in
        // tolerances, plus 1 for a normal that points elsewhere.
        const int2 q = clamp(int2(pixelPos), int2(0, 0), int2(w, h) - 1);
        float4 best = giIrradianceTex.Load(int3(q, 0));
        float bestDiff = 1e30f;
        if (best.a != 0.0f)
        {
            float texelDepth;
            float3 texelN;
            GiTexelDecode(best.a, texelDepth, texelN);
            bestDiff = abs(texelDepth - depth) / tolerance + (dot(texelN, surfaceN) < 0.7f ? 1.0f : 0.0f);
        }
        if (bestDiff > 0.25f)
        {
            [unroll] for (uint k = 0u; k < 8u; ++k)
            {
                const uint cell = k < 4u ? k : k + 1u;   // 3 x 3 without the centre
                const int2 o = int2((int)(cell % 3u) - 1, (int)(cell / 3u) - 1);
                const float4 t = giIrradianceTex.Load(int3(clamp(q + o, int2(0, 0), int2(w, h) - 1), 0));
                if (t.a == 0.0f)
                    continue;
                float texelDepth;
                float3 texelN;
                GiTexelDecode(t.a, texelDepth, texelN);
                // Straight neighbours before diagonal ones of the same fit.
                const float diff = abs(texelDepth - depth) / tolerance + (dot(texelN, surfaceN) < 0.7f ? 1.0f : 0.0f) +
                                   ((o.x != 0 && o.y != 0) ? 0.01f : 0.0f);
                if (diff < bestDiff)
                {
                    bestDiff = diff;
                    best = t;
                }
            }
        }
        if (bestDiff >= 1e30f)
            return false;
        irradiance = best.rgb;
        coverage = best.a > 0.0f ? 1.0f : 0.0f;
        return true;
    }
    const float2 p = pixelPos * 0.5f - 0.5f;
    const float2 baseF = floor(p);
    const int2 base = int2(baseF);
    const float2 f = p - baseF;

    float3 sum = float3(0.0f, 0.0f, 0.0f);
    float weight = 0.0f;
    float covered = 0.0f;
    float bestDiff = 1e30f;
    float4 best = float4(0.0f, 0.0f, 0.0f, 0.0f);
    [unroll] for (uint i = 0u; i < 4u; ++i)
    {
        const int2 o = int2(i & 1u, i >> 1u);
        const int2 q = clamp(base + o, int2(0, 0), int2(w, h) - 1);
        const float4 t = giIrradianceTex.Load(int3(q, 0));
        if (t.a == 0.0f)
            continue;   // sky
        float texelDepth;
        float3 texelN;
        GiTexelDecode(t.a, texelDepth, texelN);
        const float diff = abs(texelDepth - depth);
        if (diff < bestDiff)
        {
            bestDiff = diff;
            best = t;
        }
        const float2 b = lerp(1.0f - f, f, float2(o));
        const float d = saturate(1.0f - diff / tolerance);
        const float wt = b.x * b.y * d * d;
        sum += t.rgb * wt;
        weight += wt;
        covered += (t.a > 0.0f ? 1.0f : 0.0f) * wt;
    }
    if (weight > 1e-4f)
    {
        irradiance = sum / weight;
        coverage = covered / weight;
        return true;
    }
    // Thin geometry or an edge between the bilinear taps: the closest sample in
    // depth. Not the probe/IBL path: in an enclosed room that would outline
    // every silhouette with sky light. Only all-sky taps fall back.
    if (bestDiff < 1e30f)
    {
        irradiance = best.rgb;
        coverage = best.a > 0.0f ? 1.0f : 0.0f;
        return true;
    }
    return false;
}

// Occlusion of the environment reflection (IBL, probes) by the GI: the
// luminance of the cache irradiance against the probe/IBL irradiance the
// reflection comes from, at most 1 (bounced sunlight does not brighten the
// sky's reflection). Outdoors both agree (the cache's far field is the same
// probe/IBL); in an enclosed room the cache is dark and so is the reflection.
// strength 0 leaves the reflection as it was. SSR replaces the base reflection
// where it hits and is not affected.
float KromGiSpecularScale(float3 giIrradiance, float3 baseIrradiance, float strength)
{
    const float3 luma = float3(0.2126f, 0.7152f, 0.0722f);
    const float base = dot(baseIrradiance, luma);
    const float ratio = base > 1e-5f ? saturate(dot(giIrradiance, luma) / base) : 1.0f;
    return lerp(1.0f, ratio, saturate(strength));
}

// Debug view colour of the coverage: green = cache, red = far field, blue = not applied.
float3 KromGiCoverageColor(bool applied, float coverage)
{
    if (!applied)
        return float3(0.1f, 0.15f, 0.9f);
    return lerp(float3(0.9f, 0.15f, 0.1f), float3(0.15f, 0.85f, 0.2f), saturate(coverage));
}

#endif // KROM_PBR_GI_HLSL
