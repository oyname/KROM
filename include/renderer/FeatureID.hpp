#pragma once

#include <cstdint>
#include <string_view>
#include <functional>

namespace engine::renderer {

struct FeatureID
{
    uint32_t value = 0u;

    constexpr bool operator==(const FeatureID& o) const noexcept { return value == o.value; }
    constexpr bool operator!=(const FeatureID& o) const noexcept { return value != o.value; }
    [[nodiscard]] constexpr bool IsValid() const noexcept { return value != 0u; }

    static constexpr FeatureID FromString(std::string_view name) noexcept
    {
        uint32_t h = 2166136261u;
        for (char c : name)
            h = (h ^ static_cast<uint8_t>(c)) * 16777619u;
        return { h ? h : 1u };
    }
};

} // namespace engine::renderer

namespace std {
template<>
struct hash<engine::renderer::FeatureID>
{
    size_t operator()(engine::renderer::FeatureID id) const noexcept
    {
        return static_cast<size_t>(id.value);
    }
};
} // namespace std
