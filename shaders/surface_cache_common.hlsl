// =============================================================================
// KROM Engine - shaders/surface_cache_common.hlsl
// Radiance cache of the surface cache GI (doc/PLAN_GI.md, package 3): the
// PerFrame block, the dispatch constants and the hash grid in world space.
//
// The cache stores irradiance E (W/m^2 like the IBL irradiance cube) per cell.
// A cell is (position quantised to the cell size, level, dominant normal axis);
// the cell size doubles with each doubling of the camera distance beyond
// scLevelDistance. Open addressing with linear probing: the start slot comes
// from one hash, a second 32-bit hash (never 0) is stored as the key.
//
// Buffers (UAV slots of the cache kernels, BufSRV slots of the overlay):
//   gCellKeys   uint  per slot, 0 = empty
//   gCellA      uint4 per slot: irradiance rgb (float bits), w = sample weight
//   gCellB      uint4 per slot: position xyz (float bits), w = octahedral normal
//   gCellFrame  uint  per slot: frame of the last registration (0 = new)
// =============================================================================
#ifndef KROM_SURFACE_CACHE_COMMON_HLSL
#define KROM_SURFACE_CACHE_COMMON_HLSL

#include "krom_platform.hlsl"

struct GpuLightData
{
    float4 positionWS;
    float4 directionWS;
    float4 colorIntensity;
    float4 params;
};

// Same layout as shaders/pbr_lit.ps.hlsl (FrameConstants, RenderFrameConstants.hpp).
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
    float4       featureParams[9];
};

// Dispatch constants of every cache kernel and the overlay (SurfaceCacheConstants).
cbuffer SurfaceCacheParams : register(b3)
{
    uint   scCapacityMask;    // slot count - 1 (power of two)
    uint   scFrame;           // 1-based frame counter of the cache
    uint   scRayBudget;       // rays per frame
    uint   scTlasNodeCount;
    float  scCellSize;        // cell size of level 0 (m)
    float  scLevelDistance;   // camera distance covered by level 0 (m)
    float  scRayLength;       // m, 0 = unbounded
    uint   scMaxRaysPerCell;
    uint   scViewportWidth;
    uint   scViewportHeight;
    uint   scRegisterStep;    // one registration ray per scRegisterStep^2 pixels
    uint   scDirtyBoxCount;
    uint   scMaxAge;          // frames without registration until a cell is freed
    uint   scFlags;           // SC_FLAG_*
    uint   scMaxSamples;      // sample weight cap (history length)
    uint   scMaterialCount;
    // Material kernel and overlay
    uint   scMaterialIndex;
    uint   scView;            // overlay: SurfaceCacheDebugView
    float  scOverlayAlpha;
    float  scViewScale;       // overlay: metres shown white (distance) or exposure (irradiance)
    float4 scAlbedoFactor;    // material kernel: w = 1 if an albedo texture is bound
    float4 scEmissionFactor;  // material kernel: w = 1 if an emission texture is bound
};

static const uint SC_FLAG_IBL        = 1u << 0;   // tIBLIrradiance is bound
static const uint SC_FLAG_SHADOW_MAP = 1u << 1;   // shadowMap holds this frame's cascades
static const uint SC_FLAG_PROBES     = 1u << 2;   // tFeatureTexture holds the local probes
static const uint SC_FLAG_APPLY_HALF = 1u << 3;   // resolve for shading in half resolution

static const uint  SC_NONE        = 0xFFFFFFFFu;
static const uint  SC_MAX_PROBE   = 16u;          // linear probing steps
static const uint  SC_MAX_LEVEL   = 15u;
static const float SC_PI          = 3.14159265358979323846f;
// Fixed point of the radiance sums (uint atomics). 2^16 with dithered rounding:
// with 1024 and +0.5 a dark cell kept itself lit (E = 0.003 gave a radiance of
// 0.5/1024 per sample, rounded up to 1/1024, which is E = pi/1024 again).
// Headroom: SC_MAX_RADIANCE * 2^16 = 2^24 per sample, so at most 255 samples
// per cell and frame (the trace kernel caps the rays per cell).
static const float SC_ACCUM_SCALE = 65536.0f;
static const float SC_MAX_RADIANCE = 256.0f;      // per sample (fireflies)
// Weight of the probe/IBL start value of a new cell: 0, so the first rays of
// the cell replace it completely. With a weight it stayed in the running mean
// (4 of 100 samples = 4 % sky after 100 rays), and in an enclosed room every
// cell that came into view brought some of the sky in.
static const uint  SC_START_WEIGHT = 0u;
// History of a cell in frames of its rays. A resting cell averages over
// SC_LONG_HISTORY_FRAMES (the flicker of a resting image falls with the square
// root of it: measured in Room 2, 16 -> 64 frames halves it). A cell whose
// light changed (resolve kernel: its short mean leaves the long one by more
// than the noise) or whose surroundings moved (dirty boxes) falls back to
// SC_SHORT_HISTORY_FRAMES and settles within that time.
static const uint  SC_LONG_HISTORY_FRAMES  = 256u;
static const uint  SC_SHORT_HISTORY_FRAMES = 8u;
static const float SC_CHANGE_EVIDENCE = 10.0f;     // cumulative sum of the change detector from which the history shortens
static const uint  SC_ACCUM_WORDS = 12u;          // gAccum words per slot: 0-2 sums, 3 count, 4-5 change sums (6 free), 7 noise power,
                                                  // 8-10 display value (irradiance averaged with the neighbours)
static const uint  SC_ACCUM_DISPLAY = 8u;
// Word 11: squared distance of the cell's anchor (position in cellB) to the
// centre of the cell, SC_ANCHOR_FREE while any camera ray may replace it.
static const uint  SC_ACCUM_ANCHOR = 11u;
static const float SC_ANCHOR_FREE = 1e30f;
// Sample weight from which a cell counts in full, in the display average and in
// the resolve for shading.
static const float SC_CONFIDENT_SAMPLES = 1024.0f;

uint ScPcg(uint v)
{
    const uint state = v * 747796405u + 2891336453u;
    const uint word = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
    return (word >> 22u) ^ word;
}

float ScRandom(inout uint seed)
{
    seed = ScPcg(seed);
    return (seed & 0xFFFFFFu) / 16777216.0f;
}

uint ScOctEncode(float3 n)
{
    n /= max(abs(n.x) + abs(n.y) + abs(n.z), 1e-6f);
    float2 e = n.z >= 0.0f ? n.xy : (1.0f - abs(n.yx)) * float2(n.x >= 0.0f ? 1.0f : -1.0f, n.y >= 0.0f ? 1.0f : -1.0f);
    const uint2 q = (uint2)round(saturate(e * 0.5f + 0.5f) * 65535.0f);
    return q.x | (q.y << 16u);
}

float3 ScOctDecode(uint packed)
{
    const float2 e = float2(packed & 0xFFFFu, packed >> 16u) / 65535.0f * 2.0f - 1.0f;
    float3 n = float3(e, 1.0f - abs(e.x) - abs(e.y));
    if (n.z < 0.0f)
        n.xy = (1.0f - abs(n.yx)) * float2(n.x >= 0.0f ? 1.0f : -1.0f, n.y >= 0.0f ? 1.0f : -1.0f);
    return normalize(n);
}

float3 ScCosineDirection(float3 n, inout uint seed)
{
    const float u1 = ScRandom(seed);
    const float u2 = ScRandom(seed);
    const float r = sqrt(u1);
    const float phi = 6.28318530718f * u2;
    const float3 t = normalize(abs(n.x) > 0.5f ? cross(n, float3(0.0f, 1.0f, 0.0f)) : cross(n, float3(1.0f, 0.0f, 0.0f)));
    const float3 b = cross(n, t);
    return normalize(t * (r * cos(phi)) + b * (r * sin(phi)) + n * sqrt(max(0.0f, 1.0f - u1)));
}

// Level of the cell at p: 0 up to scLevelDistance from the camera, then one
// level per doubling of the distance.
uint ScLevel(float3 p)
{
    const float d = length(p - cameraPositionWS.xyz) / max(scLevelDistance, 1e-3f);
    return d <= 1.0f ? 0u : min((uint)floor(log2(d)) + 1u, SC_MAX_LEVEL);
}

float ScCellSizeOfLevel(uint level)
{
    return scCellSize * exp2((float)level);
}

// Squared distance of p to the centre of its cell.
float ScAnchorDistance(float3 p)
{
    const float size = ScCellSizeOfLevel(ScLevel(p));
    const float3 d = p - (floor(p / size) + 0.5f) * size;
    return dot(d, d);
}

// Dominant axis of the normal with sign: 0..5.
uint ScNormalBucket(float3 n)
{
    const float3 a = abs(n);
    if (a.x >= a.y && a.x >= a.z)
        return n.x >= 0.0f ? 0u : 1u;
    if (a.y >= a.z)
        return n.y >= 0.0f ? 2u : 3u;
    return n.z >= 0.0f ? 4u : 5u;
}

struct ScKey
{
    uint start;   // first slot (before masking)
    uint key;     // stored key, never 0
};

// Key of the cell q (integer cell coordinate) of a level and normal bucket.
// The resolve of package 4 uses it for the neighbours of a pixel's cell.
ScKey ScMakeKeyAt(int3 q, uint level, uint bucket)
{
    const uint tag = level * 8u + bucket;
    ScKey k;
    k.start = ScPcg(asuint(q.x) + ScPcg(asuint(q.y) + ScPcg(asuint(q.z) + ScPcg(tag))));
    uint h = ScPcg(tag ^ 0x68E31DA4u);
    h = ScPcg(h ^ asuint(q.z) * 0x1B873593u);
    h = ScPcg(h ^ asuint(q.y) * 0xCC9E2D51u);
    h = ScPcg(h ^ asuint(q.x) * 0x85EBCA6Bu);
    k.key = max(h, 1u);
    return k;
}

ScKey ScMakeKey(float3 p, float3 n)
{
    const uint level = ScLevel(p);
    return ScMakeKeyAt((int3)floor(p / ScCellSizeOfLevel(level)), level, ScNormalBucket(n));
}

// Slot of an existing cell or SC_NONE. Works on RW and read-only key buffers.
#define SC_DEFINE_FIND(NAME, KEYS)                                                     \
uint NAME(ScKey k)                                                                     \
{                                                                                      \
    [loop] for (uint i = 0u; i < SC_MAX_PROBE; ++i)                                    \
    {                                                                                  \
        const uint slot = (k.start + i) & scCapacityMask;                              \
        const uint stored = KEYS[slot];                                                \
        if (stored == k.key)                                                           \
            return slot;                                                               \
        if (stored == 0u)                                                              \
            return SC_NONE;                                                            \
    }                                                                                  \
    return SC_NONE;                                                                    \
}

// --- Far field: local probes, then IBL or the ambient colour ----------------

#if defined(SC_WITH_ENVIRONMENT)

TextureCube       tIBLIrradiance  : register(t5);
Texture2D<float4> tFeatureTexture : register(t19);
SamplerState      sClamp          : register(s1);

#define scProbeCount (asuint(featureParams[1].x))

float4 ScProbeTexel(uint probe, uint texel)
{
    return tFeatureTexture.Load(int3(int(texel), int(probe), 0));
}

float3 ScProbeIrradianceSH(uint probe, float3 n)
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
        e += ScProbeTexel(probe, 6u + k).rgb * basis[k];
    return max(e, 0.0f);
}

// Irradiance of a surface at p facing n without the cache: the local probes
// blended as in EvaluateLocalProbes (pbr_local_probe.hlsl), the rest from the
// IBL irradiance cube, or the ambient colour without IBL (pbr_lit: albedo *
// ambient, so E = pi * ambient). Start value of new cells and the far field
// of rays that leave the scene or end at scRayLength.
// withGlobal false: the local probes only. For a ray that ended at scRayLength
// with geometry still ahead of it: it has not left the scene, so the sky (IBL,
// ambient colour) must not count (an enclosed room larger than the ray length
// would fill with sky light).
float3 ScFarFieldIrradiance(float3 p, float3 n, bool withGlobal)
{
    float3 global = float3(0.0f, 0.0f, 0.0f);
    if (withGlobal)
    {
        global = ambientColor.rgb * ambientColor.a * SC_PI;
        if ((scFlags & SC_FLAG_IBL) != 0u)
            global = tIBLIrradiance.SampleLevel(sClamp, n, 0.0f).rgb;
    }
    if ((scFlags & SC_FLAG_PROBES) == 0u)
        return global;
    float3 probes = float3(0.0f, 0.0f, 0.0f);
    float remaining = 1.0f;
    uint used = 0u;
    [loop] for (uint i = 0u; i < scProbeCount && i < 64u; ++i)
    {
        const float4 r0 = ScProbeTexel(i, 0u);
        const float4 r1 = ScProbeTexel(i, 1u);
        const float4 r2 = ScProbeTexel(i, 2u);
        const float4 eb = ScProbeTexel(i, 3u);
        const float3 pl = float3(dot(r0.xyz, p) + r0.w, dot(r1.xyz, p) + r1.w, dot(r2.xyz, p) + r2.w);
        const float3 d = eb.xyz - abs(pl);
        const float inside = min(d.x, min(d.y, d.z));
        if (inside <= 0.0f)
            continue;
        float w = saturate(inside / eb.w);
        w = w * w * (3.0f - 2.0f * w);
        const float share = w * remaining;
        probes += ScProbeIrradianceSH(i, n) * (ScProbeTexel(i, 4u).w * share);
        remaining *= (1.0f - w);
        if (++used >= 2u || remaining < 0.01f)
            break;
    }
    return probes + global * remaining;
}

float3 ScFarFieldIrradiance(float3 p, float3 n)
{
    return ScFarFieldIrradiance(p, n, true);
}

#endif // SC_WITH_ENVIRONMENT

#endif // KROM_SURFACE_CACHE_COMMON_HLSL
