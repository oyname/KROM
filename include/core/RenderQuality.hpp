#pragma once
// =============================================================================
// KROM Engine - include/core/RenderQuality.hpp
// Render quality levels: one choice for players that sets the render
// techniques (MSAA, post-AA, GTAO, SSR, contact shadows) and the shadow-map
// resolution and filter (as Unity/Unreal do, not per light). Values such as radius
// or intensity are not part of a level, so per-area overrides (post-process
// volumes) stay independent of it. The level is never stored with the
// options: it is derived from them, and "Custom" means no level matches.
// =============================================================================
#include <cstdint>
#include <cstring>

namespace engine {

enum class RenderQualityLevel : uint8_t
{
    Low    = 0,
    Medium = 1,
    High   = 2,
    Custom = 3,   // the options match no level
};

// Same ids as renderer::addons::forward::PostAAMode.
enum class RenderPostAA : uint8_t
{
    Off  = 0,
    Fxaa = 1,
    Smaa = 2,
};

// The options a quality level sets.
struct RenderQualityOptions
{
    uint32_t     msaaSamples = 1u;   // 1, 2, 4
    RenderPostAA postAA = RenderPostAA::Off;
    bool         gtaoEnabled = false;
    bool         ssrEnabled = false;
    bool         contactShadowsEnabled = false;
    // Shadow maps (shadow add-on): tile edge in texels of each sun cascade and of
    // each point/spot light view, and the filter (engine::ShadowFilter id).
    uint32_t     sunShadowResolution = 2048u;
    uint32_t     localShadowResolution = 1024u;
    uint8_t      shadowFilter = 2u;   // Tent5x5

    friend bool operator==(const RenderQualityOptions&, const RenderQualityOptions&) = default;
};

// Low:    no MSAA, FXAA, GTAO/SSR/contact shadows off; shadows 1024/512, PCF 3x3.
// Medium: no MSAA, SMAA, GTAO and contact shadows on, SSR off; shadows 2048/1024, PCF 5x5.
// High:   MSAA 2x, SMAA, everything on; shadows 2048/2048, PCF 5x5.
// Custom returns the High options (callers keep their values instead).
[[nodiscard]] constexpr RenderQualityOptions RenderQualityPreset(RenderQualityLevel level) noexcept
{
    switch (level)
    {
    case RenderQualityLevel::Low:    return {1u, RenderPostAA::Fxaa, false, false, false, 1024u, 512u, 1u};
    case RenderQualityLevel::Medium: return {1u, RenderPostAA::Smaa, true, false, true, 2048u, 1024u, 2u};
    case RenderQualityLevel::High:
    case RenderQualityLevel::Custom:
    default:                         return {2u, RenderPostAA::Smaa, true, true, true, 2048u, 2048u, 2u};
    }
}

// Shadow-map tile edges a quality level may use (powers of two).
inline constexpr uint32_t kMinShadowQualityResolution = 256u;
inline constexpr uint32_t kMaxShadowQualityResolution = 4096u;
// Highest shadow filter id for quality options (engine::ShadowFilter::Tent7x7).
inline constexpr uint8_t kMaxShadowQualityFilter = 5u;

[[nodiscard]] constexpr uint32_t ClampShadowQualityResolution(uint32_t value) noexcept
{
    uint32_t result = kMinShadowQualityResolution;
    while (result < kMaxShadowQualityResolution && result * 2u <= value)
        result *= 2u;
    return result;
}

[[nodiscard]] constexpr RenderQualityLevel DetectRenderQualityLevel(const RenderQualityOptions& options) noexcept
{
    for (uint8_t i = 0; i < static_cast<uint8_t>(RenderQualityLevel::Custom); ++i)
        if (RenderQualityPreset(static_cast<RenderQualityLevel>(i)) == options)
            return static_cast<RenderQualityLevel>(i);
    return RenderQualityLevel::Custom;
}

[[nodiscard]] constexpr const char* RenderQualityLevelName(RenderQualityLevel level) noexcept
{
    switch (level)
    {
    case RenderQualityLevel::Low:    return "low";
    case RenderQualityLevel::Medium: return "medium";
    case RenderQualityLevel::High:   return "high";
    default:                         return "custom";
    }
}

inline bool ParseRenderQualityLevel(const char* name, RenderQualityLevel& out) noexcept
{
    if (!name)
        return false;
    for (uint8_t i = 0; i <= static_cast<uint8_t>(RenderQualityLevel::Custom); ++i)
        if (std::strcmp(name, RenderQualityLevelName(static_cast<RenderQualityLevel>(i))) == 0)
        {
            out = static_cast<RenderQualityLevel>(i);
            return true;
        }
    return false;
}

} // namespace engine
