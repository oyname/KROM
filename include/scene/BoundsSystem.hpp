#pragma once
#include "core/Math.hpp"
#include "ecs/World.hpp"

namespace engine::jobs { class JobSystem; }

namespace engine {

class BoundsSystem
{
public:
    void Update(ecs::World& world);
    void Update(ecs::World& world, jobs::JobSystem& js);

    static void ComputeBoundsForEntity(ecs::World& world,
                                       EntityID id) noexcept;

    // World AABB of one entity as Update() would write it now: local bounds through the
    // world transform computed from the current local transforms of the parent chain
    // (TransformSystem::ComputeCurrentWorldTransform), so it is current right after a
    // transform setter, before the next TransformSystem/BoundsSystem update. Reads only.
    // Entity with TransformComponent and WorldTransformComponent: current world transform;
    // only WorldTransformComponent: its stored matrix; neither: the stored
    // centerWorld/extentsWorld. Returns false without BoundsComponent (or dead entity).
    // When nothing in the parent chain changed since the last update and the stored world
    // bounds belong to the stored world transform, the stored values are returned without
    // matrix math (this assumes WorldTransformComponent of entities with a
    // TransformComponent is written only by TransformSystem and reparenting sets dirty).
    [[nodiscard]] static bool ComputeCurrentWorldBounds(const ecs::World& world, EntityID id,
                                                        math::Vec3& outCenter, math::Vec3& outExtents) noexcept;

    static void TransformAABB(const math::Vec3& localCenter,
                              const math::Vec3& localExtents,
                              const math::Mat4& worldMatrix,
                              math::Vec3& outWorldCenter,
                              math::Vec3& outWorldExtents) noexcept;
};

} // namespace engine
