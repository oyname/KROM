#pragma once
#include "addons/mesh_renderer/MeshRendererComponents.hpp"
#include "assets/AssetPipeline.hpp"
#include "ecs/Components.hpp"

namespace engine::mesh_renderer {

inline void ConfigureAssetPipeline(assets::AssetPipeline& pipeline)
{
    pipeline.RegisterSceneDirectiveHandler(
        [](const std::string& directive,
           const std::vector<std::string>& parts,
           const assets::AssetPipeline::SceneDirectiveContext& context) -> bool
        {
            if (directive == "mesh" && parts.size() >= 2)
            {
                const MeshHandle mesh = context.pipeline.LoadMesh(parts[1]);
                if (!context.world.Has<MeshComponent>(context.entity))
                    context.world.Add<MeshComponent>(context.entity, mesh);
                else
                    context.world.Get<MeshComponent>(context.entity)->mesh = mesh;

                if (!context.world.Has<BoundsComponent>(context.entity))
                    context.world.Add<BoundsComponent>(context.entity);
                context.world.Get<BoundsComponent>(context.entity)->localDirty = true;
                return true;
            }

            if (directive == "material" && parts.size() >= 2)
            {
                const MaterialHandle material = context.pipeline.LoadMaterial(parts[1]);
                if (!context.world.Has<MaterialComponent>(context.entity))
                    context.world.Add<MaterialComponent>(context.entity, material);
                else
                    context.world.Get<MaterialComponent>(context.entity)->material = material;
                return true;
            }

            return false;
        });
}

} // namespace engine::mesh_renderer
