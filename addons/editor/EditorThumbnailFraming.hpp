#pragma once

#include "addons/mesh_renderer/MeshRendererComponents.hpp"
#include "assets/AssetRegistry.hpp"
#include "ecs/World.hpp"
#include "scene/BoundsSystem.hpp"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <span>

namespace engine::renderer::addons::editor {

// Call after resolving mesh bindings and updating the hierarchy's world transforms.
// Group nodes and stale serialized bounds must not affect the thumbnail framing.
inline bool ComputeThumbnailMeshBounds(const ecs::World& world,
                                       const assets::AssetRegistry& registry,
                                       std::span<const EntityID> entities,
                                       math::Vec3& outCenter, float& outRadius)
{
    math::Vec3 min{FLT_MAX, FLT_MAX, FLT_MAX};
    math::Vec3 max{-FLT_MAX, -FLT_MAX, -FLT_MAX};
    bool found = false;
    for (EntityID entity : entities)
    {
        const auto* mesh = world.Get<MeshComponent>(entity);
        const auto* transform = world.Get<WorldTransformComponent>(entity);
        const auto* active = world.Get<ActiveComponent>(entity);
        if (!mesh || !mesh->visible || !transform || (active && !active->active))
            continue;
        const auto* asset = registry.meshes.Get(mesh->mesh);
        if (!asset)
            continue;

        math::Vec3 localMin, localMax;
        asset->ComputeBounds(localMin, localMax);
        if (localMin.x > localMax.x || localMin.y > localMax.y || localMin.z > localMax.z)
            continue;

        math::Vec3 center, extents;
        BoundsSystem::TransformAABB((localMin + localMax) * 0.5f,
                                   (localMax - localMin) * 0.5f,
                                   transform->matrix, center, extents);
        const math::Vec3 lo = center - extents;
        const math::Vec3 hi = center + extents;
        if (!std::isfinite(lo.x) || !std::isfinite(lo.y) || !std::isfinite(lo.z) ||
            !std::isfinite(hi.x) || !std::isfinite(hi.y) || !std::isfinite(hi.z))
            continue;
        min.x = std::min(min.x, lo.x); max.x = std::max(max.x, hi.x);
        min.y = std::min(min.y, lo.y); max.y = std::max(max.y, hi.y);
        min.z = std::min(min.z, lo.z); max.z = std::max(max.z, hi.z);
        found = true;
    }
    if (!found)
        return false;
    outCenter = (min + max) * 0.5f;
    outRadius = std::max(0.1f, ((max - min) * 0.5f).Length());
    return true;
}

} // namespace engine::renderer::addons::editor
