#include "addons/shadow/ShadowExtraction.hpp"

#include "addons/camera/CameraViewBuilder.hpp"
#include "addons/lighting/LightingComponents.hpp"
#include "addons/lighting/LightingFrameData.hpp"
#include "addons/mesh_renderer/MeshRendererComponents.hpp"
#include "addons/shadow/ShadowFrameData.hpp"
#include "addons/shadow/ShadowViewBuilder.hpp"
#include "ecs/Components.hpp"
#include <algorithm>
#include <limits>

namespace engine::addons::shadow {
namespace {

[[nodiscard]] bool IsEntityActive(const ecs::World& world, EntityID entity) noexcept
{
    const auto* active = world.Get<ActiveComponent>(entity);
    return active == nullptr || active->active;
}

void ExpandMinMax(const math::Vec3& point, math::Vec3& minPoint, math::Vec3& maxPoint) noexcept
{
    minPoint.x = std::min(minPoint.x, point.x);
    minPoint.y = std::min(minPoint.y, point.y);
    minPoint.z = std::min(minPoint.z, point.z);
    maxPoint.x = std::max(maxPoint.x, point.x);
    maxPoint.y = std::max(maxPoint.y, point.y);
    maxPoint.z = std::max(maxPoint.z, point.z);
}

[[nodiscard]] collision::AABB ComputeSceneCasterBounds(const ecs::World& world) noexcept
{
    collision::AABB bounds{};
    bool hasBounds = false;
    math::Vec3 minPoint(std::numeric_limits<float>::max());
    math::Vec3 maxPoint(-std::numeric_limits<float>::max());

    world.View<MeshComponent, BoundsComponent>([&](EntityID entity,
                                                   const MeshComponent& mesh,
                                                   const BoundsComponent& meshBounds)
    {
        if (!IsEntityActive(world, entity) || !mesh.castShadows)
            return;

        const float radius = meshBounds.boundingSphere > 0.f
            ? meshBounds.boundingSphere
            : meshBounds.extentsWorld.Length();
        const math::Vec3 radiusVec(radius, radius, radius);
        ExpandMinMax(meshBounds.centerWorld - radiusVec, minPoint, maxPoint);
        ExpandMinMax(meshBounds.centerWorld + radiusVec, minPoint, maxPoint);
        hasBounds = true;
    });

    if (!hasBounds)
    {
        bounds.min = math::Vec3(-1.f, -1.f, -1.f);
        bounds.max = math::Vec3(1.f, 1.f, 1.f);
        return bounds;
    }

    bounds.min = minPoint;
    bounds.max = maxPoint;
    return bounds;
}


[[nodiscard]] collision::AABB ComputePrimaryCameraFrustumBounds(const ecs::World& world) noexcept
{
    renderer::RenderView renderView{};
    if (!camera::BuildPrimaryRenderView(world, 0u, 0u, renderView))
        return ComputeSceneCasterBounds(world);

    const math::Mat4 invViewProj = (renderView.projection * renderView.view).Inverse();

    const float effectiveFar = std::max(renderView.nearPlane,
                                        std::min(renderView.farPlane, 100.0f));

    math::Vec3 minPoint(std::numeric_limits<float>::max());
    math::Vec3 maxPoint(-std::numeric_limits<float>::max());

    auto addCorner = [&](float x, float y, float z)
    {
        math::Vec4 cornerCS{x, y, z, 1.0f};
        math::Vec4 cornerWS4 = invViewProj * cornerCS;
        const float invW = std::fabs(cornerWS4.w) > 1e-6f ? (1.0f / cornerWS4.w) : 1.0f;
        math::Vec3 cornerWS = cornerWS4.xyz() * invW;

        // Clamp perspective far extent to a sane shadow distance so the
        // directional shadow fit follows the actually useful camera region.
        if (z > 0.5f)
        {
            math::Vec3 ray = cornerWS - renderView.cameraPosition;
            const float lenSq = ray.LengthSq();
            if (lenSq > 1e-8f)
                cornerWS = renderView.cameraPosition + ray.Normalized() * effectiveFar;
        }

        ExpandMinMax(cornerWS, minPoint, maxPoint);
    };

    // D3D/Vulkan style clip depth: near=0, far=1
    for (float z : {0.0f, 1.0f})
    {
        addCorner(-1.0f, -1.0f, z);
        addCorner( 1.0f, -1.0f, z);
        addCorner(-1.0f,  1.0f, z);
        addCorner( 1.0f,  1.0f, z);
    }

    collision::AABB bounds{};
    bounds.min = minPoint;
    bounds.max = maxPoint;
    return bounds;
}

[[nodiscard]] bool SupportsCurrentRenderPath(const ShadowRequest& request) noexcept
{
    return request.lightType == LightType::Directional &&
           request.technique == ShadowTechnique::ShadowMap2D &&
           !request.views.empty();
}

} // namespace

void ExtractShadow(const ecs::World& world, renderer::RenderWorld& renderWorld)
{
    ShadowFrameData& shadowData =
        renderWorld.GetOrCreateFeatureData<ShadowFrameData>("shadow.frame_data");
    shadowData.Reset();

    const lighting::LightingFrameData* lighting =
        renderWorld.GetFeatureData<lighting::LightingFrameData>();
    if (!lighting)
        return;

    const collision::AABB casterBoundsWorld = ComputeSceneCasterBounds(world);
    const collision::AABB receiverBoundsWorld = ComputePrimaryCameraFrustumBounds(world);

    ShadowLightID nextShadowLightId = 1u;
    for (const lighting::ExtractedLight& extractedLight : lighting->lights)
    {
        const auto* lightComponent = world.Get<LightComponent>(extractedLight.entity);
        const auto* worldTransform = world.Get<WorldTransformComponent>(extractedLight.entity);
        if (!lightComponent || !worldTransform)
            continue;
        if (!lightComponent->castShadows || !lightComponent->shadowSettings.enabled)
            continue;
        if (!IsEntityActive(world, extractedLight.entity))
            continue;

        ShadowRequest request{};
        request.id = nextShadowLightId++;
        request.lightEntity = extractedLight.entity;
        request.lightType = lightComponent->type;
        request.technique = ChooseShadowTechnique(*lightComponent);
        request.settings = lightComponent->shadowSettings;
        request.casterBoundsWorld = casterBoundsWorld;
        request.receiverBoundsWorld = receiverBoundsWorld;
        request.cacheable = request.settings.staticOnly;
        request.needsUpdate = request.settings.updateEveryFrame || !request.cacheable;

        if (request.technique == ShadowTechnique::None)
            continue;

        ShadowViewBuilder::BuildViews(*lightComponent, *worldTransform, casterBoundsWorld, request);
        if (request.views.empty())
            continue;

        shadowData.requests.push_back(std::move(request));
    }

    for (size_t i = 0; i < shadowData.requests.size(); ++i)
    {
        if (SupportsCurrentRenderPath(shadowData.requests[i]))
        {
            shadowData.selectedRequestIndex = i;
            break;
        }
    }
}

} // namespace engine::addons::shadow
