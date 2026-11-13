#pragma once
// =============================================================================
// KROM Engine - addons/probes/ProbeComponents.hpp
//
// Local reflection probe: captures the scene around its position and lights
// everything inside its box with that capture instead of the global IBL.
// The box is centred on the entity and turns with it; the entity's scale is
// ignored (the size comes from extents). Overlapping probes: higher priority
// first, on equal priority the smaller box. Inside blendDistance from the
// box walls the probe fades into the next one or the global IBL.
// =============================================================================
#include "core/Math.hpp"
#include "ecs/ComponentMeta.hpp"
#include <cstdint>
#include <string>

namespace engine {

struct ReflectionProbeComponent
{
    math::Vec3  extents{ 5.f, 3.f, 5.f };  // half size of the influence box, metres
    float       intensity = 1.f;
    float       blendDistance = 1.f;           // metres, measured inward from the walls
    int32_t     priority = 0;
    bool        boxProjection = true;          // parallax-corrected reflections
    uint32_t    resolution = 128u;         // prefilter mip 0 edge: 64, 128 or 256
    uint64_t    probeId = 0u;              // stable id, names the baked file
    std::string bakedDataPath;             // relative to the project's Assets folder

    bool operator==(const ReflectionProbeComponent&) const = default;
};

inline void RegisterProbeComponents(ecs::ComponentMetaRegistry& registry)
{
    using namespace ecs;
    RegisterComponent<ReflectionProbeComponent>(registry, "ReflectionProbeComponent");
}

} // namespace engine

namespace engine::addons::probes {
// Random non-zero id for a new probe (also for a copied probe, so the copy
// bakes to its own file).
[[nodiscard]] uint64_t GenerateProbeId();
} // namespace engine::addons::probes
