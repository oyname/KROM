#pragma once
#include "addons/mesh_renderer/MeshRendererComponents.hpp"
#include "assets/AssetRegistry.hpp"
#include "collision/SceneQueries.hpp"
#include "ecs/Components.hpp"
#include <cstdint>
#include <functional>

namespace engine::mesh_renderer {

struct MeshCollisionRaycastOptions
{
    bool boundsFallbackForMissingMesh = true;
    std::function<bool(EntityID)> includeEntity = {};
};

struct MeshCollisionRaycastStats
{
    uint32_t broadphaseCandidates = 0u;
    uint32_t triangleTests = 0u;
};

class MeshSceneQueries
{
public:
    static bool HasLoadedTriangleMeshForEntity(const ecs::World& world,
                                               const assets::AssetRegistry& registry,
                                               EntityID entity)
    {
        if (!entity.IsValid() || !world.IsAlive(entity))
            return false;

        const auto* mesh = world.Get<MeshComponent>(entity);
        if (!mesh || !mesh->mesh.IsValid())
            return false;

        const auto* asset = registry.meshes.Get(mesh->mesh);
        if (!asset || asset->state != assets::AssetState::Loaded)
            return false;

        for (const auto& submesh : asset->submeshes)
            if (submesh.positions.size() >= 9u)
                return true;

        return false;
    }

    static bool RaycastTrianglesForEntity(const ecs::World& world,
                                          const assets::AssetRegistry& registry,
                                          EntityID entity,
                                          const collision::Ray& ray,
                                          float maxDistance,
                                          collision::RaycastHit& outHit,
                                          MeshCollisionRaycastStats* stats = nullptr)
    {
        bool found = false;
        float best = maxDistance;

        if (!entity.IsValid() || !world.IsAlive(entity))
            return false;

        const auto* mesh = world.Get<MeshComponent>(entity);
        const auto* worldTransform = world.Get<WorldTransformComponent>(entity);
        if (!mesh || !worldTransform)
            return false;

        const auto* asset = registry.meshes.Get(mesh->mesh);
        if (!asset || asset->state != assets::AssetState::Loaded)
            return false;

        for (const auto& submesh : asset->submeshes)
        {
            const size_t vertexCount = submesh.positions.size() / 3u;
            auto testTriangle = [&](uint32_t v0, uint32_t v1, uint32_t v2)
            {
                if (v0 >= vertexCount || v1 >= vertexCount || v2 >= vertexCount)
                    return;

                const uint32_t i0 = v0 * 3u;
                const uint32_t i1 = v1 * 3u;
                const uint32_t i2 = v2 * 3u;

                collision::Triangle triangle{
                    worldTransform->matrix.TransformPoint({ submesh.positions[i0], submesh.positions[i0 + 1], submesh.positions[i0 + 2] }),
                    worldTransform->matrix.TransformPoint({ submesh.positions[i1], submesh.positions[i1 + 1], submesh.positions[i1 + 2] }),
                    worldTransform->matrix.TransformPoint({ submesh.positions[i2], submesh.positions[i2 + 1], submesh.positions[i2 + 2] })
                };

                if (stats)
                    ++stats->triangleTests;

                float triT = 0.f;
                math::Vec3 triNormal{};
                if (!collision::SceneQueries::IntersectRayTriangle(ray, triangle, best, triT, triNormal))
                    return;

                best = triT;
                outHit.entity = entity;
                outHit.distance = triT;
                outHit.position = ray.origin + ray.direction * triT;
                outHit.normal = triNormal;
                found = true;
            };

            if (!submesh.indices.empty())
            {
                for (size_t i = 0; i + 2 < submesh.indices.size(); i += 3)
                    testTriangle(submesh.indices[i + 0], submesh.indices[i + 1], submesh.indices[i + 2]);
            }
            else
            {
                for (uint32_t i = 0u; i + 2u < static_cast<uint32_t>(vertexCount); i += 3u)
                    testTriangle(i + 0u, i + 1u, i + 2u);
            }
        }

        return found;
    }

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
                const MeshComponent&,
                const WorldTransformComponent&,
                const BoundsComponent& bounds)
        {
            float sphereT = 0.f;
            if (!collision::SceneQueries::IntersectRaySphere(ray, { bounds.centerWorld, bounds.boundingSphere }, best, sphereT))
                return;

            collision::RaycastHit meshHit{};
            if (!RaycastTrianglesForEntity(world, registry, id, ray, best, meshHit))
                return;

            best = meshHit.distance;
            outHit = meshHit;
            found = true;
        });

        return found;
    }
};

class MeshCollisionPipeline
{
public:
    void Build(const ecs::World& world)
    {
        m_broadphase.Build(world);
    }

    bool Raycast(const ecs::World& world,
                 const assets::AssetRegistry& registry,
                 const collision::Ray& ray,
                 float maxDistance,
                 collision::RaycastHit& outHit,
                 const MeshCollisionRaycastOptions& options = {},
                 MeshCollisionRaycastStats* stats = nullptr) const
    {
        bool found = false;
        float best = maxDistance;

        MeshCollisionRaycastStats localStats{};
        MeshCollisionRaycastStats& activeStats = stats ? *stats : localStats;

        const auto candidates = m_broadphase.CollectRaycastCandidates(ray, maxDistance);
        activeStats.broadphaseCandidates = static_cast<uint32_t>(candidates.size());

        for (const collision::RaycastCandidate& candidate : candidates)
        {
            if (candidate.boundsDistance > best)
                break;
            if (options.includeEntity && !options.includeEntity(candidate.entity))
                continue;

            collision::RaycastHit meshHit{};
            if (MeshSceneQueries::RaycastTrianglesForEntity(
                    world, registry, candidate.entity, ray, best, meshHit, &activeStats))
            {
                best = meshHit.distance;
                outHit = meshHit;
                found = true;
                continue;
            }

            if (!options.boundsFallbackForMissingMesh ||
                MeshSceneQueries::HasLoadedTriangleMeshForEntity(world, registry, candidate.entity))
                continue;

            best = candidate.boundsDistance;
            outHit.entity = candidate.entity;
            outHit.distance = candidate.boundsDistance;
            outHit.position = ray.origin + ray.direction * candidate.boundsDistance;
            outHit.normal = (outHit.position - candidate.sphere.center).Normalized();
            found = true;
        }

        return found;
    }

private:
    collision::SceneQueries m_broadphase;
};

} // namespace engine::mesh_renderer
