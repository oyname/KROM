#pragma once
// =============================================================================
// KROM Engine - addons/forward/ForwardTonemap.hpp
// Tonemap settings of the forward pipeline (operator and exposure). Owned by the
// forward add-on; the engine core knows nothing about them.
// =============================================================================
#include <cmath>
#include <cstdint>
#include <cstring>

namespace engine::renderer::addons::forward {

// Ids are written into the tonemap shader (assets/passthrough.ps.hlsl) and into
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

struct TonemapSettings
{
    TonemapOperator op         = kDefaultTonemapOperator;
    float           exposureEv = 0.0f;   // exposure in EV stops; HDR colour *= 2^EV

    [[nodiscard]] float ExposureScale() const noexcept { return std::exp2(exposureEv); }
};

inline constexpr float kMinExposureEv = -10.0f;
inline constexpr float kMaxExposureEv = 10.0f;

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
