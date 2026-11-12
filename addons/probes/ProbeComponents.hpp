#pragma once
// =============================================================================
// KROM Engine - addons/probes/ProbeComponents.hpp
//
// Local reflection probe: captures the scene around its position and lights
// everything inside its box with that capture instead of the global IBL.
// The box is centred on the entity and axis-aligned (rotation and scale of the
// entity are ignored).
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
