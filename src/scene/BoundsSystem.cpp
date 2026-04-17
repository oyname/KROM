#include "scene/BoundsSystem.hpp"
#include "ecs/Components.hpp"
#include <cmath>

namespace engine {

void BoundsSystem::TransformAABB(const math::Vec3& localCenter,
                                 const math::Vec3& localExtents,
                                 const math::Mat4& worldMatrix,
                                 math::Vec3& outWorldCenter,
                                 math::Vec3& outWorldExtents) noexcept
{
    outWorldCenter = worldMatrix.TransformPoint(localCenter);

    const math::Mat4& m = worldMatrix;
    outWorldExtents.x =
        std::abs(m.m[0][0]) * localExtents.x +
        std::abs(m.m[1][0]) * localExtents.y +
        std::abs(m.m[2][0]) * localExtents.z;
    outWorldExtents.y =
        std::abs(m.m[0][1]) * localExtents.x +
        std::abs(m.m[1][1]) * localExtents.y +
        std::abs(m.m[2][1]) * localExtents.z;
    outWorldExtents.z =
        std::abs(m.m[0][2]) * localExtents.x +
        std::abs(m.m[1][2]) * localExtents.y +
        std::abs(m.m[2][2]) * localExtents.z;
}

void BoundsSystem::Update(ecs::World& world)
{
    world.View<WorldTransformComponent, BoundsComponent, TransformComponent>(
        [&](EntityID,
            const WorldTransformComponent& worldTransform,
            BoundsComponent& bounds,
            const TransformComponent& transform)
        {
            if (transform.worldVersion == bounds.lastTransformVersion && !bounds.localDirty)
                return;

            TransformAABB(bounds.centerLocal,
                          bounds.extentsLocal,
                          worldTransform.matrix,
                          bounds.centerWorld,
                          bounds.extentsWorld);

            bounds.boundingSphere = bounds.extentsWorld.Length();
            bounds.lastTransformVersion = transform.worldVersion;
            bounds.localDirty = false;
        });
}

void BoundsSystem::ComputeBoundsForEntity(ecs::World& world,
                                          EntityID id) noexcept
{
    if (!world.IsAlive(id))
        return;

    const auto* worldTransform = world.Get<WorldTransformComponent>(id);
    auto* bounds = world.Get<BoundsComponent>(id);
    const auto* transform = world.Get<TransformComponent>(id);
    if (!worldTransform || !bounds || !transform)
        return;

    TransformAABB(bounds->centerLocal,
                  bounds->extentsLocal,
                  worldTransform->matrix,
                  bounds->centerWorld,
                  bounds->extentsWorld);

    bounds->boundingSphere = bounds->extentsWorld.Length();
    bounds->lastTransformVersion = transform->worldVersion;
    bounds->localDirty = false;
}

} // namespace engine
