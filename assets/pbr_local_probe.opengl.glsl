// =============================================================================
// KROM Engine - assets/pbr_local_probe.opengl.glsl
// GLSL counterpart of pbr_local_probe.hlsl (add-on krom-probes).
// Included by pbr_lit.opengl.fs.glsl after the PerFrame block.
//
// featureParams slots 1..3 (kProbeFrameParameterBlock / ProbeFrameParams):
//   [1].xyz box min (world), [1].w enabled (uint bits)
//   [2].xyz box max (world), [2].w intensity
//   [3].xyz capture position, [3].w prefilter mip count
// Cubes: unit 18 prefiltered radiance, unit 19 irradiance (TexSlots::FeatureCube0/1).
// =============================================================================

uniform samplerCube tFeatureCube0;
uniform samplerCube tFeatureCube1;

#define probeBoxMin      (featureParams[1].xyz)
#define probeEnabled     (floatBitsToUint(featureParams[1].w))
#define probeBoxMax      (featureParams[2].xyz)
#define probeIntensity   (featureParams[2].w)
#define probeCapturePos  (featureParams[3].xyz)
#define probeMipCount    (featureParams[3].w)

// Same mip bias as SamplePrefilteredIBL. Uses screen derivatives: call it
// only in uniform control flow.
float LocalProbeReflectionMipBias(vec3 R)
{
    vec3 dRdx = dFdx(R);
    vec3 dRdy = dFdy(R);
    float reflectionVariance = max(dot(dRdx, dRdx), dot(dRdy, dRdy));
    return clamp(0.5 * log2(1.0 + reflectionVariance * 512.0), 0.0, 2.0);
}

bool InsideLocalProbe(vec3 positionWS)
{
    return all(greaterThanEqual(positionWS, probeBoxMin)) && all(lessThanEqual(positionWS, probeBoxMax));
}

vec3 SampleLocalProbeIrradiance(vec3 N)
{
    return textureLod(tFeatureCube1, N, 0.0).rgb * probeIntensity;
}

vec3 SampleLocalProbePrefiltered(vec3 R, float roughness, float mipBias)
{
    float lod = roughness * max(probeMipCount - 1.0, 0.0) + mipBias;
    return textureLod(tFeatureCube0, normalize(R), lod).rgb * probeIntensity;
}
