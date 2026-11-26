// =============================================================================
// KROM Engine - addons/forward/ForwardAutoExposure.cpp
// Shader sources of the automatic exposure (see ForwardAutoExposure.hpp). The
// adaptation mirrors AutoExposureTargetEv / AdaptExposureEv in ForwardTonemap.hpp.
// =============================================================================
#include "addons/forward/ForwardAutoExposure.hpp"

namespace engine::renderer::addons::forward::auto_exposure {

// Plain string literals: GCC before 13 cannot parse a multi-line raw string inside #define.
#define KROM_AE_PER_PASS \
    "\n" \
    "cbuffer PerPass : register(b3)\n" \
    "{\n" \
    "    float4 meter;    // x = min log2 luminance, y = log2 range, z = width, w = height\n" \
    "    float4 adapt;    // x = dt (s), y = EV/s brighter, z = EV/s darker, w = reset\n" \
    "    float4 limits;   // x = min EV, y = max EV, z = freeze, w = low percentile\n" \
    "    float4 extra;    // x = high percentile\n" \
    "};\n" \
    "static const uint kBins = 64u;\n"

const char* const kHistogramCsHlsl = KROM_AE_PER_PASS R"(
Texture2D<float4>        uHDRInput    : register(t8);
RWStructuredBuffer<uint> outHistogram : register(u0);

groupshared uint gsBins[kBins];

[numthreads(16, 16, 1)]
void main(uint3 dtid : SV_DispatchThreadID, uint gi : SV_GroupIndex)
{
    if (gi < kBins)
        gsBins[gi] = 0u;
    GroupMemoryBarrierWithGroupSync();

    if (dtid.x < (uint)meter.z && dtid.y < (uint)meter.w)
    {
        const float3 c = uHDRInput.Load(int3(dtid.xy, 0)).rgb;
        float lum = dot(max(c, 0.0f), float3(0.2126f, 0.7152f, 0.0722f));
        if (!(lum < 1e30f))   // NaN or Inf
            lum = 0.0f;
        const float t = saturate((log2(max(lum, 1e-10f)) - meter.x) / meter.y);
        const uint bin = min((uint)(t * (float)(kBins - 1u) + 0.5f), kBins - 1u);
        InterlockedAdd(gsBins[bin], 1u);
    }
    GroupMemoryBarrierWithGroupSync();

    if (gi < kBins && gsBins[gi] != 0u)
        InterlockedAdd(outHistogram[gi], gsBins[gi]);
}
)";

const char* const kAdaptCsHlsl = KROM_AE_PER_PASS R"(
RWStructuredBuffer<uint>   ioHistogram : register(u0);
RWStructuredBuffer<float4> ioState     : register(u1);

groupshared uint gsCount[kBins];

[numthreads(64, 1, 1)]
void main(uint gi : SV_GroupIndex)
{
    gsCount[gi] = ioHistogram[gi];
    ioHistogram[gi] = 0u;   // ready for the next frame
    GroupMemoryBarrierWithGroupSync();
    if (gi != 0u)
        return;

    const float4 state = ioState[0];
    const bool haveState = state.y > 0.5f && adapt.w < 0.5f;
    float total = 0.0f;
    for (uint i = 0u; i < kBins; ++i)
        total += (float)gsCount[i];
    if (total < 1.0f)
    {
        ioState[0] = haveState ? state : float4(0.0f, 0.0f, 0.0f, 0.0f);
        return;
    }

    // Average log2 luminance of the pixels between the two percentiles.
    const float lo = total * limits.w;
    const float hi = total * extra.x;
    float below = 0.0f;
    float weightSum = 0.0f;
    float logSum = 0.0f;
    for (uint b = 0u; b < kBins; ++b)
    {
        const float count = (float)gsCount[b];
        const float w = max(min(below + count, hi) - max(below, lo), 0.0f);
        const float binLog = meter.x + meter.y * ((float)b / (float)(kBins - 1u));
        logSum += w * binLog;
        weightSum += w;
        below += count;
    }
    const float middleGrey = log2(0.18f);
    const float averageLog = weightSum > 0.0f ? logSum / weightSum : middleGrey;
    const float target = clamp(middleGrey - averageLog, limits.x, max(limits.x, limits.y));

    float current = haveState ? state.x : target;
    if (haveState && limits.z < 0.5f)
    {
        const float delta = target - current;
        const float speed = delta < 0.0f ? adapt.y : adapt.z;
        const float step = max(speed, 0.0f) * max(adapt.x, 0.0f);
        current += clamp(delta, -step, step);
    }
    current = clamp(current, limits.x, max(limits.x, limits.y));
    ioState[0] = float4(current, 1.0f, target, averageLog);
}
)";

#undef KROM_AE_PER_PASS

} // namespace engine::renderer::addons::forward::auto_exposure
