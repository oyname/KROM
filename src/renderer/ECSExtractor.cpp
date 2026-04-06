// =============================================================================
// KROM Engine - src/renderer/ECSExtractor.cpp
// =============================================================================
#include "renderer/ECSExtractor.hpp"
#include "core/Debug.hpp"
#include "ecs/Components.hpp"

namespace engine::renderer {
namespace {

void ExtractLegacyRenderables(const ecs::World& world, SceneSnapshot& snapshot)
{
    world.View<WorldTransformComponent, MeshComponent, MaterialComponent>(
        [&](EntityID id,
            const WorldTransformComponent& wt,
            const MeshComponent& mesh,
            const MaterialComponent& mat)
        {
            if (!ECSExtractor::IsEntityActive(world, id))
                return;
            if (!mesh.mesh.IsValid())
                return;

            RenderableEntry e{};
            e.entity = id;
            e.mesh = mesh.mesh;
            e.material = mat.material;
            e.submeshIndex = mat.submeshIndex;
            e.worldMatrix = wt.matrix;
            e.worldMatrixInvT = wt.inverse.Transposed();
            e.layerMask = mesh.layerMask;
            e.castShadows = mesh.castShadows;
            e.receiveShadows = mesh.receiveShadows;

            if (const auto* b = world.Get<BoundsComponent>(id))
            {
                e.boundsCenter = b->centerWorld;
                e.boundsExtents = b->extentsWorld;
                e.boundsRadius = b->boundingSphere;
            }
            else
            {
                e.boundsCenter = math::Vec3(wt.matrix.m[3][0], wt.matrix.m[3][1], wt.matrix.m[3][2]);
                e.boundsRadius = 0.f;
            }
            snapshot.renderables.push_back(e);
        });
}

void ExtractLegacyLights(const ecs::World& world, SceneSnapshot& snapshot)
{
    world.View<WorldTransformComponent, LightComponent>(
        [&](EntityID id,
            const WorldTransformComponent& wt,
            const LightComponent& lc)
        {
            if (!ECSExtractor::IsEntityActive(world, id))
                return;

            LightEntry e{};
            e.entity = id;
            e.lightType = lc.type;
            e.color = lc.color;
            e.intensity = lc.intensity;
            e.range = lc.range;
            e.spotInnerDeg = lc.spotInnerDeg;
            e.spotOuterDeg = lc.spotOuterDeg;
            e.castShadows = lc.castShadows;
            e.positionWorld = wt.matrix.TransformPoint(math::Vec3(0, 0, 0));
            e.directionWorld = wt.matrix.TransformDirection(math::Vec3(0, 0, -1)).Normalized();
            snapshot.lights.push_back(e);
        });
}

} // namespace

void ECSExtractor::BeginSnapshot(SceneSnapshot& snapshot)
{
    snapshot.Clear();
}

bool ECSExtractor::IsEntityActive(const ecs::World& world, EntityID entity)
{
    const auto* active = world.Get<ActiveComponent>(entity);
    return active == nullptr || active->active;
}

void ECSExtractor::Extract(const ecs::World& world, SceneSnapshot& snapshot)
{
    BeginSnapshot(snapshot);

    const size_t renderableOffset = snapshot.renderables.size();
    const size_t lightOffset = snapshot.lights.size();
    ExtractLegacyRenderables(world, snapshot);
    snapshot.RecordContribution("ecs_legacy.renderables",
                                renderableOffset,
                                lightOffset,
                                snapshot.renderables.size() - renderableOffset,
                                0u);

    const size_t lightOnlyOffset = snapshot.lights.size();
    ExtractLegacyLights(world, snapshot);
    snapshot.RecordContribution("ecs_legacy.lights",
                                snapshot.renderables.size(),
                                lightOnlyOffset,
                                0u,
                                snapshot.lights.size() - lightOnlyOffset);

    Debug::LogVerbose("ECSExtractor: %zu renderables, %zu lights",
        snapshot.renderables.size(), snapshot.lights.size());
}

} // namespace engine::renderer
