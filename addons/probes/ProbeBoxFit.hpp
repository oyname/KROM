#pragma once

#include "core/Math.hpp"
#include "core/Types.hpp"

#include <optional>

namespace engine::assets { class AssetRegistry; }
namespace engine::ecs { class World; }

namespace engine::addons::probes {

struct ProbeBoxFit
{
    math::Vec3 extents;
    math::Vec3 boxOffset;
};

// Fits an influence box around the capture point in the probe's local axes.
// Returns no fit if the entity has no probe or world transform.
[[nodiscard]] std::optional<ProbeBoxFit> FitProbeBox(const ecs::World& world,
                                                    const assets::AssetRegistry& registry,
                                                    EntityID probeEntity);

} // namespace engine::addons::probes
