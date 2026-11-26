// =============================================================================
// KROM Engine - shaders/surface_cache_update.hlsl
// Kernels of the radiance cache (doc/PLAN_GI.md, package 3). The includer
// defines exactly one SC_KERNEL_* before including this file:
//
//   SC_KERNEL_BEGIN     1 thread: keeps last frame's counters, clears them
//   SC_KERNEL_REGISTER  camera rays on a jittered grid (one per scRegisterStep^2
//                       pixels) insert their cells and append them to the
//                       active list; new cells start with the probe/IBL value
//   SC_KERNEL_PENDING   appends the cells last frame's cache rays hit outside
//                       the view (pending list) to the active list (package 4)
//   SC_KERNEL_TRACE     scRayBudget cosine rays from the active cells; shading
//                       at the hit: emission + albedo / pi * (sun + lights +
//                       cache of the hit cell); miss: far field (the sky only if
//                       nothing lies beyond scRayLength either). A hit without a
//                       cell inserts it; a hit cell not active this frame goes
//                       to the pending list, so the cache also knows surfaces
//                       outside the view (walls behind the camera)
//   SC_KERNEL_RESOLVE   per active cell: mean of this frame's rays blended into
//                       the stored irradiance (weight capped at scMaxSamples)
//   SC_KERNEL_AGE       per slot: frees cells not registered for scMaxAge
//                       frames, lowers the weight in changed regions
//   SC_KERNEL_MATERIAL  one group: mean albedo/emission of one material from
//                       the smallest mips of its textures
//
// UAVs of the cache kernels: u0 keys, u1 cellA, u2 cellB, u3 cellFrame,
// u4 accum (12 uint per slot: rgb sums, sample count, change detector, display value, anchor distance), u5 active list,
// u6 counters (SC_COUNTER_*; 16..31 hold the previous frame's values),
// u7 pending list (cells hit by cache rays, registered next frame).
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
#define SC_COUNTER_PENDING   12u  // cells put on the pending list by cache rays
#define SC_COUNTER_FROM_HITS 13u  // cells activated from the pending list
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
RWStructuredBuffer<uint>  gPending    : register(u7);

KROM_BUFFER_SRV(StructuredBuffer<float4>, gMaterials,  10);   // 2 per material
KROM_BUFFER_SRV(StructuredBuffer<float4>, gDirtyBoxes, 11);   // 2 per box: min, max

SC_DEFINE_FIND(ScFind, gCellKeys)

// Slot of the cell, inserted if missing; SC_NONE if the probing range is full.
// inserted: true if this call created the key.
uint ScFindOrInsert(ScKey k, out bool inserted)
{
    inserted = false;
    [loop] for (uint i = 0u; i < SC_MAX_PROBE; ++i)
    {
        const uint slot = (k.start + i) & scCapacityMask;
        uint previous;
        InterlockedCompareExchange(gCellKeys[slot], 0u, k.key, previous);
        if (previous == 0u)
        {
            inserted = true;
            return slot;
        }
        if (previous == k.key)
            return slot;
    }
    return SC_NONE;
}

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

    bool inserted;
    const uint slot = ScFindOrInsert(ScMakeKey(p, n), inserted);
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
    // The anchor (the point the cell's rays start from and the resolve and
    // the neighbour average weight the cell by) stays where it is: only a
    // hit clearly closer to the centre of the cell replaces it, so it comes
    // to rest after a few frames. Taking this frame's jittered hit every
    // time moved it every frame; where a cell holds more than one surface
    // or a curved one (a ramp over the floor, a small box, a detailed mesh)
    // those weights then jumped from frame to frame and the image flickered.
    const uint anchor = slot * SC_ACCUM_WORDS + SC_ACCUM_ANCHOR;
    const float anchorDistance = ScAnchorDistance(p);
    if (lastFrame == 0u || anchorDistance < 0.8f * asfloat(gAccum[anchor]))
    {
        gCellB[slot] = uint4(asuint(p), ScOctEncode(n));
        gAccum[anchor] = asuint(anchorDistance);
    }
    if (lastFrame == 0u)
    {
        gCellA[slot] = uint4(asuint(ScFarFieldIrradiance(p, n)), SC_START_WEIGHT);
        InterlockedAdd(gCounters[SC_COUNTER_NEW], 1u, previous);
    }
}

#elif defined(SC_KERNEL_PENDING)

// Cells last frame's cache rays hit outside the view (count in the previous
// counters, the begin kernel moved it there): active from this frame on,
// unless a camera ray registered them already.
[numthreads(64, 1, 1)]
void main(uint3 dtid : SV_DispatchThreadID)
{
    if (dtid.x >= gCounters[SC_COUNTER_PREVIOUS + SC_COUNTER_PENDING] || dtid.x > scCapacityMask)
        return;
    const uint slot = gPending[dtid.x];
    if (gCellKeys[slot] == 0u)
        return;   // freed meanwhile
    uint lastFrame;
    InterlockedExchange(gCellFrame[slot], scFrame, lastFrame);
    if (lastFrame == scFrame)
        return;
    uint index, previous;
    InterlockedAdd(gCounters[SC_COUNTER_ACTIVE], 1u, index);
    gActiveList[index] = slot;
    InterlockedAdd(gCounters[SC_COUNTER_FROM_HITS], 1u, previous);
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
    // Active list: the cells camera rays registered (first), then the cells
    // activated from the pending list (outside the view, seen only through
    // bounces): those get a quarter of the rays per cell and the budget that
    // is left.
    const uint active = gCounters[SC_COUNTER_ACTIVE];
    if (active == 0u)
        return;
    const uint fromHits = min(gCounters[SC_COUNTER_FROM_HITS], active);
    const uint onScreen = active - fromHits;
    const uint raysPerCell = clamp(scMaxRaysPerCell, 1u, 255u);   // fixed-point headroom (SC_ACCUM_SCALE)
    const uint onScreenRays = min(scRayBudget, onScreen * raysPerCell);
    const uint hitRays = min(scRayBudget - onScreenRays, fromHits * max(raysPerCell / 4u, 1u));
    if (dtid.x >= onScreenRays + hitRays)
        return;
    // More cells than rays: a different share of the cells every frame.
    const uint listIndex = dtid.x < onScreenRays
        ? (dtid.x + scFrame * onScreenRays) % onScreen
        : onScreen + (dtid.x - onScreenRays + scFrame * hitRays) % fromHits;
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
        // Next bounce from the cache (last frame's value). Rays of cells on
        // screen: a surface without a cell gets one (start value: the far
        // field) and, like every hit cell not active this frame, goes to the
        // pending list: next frame it gets rays of its own. Rays of cells that
        // came from the pending list only look up (one step out of the view;
        // otherwise chains of such cells spread through the scene and keep
        // themselves alive inside closed meshes).
        const bool fromScreen = listIndex < onScreen;
        bool inserted = false;
        const ScKey hitKey = ScMakeKey(hp, hn);
        const uint hitSlot = fromScreen ? ScFindOrInsert(hitKey, inserted) : ScFind(hitKey);
        float3 indirect;
        if (hitSlot == SC_NONE)
        {
            // A surface the cache does not know: only its emission and direct
            // light count, no further bounce. Never the far field: the ray hit
            // geometry, and in an enclosed room the far field (sky, probes) is
            // light from outside (biased dark rather than leaking light in).
            // The far field stays only for a full probing range.
            indirect = float3(0.0f, 0.0f, 0.0f);
            uint full;
            if (fromScreen)
            {
                indirect = ScFarFieldIrradiance(hp, hn);
                InterlockedAdd(gCounters[SC_COUNTER_FULL], 1u, full);
            }
        }
        else
        {
            if (inserted)
            {
                // Start value for the display until the cell's own rays arrive
                // (weight 0); this ray takes no bounce from it, like a surface
                // without a cell, so no sky enters an enclosed room this way.
                indirect = float3(0.0f, 0.0f, 0.0f);
                gCellA[hitSlot] = uint4(asuint(ScFarFieldIrradiance(hp, hn)), SC_START_WEIGHT);
                gCellB[hitSlot] = uint4(asuint(hp), ScOctEncode(hn));
                // A point the camera may not see: the first camera ray replaces it.
                gAccum[hitSlot * SC_ACCUM_WORDS + SC_ACCUM_ANCHOR] = asuint(SC_ANCHOR_FREE);
                uint created;
                InterlockedAdd(gCounters[SC_COUNTER_NEW], 1u, created);
            }
            else
            {
                // A cell without rays of its own yet (weight 0) holds only its
                // start value, the far field: no bounce from it either.
                const uint4 hitCell = gCellA[hitSlot];
                indirect = hitCell.w > 0u ? asfloat(hitCell.xyz) : float3(0.0f, 0.0f, 0.0f);
            }
            // Stamped with this frame: stays alive while rays from the screen
            // hit it, and is queued once per frame.
            if (fromScreen)
            {
                uint lastFrame;
                InterlockedExchange(gCellFrame[hitSlot], scFrame, lastFrame);
                if (lastFrame != scFrame)
                {
                    uint index;
                    InterlockedAdd(gCounters[SC_COUNTER_PENDING], 1u, index);
                    if (index <= scCapacityMask)
                        gPending[index] = hitSlot;
                }
            }
        }
        radiance = emission + albedo * (1.0f / SC_PI) * (ScDirectIrradiance(hpo, hn, seed, stats) + indirect);
    }
    else
    {
        // No hit up to tMax: radiance of the far field in the ray direction
        // (irradiance of a surface facing it, / pi). Only a ray that is free
        // all the way has left the scene and sees the sky. A ray that ended at
        // scRayLength asks an occlusion ray for the rest of its way; with
        // geometry ahead only the local probes count (an enclosed room larger
        // than the ray length filled with sky light otherwise).
        bool seesSky = true;
        if (scRayLength > 0.0f)
            seesSky = !TraceAny(o, dir, tMax, 1e30f, scTlasNodeCount, stats);
        radiance = ScFarFieldIrradiance(o + dir * min(tMax, 1e4f), dir, seesSky) * (1.0f / SC_PI);
    }
    radiance = min(max(radiance, 0.0f), SC_MAX_RADIANCE);
    const uint3 fixedPoint = (uint3)(radiance * SC_ACCUM_SCALE + ScRandom(seed));   // dithered: unbiased
    uint previous;
    InterlockedAdd(gAccum[slot * SC_ACCUM_WORDS + 0u], fixedPoint.x, previous);
    InterlockedAdd(gAccum[slot * SC_ACCUM_WORDS + 1u], fixedPoint.y, previous);
    InterlockedAdd(gAccum[slot * SC_ACCUM_WORDS + 2u], fixedPoint.z, previous);
    InterlockedAdd(gAccum[slot * SC_ACCUM_WORDS + 3u], 1u, previous);
    InterlockedAdd(gCounters[SC_COUNTER_RAYS], 1u, previous);
}

#elif defined(SC_KERNEL_RESOLVE)

[numthreads(64, 1, 1)]
void main(uint3 dtid : SV_DispatchThreadID)
{
    if (dtid.x >= gCounters[SC_COUNTER_ACTIVE])
        return;
    const uint slot = gActiveList[dtid.x];
    const uint a = slot * SC_ACCUM_WORDS;
    const uint samples = gAccum[a + 3u];
    if (samples == 0u)
        return;
    // Cosine-weighted rays: E = pi * mean radiance.
    const float3 mean = float3(gAccum[a + 0u], gAccum[a + 1u], gAccum[a + 2u]) *
                        (SC_PI / (SC_ACCUM_SCALE * (float)samples));
    gAccum[a + 0u] = 0u;
    gAccum[a + 1u] = 0u;
    gAccum[a + 2u] = 0u;
    gAccum[a + 3u] = 0u;
    const uint4 cell = gCellA[slot];
    const float3 old = asfloat(cell.xyz);
    const float3 luma = float3(0.2126f, 0.7152f, 0.0722f);
    const float lumOld = dot(old, luma);
    const float lumMean = dot(mean, luma);

    // History: a running mean up to SC_LONG_HISTORY_FRAMES frames of this
    // cell's rays (scMaxSamples if that is more). What is left of the flicker
    // of a resting image is plain sampling noise, so it is set by the number
    // of rays in the history.
    const uint longCap = max(max(scMaxSamples, 1u), samples * SC_LONG_HISTORY_FRAMES);
    const uint shortCap = samples * SC_SHORT_HISTORY_FRAMES;
    uint weight = min(cell.w + samples, longCap);

    // Change detector: a short mean of the frame values next to the long one
    // and the noise power of the frame values around the long mean. Where the
    // short mean leaves the long one by more than its own noise allows (2.5 to
    // 5 standard deviations, at least 3 %), the light of the cell has changed
    // (a light moved or switched, a door opened further away): the history
    // falls back to the short one and the cell follows within a few frames.
    float3 fast = mean;
    float power = 0.0f;
    if (cell.w > 0u)
    {
        const float running = min((float)samples / (float)weight, 1.0f);
        const float alphaFast = max(1.0f / (float)SC_SHORT_HISTORY_FRAMES, running);
        const float alphaPower = max(1.0f / (2.0f * (float)SC_SHORT_HISTORY_FRAMES), running);
        // The frame value enters the short mean limited to three standard
        // deviations of the frame values around the long mean: one outlier
        // frame (a few rays that hit a very bright spot) then cannot trip the
        // detector, only a deviation that lasts. Without the limit resting
        // cells kept falling back to the short history and showed the outlier
        // as a patch that came and went.
        const float oldPower = asfloat(gAccum[a + 7u]);
        const float limit = 3.0f * sqrt(oldPower) + 0.03f * lumOld + 1e-5f;
        const float offset = abs(lumMean - lumOld);
        const float3 limited = offset > limit ? old + (mean - old) * (limit / offset) : mean;
        fast = lerp(asfloat(uint3(gAccum[a + 4u], gAccum[a + 5u], gAccum[a + 6u])), limited, alphaFast);
        power = lerp(oldPower, (lumMean - lumOld) * (lumMean - lumOld), alphaPower);
        if (cell.w >= 2u * shortCap)
        {
            // Standard deviation of the short mean of independent frame values.
            const float sigma = max(sqrt(power * alphaFast / (2.0f - alphaFast)), 0.03f * lumOld + 1e-5f);
            const float deviation = abs(dot(fast, luma) - lumOld);
            const float changed = saturate((deviation - 2.5f * sigma) / (2.5f * sigma));
            weight = (uint)lerp((float)weight, (float)min(weight, shortCap), changed);
        }
    }
    gAccum[a + 4u] = asuint(fast.x);
    gAccum[a + 5u] = asuint(fast.y);
    gAccum[a + 6u] = asuint(fast.z);
    gAccum[a + 7u] = asuint(power);

    const float alpha = min((float)samples / (float)max(weight, 1u), 1.0f);
    const float3 e = lerp(old, mean, alpha);
    gCellA[slot] = uint4(asuint(e), weight);

    // Display value (read by the resolve for shading, not by the cache rays):
    // this cell averaged with its four neighbours in the plane of the surface,
    // each at half weight. The cells' noise is independent, so this about
    // halves the patches of a resting image, at the price of indirect light
    // that is half a cell softer. A neighbour counts only on the same surface
    // (near this cell's tangent plane, facing alike) and with a free line to
    // it: no light through a wall that stands between two floor cells.
    {
        const uint4 cellB = gCellB[slot];
        const float3 p = asfloat(cellB.xyz);
        const float3 n = ScOctDecode(cellB.w);
        const uint level = ScLevel(p);
        const float size = ScCellSizeOfLevel(level);
        const uint bucket = ScNormalBucket(n);
        const int3 q = (int3)floor(p / size);
        const uint axis = bucket / 2u;
        const float lift = 2e-3f + 2e-4f * length(p - cameraPositionWS.xyz);
        const float3 po = p + n * lift;
        const float self = min((float)weight, SC_CONFIDENT_SAMPLES);
        float3 sum = e * self;
        float total = self;
        TraceStats stats = TraceStatsZero();
        [loop] for (uint i = 0u; i < 4u; ++i)
        {
            const uint inPlane = (axis + 1u + (i >> 1u)) % 3u;
            const int3 step = int3(inPlane == 0u ? 1 : 0, inPlane == 1u ? 1 : 0, inPlane == 2u ? 1 : 0) * ((i & 1u) != 0u ? 1 : -1);
            const uint other = ScFind(ScMakeKeyAt(q + step, level, bucket));
            if (other == SC_NONE)
                continue;
            const uint4 otherA = gCellA[other];
            if (otherA.w == 0u)
                continue;
            const uint4 otherB = gCellB[other];
            const float3 op = asfloat(otherB.xyz);
            if (abs(dot(op - p, n)) > 0.25f * size || dot(ScOctDecode(otherB.w), n) < 0.9f)
                continue;
            const float3 toOther = op + n * lift - po;
            const float dist = length(toOther);
            if (dist > 1e-4f && TraceAny(po, toOther / dist, 0.0f, dist, scTlasNodeCount, stats))
                continue;
            const float w = 0.5f * min((float)otherA.w, SC_CONFIDENT_SAMPLES);
            sum += asfloat(otherA.xyz) * w;
            total += w;
        }
        const float3 display = sum / max(total, 1e-6f);
        gAccum[a + SC_ACCUM_DISPLAY + 0u] = asuint(display.x);
        gAccum[a + SC_ACCUM_DISPLAY + 1u] = asuint(display.y);
        gAccum[a + SC_ACCUM_DISPLAY + 2u] = asuint(display.z);
    }

    const float lumNew = dot(e, luma);
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
            // The anchor may lie on a surface that is gone: the next camera ray sets it anew.
            gAccum[slot * SC_ACCUM_WORDS + SC_ACCUM_ANCHOR] = asuint(SC_ANCHOR_FREE);
            const uint4 cell = gCellA[slot];
            // The short history, not none: while something keeps moving the
            // cells around it would otherwise show the raw noise of one frame.
            const uint dirtyWeight = max(scMaxRaysPerCell, 1u) * SC_SHORT_HISTORY_FRAMES / 2u;
            if (cell.w > dirtyWeight)
                gCellA[slot] = uint4(cell.xyz, dirtyWeight);
            break;
        }
    }
}

#endif

#endif // SC_KERNEL_MATERIAL
