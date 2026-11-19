#pragma once
// =============================================================================
// KROM Engine - addons/forward/ForwardAntiAliasing.hpp
// MSAA and post-process AA settings of the forward pipeline. Owned by the forward add-on; the engine
// core only knows render targets and pipelines with a sample count.
// =============================================================================
#include <cstdint>
#include <cstring>

namespace engine::renderer::addons::forward {

struct MsaaSettings
{
    // 1 = off, 2 or 4 samples. Other values are rounded down to the next of these.
    uint32_t sampleCount = 1u;
};

[[nodiscard]] inline constexpr uint32_t NormalizeMsaaSamples(uint32_t samples) noexcept
{
    return samples >= 4u ? 4u : (samples >= 2u ? 2u : 1u);
}

// Post-process anti-aliasing after the tonemap pass, independent of MSAA. Ids are
// stored in project files by name (PostAAModeName); keep both in sync.
enum class PostAAMode : uint8_t
{
    Off  = 0,
    Fxaa = 1,
    // SMAA 1x: edge detection, blending weights, neighborhood blending.
    Smaa = 2,
};

struct PostAASettings
{
    PostAAMode mode = PostAAMode::Off;
};

[[nodiscard]] inline const char* PostAAModeName(PostAAMode mode) noexcept
{
    switch (mode)
    {
    case PostAAMode::Fxaa: return "fxaa";
    case PostAAMode::Smaa: return "smaa";
    case PostAAMode::Off:
    default:               return "off";
    }
}

[[nodiscard]] inline bool ParsePostAAMode(const char* name, PostAAMode& out) noexcept
{
    if (!name)
        return false;
    for (PostAAMode mode : { PostAAMode::Off, PostAAMode::Fxaa, PostAAMode::Smaa })
    {
        if (std::strcmp(name, PostAAModeName(mode)) == 0)
        {
            out = mode;
            return true;
        }
    }
    return false;
}

} // namespace engine::renderer::addons::forward
