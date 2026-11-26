// =============================================================================
// KROM Engine - shaders/surface_cache_update.hlsl
// Kernels of the radiance cache (doc/PLAN_GI.md, package 3). The includer
// defines exactly one SC_KERNEL_* before including this file:
//
//   SC_KERNEL_BEGIN     1 thread: keeps last frame's counters, clears them
//   SC_KERNEL_REGISTER  camera rays on a jittered grid (one per scRegisterStep^2
//                       pixels) insert their cells and append them to the
//                       active list; new cells start with the probe/IBL value
//   SC_KERNEL_TRACE     scRayBudget cosine rays from the active cells; shading
//                       at the hit: emission + albedo / pi * (sun + lights +
//                       cache of the hit cell); miss: far field
//   SC_KERNEL_RESOLVE   per active cell: mean of this frame's rays blended into
//                       the stored irradiance (weight capped at scMaxSamples)
//   SC_KERNEL_AGE       per slot: frees cells not registered for scMaxAge
//                       frames, lowers the weight in changed regions
//   SC_KERNEL_MATERIAL  one group: mean albedo/emission of one material from
//                       the smallest mips of its textures
//
// UAVs of the cache kernels: u0 keys, u1 cellA, u2 cellB, u3 cellFrame,
// u4 accum (4 uint per slot: rgb sums, sample count), u5 active list,
// u6 counters (SC_COUNTER_*; 16..31 hold the previous frame's values).
// =============================================================================

#define SC_COUNTER_ACTIVE    0u
#define SC_COUNTER_NEW       1u
#define SC_COUNTER_FULL      2u   // insert failed: probing range full
#define SC_COUNTER_RAYS      3u
#define SC_COUNTER_OCCUPIED  4u
#define SC_COUNTER_EVICTED   5u
#define SC_COUNTER_CHANGE    6u   // sum of |dE| / E over resolved cells, x 1000
#define SC_COUNTER_RESOLVED  7u
#define SC_COUNTER_RADIANCE  8u   // sum of the resolved cells' luminance of E / pi, x 1000
#define SC_COUNTER_SUN_CSM   9u   // sun visibility taken from the cascades
#define SC_COUNTER_SUN_RAYS  10u  // sun visibility from an occlusion ray
#define SC_COUNTER_SUN_LIT   11u  // sum of the sun visibility of both, x 1000
#define SC_COUNTER_PREVIOUS  16u

#if defined(SC_KERNEL_MATERIAL)

// ─── Mean albedo and emission of one material ───────────────────────────────

#include "surface_cache_common.hlsl"

Texture2D<float4> tMaterialAlbedo   : register(t8);
Texture2D<float4> tMaterialEmission : register(t9);
RWStructuredBuffer<float4> gMaterials : register(u0);   // 2 per material: albedo, emission

groupshared float4 gsSum[64];

// Mean of the mip with at most 8 x 8 texels (Load: no sampler, BC formats
// decode in the texture unit; sRGB views return linear values).
float4 MeanOfSmallestMip(Texture2D<float4> tex, uint2 gtid, uint gi)
{
    uint w, h, levels;
    tex.GetDimensions(0u, w, h, levels);
    uint mip = 0u;
    [loop] while (mip + 1u < levels && max(w >> mip, h >> mip) > 8u)
        ++mip;
    const uint mw = max(w >> mip, 1u);
    const uint mh = max(h >> mip, 1u);
    float4 sum = float4(0.0f, 0.0f, 0.0f, 0.0f);
    [loop] for (uint y = gtid.y; y < mh; y += 8u)
        [loop] for (uint x = gtid.x; x < mw; x += 8u)
            sum += float4(tex.Load(int3(int(x), int(y), int(mip))).rgb, 1.0f);
    gsSum[gi] = sum;
    GroupMemoryBarrierWithGroupSync();
    [unroll] for (uint stride = 32u; stride > 0u; stride >>= 1u)
    {
        if (gi < stride)
            gsSum[gi] += gsSum[gi + stride];
        GroupMemoryBarrierWithGroupSync();
    }
    const float4 total = gsSum[0];
    GroupMemoryBarrierWithGroupSync();
    return total.w > 0.0f ? float4(total.rgb / total.w, 1.0f) : float4(1.0f, 1.0f, 1.0f, 1.0f);
}

[numthreads(8, 8, 1)]
void main(uint3 gtid : SV_GroupThreadID, uint gi : SV_GroupIndex)
{
    // Both means always (barriers in uniform flow); unbound textures are ignored.
    const float3 albedoMean = MeanOfSmallestMip(tMaterialAlbedo, gtid.xy, gi).rgb;
    const float3 emissionMean = MeanOfSmallestMip(tMaterialEmission, gtid.xy, gi).rgb;
    const float3 albedo = scAlbedoFactor.rgb * (scAlbedoFactor.w > 0.5f ? albedoMean : float3(1.0f, 1.0f, 1.0f));
    const float3 emission = scEmissionFactor.rgb * (scEmissionFactor.w > 0.5f ? emissionMean : float3(1.0f, 1.0f, 1.0f));
    if (gi == 0u)
    {
        gMaterials[scMaterialIndex * 2u + 0u] = float4(saturate(albedo), 1.0f);
        gMaterials[scMaterialIndex * 2u + 1u] = float4(max(emission, 0.0f), 0.0f);
    }
}

#else

// ─── Cache kernels ──────────────────────────────────────────────────────────

#define SC_WITH_ENVIRONMENT 1
#include "surface_cache_trace.hlsl"
#include "surface_cache_common.hlsl"

RWStructuredBuffer<uint>  gCellKeys   : register(u0);
RWStructuredBuffer<uint4> gCellA      : register(u1);
RWStructuredBuffer<uint4> gCellB      : register(u2);
RWStructuredBuffer<uint>  gCellFrame  : register(u3);
RWStructuredBuffer<uint>  gAccum      : register(u4);
RWStructuredBuffer<uint>  gActiveList : register(u5);
RWStructuredBuffer<uint>  gCounters   : register(u6);

KROM_BUFFER_SRV(StructuredBuffer<float4>, gMaterials,  10);   // 2 per material
KROM_BUFFER_SRV(StructuredBuffer<float4>, gDirtyBoxes, 11);   // 2 per box: min, max

SC_DEFINE_FIND(ScFind, gCellKeys)

#if defined(SC_KERNEL_BEGIN)

[numthreads(1, 1, 1)]
void main()
{
    [unroll] for (uint i = 0u; i < 16u; ++i)
    {
        gCounters[SC_COUNTER_PREVIOUS + i] = gCounters[i];
        gCounters[i] = 0u;
    }
}

#elif defined(SC_KERNEL_REGISTER)

// Slot of the cell, inserted if missing; SC_NONE if the probing range is full.
uint ScFindOrInsert(ScKey k)
{
    [loop] for (uint i = 0u; i < SC_MAX_PROBE; ++i)
    {
        const uint slot = (k.start + i) & scCapacityMask;
        uint previous;
        InterlockedCompareExchange(gCellKeys[slot], 0u, k.key, previous);
        if (previous == 0u || previous == k.key)
            return slot;
    }
    return SC_NONE;
}

[numthreads(8, 8, 1)]
void main(uint3 dtid : SV_DispatchThreadID)
{
    const uint step = max(scRegisterStep, 1u);
    const uint gridW = (scViewportWidth + step - 1u) / step;
    const uint gridH = (scViewportHeight + step - 1u) / step;
    if (dtid.x >= gridW || dtid.y >= gridH)
        return;
    // One jittered pixel per block, a new one every frame.
    const uint jitter = ScPcg((dtid.y * gridW + dtid.x) ^ ScPcg(scFrame * 0x9E3779B9u));
    const uint2 pixel = dtid.xy * step + uint2(jitter % step, (jitter / step) % step);
    if (pixel.x >= scViewportWidth || pixel.y >= scViewportHeight)
        return;

    const float2 uv = (float2(pixel) + 0.5f) / float2(scViewportWidth, scViewportHeight);
    const float2 ndc = float2(uv.x * 2.0f - 1.0f, 1.0f - uv.y * 2.0f);
    const float4 h = mul(invViewProjMatrix, float4(ndc, 0.5f, 1.0f));
    const float3 origin = cameraPositionWS.xyz;
    const float3 dir = normalize(h.xyz / h.w - origin);

    TraceStats stats = TraceStatsZero();
    TraceHit hit;
    if (!TraceClosest(origin, dir, 0.0f, 1e30f, scTlasNodeCount, hit, stats))
        return;
    float3 n = hit.normalWS;
    if (dot(n, dir) > 0.0f)
        n = -n;
    const float3 p = origin + dir * hit.t;

    const uint slot = ScFindOrInsert(ScMakeKey(p, n));
    uint previous;
    if (slot == SC_NONE)
    {
        InterlockedAdd(gCounters[SC_COUNTER_FULL], 1u, previous);
        return;
    }
    uint lastFrame;
    InterlockedExchange(gCellFrame[slot], scFrame, lastFrame);
    if (lastFrame == scFrame)
        return;   // another ray registered this cell already
    uint index;
    InterlockedAdd(gCounters[SC_COUNTER_ACTIVE], 1u, index);
    gActiveList[index] = slot;
    gCellB[slot] = uint4(asuint(p), ScOctEncode(n));
    if (lastFrame == 0u)
    {
        gCellA[slot] = uint4(asuint(ScFarFieldIrradiance(p, n)), SC_START_WEIGHT);
        InterlockedAdd(gCounters[SC_COUNTER_NEW], 1u, previous);
    }
}

#elif defined(SC_KERNEL_TRACE)

#define KROM_SHADOW_CSM_LOAD 1
Texture2D<float> shadowMap : register(t4);
#include "shadow_csm_lookup.hlsl"

float ScCalcAttenuation(float dist, float range)
{
    const float ratio = saturate(1.0f - dist / max(range, 0.001f));
    return (ratio * ratio) / max(dist * dist, 1e-4f);
}

// Direct irradiance at p (normal n): every directional light (cascades or an
// occlusion ray), one local light picked at random (occlusion ray, weighted
// by the number of local lights).
float3 ScDirectIrradiance(float3 p, float3 n, inout uint seed, inout TraceStats stats)
{
    float3 e = float3(0.0f, 0.0f, 0.0f);
    uint localCount = 0u;
    const uint count = min(lightCount, 7u);
    [loop] for (uint i = 0u; i < count; ++i)
    {
        if (lights[i].params.w < 0.5f)
        {
            const float3 L = normalize(-lights[i].positionWS.xyz);
            const float NoL = dot(n, L);
            if (NoL <= 0.0f)
                continue;
            float visibility = 1.0f;
            bool inside = false;
            if ((scFlags & SC_FLAG_SHADOW_MAP) != 0u)
                visibility = KromCsmVisibilityLoad(p, n, L, i, inside);
            uint previous;
            if (!inside)
            {
                visibility = TraceAny(p, L, 0.0f, 1e30f, scTlasNodeCount, stats) ? 0.0f : 1.0f;
                InterlockedAdd(gCounters[SC_COUNTER_SUN_RAYS], 1u, previous);
            }
            else
            {
                InterlockedAdd(gCounters[SC_COUNTER_SUN_CSM], 1u, previous);
            }
            InterlockedAdd(gCounters[SC_COUNTER_SUN_LIT], (uint)(visibility * 1000.0f + 0.5f), previous);
            e += lights[i].colorIntensity.rgb * lights[i].colorIntensity.w * NoL * visibility;
        }
        else
        {
            ++localCount;
        }
    }
    if (localCount == 0u)
        return e;
    uint pick = min((uint)(ScRandom(seed) * localCount), localCount - 1u);
    [loop] for (uint j = 0u; j < count; ++j)
    {
        if (lights[j].params.w < 0.5f)
            continue;
        if (pick-- != 0u)
            continue;
        const float3 toL = lights[j].positionWS.xyz - p;
        const float dist = length(toL);
        if (dist < 1e-4f)
            break;
        const float3 L = toL / dist;
        const float NoL = dot(n, L);
        float atten = ScCalcAttenuation(dist, lights[j].params.z);
        if (lights[j].params.w > 1.5f)
        {
            const float cosAngle = dot(-L, normalize(lights[j].directionWS.xyz));
            atten *= saturate((cosAngle - lights[j].params.y) / max(lights[j].params.x - lights[j].params.y, 1e-4f));
        }
        if (NoL <= 0.0f || atten <= 0.0f)
            break;
        if (!TraceAny(p, L, 0.0f, dist - 1e-3f, scTlasNodeCount, stats))
            e += lights[j].colorIntensity.rgb * lights[j].colorIntensity.w * atten * NoL * (float)localCount;
        break;
    }
    return e;
}

[numthreads(64, 1, 1)]
void main(uint3 dtid : SV_DispatchThreadID)
{
    const uint active = gCounters[SC_COUNTER_ACTIVE];
    if (active == 0u)
        return;
    const uint total = min(scRayBudget, active * max(scMaxRaysPerCell, 1u));
    if (dtid.x >= total)
        return;
    // More cells than rays: a different share of the cells every frame.
    const uint listIndex = (dtid.x + scFrame * total) % active;
    const uint slot = gActiveList[listIndex];
    const uint4 cell = gCellB[slot];
    const float3 origin = asfloat(cell.xyz);
    const float3 n = ScOctDecode(cell.w);
    uint seed = ScPcg(dtid.x ^ ScPcg(scFrame * 0x9E3779B9u + slot));
    const float3 dir = ScCosineDirection(n, seed);
    const float dist = length(origin - cameraPositionWS.xyz);
    const float3 o = origin + n * (2e-3f + 2e-4f * dist);
    const float tMax = scRayLength > 0.0f ? scRayLength : 1e30f;

    TraceStats stats = TraceStatsZero();
    TraceHit hit;
    float3 radiance;
    if (TraceClosest(o, dir, 0.0f, tMax, scTlasNodeCount, hit, stats))
    {
        const BvhInstance inst = gInstances[hit.instance];
        float3 albedo = float3(0.5f, 0.5f, 0.5f);
        float3 emission = float3(0.0f, 0.0f, 0.0f);
        if (inst.materialId < scMaterialCount)
        {
            albedo = gMaterials[inst.materialId * 2u + 0u].rgb;
            emission = gMaterials[inst.materialId * 2u + 1u].rgb;
        }
        float3 hn = hit.normalWS;
        if (dot(hn, dir) > 0.0f)
            hn = -hn;
        const float3 hp = o + dir * hit.t;
        const float3 hpo = hp + hn * (2e-3f + 2e-4f * length(hp - cameraPositionWS.xyz));
        // Next bounce from the cache (last frame's value), else the far field.
        const uint hitSlot = ScFind(ScMakeKey(hp, hn));
        const float3 indirect = hitSlot != SC_NONE ? asfloat(gCellA[hitSlot].xyz) : ScFarFieldIrradiance(hp, hn);
        radiance = emission + albedo * (1.0f / SC_PI) * (ScDirectIrradiance(hpo, hn, seed, stats) + indirect);
    }
    else
    {
        // Left the scene or ended at scRayLength: radiance of the far field in
        // the ray direction (irradiance of a surface facing it, / pi).
        radiance = ScFarFieldIrradiance(o + dir * min(tMax, 1e4f), dir) * (1.0f / SC_PI);
    }
    radiance = min(max(radiance, 0.0f), SC_MAX_RADIANCE);
    const uint3 fixedPoint = (uint3)(radiance * SC_ACCUM_SCALE + 0.5f);
    uint previous;
    InterlockedAdd(gAccum[slot * 4u + 0u], fixedPoint.x, previous);
    InterlockedAdd(gAccum[slot * 4u + 1u], fixedPoint.y, previous);
    InterlockedAdd(gAccum[slot * 4u + 2u], fixedPoint.z, previous);
    InterlockedAdd(gAccum[slot * 4u + 3u], 1u, previous);
    InterlockedAdd(gCounters[SC_COUNTER_RAYS], 1u, previous);
}

#elif defined(SC_KERNEL_RESOLVE)

[numthreads(64, 1, 1)]
void main(uint3 dtid : SV_DispatchThreadID)
{
    if (dtid.x >= gCounters[SC_COUNTER_ACTIVE])
        return;
    const uint slot = gActiveList[dtid.x];
    const uint samples = gAccum[slot * 4u + 3u];
    if (samples == 0u)
        return;
    // Cosine-weighted rays: E = pi * mean radiance.
    const float3 mean = float3(gAccum[slot * 4u + 0u], gAccum[slot * 4u + 1u], gAccum[slot * 4u + 2u]) *
                        (SC_PI / (SC_ACCUM_SCALE * (float)samples));
    gAccum[slot * 4u + 0u] = 0u;
    gAccum[slot * 4u + 1u] = 0u;
    gAccum[slot * 4u + 2u] = 0u;
    gAccum[slot * 4u + 3u] = 0u;
    const uint4 cell = gCellA[slot];
    const float3 old = asfloat(cell.xyz);
    const uint weight = min(cell.w + samples, max(scMaxSamples, 1u));
    const float alpha = min((float)samples / (float)weight, 1.0f);
    const float3 e = lerp(old, mean, alpha);
    gCellA[slot] = uint4(asuint(e), weight);

    const float lumOld = dot(old, float3(0.2126f, 0.7152f, 0.0722f));
    const float lumNew = dot(e, float3(0.2126f, 0.7152f, 0.0722f));
    const float change = abs(lumNew - lumOld) / max(max(lumOld, lumNew), 1e-3f);
    uint previous;
    InterlockedAdd(gCounters[SC_COUNTER_CHANGE], (uint)(min(change, 1.0f) * 1000.0f + 0.5f), previous);
    InterlockedAdd(gCounters[SC_COUNTER_RESOLVED], 1u, previous);
    InterlockedAdd(gCounters[SC_COUNTER_RADIANCE], (uint)(min(lumNew / SC_PI, 10.0f) * 1000.0f + 0.5f), previous);
}

#elif defined(SC_KERNEL_AGE)

[numthreads(64, 1, 1)]
void main(uint3 dtid : SV_DispatchThreadID)
{
    const uint slot = dtid.x;
    if (slot > scCapacityMask || gCellKeys[slot] == 0u)
        return;
    uint previous;
    const uint last = gCellFrame[slot];
    if (last != 0u && scFrame - last > scMaxAge)
    {
        // Free the slot. Cells behind it in the probing chain become
        // unreachable; they are registered again (new start value) or age out.
        gCellKeys[slot] = 0u;
        gCellFrame[slot] = 0u;
        gCellA[slot] = uint4(0u, 0u, 0u, 0u);
        InterlockedAdd(gCounters[SC_COUNTER_EVICTED], 1u, previous);
        return;
    }
    InterlockedAdd(gCounters[SC_COUNTER_OCCUPIED], 1u, previous);
    if (scDirtyBoxCount == 0u || last == 0u)
        return;
    // Geometry moved, came or went here: keep little of the old value so the
    // cell settles again quickly.
    const float3 p = asfloat(gCellB[slot].xyz);
    [loop] for (uint i = 0u; i < scDirtyBoxCount; ++i)
    {
        const float3 lo = gDirtyBoxes[i * 2u + 0u].xyz;
        const float3 hi = gDirtyBoxes[i * 2u + 1u].xyz;
        if (all(p >= lo) && all(p <= hi))
        {
            const uint4 cell = gCellA[slot];
            if (cell.w > SC_DIRTY_WEIGHT)
                gCellA[slot] = uint4(cell.xyz, SC_DIRTY_WEIGHT);
            break;
        }
    }
}

#endif

#endif // SC_KERNEL_MATERIAL
