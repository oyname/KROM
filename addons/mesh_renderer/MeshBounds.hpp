#pragma once
#include "addons/mesh_renderer/MeshRendererComponents.hpp"
#include "assets/AssetRegistry.hpp"
#include "ecs/Components.hpp"

namespace engine::mesh_renderer {

inline void UpdateLocalBoundsFromMeshes(ecs::World& world,
                                        const assets::AssetRegistry& registry)
{
    world.View<MeshComponent, BoundsComponent>([&](EntityID,
                                                   const MeshComponent& mesh,
                                                   BoundsComponent& bounds)
    {
        if (!bounds.localDirty || !mesh.mesh.IsValid())
            return;

        const assets::MeshAsset* asset = registry.meshes.Get(mesh.mesh);
        if (!asset || asset->state != assets::AssetState::Loaded || asset->submeshes.empty())
            return;

        math::Vec3 minPos{};
        math::Vec3 maxPos{};
        asset->ComputeBounds(minPos, maxPos);
        bounds.centerLocal = (minPos + maxPos) * 0.5f;
        bounds.extentsLocal = (maxPos - minPos) * 0.5f;
        bounds.localDirty = false;
    });
}

inline void UpdateLocalBoundsForEntity(ecs::World& world,
                                       EntityID id,
                                       const assets::AssetRegistry& registry) noexcept
{
    if (!world.IsAlive(id))
        return;

    const auto* mesh = world.Get<MeshComponent>(id);
    auto* bounds = world.Get<BoundsComponent>(id);
    if (!mesh || !bounds || !bounds->localDirty || !mesh->mesh.IsValid())
        return;

    const assets::MeshAsset* asset = registry.meshes.Get(mesh->mesh);
    if (!asset || asset->state != assets::AssetState::Loaded || asset->submeshes.empty())
        return;

    math::Vec3 minPos{};
    math::Vec3 maxPos{};
    asset->ComputeBounds(minPos, maxPos);
    bounds->centerLocal = (minPos + maxPos) * 0.5f;
    bounds->extentsLocal = (maxPos - minPos) * 0.5f;
    bounds->localDirty = false;
}

} // namespace engine::mesh_renderer
