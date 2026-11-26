// =============================================================================
// KROM Engine - shaders/surface_cache_apply.hlsl
// Resolve of the radiance cache for shading (doc/PLAN_GI.md, package 4): a
// fullscreen pass (full resolution, or half with SC_FLAG_APPLY_HALF) between the depth prepass and the opaque
// pass. Per pixel it looks up the cells around the visible surface and writes
// the indirect diffuse irradiance for the PBR shaders (TexSlots::GlobalIllumination,
// read by shaders/pbr_gi.hlsl).
//
// Output (RGBA16F): rgb = irradiance E (W/m^2, like the IBL irradiance cube),
// a = linear view depth of the sample, negative where the cache had too few
// cells and the far field (probes, IBL) filled in, 0 for the sky.
//
// Per pixel: the nearest of the 2 x 2 full-resolution depths, the world
// position and a normal reconstructed from the depth (like GTAO). Trilinear
// over the 8 cells around the position at the pixel's cell level, for up to
// three normal axes weighted by n^2 (the cache key holds the dominant axis
// only, so curved surfaces would otherwise show six facets). Cells whose
// stored surface point lies off the pixel's tangent plane (other side of a
// wall, below a floor) do not count, nor cells without rays of their own yet.
// Missing cells drop out and the rest is normalised; only where no cell is
// found the far field (probes, IBL) fills in.
// =============================================================================

#define SC_WITH_ENVIRONMENT 1
#include "surface_cache_common.hlsl"

KROM_BUFFER_SRV(StructuredBuffer<uint>,  gApplyKeys,  12);
KROM_BUFFER_SRV(StructuredBuffer<uint4>, gApplyCellA, 13);
KROM_BUFFER_SRV(StructuredBuffer<uint4>, gApplyCellB, 14);
KROM_BUFFER_SRV(StructuredBuffer<uint>,  gApplyAccum, 15);   // SC_ACCUM_WORDS per slot, the display value

#ifdef SC_APPLY_MSAA
Texture2DMS<float> tApplyDepth : register(t8);
#else
Texture2D<float>   tApplyDepth : register(t8);
#endif

SC_DEFINE_FIND(ScFindApply, gApplyKeys)

static int2 gDepthSize;

// Axis weight below which a normal axis is skipped (n^2, about 13 degrees).
static const float SC_APPLY_MIN_AXIS_WEIGHT = 0.05f;
// Trilinear weight that counts as fully covered by the cache.
static const float SC_APPLY_FULL_WEIGHT = 0.25f;
// A cell counts in full from SC_CONFIDENT_SAMPLES on. A younger cell fades in
// next to its neighbours: its first frames are a mean of a few rays, and shown
// in full every new cell was a patch that came and went (cells are born all the
// time, also in a resting view: registration reaches small ones only now and then).

float ScApplyLoadDepth(int2 pixel)
{
    const int2 p = clamp(pixel, int2(0, 0), gDepthSize - 1);
#ifdef SC_APPLY_MSAA
    return tApplyDepth.Load(p, 0);
#else
    return tApplyDepth.Load(int3(p, 0));
#endif
}

float3 ScApplyWorldPosition(int2 pixel, float depth)
{
    const float2 uv = (float2(pixel) + 0.5f) / float2(gDepthSize);
    const float2 ndc = float2(uv.x * 2.0f - 1.0f, 1.0f - uv.y * 2.0f);
    const float4 h = mul(invViewProjMatrix, float4(ndc, depth, 1.0f));
    return h.xyz / h.w;
}

float3 ScApplyPositionAt(int2 pixel)
{
    const int2 p = clamp(pixel, int2(0, 0), gDepthSize - 1);
    return ScApplyWorldPosition(p, ScApplyLoadDepth(p));
}

// Normal from the depth: per axis the neighbour with the smaller depth step
// (keeps silhouettes from bending the normal), facing the camera.
float3 ScApplyReconstructNormal(int2 pixel, float3 P)
{
    const float3 toCamera = cameraPositionWS.xyz - P;
    const float3 pl = ScApplyPositionAt(pixel + int2(-1,  0));
    const float3 pr = ScApplyPositionAt(pixel + int2( 1,  0));
    const float3 pu = ScApplyPositionAt(pixel + int2( 0, -1));
    const float3 pd = ScApplyPositionAt(pixel + int2( 0,  1));
    const float3 viewDir = normalize(toCamera);
    // At the image border only the neighbour inside the image counts (the
    // clamped one is the pixel itself and would give a zero tangent).
    const bool hasL = pixel.x > 0, hasR = pixel.x + 1 < gDepthSize.x;
    const bool hasU = pixel.y > 0, hasD = pixel.y + 1 < gDepthSize.y;
    const float3 dx = (!hasL || (hasR && abs(dot(pr - P, viewDir)) < abs(dot(P - pl, viewDir)))) ? (pr - P) : (P - pl);
    const float3 dy = (!hasU || (hasD && abs(dot(pd - P, viewDir)) < abs(dot(P - pu, viewDir)))) ? (pd - P) : (P - pu);
    float3 n = cross(dx, dy);
    const float len = length(n);
    n = len > 1e-12f ? n / len : viewDir;
    return dot(n, toCamera) < 0.0f ? -n : n;
}

float4 main(float4 pos : SV_POSITION) : SV_Target
{
#ifdef SC_APPLY_MSAA
    uint w, h, samples;
    tApplyDepth.GetDimensions(w, h, samples);
#else
    uint w, h;
    tApplyDepth.GetDimensions(w, h);
#endif
    gDepthSize = int2(w, h);

    // Full resolution: the pixel itself. Half resolution: the nearest of the
    // 2 x 2 full-resolution pixels.
    const int2 q = int2(pos.xy);
    int2 pixel = q;
    float depth = 1.0f;
    if ((scFlags & SC_FLAG_APPLY_HALF) != 0u)
    {
        pixel = q * 2;
        [unroll] for (uint i = 0u; i < 4u; ++i)
        {
            const int2 candidate = q * 2 + int2(i & 1u, i >> 1u);
            if (any(candidate >= gDepthSize))
                continue;
            const float d = ScApplyLoadDepth(candidate);
            if (d < depth)
            {
                depth = d;
                pixel = candidate;
            }
        }
    }
    else
    {
        depth = ScApplyLoadDepth(pixel);
    }
    if (depth >= 1.0f)
        return float4(0.0f, 0.0f, 0.0f, 0.0f);   // sky

    const float3 P = ScApplyWorldPosition(pixel, depth);
    const float3 n = ScApplyReconstructNormal(pixel, P);
    const float viewDepth = max(dot(P - cameraPositionWS.xyz, normalize(cameraForwardWS.xyz)), 1e-4f);

    const uint level = ScLevel(P);
    const float size = ScCellSizeOfLevel(level);
    const float3 g = P / size - 0.5f;
    const float3 baseF = floor(g);
    const int3 base = (int3)baseF;
    const float3 f = g - baseF;
    const float3 axisWeight = n * n;

    float3 sum = float3(0.0f, 0.0f, 0.0f);
    float weight = 0.0f;
    float found = 0.0f;   // trilinear weight of the cells found, young or not
    [unroll] for (uint axis = 0u; axis < 3u; ++axis)
    {
        const float wa = axisWeight[axis];
        if (wa < SC_APPLY_MIN_AXIS_WEIGHT)
            continue;
        const uint bucket = axis * 2u + (n[axis] >= 0.0f ? 0u : 1u);
        [unroll] for (uint c = 0u; c < 8u; ++c)
        {
            const int3 o = int3(c & 1u, (c >> 1u) & 1u, (c >> 2u) & 1u);
            const float3 t = lerp(1.0f - f, f, float3(o));
            float wc = wa * t.x * t.y * t.z;
            if (wc < 1e-4f)
                continue;
            const uint slot = ScFindApply(ScMakeKeyAt(base + o, level, bucket));
            if (slot == SC_NONE)
                continue;
            const uint4 cell = gApplyCellA[slot];
            if (cell.w == 0u)
                continue;   // only its start value (the far field), no rays of its own yet
            const float plane = abs(dot(asfloat(gApplyCellB[slot].xyz) - P, n));
            wc *= saturate(1.0f - plane / size);
            found += wc;
            // Young cells count little while older ones are around; alone they
            // still fill the pixel (the sum is normalised).
            wc *= max(saturate((float)cell.w / SC_CONFIDENT_SAMPLES), 0.02f);
            // The display value: the cell averaged with its neighbours on the surface.
            const uint d = slot * SC_ACCUM_WORDS + SC_ACCUM_DISPLAY;
            sum += asfloat(uint3(gApplyAccum[d], gApplyAccum[d + 1u], gApplyAccum[d + 2u])) * wc;
            weight += wc;
        }
    }

    // Missing cells drop out and the rest is normalised; the far field only
    // where no cell was found at all. Blending it in by the missing share put
    // sky light into the corners and edges of enclosed rooms.
    const float coverage = saturate(found / SC_APPLY_FULL_WEIGHT);
    const float3 irradiance = weight > 1e-7f ? sum / weight : ScFarFieldIrradiance(P, n);
    return float4(max(irradiance, 0.0f), coverage >= 0.5f ? viewDepth : -viewDepth);
}
