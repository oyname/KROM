#include "addons/probes/ProbeBoxFit.hpp"

#include "addons/mesh_renderer/MeshSceneQueries.hpp"
#include "addons/probes/ProbeComponents.hpp"
#include "addons/probes/ProbeBake.hpp"
#include "renderer/ECSExtractor.hpp"

#include <algorithm>
#include <array>
#include <vector>

namespace engine::addons::probes {
namespace {

constexpr int kRaysPerSide = 5;
constexpr float kSpreadStep = 0.1f;
constexpr float kMaxWallDistance = 50.f;
constexpr float kWallClearance = 0.02f;
constexpr float kMinExtent = 0.05f;

float MedianWallDistance(const ecs::World& world, const assets::AssetRegistry& registry,
                         const std::vector<EntityID>& meshes, const math::Vec3& origin,
                         const math::Vec3& axis, const math::Vec3& u, const math::Vec3& v)
{
    std::array<float, kRaysPerSide * kRaysPerSide> distances{};
    size_t hitCount = 0;
    for (int y = -kRaysPerSide / 2; y <= kRaysPerSide / 2; ++y)
    {
        for (int x = -kRaysPerSide / 2; x <= kRaysPerSide / 2; ++x)
        {
            const math::Vec3 direction =
                (axis + u * (x * kSpreadStep) + v * (y * kSpreadStep)).Normalized();
            const collision::Ray ray{ origin, direction };
            const float axisProjection = math::Vec3::Dot(axis, direction);
            float best = kMaxWallDistance / axisProjection;
            bool found = false;
            for (const EntityID entity : meshes)
            {
                if (const auto* bounds = world.Get<BoundsComponent>(entity))
                {
                    float sphereDistance = 0.f;
                    if (!collision::SceneQueries::IntersectRaySphere(
                            ray, { bounds->centerWorld, bounds->boundingSphere }, best, sphereDistance))
                        continue;
                }

                collision::RaycastHit hit{};
                if (mesh_renderer::MeshSceneQueries::RaycastTrianglesForEntity(
                        world, registry, entity, ray, best, hit))
                {
                    best = hit.distance;
                    found = true;
                }
            }
            if (found)
                distances[hitCount++] = best * axisProjection;
        }
    }

    if (hitCount == 0)
        return kMaxWallDistance;

    std::sort(distances.begin(), distances.begin() + hitCount);
    return (distances[(hitCount - 1) / 2] + distances[hitCount / 2]) * 0.5f;
}

} // namespace

std::optional<ProbeBoxFit> FitProbeBox(const ecs::World& world, const assets::AssetRegistry& registry,
                                       EntityID probeEntity)
{
    const auto* transform = world.Get<WorldTransformComponent>(probeEntity);
    if (!transform || !world.Has<ReflectionProbeComponent>(probeEntity))
        return std::nullopt;

    std::vector<EntityID> meshes;
    world.View<MeshComponent, WorldTransformComponent>(
        [&](EntityID entity, const MeshComponent& mesh, const WorldTransformComponent&) {
            if (entity != probeEntity && mesh.visible && (mesh.layerMask & kProbeCaptureLayers) != 0u &&
                renderer::ECSExtractor::IsEntityActive(world, entity) &&
                mesh_renderer::MeshSceneQueries::HasLoadedTriangleMeshForEntity(world, registry, entity))
                meshes.push_back(entity);
        });

    const math::Quat rotation = transform->rotation.Normalized();
    const math::Vec3 axes[] = {
        rotation.Rotate({ 1.f, 0.f, 0.f }),
        rotation.Rotate({ 0.f, 1.f, 0.f }),
        rotation.Rotate({ 0.f, 0.f, 1.f }),
    };
    float halfSize[3]{};
    float offset[3]{};
    for (int i = 0; i < 3; ++i)
    {
        const math::Vec3& u = axes[(i + 1) % 3];
        const math::Vec3& v = axes[(i + 2) % 3];
        const float positive = MedianWallDistance(world, registry, meshes, transform->position, axes[i], u, v);
        const float negative = MedianWallDistance(world, registry, meshes, transform->position, -axes[i], u, v);
        halfSize[i] = std::max((positive + negative) * 0.5f - kWallClearance, kMinExtent);
        offset[i] = (positive - negative) * 0.5f;
    }
    return ProbeBoxFit{ { halfSize[0], halfSize[1], halfSize[2] }, { offset[0], offset[1], offset[2] } };
}

} // namespace engine::addons::probes
