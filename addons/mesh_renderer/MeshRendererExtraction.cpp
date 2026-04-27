#include "addons/mesh_renderer/MeshRendererExtraction.hpp"

#include "addons/mesh_renderer/MeshRendererComponents.hpp"
#include "ecs/Components.hpp"
#include "jobs/JobSystem.hpp"

namespace engine::addons::mesh_renderer {
namespace {

[[nodiscard]] bool IsEntityActive(const ecs::World& world, EntityID entity) noexcept
{
    const auto* active = world.Get<ActiveComponent>(entity);
    return active == nullptr || active->active;
}

void ExtractToRenderWorld(const ecs::World& world, renderer::RenderWorld& renderWorld)
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

            math::Vec3 boundsCenter = math::Vec3(wt.matrix.m[3][0], wt.matrix.m[3][1], wt.matrix.m[3][2]);
            math::Vec3 boundsExtents = math::Vec3(1.f, 1.f, 1.f);
            float boundsRadius = 0.f;

            if (const auto* b = world.Get<BoundsComponent>(id))
            {
                boundsCenter = b->centerWorld;
                boundsExtents = b->extentsWorld;
                boundsRadius = b->boundingSphere;
            }

            renderWorld.AddRenderable(
                id, mesh.mesh, mat.material,
                wt.matrix, wt.inverse.Transposed(),
                boundsCenter, boundsExtents, boundsRadius,
                mesh.layerMask, mesh.castShadows);
        });
}

} // namespace

void ExtractRenderables(const ecs::World& world, renderer::RenderSceneSnapshot& snapshot)
{
    ExtractToRenderWorld(world, snapshot.GetWorld());
}

void ExtractRenderables(const ecs::World& world, renderer::RenderWorld& renderWorld)
{
    ExtractToRenderWorld(world, renderWorld);
}

void ExtractRenderables(const ecs::World& world,
                        renderer::RenderSceneSnapshot& snapshot,
                        jobs::JobSystem* /*jobSystem*/)
{
    ExtractToRenderWorld(world, snapshot.GetWorld());
}

void ExtractRenderables(const ecs::World& world,
                        renderer::RenderWorld& renderWorld,
                        jobs::JobSystem* /*jobSystem*/)
{
    ExtractToRenderWorld(world, renderWorld);
}

} // namespace engine::addons::mesh_renderer
