#include "addons/shadow/ShadowExtraction.hpp"

#include "addons/camera/CameraViewBuilder.hpp"
#include "addons/lighting/LightingComponents.hpp"
#include "addons/lighting/LightingFrameData.hpp"
#include "addons/mesh_renderer/MeshRendererComponents.hpp"
#include "addons/shadow/ShadowFrameData.hpp"
#include "addons/shadow/ShadowViewBuilder.hpp"
#include "ecs/Components.hpp"
#include <algorithm>
#include <cmath>
#include <limits>
#include <vector>

namespace engine::addons::shadow {
namespace {

[[nodiscard]] uint32_t ComputeCurrentRenderPathAtlasGridDim(size_t viewCount) noexcept
{
    return std::max(1u, static_cast<uint32_t>(std::ceil(std::sqrt(static_cast<float>(std::max<size_t>(1u, viewCount))))));
}

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
        if (!IsEntityActive(world, entity) || !mesh.castShadows || !mesh.mesh.IsValid())
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


[[nodiscard]] collision::AABB ComputePrimaryCameraFrustumBounds(const ecs::World& world,
                                                               const renderer::RenderView* currentView,
                                                               float maxShadowDistance) noexcept
{
    renderer::RenderView renderView{};
    if (currentView)
    {
        renderView = *currentView;
    }
    else if (!camera::BuildPrimaryRenderView(world, 0u, 0u, renderView))
    {
        return ComputeSceneCasterBounds(world);
    }

    const math::Mat4 invViewProj = (renderView.projection * renderView.view).Inverse();

    const float effectiveFar = std::max(renderView.nearPlane,
                                        std::min(renderView.farPlane, std::max(maxShadowDistance, renderView.nearPlane)));

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

// Berechnet AABB + die 8 Welt-Ecken eines Frustum-Slice im Bereich
// [nearDist, farDist] (Welt-Einheiten vom Kamera-Origin).
[[nodiscard]] collision::AABB ComputeFrustumSliceBounds(const renderer::RenderView& renderView,
                                                        float nearDist,
                                                        float farDist,
                                                        std::array<math::Vec3, 8>& outCorners) noexcept
{
    const math::Mat4 invViewProj = (renderView.projection * renderView.view).Inverse();
    math::Vec3 minPoint(std::numeric_limits<float>::max());
    math::Vec3 maxPoint(-std::numeric_limits<float>::max());
    uint32_t cornerIndex = 0u;

    auto addCorner = [&](float x, float y, float z)
    {
        math::Vec4 clipCS{x, y, z, 1.0f};
        math::Vec4 ws4 = invViewProj * clipCS;
        const float invW = std::fabs(ws4.w) > 1e-6f ? (1.0f / ws4.w) : 1.0f;
        math::Vec3 wsPos = ws4.xyz() * invW;

        if (z > 0.5f)
        {
            math::Vec3 ray = wsPos - renderView.cameraPosition;
            const float lenSq = ray.LengthSq();
            if (lenSq > 1e-8f)
                wsPos = renderView.cameraPosition + ray.Normalized() * farDist;
        }
        else
        {
            math::Vec3 ray = wsPos - renderView.cameraPosition;
            const float lenSq = ray.LengthSq();
            if (lenSq > 1e-8f)
                wsPos = renderView.cameraPosition + ray.Normalized() * nearDist;
        }

        if (cornerIndex < outCorners.size())
            outCorners[cornerIndex++] = wsPos;
        ExpandMinMax(wsPos, minPoint, maxPoint);
    };

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

// Berechnet N Cascade-Frustum-Bounds mit Practical Split Scheme (lambda-Blend log/uniform).
[[nodiscard]] std::vector<collision::AABB> ComputeCascadeBounds(const ecs::World& world,
                                                                const renderer::RenderView* currentView,
                                                                uint32_t cascadeCount,
                                                                float shadowMaxDistance,
                                                                float cascadeLambda,
                                                                std::vector<std::array<math::Vec3, 8>>& outCorners) noexcept
{
    std::vector<collision::AABB> result;
    outCorners.clear();
    if (cascadeCount <= 1u)
        return result;

    renderer::RenderView renderView{};
    if (currentView)
    {
        renderView = *currentView;
    }
    else if (!camera::BuildPrimaryRenderView(world, 0u, 0u, renderView))
    {
        return result;
    }

    const float camNear = renderView.nearPlane;
    const float camFar  = std::min(renderView.farPlane, std::max(shadowMaxDistance, camNear + 1.0f));
    const float N       = static_cast<float>(cascadeCount);

    // Compute N+1 split distances
    std::vector<float> splits(cascadeCount + 1u);
    splits[0] = camNear;
    splits[cascadeCount] = camFar;
    for (uint32_t i = 1u; i < cascadeCount; ++i)
    {
        const float fi = static_cast<float>(i) / N;
        const float splitLog     = camNear * std::pow(camFar / std::max(camNear, 1e-4f), fi);
        const float splitUniform = camNear + (camFar - camNear) * fi;
        splits[i] = splitUniform + (splitLog - splitUniform) * cascadeLambda;
    }

    result.reserve(cascadeCount);
    outCorners.reserve(cascadeCount);
    for (uint32_t i = 0u; i < cascadeCount; ++i)
    {
        std::array<math::Vec3, 8> corners{};
        result.push_back(ComputeFrustumSliceBounds(renderView, splits[i], splits[i + 1u], corners));
        outCorners.push_back(corners);
    }

    return result;
}

[[nodiscard]] bool SupportsCurrentRenderPath(const ShadowRequest& request) noexcept
{
    return (request.technique == ShadowTechnique::ShadowMap2D ||
            request.technique == ShadowTechnique::ShadowMapCube ||
            request.technique == ShadowTechnique::CascadedShadowMap) &&
           !request.views.empty();
}

[[nodiscard]] bool SupportsCurrentRenderPathCandidate(const ShadowRequest& request) noexcept
{
    return request.technique == ShadowTechnique::ShadowMap2D ||
           request.technique == ShadowTechnique::ShadowMapCube ||
           request.technique == ShadowTechnique::CascadedShadowMap;
}

} // namespace

void ExtractShadow(const renderer::SceneExtractionContext& context)
{
    const ecs::World& world = context.world;
    renderer::RenderExtractionView extraction = context.extractionView;
    ShadowFrameData& shadowData =
        extraction.GetOrCreateFrameData<ShadowFrameData>("shadow.frame_data");
    shadowData.Reset();
    if (context.view && !context.view->enableShadows)
    {
        extraction.SetActiveShadowResolution(0u);
        return;
    }

    const lighting::LightingFrameData* lighting =
        extraction.GetFrameData<lighting::LightingFrameData>();
    if (!lighting)
        return;

    const collision::AABB casterBoundsWorld = ComputeSceneCasterBounds(world);

    // Baue zuerst alle Requests ohne receiverBounds auf (brauchen shadowDistance des gewählten Lichts)
    ShadowLightID nextShadowLightId = 1u;
    for (size_t lightIndex = 0; lightIndex < lighting->lights.size(); ++lightIndex)
    {
        const lighting::ExtractedLight& extractedLight = lighting->lights[lightIndex];
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
        request.visibleLightIndex = static_cast<uint32_t>(lightIndex);
        request.lightType = lightComponent->type;
        request.technique = ChooseShadowTechnique(*lightComponent);
        request.settings = lightComponent->shadowSettings;
        request.casterBoundsWorld = casterBoundsWorld;
        request.cacheable = false;
        request.needsUpdate = true;

        if (request.technique == ShadowTechnique::None)
            continue;

        shadowData.AddShadowedLightIndex(request.visibleLightIndex);
        shadowData.requests.push_back(std::move(request));
    }

    // Wähle das erste kompatible Request — dessen maxDistance bestimmt die receiverBounds
    for (size_t i = 0; i < shadowData.requests.size(); ++i)
    {
        if (SupportsCurrentRenderPathCandidate(shadowData.requests[i]))
        {
            shadowData.AddCurrentRenderPathRequest(i, shadowData.requests[i].visibleLightIndex);
        }
    }

    // receiverBounds mit shadowDistance des gewählten Lichts berechnen und nachträglich setzen
    float shadowDistance = 100.0f;
    if (const ShadowRequest* currentRenderPathPrimaryRequest = shadowData.GetCurrentRenderPathPrimaryRequest())
        shadowDistance = std::max(currentRenderPathPrimaryRequest->settings.maxDistance, 0.1f);

    const collision::AABB receiverBoundsWorld = ComputePrimaryCameraFrustumBounds(world, context.view, shadowDistance);
    for (auto& req : shadowData.requests)
    {
        req.receiverBoundsWorld = receiverBoundsWorld;
        req.views.clear();
        req.cascadeReceiverBounds.clear();
        req.cascadeFrustumCornersWorld.clear();

        if (req.technique == ShadowTechnique::CascadedShadowMap &&
            req.settings.cascadeCount > 1u)
        {
            req.cascadeReceiverBounds = ComputeCascadeBounds(
                world,
                context.view,
                req.settings.cascadeCount,
                req.settings.maxDistance,
                req.settings.cascadeLambda,
                req.cascadeFrustumCornersWorld);
        }

        const auto* lightComponent = world.Get<LightComponent>(req.lightEntity);
        const auto* worldTransform = world.Get<WorldTransformComponent>(req.lightEntity);
        if (!lightComponent || !worldTransform)
            continue;

        ShadowViewBuilder::BuildViews(*lightComponent, *worldTransform, casterBoundsWorld, req);
    }

    shadowData.ClearCurrentRenderPathRequests();
    for (size_t i = 0; i < shadowData.requests.size(); ++i)
    {
        if (SupportsCurrentRenderPath(shadowData.requests[i]))
        {
            shadowData.AddCurrentRenderPathRequest(i, shadowData.requests[i].visibleLightIndex);
        }
    }

    // Auflösung des gewählten Requests in die Queue schreiben — wird von Pipeline-Aufbau gelesen
    uint32_t activeShadowResolution = 0u;
    if (shadowData.HasCurrentRenderPathRequests())
    {
        uint32_t maxTileResolution = 0u;
        size_t totalViewCount = 0u;
        for (size_t requestIndex : shadowData.currentRenderPath.requestIndices)
        {
            if (requestIndex >= shadowData.requests.size())
                continue;
            maxTileResolution = std::max(maxTileResolution,
                                         std::max(1u, shadowData.requests[requestIndex].settings.resolution));
            totalViewCount += shadowData.requests[requestIndex].views.size();
        }
        totalViewCount = std::min<size_t>(renderer::kMaxShadowViewsPerFrame, totalViewCount);
        const uint32_t gridDim = ComputeCurrentRenderPathAtlasGridDim(totalViewCount);
        activeShadowResolution = maxTileResolution * gridDim;
    }
    extraction.SetActiveShadowResolution(activeShadowResolution);
}

} // namespace engine::addons::shadow
