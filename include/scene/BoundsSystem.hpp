#pragma once
#include "core/Math.hpp"
#include "ecs/World.hpp"

namespace engine {

class BoundsSystem
{
public:
    void Update(ecs::World& world);

    static void ComputeBoundsForEntity(ecs::World& world,
                                       EntityID id) noexcept;

    static void TransformAABB(const math::Vec3& localCenter,
                              const math::Vec3& localExtents,
                              const math::Mat4& worldMatrix,
                              math::Vec3& outWorldCenter,
                              math::Vec3& outWorldExtents) noexcept;
};

} // namespace engine
