#pragma once
// =============================================================================
// KROM Engine - addons/forward/ForwardAutoExposure.hpp
// Automatic exposure of the forward add-on: two compute shaders that run right
// before the tonemap draw. The first builds a log2-luminance histogram of the
// HDR image, the second averages it without outliers, moves the exposure
// towards the target and resets the histogram. The exposure stays on the GPU in
// a one-element buffer that the tonemap pixel shader reads.
// =============================================================================
#include <cstdint>

namespace engine::renderer::addons::forward::auto_exposure {

inline constexpr uint32_t kHistogramBins = 64u;
inline constexpr uint32_t kHistogramGroupSize = 16u;   // 16x16 threads, one pixel each
// Luminance range of the histogram (log2). Covers the exposure limits -10..+10 EV
// around middle grey; brighter or darker pixels land in the outer bins.
inline constexpr float kMinLog2Luminance = -14.0f;
inline constexpr float kMaxLog2Luminance = 8.0f;
// Part of the pixels (sorted by luminance) the average uses: 10 % to 90 %.
inline constexpr float kLowPercentile = 0.10f;
inline constexpr float kHighPercentile = 0.90f;

// PerPass block (b3) of both shaders.
struct alignas(16) PassConstants
{
    float minLog2Luminance = kMinLog2Luminance;
    float log2LuminanceRange = kMaxLog2Luminance - kMinLog2Luminance;
    float width = 0.0f;
    float height = 0.0f;

    float deltaTime = 0.0f;       // seconds since the last measurement
    float toBrightSpeed = 3.0f;   // EV/s
    float toDarkSpeed = 1.0f;     // EV/s
    float reset = 0.0f;           // 1: jump to the target (scene load, first frame)

    float minEv = -4.0f;
    float maxEv = 4.0f;
    float freeze = 0.0f;          // 1: keep the current exposure
    float lowPercentile = kLowPercentile;

    float highPercentile = kHighPercentile;
    float pad0 = 0.0f;
    float pad1 = 0.0f;
    float pad2 = 0.0f;
};
static_assert(sizeof(PassConstants) == 64u, "PassConstants must match PerPass in the auto-exposure shaders");

// Element of the state buffer: x = current exposure EV (without correction),
// y = 1 once measured, z = target EV of the last measurement, w = average log2 luminance.
struct alignas(16) StateGpu
{
    float exposureEv = 0.0f;
    float valid = 0.0f;
    float targetEv = 0.0f;
    float averageLog2Luminance = 0.0f;
};
static_assert(sizeof(StateGpu) == 16u, "StateGpu must match float4 in the auto-exposure shaders");

// Compute shaders (HLSL, entry "main").
//   histogram: t8 = HDR colour (Load), u0 = histogram (uint x kHistogramBins)
//   adapt:     u0 = histogram (read and reset), u1 = state (float4)
extern const char* const kHistogramCsHlsl;
extern const char* const kAdaptCsHlsl;

} // namespace engine::renderer::addons::forward::auto_exposure
