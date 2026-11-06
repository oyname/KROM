#pragma once

#include "addons/mesh_renderer/MeshRendererComponents.hpp"
#include "assets/AssetRegistry.hpp"
#include "ecs/World.hpp"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <span>

namespace engine::renderer::addons::editor {

inline constexpr float kAssetThumbnailFov = 35.f * math::DEG_TO_RAD;

// Call after resolving mesh bindings and updating the hierarchy's world transforms.
inline bool FitAssetThumbnailCamera(const ecs::World& world,
                                    const assets::AssetRegistry& registry,
                                    std::span<const EntityID> entities,
                                    const math::Vec3& origin,
                                    math::Vec3& outCenter, math::Vec3& outEyeOffset)
{
    const math::Vec3 back = math::Vec3{0.65f, 0.55f, 1.f}.Normalized();
    const math::Vec3 right = math::Vec3::Cross(math::Vec3::Up(), back).Normalized();
    const math::Vec3 up = math::Vec3::Cross(back, right);
    const float slope = std::tan(kAssetThumbnailFov * 0.5f) * 0.88f;
    math::Vec2 lower{FLT_MAX, FLT_MAX};
    math::Vec2 upper{-FLT_MAX, -FLT_MAX};
    float minDepth = FLT_MAX;
    float maxDepth = -FLT_MAX;
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

        for (float x : {localMin.x, localMax.x})
            for (float y : {localMin.y, localMax.y})
                for (float z : {localMin.z, localMax.z})
                {
                    const math::Vec3 point = transform->matrix.TransformPoint({x, y, z}) - origin;
                    if (!std::isfinite(point.x) || !std::isfinite(point.y) || !std::isfinite(point.z))
                        continue;
                    const float h = math::Vec3::Dot(point, right);
                    const float v = math::Vec3::Dot(point, up);
                    const float depth = math::Vec3::Dot(point, back);
                    minDepth = std::min(minDepth, depth);
                    maxDepth = std::max(maxDepth, depth);
                    lower.x = std::min(lower.x, h - depth * slope);
                    lower.y = std::min(lower.y, v - depth * slope);
                    upper.x = std::max(upper.x, h + depth * slope);
                    upper.y = std::max(upper.y, v + depth * slope);
                }
    }
    if (minDepth > maxDepth)
    {
        outCenter = origin;
        outEyeOffset = back * 4.f;
        return false;
    }

    // At camera depth d each corner spans (d - depth) * slope about the image center.
    // Fit the horizontal and vertical intervals independently, keeping a border on each edge.
    const float centerDepth = (minDepth + maxDepth) * 0.5f;
    const float fitDepth = std::max(upper.x - lower.x, upper.y - lower.y) / (2.f * slope);
    const float frontClearance = std::max(0.001f, (maxDepth - minDepth) * 0.05f);
    const float eyeDepth = std::max(fitDepth, maxDepth + frontClearance);
    outCenter = origin + right * ((lower.x + upper.x) * 0.5f)
                       + up * ((lower.y + upper.y) * 0.5f) + back * centerDepth;
    outEyeOffset = back * (eyeDepth - centerDepth);
    return true;
}

} // namespace engine::renderer::addons::editor
