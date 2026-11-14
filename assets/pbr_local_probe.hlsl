// =============================================================================
// KROM Engine - assets/pbr_local_probe.hlsl
// Local reflection probes and specular occlusion (add-on krom-probes,
// addons/probes/ProbeFeature.hpp). Included by the PBR pixel shaders after the
// PerFrame block and sClamp.
//
// featureParams[1] (kProbeFrameParameterBlock / ProbeFrameParams):
//   x probe count (uint bits), y flags (uint bits: bit 0 specular occlusion,
//   bits 8..15 debug view), z prefilter mip count
// t18 TexSlots::FeatureCubeArray: prefiltered radiance, one cube per probe
// t19 TexSlots::FeatureTexture:   one row per probe, see ProbeFeature.hpp
// =============================================================================

TextureCubeArray  tFeatureCubeArray : register(t18);
Texture2D<float4> tFeatureTexture   : register(t19);

#define probeCount       (asuint(featureParams[1].x))
#define probeFlags       (asuint(featureParams[1].y))
#define probeMipCount    (featureParams[1].z)
#define probeDebugView   ((probeFlags >> 8u) & 0xFFu)
#define probeSpecularOcclusionEnabled ((probeFlags & 1u) != 0u)

static const uint kLocalProbeMaxPerPixel = 2u;

struct LocalProbeIBL
{
    float3 irradiance;   // sum of the probes' irradiance, already weighted
    float3 specular;     // sum of the probes' prefiltered radiance, already weighted
    float  remaining;    // share left for the global IBL
    int    firstProbe;   // -1 = none
};

// Same mip bias as SamplePrefilteredIBL. Uses screen derivatives: call it
// only in uniform control flow.
float LocalProbeReflectionMipBias(float3 R)
{
    float3 dRdx = ddx(R);
    float3 dRdy = ddy(R);
    float reflectionVariance = max(dot(dRdx, dRdx), dot(dRdy, dRdy));
    return clamp(0.5f * log2(1.0f + reflectionVariance * 512.0f), 0.0f, 2.0f);
}

float4 LocalProbeTexel(uint probe, uint texel)
{
    return tFeatureTexture.Load(int3(int(texel), int(probe), 0));
}

float3 LocalProbeIrradianceSH(uint probe, float3 n)
{
    const float basis[9] = {
        0.282095f,
        0.488603f * n.y, 0.488603f * n.z, 0.488603f * n.x,
        1.092548f * n.x * n.y, 1.092548f * n.y * n.z,
        0.315392f * (3.0f * n.z * n.z - 1.0f),
        1.092548f * n.x * n.z, 0.546274f * (n.x * n.x - n.y * n.y)
    };
    float3 e = float3(0.0f, 0.0f, 0.0f);
    [unroll] for (uint k = 0u; k < 9u; ++k)
        e += LocalProbeTexel(probe, 6u + k).rgb * basis[k];
    return max(e, 0.0f);
}

LocalProbeIBL EvaluateLocalProbes(float3 positionWS, float3 N, float3 R, float roughness, float mipBias)
{
    LocalProbeIBL result;
    result.irradiance = float3(0.0f, 0.0f, 0.0f);
    result.specular = float3(0.0f, 0.0f, 0.0f);
    result.remaining = 1.0f;
    result.firstProbe = -1;

    const float lod = roughness * max(probeMipCount - 1.0f, 0.0f) + mipBias;
    uint used = 0u;
    [loop] for (uint i = 0u; i < probeCount; ++i)
    {
        const float4 r0 = LocalProbeTexel(i, 0u);
        const float4 r1 = LocalProbeTexel(i, 1u);
        const float4 r2 = LocalProbeTexel(i, 2u);
        const float4 eb = LocalProbeTexel(i, 3u);
        const float3 pl = float3(dot(r0.xyz, positionWS) + r0.w,
                                 dot(r1.xyz, positionWS) + r1.w,
                                 dot(r2.xyz, positionWS) + r2.w);
        const float3 d = eb.xyz - abs(pl);
        const float inside = min(d.x, min(d.y, d.z));
        if (inside <= 0.0f)
            continue;

        float w = saturate(inside / eb.w);
        w = w * w * (3.0f - 2.0f * w);
        const float share = w * result.remaining;

        const float4 ci = LocalProbeTexel(i, 4u);
        const float4 pr = LocalProbeTexel(i, 5u);

        // Box projection (parallax correction) in box space; rough surfaces
        // fall back towards the plain reflection direction.
        float3 dir = R;
        if (pr.x > 0.5f)
        {
            const float3 rl = float3(dot(r0.xyz, R), dot(r1.xyz, R), dot(r2.xyz, R));
            const float3 safe = float3(abs(rl.x) > 1e-5f ? rl.x : 1e-5f,
                                       abs(rl.y) > 1e-5f ? rl.y : 1e-5f,
                                       abs(rl.z) > 1e-5f ? rl.z : 1e-5f);
            const float3 t1 = (eb.xyz - pl) / safe;
            const float3 t2 = (-eb.xyz - pl) / safe;
            const float3 tMax = max(t1, t2);
            const float dist = min(tMax.x, min(tMax.y, tMax.z));
            const float3 dl = pl + rl * dist - ci.xyz;
            const float3 dw = r0.xyz * dl.x + r1.xyz * dl.y + r2.xyz * dl.z;
            dir = lerp(normalize(dw), R, roughness * roughness);
        }

        result.irradiance += LocalProbeIrradianceSH(i, N) * (ci.w * share);
        result.specular += tFeatureCubeArray.SampleLevel(sClamp, float4(normalize(dir), pr.y), lod).rgb * (ci.w * share);
        result.remaining *= (1.0f - w);
        if (result.firstProbe < 0)
            result.firstProbe = int(i);
        if (++used >= kLocalProbeMaxPerPixel || result.remaining < 0.01f)
            break;
    }
    return result;
}

// Lagarde & de Rousiers, "Moving Frostbite to PBR" (2014).
float ComputeSpecularOcclusion(float NoV, float ao, float roughness)
{
    return saturate(pow(NoV + ao, exp2(-16.0f * roughness - 1.0f)) - 1.0f + ao);
}

float3 LocalProbeDebugColor(int probe)
{
    if (probe < 0)
        return float3(0.0f, 0.0f, 0.0f);
    const float h = frac(float(probe) * 0.618034f);
    return saturate(abs(frac(h + float3(0.0f, 2.0f / 3.0f, 1.0f / 3.0f)) * 6.0f - 3.0f) - 1.0f) * 0.8f + 0.2f;
}
