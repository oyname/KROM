// =============================================================================
// KROM Engine - src/renderer/ECSExtractor.cpp
// =============================================================================
#include "renderer/ECSExtractor.hpp"
#include "core/Debug.hpp"
#include "ecs/Components.hpp"

namespace engine::renderer {

bool ECSExtractor::IsEntityActive(const ecs::World& world, EntityID entity)
{
    const auto* active = world.Get<ActiveComponent>(entity);
    return active == nullptr || active->active;
}

void ECSExtractor::ExtractRenderables(const ecs::World& world, RenderWorld& renderWorld)
{
    world.View<WorldTransformComponent, MeshComponent, MaterialComponent>(
        [&](EntityID id,
            const WorldTransformComponent& wt,
            const MeshComponent& mesh,
            const MaterialComponent& mat)
        {
            if (!IsEntityActive(world, id))
                return;
            if (!mesh.mesh.IsValid())
                return;

            math::Vec3 boundsCenter  = math::Vec3(wt.matrix.m[3][0], wt.matrix.m[3][1], wt.matrix.m[3][2]);
            math::Vec3 boundsExtents = math::Vec3(1.f, 1.f, 1.f);
            float      boundsRadius  = 0.f;

            if (const auto* b = world.Get<BoundsComponent>(id))
            {
                boundsCenter  = b->centerWorld;
                boundsExtents = b->extentsWorld;
                boundsRadius  = b->boundingSphere;
            }

            renderWorld.AddRenderable(
                id, mesh.mesh, mat.material,
                wt.matrix, wt.inverse.Transposed(),
                boundsCenter, boundsExtents, boundsRadius,
                mesh.layerMask, mesh.castShadows);
        });
}

void ECSExtractor::ExtractLights(const ecs::World& world, RenderWorld& renderWorld)
{
    world.View<WorldTransformComponent, LightComponent>(
        [&](EntityID id,
            const WorldTransformComponent& wt,
            const LightComponent& lc)
        {
            if (!IsEntityActive(world, id))
                return;

            renderWorld.AddLight(
                id, lc.type,
                wt.matrix.TransformPoint(math::Vec3(0, 0, 0)),
                wt.matrix.TransformDirection(math::Vec3(0, 0, -1)).Normalized(),
                lc.color, lc.intensity, lc.range,
                lc.spotInnerDeg, lc.spotOuterDeg,
                lc.castShadows);
        });
}

} // namespace engine::renderer
