#include "addons/shadow/ShadowExtraction.hpp"

#include "addons/camera/CameraViewBuilder.hpp"
#include "addons/lighting/LightingComponents.hpp"
#include "addons/lighting/LightingFrameData.hpp"
#include "addons/mesh_renderer/MeshRendererComponents.hpp"
#include "addons/shadow/ShadowFrameData.hpp"
#include "addons/shadow/ShadowViewBuilder.hpp"
#include "collision/SceneQueries.hpp"
#include "ecs/Components.hpp"
#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>
#include <vector>

namespace engine::addons::shadow {
namespace {

[[nodiscard]] uint32_t ComputeCurrentRenderPathAtlasGridDim(size_t viewCount) noexcept
{
    return std::max(1u, static_cast<uint32_t>(std::ceil(std::sqrt(static_cast<float>(std::max<size_t>(1u, viewCount))))));
}

[[nodiscard]] uint32_t ComputeShadowAtlasResolution(uint32_t maxTileResolution,
                                                    size_t viewCount,
                                                    uint32_t maxAtlasResolution) noexcept
{
    if (maxTileResolution == 0u || viewCount == 0u)
        return 0u;

    const uint32_t gridDim = ComputeCurrentRenderPathAtlasGridDim(viewCount);
    const uint64_t requested = static_cast<uint64_t>(maxTileResolution) *
                               static_cast<uint64_t>(gridDim);
    const uint32_t capped = static_cast<uint32_t>(
        std::min<uint64_t>(requested, std::max(1u, maxAtlasResolution)));
    return std::max(gridDim, capped);
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

[[nodiscard]] math::Vec3 TransformForward(const WorldTransformComponent& worldTransform) noexcept
{
    return worldTransform.matrix.TransformDirection(math::Vec3::Forward()).Normalized();
}

void BuildAABBCorners(const collision::AABB& bounds, std::array<math::Vec3, 8>& corners) noexcept
{
    const math::Vec3& min = bounds.min;
    const math::Vec3& max = bounds.max;
    corners = {{
        { min.x, min.y, min.z },
        { max.x, min.y, min.z },
        { min.x, max.y, min.z },
        { max.x, max.y, min.z },
        { min.x, min.y, max.z },
        { max.x, min.y, max.z },
        { min.x, max.y, max.z },
        { max.x, max.y, max.z },
    }};
}

struct LightSpaceRect
{
    float minR = std::numeric_limits<float>::max();
    float maxR = -std::numeric_limits<float>::max();
    float minU = std::numeric_limits<float>::max();
    float maxU = -std::numeric_limits<float>::max();
};

[[nodiscard]] bool Overlaps(const LightSpaceRect& a, const LightSpaceRect& b) noexcept
{
    return a.minR <= b.maxR && a.maxR >= b.minR &&
           a.minU <= b.maxU && a.maxU >= b.minU;
}

[[nodiscard]] LightSpaceRect ComputeLightSpaceRect(const collision::AABB& bounds,
                                                   const math::Vec3& axisR,
                                                   const math::Vec3& axisU) noexcept
{
    LightSpaceRect rect{};
    std::array<math::Vec3, 8> corners{};
    BuildAABBCorners(bounds, corners);
    for (const math::Vec3& corner : corners)
    {
        const float r = math::Vec3::Dot(axisR, corner);
        const float u = math::Vec3::Dot(axisU, corner);
        rect.minR = std::min(rect.minR, r);
        rect.maxR = std::max(rect.maxR, r);
        rect.minU = std::min(rect.minU, u);
        rect.maxU = std::max(rect.maxU, u);
    }
    return rect;
}

[[nodiscard]] collision::AABB ComputeDirectionalCasterBounds(const ecs::World& world,
                                                            const collision::AABB& receiverBoundsWorld,
                                                            const WorldTransformComponent& lightTransform) noexcept
{
    const math::Vec3 lightDir = TransformForward(lightTransform);
    const math::Vec3 up = (std::fabs(lightDir.y) < 0.999f) ? math::Vec3::Up() : math::Vec3::Right();
    const math::Vec3 axisR = math::Vec3::Cross(lightDir, up).Normalized();
    const math::Vec3 axisU = math::Vec3::Cross(axisR, lightDir);

    LightSpaceRect receiverRect = ComputeLightSpaceRect(receiverBoundsWorld, axisR, axisU);
    constexpr float kReceiverMargin = 2.0f;
    receiverRect.minR -= kReceiverMargin;
    receiverRect.maxR += kReceiverMargin;
    receiverRect.minU -= kReceiverMargin;
    receiverRect.maxU += kReceiverMargin;

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
        const float centerR = math::Vec3::Dot(axisR, meshBounds.centerWorld);
        const float centerU = math::Vec3::Dot(axisU, meshBounds.centerWorld);
        const LightSpaceRect casterRect{
            centerR - radius,
            centerR + radius,
            centerU - radius,
            centerU + radius
        };
        if (!Overlaps(receiverRect, casterRect))
            return;

        const math::Vec3 radiusVec(radius, radius, radius);
        ExpandMinMax(meshBounds.centerWorld - radiusVec, minPoint, maxPoint);
        ExpandMinMax(meshBounds.centerWorld + radiusVec, minPoint, maxPoint);
        hasBounds = true;
    });

    if (!hasBounds)
        return receiverBoundsWorld;

    return collision::AABB{minPoint, maxPoint};
}

struct ViewFrustumCull
{
    bool valid = false;
    math::Mat4 view = math::Mat4::Identity();
    std::array<math::Vec4, 6> planes{};
    std::array<math::Vec3, 8> corners{};
    float projX = 1.0f;
    float projY = 1.0f;
    float nearPlane = 0.1f;
    float farPlane = 100.0f;
};

void ExtractFrustumPlanes(const math::Mat4& viewProj,
                          std::array<math::Vec4, 6>& planes) noexcept
{
    auto row = [&](int r) -> math::Vec4
    {
        return { viewProj.m[0][r], viewProj.m[1][r], viewProj.m[2][r], viewProj.m[3][r] };
    };

    const math::Vec4 r0 = row(0);
    const math::Vec4 r1 = row(1);
    const math::Vec4 r2 = row(2);
    const math::Vec4 r3 = row(3);

    planes[0] = { r3.x + r0.x, r3.y + r0.y, r3.z + r0.z, r3.w + r0.w };
    planes[1] = { r3.x - r0.x, r3.y - r0.y, r3.z - r0.z, r3.w - r0.w };
    planes[2] = { r3.x + r1.x, r3.y + r1.y, r3.z + r1.z, r3.w + r1.w };
    planes[3] = { r3.x - r1.x, r3.y - r1.y, r3.z - r1.z, r3.w - r1.w };
    planes[4] = { r2.x,        r2.y,        r2.z,        r2.w        };
    planes[5] = { r3.x - r2.x, r3.y - r2.y, r3.z - r2.z, r3.w - r2.w };

    for (math::Vec4& plane : planes)
    {
        const float len = std::sqrt(plane.x * plane.x + plane.y * plane.y + plane.z * plane.z);
        if (len > 1e-6f)
            plane = plane * (1.0f / len);
    }
}

void BuildFrustumCorners(const math::Mat4& viewProj,
                         std::array<math::Vec3, 8>& corners) noexcept
{
    const math::Mat4 invViewProj = viewProj.Inverse();
    uint32_t index = 0u;
    for (float z : {0.0f, 1.0f})
    {
        for (float y : {-1.0f, 1.0f})
        {
            for (float x : {-1.0f, 1.0f})
            {
                math::Vec4 clip{x, y, z, 1.0f};
                math::Vec4 world = invViewProj * clip;
                const float invW = std::fabs(world.w) > 1e-6f ? (1.0f / world.w) : 1.0f;
                corners[index++] = world.xyz() * invW;
            }
        }
    }
}

[[nodiscard]] bool FrustumCornersInsidePlanes(const std::array<math::Vec3, 8>& corners,
                                              const std::array<math::Vec4, 6>& planes) noexcept
{
    constexpr float kFrustumIntersectionMargin = 0.5f;
    for (const math::Vec4& plane : planes)
    {
        bool anyInside = false;
        for (const math::Vec3& corner : corners)
        {
            const float distance =
                plane.x * corner.x + plane.y * corner.y + plane.z * corner.z + plane.w;
            if (distance >= -kFrustumIntersectionMargin)
            {
                anyInside = true;
                break;
            }
        }
        if (!anyInside)
            return false;
    }
    return true;
}

[[nodiscard]] bool FrustumsIntersect(const std::array<math::Vec3, 8>& aCorners,
                                     const std::array<math::Vec4, 6>& aPlanes,
                                     const std::array<math::Vec3, 8>& bCorners,
                                     const std::array<math::Vec4, 6>& bPlanes) noexcept
{
    return FrustumCornersInsidePlanes(aCorners, bPlanes) &&
           FrustumCornersInsidePlanes(bCorners, aPlanes);
}

[[nodiscard]] ViewFrustumCull BuildViewFrustumCull(const renderer::RenderView* renderView,
                                                   float maxShadowDistance) noexcept
{
    ViewFrustumCull result{};
    if (!renderView)
        return result;

    result.valid = true;
    result.view = renderView->view;
    const math::Mat4 viewProj = renderView->projection * renderView->view;
    ExtractFrustumPlanes(viewProj, result.planes);
    BuildFrustumCorners(viewProj, result.corners);
    result.projX = std::max(std::fabs(renderView->projection.m[0][0]), 1e-4f);
    result.projY = std::max(std::fabs(renderView->projection.m[1][1]), 1e-4f);
    result.nearPlane = std::max(renderView->nearPlane, 1e-4f);
    result.farPlane = std::max(result.nearPlane + 0.1f,
                               std::min(renderView->farPlane,
                                        std::max(maxShadowDistance, result.nearPlane + 0.1f)));
    return result;
}

[[nodiscard]] bool ShadowViewsIntersectCameraFrustum(const ShadowRequest& request,
                                                     const ViewFrustumCull& cameraFrustum) noexcept
{
    if (!cameraFrustum.valid || request.lightType == LightType::Directional)
        return true;

    for (const ShadowView& shadowView : request.views)
    {
        std::array<math::Vec4, 6> shadowPlanes{};
        std::array<math::Vec3, 8> shadowCorners{};
        ExtractFrustumPlanes(shadowView.viewProj, shadowPlanes);
        BuildFrustumCorners(shadowView.viewProj, shadowCorners);
        if (FrustumsIntersect(shadowCorners, shadowPlanes, cameraFrustum.corners, cameraFrustum.planes))
            return true;
    }
    return false;
}

[[nodiscard]] bool SphereIntersectsViewFrustum(const ViewFrustumCull& frustum,
                                               const collision::Sphere& sphere) noexcept
{
    if (!frustum.valid)
        return true;

    const math::Vec3 centerView = frustum.view.TransformPoint(sphere.center);
    const float depth = -centerView.z;
    const float radius = std::max(sphere.radius, 0.0f);
    if (depth + radius < frustum.nearPlane)
        return false;
    if (depth - radius > frustum.farPlane)
        return false;

    const float clampedDepth = std::max(depth, frustum.nearPlane);
    const float halfWidth = clampedDepth / frustum.projX;
    const float halfHeight = clampedDepth / frustum.projY;
    if (centerView.x + radius < -halfWidth || centerView.x - radius > halfWidth)
        return false;
    if (centerView.y + radius < -halfHeight || centerView.y - radius > halfHeight)
        return false;
    return true;
}

[[nodiscard]] bool HasShadowReceiverInViewFrustum(const ecs::World& world,
                                                  const ViewFrustumCull& frustum,
                                                  uint32_t visibilityLayerMask) noexcept
{
    if (!frustum.valid)
        return true;

    bool hasReceiver = false;
    world.View<MeshComponent, BoundsComponent>([&](EntityID entity,
                                                   const MeshComponent& mesh,
                                                   const BoundsComponent& meshBounds)
    {
        if (hasReceiver)
            return;
        if (!IsEntityActive(world, entity) ||
            !mesh.visible ||
            !mesh.receiveShadows ||
            !mesh.mesh.IsValid() ||
            (mesh.layerMask & visibilityLayerMask) == 0u)
        {
            return;
        }

        const float radius = meshBounds.boundingSphere > 0.f
            ? meshBounds.boundingSphere
            : meshBounds.extentsWorld.Length();
        hasReceiver = SphereIntersectsViewFrustum(
            frustum,
            collision::Sphere{meshBounds.centerWorld, std::max(radius, 0.05f)});
    });
    return hasReceiver;
}

[[nodiscard]] bool LocalShadowRequestIntersectsReceiver(const ShadowRequest& request,
                                                        const LightComponent& light,
                                                        const WorldTransformComponent& worldTransform,
                                                        const collision::Sphere& receiverSphere) noexcept
{
    if (request.lightType == LightType::Point)
    {
        const math::Vec3 lightPosition = worldTransform.matrix.TransformPoint(math::Vec3(0.f, 0.f, 0.f));
        const float shadowRange = std::max(request.settings.maxDistance, 0.1f);
        const float maxDistance = shadowRange + receiverSphere.radius;
        return (receiverSphere.center - lightPosition).LengthSq() <= maxDistance * maxDistance;
    }

    if (request.lightType == LightType::Spot)
    {
        const math::Vec3 lightPosition = worldTransform.matrix.TransformPoint(math::Vec3(0.f, 0.f, 0.f));
        const math::Vec3 forward = TransformForward(worldTransform);
        const float shadowRange = std::max(request.settings.maxDistance, 0.1f);
        const float halfAngleRad = std::max(0.1f, light.spotOuterDeg) * math::DEG_TO_RAD;
        const float coneTan = std::tan(halfAngleRad);
        const math::Vec3 toReceiver = receiverSphere.center - lightPosition;
        const float distanceAlongCone = math::Vec3::Dot(toReceiver, forward);
        if (distanceAlongCone + receiverSphere.radius < 0.f ||
            distanceAlongCone - receiverSphere.radius > shadowRange)
        {
            return false;
        }

        const math::Vec3 closestOnAxis = lightPosition + forward * std::clamp(distanceAlongCone, 0.f, shadowRange);
        const float radialDistance = (receiverSphere.center - closestOnAxis).Length();
        const float coneRadius = std::max(distanceAlongCone, 0.f) * coneTan;
        return radialDistance <= coneRadius + receiverSphere.radius;
    }

    return true;
}

[[nodiscard]] bool LocalShadowRequestCanAffectVisibleReceiver(const ecs::World& world,
                                                             const ShadowRequest& request,
                                                             const ViewFrustumCull& frustum,
                                                             uint32_t visibilityLayerMask,
                                                             const LightComponent& light,
                                                             const WorldTransformComponent& worldTransform) noexcept
{
    if (!frustum.valid)
        return true;
    if (request.lightType == LightType::Directional)
        return true;

    bool affectsReceiver = false;
    world.View<MeshComponent, BoundsComponent>([&](EntityID entity,
                                                   const MeshComponent& mesh,
                                                   const BoundsComponent& meshBounds)
    {
        if (affectsReceiver)
            return;
        if (!IsEntityActive(world, entity) ||
            !mesh.visible ||
            !mesh.receiveShadows ||
            !mesh.mesh.IsValid() ||
            (mesh.layerMask & visibilityLayerMask) == 0u)
        {
            return;
        }

        const float radius = meshBounds.boundingSphere > 0.f
            ? meshBounds.boundingSphere
            : meshBounds.extentsWorld.Length();
        const collision::Sphere receiverSphere{meshBounds.centerWorld, std::max(radius, 0.05f)};
        if (!SphereIntersectsViewFrustum(frustum, receiverSphere))
            return;

        affectsReceiver = LocalShadowRequestIntersectsReceiver(
            request,
            light,
            worldTransform,
            receiverSphere);
    });
    return affectsReceiver;
}

[[nodiscard]] collision::AABB ComputeRequestCasterBounds(const ecs::World& world,
                                                        const ShadowRequest& request,
                                                        const collision::AABB& receiverBoundsWorld,
                                                        const collision::AABB& fallbackCasterBoundsWorld,
                                                        const WorldTransformComponent& lightTransform) noexcept
{
    if (request.lightType == LightType::Directional)
        return ComputeDirectionalCasterBounds(world, receiverBoundsWorld, lightTransform);
    return fallbackCasterBoundsWorld;
}

[[nodiscard]] uint32_t GetRetentionFrames(const ShadowExtractionTemporalState* state,
                                          EntityID entity) noexcept
{
    if (!state)
        return 0u;
    for (const auto& entry : state->localShadowRetention)
    {
        if (entry.first == entity)
            return entry.second;
    }
    return 0u;
}

void SetRetentionFrames(ShadowExtractionTemporalState* state, EntityID entity, uint32_t frames)
{
    if (!state)
        return;
    for (auto& entry : state->localShadowRetention)
    {
        if (entry.first == entity)
        {
            entry.second = frames;
            return;
        }
    }
    state->localShadowRetention.push_back({entity, frames});
}

void DecayUnusedRetentions(ShadowExtractionTemporalState* state)
{
    if (!state)
        return;
    for (auto& entry : state->localShadowRetention)
    {
        if (entry.second > 0u)
            --entry.second;
    }
    state->localShadowRetention.erase(
        std::remove_if(state->localShadowRetention.begin(),
                       state->localShadowRetention.end(),
                       [](const auto& entry) { return entry.second == 0u; }),
        state->localShadowRetention.end());
}

[[nodiscard]] bool ShadowRequestCanAffectReceivers(const ShadowRequest& request,
                                                   const collision::AABB& receiverBoundsWorld,
                                                   const ViewFrustumCull& viewFrustum,
                                                   bool hasVisibleShadowReceiver,
                                                   const LightComponent& light,
                                                   const WorldTransformComponent& worldTransform) noexcept
{
    if (request.lightType == LightType::Directional)
        return true;

    if (viewFrustum.valid && !hasVisibleShadowReceiver)
        return false;

    const math::Vec3 lightPosition = worldTransform.matrix.TransformPoint(math::Vec3(0.f, 0.f, 0.f));
    const float shadowRange = std::max(request.settings.maxDistance, 0.1f);
    const float margin = std::max(1.0f, shadowRange * 0.05f);
    if (request.lightType == LightType::Spot)
    {
        const math::Vec3 forward = TransformForward(worldTransform);
        const float coneTan = std::tan(std::max(0.1f, light.spotOuterDeg) * math::DEG_TO_RAD);
        const collision::Sphere apexSphere{lightPosition, margin};
        if (viewFrustum.valid
                ? SphereIntersectsViewFrustum(viewFrustum, apexSphere)
                : collision::SceneQueries::Intersects(apexSphere, receiverBoundsWorld))
        {
            return true;
        }
        for (float t : {0.2f, 0.4f, 0.6f, 0.8f, 1.0f})
        {
            const float distance = shadowRange * t;
            const math::Vec3 center = lightPosition + forward * distance;
            const float radius = distance * coneTan + margin;
            const collision::Sphere sampleSphere{center, radius};
            if (viewFrustum.valid
                    ? SphereIntersectsViewFrustum(viewFrustum, sampleSphere)
                    : collision::SceneQueries::Intersects(sampleSphere, receiverBoundsWorld))
            {
                return true;
            }
        }
        return false;
    }

    const collision::Sphere influenceSphere{ lightPosition, shadowRange + margin };
    return viewFrustum.valid
        ? SphereIntersectsViewFrustum(viewFrustum, influenceSphere)
        : collision::SceneQueries::Intersects(influenceSphere, receiverBoundsWorld);
}

} // namespace

void ExtractShadow(const renderer::SceneExtractionContext& context,
                   ShadowExtractionConfig config,
                   ShadowExtractionTemporalState* temporalState)
{
    DecayUnusedRetentions(temporalState);
    if (config.maxShadowLights == 0u || config.maxShadowViews == 0u)
    {
        renderer::RenderExtractionView extraction = context.extractionView;
        ShadowFrameData& shadowData =
            extraction.GetOrCreateFrameData<ShadowFrameData>("shadow.frame_data");
        shadowData.Reset();
        extraction.SetActiveShadowResolution(0u);
        return;
    }

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
    float shadowDistance = 0.0f;
    for (const ShadowRequest& req : shadowData.requests)
    {
        if (SupportsCurrentRenderPathCandidate(req))
            shadowDistance = std::max(shadowDistance, std::max(req.settings.maxDistance, 0.1f));
    }
    if (shadowDistance <= 0.0f)
        shadowDistance = 100.0f;

    const collision::AABB receiverBoundsWorld = ComputePrimaryCameraFrustumBounds(world, context.view, shadowDistance);
    const ViewFrustumCull viewFrustum = BuildViewFrustumCull(context.view, shadowDistance);
    const bool hasVisibleShadowReceiver = HasShadowReceiverInViewFrustum(
        world,
        viewFrustum,
        context.view ? context.view->visibilityLayerMask : 0xFFFFFFFFu);
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
        const bool affectsVisibleReceiver =
            !context.view ||
            LocalShadowRequestCanAffectVisibleReceiver(
                world,
                req,
                viewFrustum,
                context.view ? context.view->visibilityLayerMask : 0xFFFFFFFFu,
                *lightComponent,
                *worldTransform);
        const bool affectsReceivers =
            !context.view ||
            (affectsVisibleReceiver &&
             ShadowRequestCanAffectReceivers(req, receiverBoundsWorld, viewFrustum, hasVisibleShadowReceiver, *lightComponent, *worldTransform));
        const uint32_t existingRetention = GetRetentionFrames(temporalState, req.lightEntity);
        const bool allowRetention = !viewFrustum.valid || affectsVisibleReceiver || existingRetention > 0u;
        const uint32_t retainedFrames = allowRetention ? existingRetention : 0u;
        if (!affectsReceivers && retainedFrames == 0u)
        {
            ++shadowData.culledReceiverCount;
            continue;
        }
        const collision::AABB requestCasterBoundsWorld =
            ComputeRequestCasterBounds(world, req, receiverBoundsWorld, casterBoundsWorld, *worldTransform);
        req.casterBoundsWorld = requestCasterBoundsWorld;
        ShadowViewBuilder::BuildViews(*lightComponent, *worldTransform, requestCasterBoundsWorld, req);
        const bool shadowViewsIntersectCamera = ShadowViewsIntersectCameraFrustum(req, viewFrustum);
        if (!shadowViewsIntersectCamera && retainedFrames == 0u)
        {
            req.views.clear();
            ++shadowData.culledReceiverCount;
            continue;
        }
        if (allowRetention && shadowViewsIntersectCamera && req.lightType != LightType::Directional)
            SetRetentionFrames(temporalState, req.lightEntity, config.hysteresisFrames);
    }

    shadowData.ClearCurrentRenderPathRequests();
    uint32_t selectedShadowLights = 0u;
    uint32_t selectedShadowViews = 0u;
    for (size_t i = 0; i < shadowData.requests.size(); ++i)
    {
        if (!SupportsCurrentRenderPath(shadowData.requests[i]))
            continue;

        const uint32_t requestViewCount = static_cast<uint32_t>(
            std::min<size_t>(shadowData.requests[i].views.size(), UINT32_MAX));
        if (selectedShadowLights >= config.maxShadowLights ||
            requestViewCount > config.maxShadowViews - selectedShadowViews)
        {
            ++shadowData.culledBudgetCount;
            continue;
        }

        shadowData.AddCurrentRenderPathRequest(i, shadowData.requests[i].visibleLightIndex);
        ++selectedShadowLights;
        selectedShadowViews += requestViewCount;
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
        if (temporalState)
        {
            const uint32_t currentViewCount = static_cast<uint32_t>(
                std::min<size_t>(totalViewCount, UINT32_MAX));
            temporalState->stableAtlasViewCount = std::max(
                temporalState->stableAtlasViewCount,
                currentViewCount);
            totalViewCount = std::max<size_t>(totalViewCount,
                                              temporalState->stableAtlasViewCount);
        }
        shadowData.atlasViewCountHint = static_cast<uint32_t>(
            std::min<size_t>(totalViewCount, UINT32_MAX));
        activeShadowResolution = ComputeShadowAtlasResolution(
            maxTileResolution,
            totalViewCount,
            config.maxShadowAtlasResolution);
    }
    else if (temporalState)
    {
        temporalState->stableAtlasViewCount = 0u;
    }
    extraction.SetActiveShadowResolution(activeShadowResolution);
}

} // namespace engine::addons::shadow
