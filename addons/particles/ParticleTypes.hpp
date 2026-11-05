#pragma once

#include <cmath>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>

namespace engine {

struct ParticleID
{
    uint32_t system = 0u;
    uint32_t slot = 0u;
    uint32_t generation = 0u;
    [[nodiscard]] bool IsValid() const noexcept { return system != 0u && generation != 0u; }
    bool operator==(const ParticleID&) const = default;
};

inline constexpr ParticleID NULL_PARTICLE{};

inline constexpr uint32_t kLastParticleFrame = UINT32_MAX;
enum class ParticleFrameAnimation : uint8_t { OverLifetime, Rate, Fixed, Random };
enum class ParticleClipTiming : uint8_t { Rate, OverLifetime };

struct ParticleFrameSettings
{
    ParticleFrameAnimation mode = ParticleFrameAnimation::OverLifetime;
    uint32_t first = 0u, last = kLastParticleFrame;
    float fps = 12.f;
    bool loop = true, randomStart = false;
    bool operator==(const ParticleFrameSettings&) const = default;
};

struct ParticleClip
{
    std::string name;
    uint32_t first = 0u, last = kLastParticleFrame;
    ParticleClipTiming timing = ParticleClipTiming::Rate;
    float fps = 12.f;
    bool loop = false;
    bool operator==(const ParticleClip&) const = default;
};

[[nodiscard]] inline bool IsParticleClipNameValid(std::string_view name) noexcept
{
    uint32_t characters = 0u;
    for (unsigned char character : name)
    {
        if (character == 0u) return false;
        if ((character & 0xc0u) != 0x80u && ++characters > 64u) return false;
    }
    return characters > 0u;
}

struct ParticleFrame
{
    uint32_t x = 0u, y = 0u;
    uint32_t width = 0u, height = 0u;
    float pivotX = 0.5f, pivotY = 0.5f;

    [[nodiscard]] bool IsValid() const noexcept
    {
        return width > 0u && height > 0u && std::isfinite(pivotX) && std::isfinite(pivotY) &&
            uint64_t(x) + width <= std::numeric_limits<uint32_t>::max() &&
            uint64_t(y) + height <= std::numeric_limits<uint32_t>::max();
    }
    bool operator==(const ParticleFrame&) const = default;
};

} // namespace engine
