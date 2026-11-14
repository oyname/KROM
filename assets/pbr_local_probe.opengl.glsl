// =============================================================================
// KROM Engine - assets/pbr_local_probe.opengl.glsl
// GLSL counterpart of pbr_local_probe.hlsl (add-on krom-probes).
// Included by pbr_lit.opengl.fs.glsl after the PerFrame block.
//
// featureParams[1]: x probe count (uint bits), y flags (bit 0 specular
// occlusion, bits 8..15 debug view), z prefilter mip count
// Unit 18 TexSlots::FeatureCubeArray: prefiltered radiance, one cube per probe
// Unit 19 TexSlots::FeatureTexture:   one row per probe, see ProbeFeature.hpp
// =============================================================================

uniform samplerCubeArray tFeatureCubeArray;
uniform sampler2D        tFeatureTexture;

#define probeCount       (floatBitsToUint(featureParams[1].x))
#define probeFlags       (floatBitsToUint(featureParams[1].y))
#define probeMipCount    (featureParams[1].z)
#define probeDebugView   ((probeFlags >> 8u) & 0xFFu)
#define probeSpecularOcclusionEnabled ((probeFlags & 1u) != 0u)

const uint kLocalProbeMaxPerPixel = 2u;

struct LocalProbeIBL
{
    vec3  irradiance;
    vec3  specular;
    float remaining;
    int   firstProbe;
};

// Same mip bias as SamplePrefilteredIBL. Uses screen derivatives: call it
// only in uniform control flow.
float LocalProbeReflectionMipBias(vec3 R)
{
    vec3 dRdx = dFdx(R);
    vec3 dRdy = dFdy(R);
    float reflectionVariance = max(dot(dRdx, dRdx), dot(dRdy, dRdy));
    return clamp(0.5 * log2(1.0 + reflectionVariance * 512.0), 0.0, 2.0);
}

vec4 LocalProbeTexel(uint probe, uint texel)
{
    return texelFetch(tFeatureTexture, ivec2(int(texel), int(probe)), 0);
}

vec3 LocalProbeIrradianceSH(uint probe, vec3 n)
{
    float basis[9] = float[9](
        0.282095,
        0.488603 * n.y, 0.488603 * n.z, 0.488603 * n.x,
        1.092548 * n.x * n.y, 1.092548 * n.y * n.z,
        0.315392 * (3.0 * n.z * n.z - 1.0),
        1.092548 * n.x * n.z, 0.546274 * (n.x * n.x - n.y * n.y));
    vec3 e = vec3(0.0);
    for (uint k = 0u; k < 9u; ++k)
        e += LocalProbeTexel(probe, 6u + k).rgb * basis[k];
    return max(e, vec3(0.0));
}

LocalProbeIBL EvaluateLocalProbes(vec3 positionWS, vec3 N, vec3 R, float roughness, float mipBias)
{
    LocalProbeIBL result;
    result.irradiance = vec3(0.0);
    result.specular = vec3(0.0);
    result.remaining = 1.0;
    result.firstProbe = -1;

    float lod = roughness * max(probeMipCount - 1.0, 0.0) + mipBias;
    uint used = 0u;
    for (uint i = 0u; i < probeCount; ++i)
    {
        vec4 r0 = LocalProbeTexel(i, 0u);
        vec4 r1 = LocalProbeTexel(i, 1u);
        vec4 r2 = LocalProbeTexel(i, 2u);
        vec4 eb = LocalProbeTexel(i, 3u);
        vec3 pl = vec3(dot(r0.xyz, positionWS) + r0.w,
                       dot(r1.xyz, positionWS) + r1.w,
                       dot(r2.xyz, positionWS) + r2.w);
        vec3 d = eb.xyz - abs(pl);
        float inside = min(d.x, min(d.y, d.z));
        if (inside <= 0.0)
            continue;

        float w = clamp(inside / eb.w, 0.0, 1.0);
        w = w * w * (3.0 - 2.0 * w);
        float share = w * result.remaining;

        vec4 ci = LocalProbeTexel(i, 4u);
        vec4 pr = LocalProbeTexel(i, 5u);

        vec3 dir = R;
        if (pr.x > 0.5)
        {
            vec3 rl = vec3(dot(r0.xyz, R), dot(r1.xyz, R), dot(r2.xyz, R));
            vec3 safe = vec3(abs(rl.x) > 1e-5 ? rl.x : 1e-5,
                             abs(rl.y) > 1e-5 ? rl.y : 1e-5,
                             abs(rl.z) > 1e-5 ? rl.z : 1e-5);
            vec3 t1 = (eb.xyz - pl) / safe;
            vec3 t2 = (-eb.xyz - pl) / safe;
            vec3 tMax = max(t1, t2);
            float dist = min(tMax.x, min(tMax.y, tMax.z));
            vec3 dl = pl + rl * dist - ci.xyz;
            vec3 dw = r0.xyz * dl.x + r1.xyz * dl.y + r2.xyz * dl.z;
            dir = mix(normalize(dw), R, roughness * roughness);
        }

        result.irradiance += LocalProbeIrradianceSH(i, N) * (ci.w * share);
        result.specular += textureLod(tFeatureCubeArray, vec4(normalize(dir), pr.y), lod).rgb * (ci.w * share);
        result.remaining *= (1.0 - w);
        if (result.firstProbe < 0)
            result.firstProbe = int(i);
        used += 1u;
        if (used >= kLocalProbeMaxPerPixel || result.remaining < 0.01)
            break;
    }
    return result;
}

// Lagarde & de Rousiers, "Moving Frostbite to PBR" (2014).
float ComputeSpecularOcclusion(float NoV, float ao, float roughness)
{
    return clamp(pow(NoV + ao, exp2(-16.0 * roughness - 1.0)) - 1.0 + ao, 0.0, 1.0);
}

vec3 LocalProbeDebugColor(int probe)
{
    if (probe < 0)
        return vec3(0.0);
    float h = fract(float(probe) * 0.618034);
    return clamp(abs(fract(h + vec3(0.0, 2.0 / 3.0, 1.0 / 3.0)) * 6.0 - 3.0) - 1.0, 0.0, 1.0) * 0.8 + 0.2;
}
