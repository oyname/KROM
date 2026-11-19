#pragma once
// =============================================================================
// KROM Engine - addons/forward/ForwardTonemap.hpp
// Tonemap settings of the forward pipeline (operator and exposure). Owned by the
// forward add-on; the engine core knows nothing about them.
// =============================================================================
#include <algorithm>
#include <atomic>
#include "addons/forward/ForwardColorGrading.hpp"
#include <cmath>
#include <cstdint>
#include <cstring>

namespace engine::renderer::addons::forward {

// Ids are written into the tonemap shader (shaders/passthrough.ps.hlsl) and into
// scene files by name; keep both in sync when adding one.
enum class TonemapOperator : uint8_t
{
    Reinhard          = 0,
    AcesFitted        = 1,
    AgX               = 2,
    KhronosPbrNeutral = 3,
};

inline constexpr TonemapOperator kDefaultTonemapOperator = TonemapOperator::AgX;
// Scenes written before tonemap settings existed keep their former look.
inline constexpr TonemapOperator kLegacyTonemapOperator = TonemapOperator::Reinhard;

// Fixed: exposureEv is the exposure. Auto: the exposure is measured on the GPU
// (luminance histogram of the HDR image) and exposureEv is added as correction.
// Scenes without the key keep Fixed, so their look does not change.
enum class ExposureMode : uint8_t
{
    Fixed = 0,
    Auto  = 1,
};

inline constexpr float kMinExposureEv = -10.0f;
inline constexpr float kMaxExposureEv = 10.0f;
inline constexpr float kMinAutoExposureSpeed = 0.1f;    // EV per second
inline constexpr float kMaxAutoExposureSpeed = 20.0f;

// Limits and adaptation of the automatic exposure (EV stops, same scale as exposureEv).
struct AutoExposureSettings
{
    float minEv = -4.0f;           // lowest exposure the measurement may choose
    float maxEv = 4.0f;            // highest exposure the measurement may choose
    float toBrightSpeed = 3.0f;    // EV/s when the view gets brighter (exposure goes down)
    float toDarkSpeed = 1.0f;      // EV/s when the view gets darker (exposure goes up)

    // Keeps the values in range and minEv <= maxEv.
    void Clamp() noexcept
    {
        minEv = std::clamp(minEv, kMinExposureEv, kMaxExposureEv);
        maxEv = std::clamp(maxEv, kMinExposureEv, kMaxExposureEv);
        if (maxEv < minEv)
            maxEv = minEv;
        toBrightSpeed = std::clamp(toBrightSpeed, kMinAutoExposureSpeed, kMaxAutoExposureSpeed);
        toDarkSpeed = std::clamp(toDarkSpeed, kMinAutoExposureSpeed, kMaxAutoExposureSpeed);
    }
};

struct TonemapSettings
{
    TonemapOperator op         = kDefaultTonemapOperator;
    float           exposureEv = 0.0f;   // Fixed: exposure in EV stops (HDR colour *= 2^EV); Auto: correction
    ExposureMode    exposureMode = ExposureMode::Fixed;
    AutoExposureSettings autoExposure{};
    // Colour grading of the scene (ForwardColorGrading.hpp); default = neutral.
    ColorGradingSettings grading{};
    // Editor only, never saved: keeps the current automatic exposure.
    bool            freezeAutoExposure = false;

    [[nodiscard]] float ExposureScale() const noexcept { return std::exp2(exposureEv); }
};

[[nodiscard]] inline const char* ExposureModeName(ExposureMode mode) noexcept
{
    return mode == ExposureMode::Auto ? "auto" : "fixed";
}

// Unknown or missing names give Fixed.
[[nodiscard]] inline ExposureMode ParseExposureMode(const char* name) noexcept
{
    return (name && std::strcmp(name, "auto") == 0) ? ExposureMode::Auto : ExposureMode::Fixed;
}

// Automatic exposure as the renderer last measured it (read back from the GPU a few
// frames late). Shared like the settings; the owner reads it for display and asks for
// a reset when a scene is loaded, so the new scene starts at its own value.
struct AutoExposureStatus
{
    std::atomic<float>    currentEv{ 0.0f };   // applied exposure incl. correction
    std::atomic<bool>     valid{ false };      // false: off or nothing measured yet
    std::atomic<uint32_t> resetSerial{ 0u };

    void RequestReset() noexcept { resetSerial.fetch_add(1u, std::memory_order_relaxed); }
};

// Target exposure of the metering: the log2 average luminance maps to middle grey.
inline constexpr float kAutoExposureMiddleGrey = 0.18f;

[[nodiscard]] inline float AutoExposureTargetEv(float averageLog2Luminance,
                                                const AutoExposureSettings& settings) noexcept
{
    const float ev = std::log2(kAutoExposureMiddleGrey) - averageLog2Luminance;
    return std::clamp(ev, settings.minEv, std::max(settings.minEv, settings.maxEv));
}

// One adaptation step: moves current towards target with the speed of its direction.
[[nodiscard]] inline float AdaptExposureEv(float current, float target, float dt,
                                           const AutoExposureSettings& settings) noexcept
{
    const float delta = target - current;
    const float speed = delta < 0.0f ? settings.toBrightSpeed : settings.toDarkSpeed;
    const float step = std::max(speed, 0.0f) * std::max(dt, 0.0f);
    return current + std::clamp(delta, -step, step);
}

[[nodiscard]] inline const char* TonemapOperatorName(TonemapOperator op) noexcept
{
    switch (op)
    {
    case TonemapOperator::AcesFitted:        return "aces";
    case TonemapOperator::AgX:               return "agx";
    case TonemapOperator::KhronosPbrNeutral: return "pbrNeutral";
    case TonemapOperator::Reinhard:
    default:                                 return "reinhard";
    }
}

[[nodiscard]] inline bool ParseTonemapOperator(const char* name, TonemapOperator& out) noexcept
{
    if (!name)
        return false;
    for (TonemapOperator op : { TonemapOperator::Reinhard, TonemapOperator::AcesFitted,
                                TonemapOperator::AgX, TonemapOperator::KhronosPbrNeutral })
    {
        if (std::strcmp(name, TonemapOperatorName(op)) == 0)
        {
            out = op;
            return true;
        }
    }
    return false;
}

} // namespace engine::renderer::addons::forward
