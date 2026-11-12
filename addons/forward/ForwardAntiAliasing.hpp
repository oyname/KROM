#pragma once
// =============================================================================
// KROM Engine - addons/forward/ForwardAntiAliasing.hpp
// MSAA setting of the forward pipeline. Owned by the forward add-on; the engine
// core only knows render targets and pipelines with a sample count.
// =============================================================================
#include <cstdint>

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

} // namespace engine::renderer::addons::forward
