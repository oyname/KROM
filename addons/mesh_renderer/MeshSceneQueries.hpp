#pragma once
#include "addons/mesh_renderer/MeshRendererComponents.hpp"
#include "assets/AssetRegistry.hpp"
#include "collision/SceneQueries.hpp"
#include "ecs/Components.hpp"

namespace engine::mesh_renderer {

class MeshSceneQueries
{
public:
    static bool RaycastTriangles(const ecs::World& world,
                                 const assets::AssetRegistry& registry,
                                 const collision::Ray& ray,
                                 float maxDistance,
                                 collision::RaycastHit& outHit)
    {
        bool found = false;
        float best = maxDistance;

        world.View<MeshComponent, WorldTransformComponent, BoundsComponent>(
            [&](EntityID id,
                const MeshComponent& mesh,
                const WorldTransformComponent& worldTransform,
                const BoundsComponent& bounds)
        {
            float sphereT = 0.f;
            if (!collision::SceneQueries::IntersectRaySphere(ray, { bounds.centerWorld, bounds.boundingSphere }, best, sphereT))
                return;

            const auto* asset = registry.meshes.Get(mesh.mesh);
            if (!asset || asset->state != assets::AssetState::Loaded)
                return;

            for (const auto& submesh : asset->submeshes)
            {
                for (size_t i = 0; i + 2 < submesh.indices.size(); i += 3)
                {
                    const uint32_t i0 = submesh.indices[i + 0] * 3u;
                    const uint32_t i1 = submesh.indices[i + 1] * 3u;
                    const uint32_t i2 = submesh.indices[i + 2] * 3u;
                    if (i2 + 2 >= submesh.positions.size())
                        continue;

                    collision::Triangle triangle{
                        worldTransform.matrix.TransformPoint({ submesh.positions[i0], submesh.positions[i0 + 1], submesh.positions[i0 + 2] }),
                        worldTransform.matrix.TransformPoint({ submesh.positions[i1], submesh.positions[i1 + 1], submesh.positions[i1 + 2] }),
                        worldTransform.matrix.TransformPoint({ submesh.positions[i2], submesh.positions[i2 + 1], submesh.positions[i2 + 2] })
                    };

                    float triT = 0.f;
                    math::Vec3 triNormal{};
                    if (!collision::SceneQueries::IntersectRayTriangle(ray, triangle, best, triT, triNormal))
                        continue;

                    best = triT;
                    outHit.entity = id;
                    outHit.distance = triT;
                    outHit.position = ray.origin + ray.direction * triT;
                    outHit.normal = triNormal;
                    found = true;
                }
            }
        });

        return found;
    }
};

} // namespace engine::mesh_renderer
