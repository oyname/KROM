#include "addons/shadow/ShadowViewBuilder.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>

namespace engine::addons::shadow {
namespace {

[[nodiscard]] math::Vec3 TransformOrigin(const WorldTransformComponent& worldTransform) noexcept
{
    return worldTransform.matrix.TransformPoint(math::Vec3::Zero());
}

[[nodiscard]] math::Vec3 TransformForward(const WorldTransformComponent& worldTransform) noexcept
{
    return worldTransform.matrix.TransformDirection(math::Vec3::Forward()).Normalized();
}

[[nodiscard]] collision::AABB SanitizeBounds(const collision::AABB& bounds) noexcept
{
    collision::AABB sanitized = bounds;
    if (sanitized.min.x > sanitized.max.x) std::swap(sanitized.min.x, sanitized.max.x);
    if (sanitized.min.y > sanitized.max.y) std::swap(sanitized.min.y, sanitized.max.y);
    if (sanitized.min.z > sanitized.max.z) std::swap(sanitized.min.z, sanitized.max.z);
    return sanitized;
}

[[nodiscard]] ShadowViewport MakeViewport(uint32_t resolution) noexcept
{
    const float size = static_cast<float>(std::max(1u, resolution));
    return ShadowViewport{0.f, 0.f, size, size};
}

[[nodiscard]] ShadowScissorRect MakeScissor(uint32_t resolution) noexcept
{
    const uint32_t size = std::max(1u, resolution);
    return ShadowScissorRect{0, 0, size, size};
}

void FinalizeView(ShadowView& view, uint32_t resolution) noexcept
{
    view.viewProj = view.proj * view.view;
    view.viewport = MakeViewport(resolution);
    view.scissor = MakeScissor(resolution);
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

} // namespace

void ShadowViewBuilder::BuildViews(const LightComponent& light,
                                   const WorldTransformComponent& worldTransform,
                                   const collision::AABB& casterBoundsWorld,
                                   ShadowRequest& outRequest)
{
    outRequest.views.clear();
    switch (outRequest.technique)
    {
    case ShadowTechnique::ShadowMap2D:
        if (light.type == LightType::Directional)
            BuildDirectional(light, worldTransform, casterBoundsWorld, outRequest);
        else
            BuildSpot(light, worldTransform, outRequest);
        break;
    case ShadowTechnique::ShadowMapCube:
        BuildPoint(light, worldTransform, outRequest);
        break;
    case ShadowTechnique::CascadedShadowMap:
        BuildDirectional(light, worldTransform, casterBoundsWorld, outRequest);
        break;
    case ShadowTechnique::None:
    default:
        break;
    }

    outRequest.allocation.desc = BuildShadowMapDesc(outRequest);
}

void ShadowViewBuilder::BuildDirectional(const LightComponent& light,
                                         const WorldTransformComponent& worldTransform,
                                         const collision::AABB& casterBoundsWorld,
                                         ShadowRequest& outRequest)
{
    (void)light;
    collision::AABB fitBounds = outRequest.receiverBoundsWorld;
    if (fitBounds.min.x > fitBounds.max.x ||
        fitBounds.min.y > fitBounds.max.y ||
        fitBounds.min.z > fitBounds.max.z)
    {
        fitBounds = casterBoundsWorld;
    }

    // Make sure nearby casters are not completely excluded even if the camera
    // fit is smaller than the overall scene bounds.
    fitBounds.min.x = std::min(fitBounds.min.x, casterBoundsWorld.min.x);
    fitBounds.min.y = std::min(fitBounds.min.y, casterBoundsWorld.min.y);
    fitBounds.min.z = std::min(fitBounds.min.z, casterBoundsWorld.min.z);
    fitBounds.max.x = std::max(fitBounds.max.x, casterBoundsWorld.max.x);
    fitBounds.max.y = std::max(fitBounds.max.y, casterBoundsWorld.max.y);
    fitBounds.max.z = std::max(fitBounds.max.z, casterBoundsWorld.max.z);

    const collision::AABB bounds = SanitizeBounds(fitBounds);
    const math::Vec3 sceneCenter = (bounds.min + bounds.max) * 0.5f;
    const math::Vec3 sceneExtents = (bounds.max - bounds.min) * 0.5f;
    const float sceneRadius = std::max(sceneExtents.Length(), 1.0f);

    const math::Vec3 dir = TransformForward(worldTransform);
    const math::Vec3 up = (std::fabs(dir.y) < 0.999f) ? math::Vec3::Up() : math::Vec3::Right();
    const float eyeDistance = std::max(sceneRadius * 2.5f, 10.0f);
    const math::Vec3 eye = sceneCenter - dir * eyeDistance;

    ShadowView view{};
    view.view = math::Mat4::LookAtRH(eye, sceneCenter, up);

    std::array<math::Vec3, 8> corners{};
    BuildAABBCorners(bounds, corners);

    math::Vec3 minView(std::numeric_limits<float>::max());
    math::Vec3 maxView(-std::numeric_limits<float>::max());
    for (const math::Vec3& corner : corners)
    {
        const math::Vec3 cornerView = view.view.TransformPoint(corner);
        minView.x = std::min(minView.x, cornerView.x);
        minView.y = std::min(minView.y, cornerView.y);
        minView.z = std::min(minView.z, cornerView.z);
        maxView.x = std::max(maxView.x, cornerView.x);
        maxView.y = std::max(maxView.y, cornerView.y);
        maxView.z = std::max(maxView.z, cornerView.z);
    }

    const float extentX = std::max(0.5f, (maxView.x - minView.x) * 0.5f);
    const float extentY = std::max(0.5f, (maxView.y - minView.y) * 0.5f);
    const float centerX = (minView.x + maxView.x) * 0.5f;
    const float centerY = (minView.y + maxView.y) * 0.5f;

    constexpr float kXYMargin = 0.5f;
    constexpr float kZMargin = 2.0f;
    const uint32_t resolution = std::max(1u, outRequest.settings.resolution);
    const float worldUnitsPerTexelX = ((extentX * 2.0f) + (kXYMargin * 2.0f)) / static_cast<float>(resolution);
    const float worldUnitsPerTexelY = ((extentY * 2.0f) + (kXYMargin * 2.0f)) / static_cast<float>(resolution);
    const float snappedCenterX = std::round(centerX / worldUnitsPerTexelX) * worldUnitsPerTexelX;
    const float snappedCenterY = std::round(centerY / worldUnitsPerTexelY) * worldUnitsPerTexelY;

    const float left = snappedCenterX - extentX - kXYMargin;
    const float right = snappedCenterX + extentX + kXYMargin;
    const float bottom = snappedCenterY - extentY - kXYMargin;
    const float top = snappedCenterY + extentY + kXYMargin;
    view.nearPlane = std::max(0.1f, -std::max(minView.z, maxView.z) - kZMargin);
    view.farPlane = std::max(view.nearPlane + 1.0f, -std::min(minView.z, maxView.z) + kZMargin);
    view.proj = math::Mat4::OrthoRH(left, right, bottom, top, view.nearPlane, view.farPlane);
    FinalizeView(view, resolution);
    outRequest.views.push_back(view);
}

void ShadowViewBuilder::BuildSpot(const LightComponent& light,
                                  const WorldTransformComponent& worldTransform,
                                  ShadowRequest& outRequest)
{
    const math::Vec3 position = TransformOrigin(worldTransform);
    const math::Vec3 direction = TransformForward(worldTransform);
    const math::Vec3 up = (std::fabs(direction.y) < 0.999f) ? math::Vec3::Up() : math::Vec3::Right();

    ShadowView view{};
    view.view = math::Mat4::LookAtRH(position, position + direction, up);
    view.nearPlane = 0.1f;
    view.farPlane = std::max(light.range, 1.0f);
    const float fovRadians = std::max(light.spotOuterDeg * 2.0f * math::DEG_TO_RAD, 0.1f);
    view.proj = math::Mat4::PerspectiveFovRH(fovRadians, 1.0f, view.nearPlane, view.farPlane);
    FinalizeView(view, std::max(1u, outRequest.settings.resolution));
    outRequest.views.push_back(view);
}

void ShadowViewBuilder::BuildPoint(const LightComponent& light,
                                   const WorldTransformComponent& worldTransform,
                                   ShadowRequest& outRequest)
{
    const math::Vec3 position = TransformOrigin(worldTransform);
    const float nearPlane = 0.1f;
    const float farPlane = std::max(light.range, 1.0f);
    const math::Mat4 proj = math::Mat4::PerspectiveFovRH(math::HALF_PI, 1.0f, nearPlane, farPlane);
    const uint32_t resolution = std::max(1u, outRequest.settings.resolution);

    static const std::array<math::Vec3, 6> kDirections = {
        math::Vec3{ 1.f,  0.f,  0.f},
        math::Vec3{-1.f,  0.f,  0.f},
        math::Vec3{ 0.f,  1.f,  0.f},
        math::Vec3{ 0.f, -1.f,  0.f},
        math::Vec3{ 0.f,  0.f,  1.f},
        math::Vec3{ 0.f,  0.f, -1.f},
    };
    static const std::array<math::Vec3, 6> kUpVectors = {
        math::Vec3{0.f, -1.f,  0.f},
        math::Vec3{0.f, -1.f,  0.f},
        math::Vec3{0.f,  0.f,  1.f},
        math::Vec3{0.f,  0.f, -1.f},
        math::Vec3{0.f, -1.f,  0.f},
        math::Vec3{0.f, -1.f,  0.f},
    };

    outRequest.views.reserve(6u);
    for (uint32_t faceIndex = 0u; faceIndex < 6u; ++faceIndex)
    {
        ShadowView view{};
        view.faceIndex = faceIndex;
        view.arrayLayer = faceIndex;
        view.nearPlane = nearPlane;
        view.farPlane = farPlane;
        view.view = math::Mat4::LookAtRH(position, position + kDirections[faceIndex], kUpVectors[faceIndex]);
        view.proj = proj;
        FinalizeView(view, resolution);
        outRequest.views.push_back(view);
    }
}

} // namespace engine::addons::shadow
