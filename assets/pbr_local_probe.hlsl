// =============================================================================
// KROM Engine - assets/pbr_local_probe.hlsl
// Local reflection probe (add-on krom-probes, addons/probes/ProbeFeature.hpp).
// Included by the PBR pixel shaders after the PerFrame block and sClamp.
//
// featureParams slots 1..3 (kProbeFrameParameterBlock / ProbeFrameParams):
//   [1].xyz box min (world), [1].w enabled (uint bits)
//   [2].xyz box max (world), [2].w intensity
//   [3].xyz capture position, [3].w prefilter mip count
// Cubes: t18 prefiltered radiance, t19 irradiance (TexSlots::FeatureCube0/1).
// =============================================================================

TextureCube tFeatureCube0 : register(t18);
TextureCube tFeatureCube1 : register(t19);

#define probeBoxMin      (featureParams[1].xyz)
#define probeEnabled     (asuint(featureParams[1].w))
#define probeBoxMax      (featureParams[2].xyz)
#define probeIntensity   (featureParams[2].w)
#define probeCapturePos  (featureParams[3].xyz)
#define probeMipCount    (featureParams[3].w)

// Same mip bias as SamplePrefilteredIBL. Uses screen derivatives: call it
// only in uniform control flow.
float LocalProbeReflectionMipBias(float3 R)
{
    float3 dRdx = ddx(R);
    float3 dRdy = ddy(R);
    float reflectionVariance = max(dot(dRdx, dRdx), dot(dRdy, dRdy));
    return clamp(0.5f * log2(1.0f + reflectionVariance * 512.0f), 0.0f, 2.0f);
}

bool InsideLocalProbe(float3 positionWS)
{
    return all(positionWS >= probeBoxMin) && all(positionWS <= probeBoxMax);
}

float3 SampleLocalProbeIrradiance(float3 N)
{
    return tFeatureCube1.SampleLevel(sClamp, N, 0.0f).rgb * probeIntensity;
}

float3 SampleLocalProbePrefiltered(float3 R, float roughness, float mipBias)
{
    float lod = roughness * max(probeMipCount - 1.0f, 0.0f) + mipBias;
    return tFeatureCube0.SampleLevel(sClamp, normalize(R), lod).rgb * probeIntensity;
}
