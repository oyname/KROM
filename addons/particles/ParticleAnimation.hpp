#pragma once

#include "addons/particles/ParticleTypes.hpp"
#include <algorithm>

namespace engine::particles {

struct ParticleFrameRange
{
    uint32_t first = 0u, last = 0u;
    [[nodiscard]] uint64_t Count() const noexcept
    {
        return uint64_t(std::max(first, last)) - std::min(first, last) + 1u;
    }
};

[[nodiscard]] inline ParticleFrameRange ResolveParticleFrameRange(uint32_t first, uint32_t last, uint32_t frameCount) noexcept
{
    const uint32_t max = std::max(frameCount, 1u) - 1u;
    return {std::min(first, max), std::min(last, max)};
}

// Shared by the simulation, script-driven progress, and the Inspector preview.
[[nodiscard]] inline uint32_t ResolveParticleFrame(ParticleFrameAnimation mode, uint32_t first, uint32_t last,
    double fps, bool loop, double time, double length, uint32_t frameCount, uint32_t randomOffset = 0u) noexcept
{
    const auto range = ResolveParticleFrameRange(first, last, frameCount);
    if (std::isnan(time)) return range.first;
    const uint64_t count = range.Count();
    double step = 0.;
    switch (mode)
    {
    case ParticleFrameAnimation::OverLifetime:
    {
        const double progress = length > 0. ? std::clamp(time / length, 0., 1.) : 1.;
        step = std::min(std::floor(progress * count), double(count - 1u));
        break;
    }
    case ParticleFrameAnimation::Rate:
        if (std::isfinite(fps) && fps > 0. && std::isfinite(time))
        {
            const double elapsed = std::floor(std::max(time, 0.) * fps);
            if (std::isfinite(elapsed))
                step = loop ? std::fmod(elapsed, double(count)) : std::min(elapsed, double(count - 1u));
        }
        break;
    case ParticleFrameAnimation::Random:
        step = double(randomOffset % count);
        break;
    case ParticleFrameAnimation::Fixed: break;
    }
    const auto offset = static_cast<uint32_t>(step);
    return range.first <= range.last ? range.first + offset : range.first - offset;
}

} // namespace engine::particles
