#include "addons/camera/CameraViewBuilder.hpp"

#include <algorithm>
#include <cmath>

namespace engine::addons::camera {
namespace {

[[nodiscard]] bool IsEntityActive(const ecs::World& world, EntityID entity) noexcept
{
    const auto* active = world.Get<ActiveComponent>(entity);
    return active == nullptr || active->active;
}

[[nodiscard]] float ResolveAspectRatio(const CameraComponent& camera,
                                       uint32_t viewportWidth,
                                       uint32_t viewportHeight) noexcept
{
    if (viewportWidth > 0u && viewportHeight > 0u)
        return static_cast<float>(viewportWidth) / static_cast<float>(viewportHeight);

    return camera.aspectRatio > 1e-6f ? camera.aspectRatio : (16.f / 9.f);
}

[[nodiscard]] math::Vec3 ExtractCameraPosition(const WorldTransformComponent& worldTransform) noexcept
{
    return {
        worldTransform.matrix.m[3][0],
        worldTransform.matrix.m[3][1],
        worldTransform.matrix.m[3][2]
    };
}

[[nodiscard]] math::Vec3 ExtractCameraForward(const WorldTransformComponent& worldTransform) noexcept
{
    math::Vec3 forward = worldTransform.matrix.TransformDirection({ 0.f, 0.f, -1.f }).Normalized();
    if (forward.LengthSq() < 1e-8f)
        forward = { 0.f, 0.f, -1.f };
    return forward;
}

[[nodiscard]] math::Mat4 BuildProjectionMatrix(const CameraComponent& camera,
                                               uint32_t viewportWidth,
                                               uint32_t viewportHeight,
                                               float& outNearPlane,
                                               float& outFarPlane) noexcept
{
    outNearPlane = std::max(camera.nearPlane, 1e-4f);
    outFarPlane = std::max(camera.farPlane, outNearPlane + 1e-3f);

    const float aspect = ResolveAspectRatio(camera, viewportWidth, viewportHeight);
    if (camera.projection == ProjectionType::Orthographic)
    {
        const float halfHeight = std::max(camera.orthoSize, 1e-3f);
        const float halfWidth = halfHeight * aspect;
        return math::Mat4::OrthoRH(-halfWidth, halfWidth, -halfHeight, halfHeight,
                                   outNearPlane, outFarPlane);
    }

    const float fovYDeg = std::clamp(camera.fovYDeg, 1.f, 179.f);
    return math::Mat4::PerspectiveFovRH(fovYDeg * math::DEG_TO_RAD, aspect,
                                        outNearPlane, outFarPlane);
}

} // namespace

EntityID FindPrimaryCameraEntity(const ecs::World& world) noexcept
{
    EntityID bestMain = NULL_ENTITY;
    EntityID bestFallback = NULL_ENTITY;

    world.View<CameraComponent, WorldTransformComponent>(
        [&](EntityID entity,
            const CameraComponent& camera,
            const WorldTransformComponent&)
        {
            if (!IsEntityActive(world, entity))
                return;

            if (camera.isMainCamera)
            {
                if (!bestMain.IsValid() || entity.value < bestMain.value)
                    bestMain = entity;
            }
            else if (!bestFallback.IsValid() || entity.value < bestFallback.value)
            {
                bestFallback = entity;
            }
        });

    return bestMain.IsValid() ? bestMain : bestFallback;
}

bool BuildRenderViewFromCamera(const ecs::World& world,
                               EntityID cameraEntity,
                               uint32_t viewportWidth,
                               uint32_t viewportHeight,
                               renderer::RenderView& outView,
                               const CameraBuildOptions& options) noexcept
{
    const auto* camera = world.Get<CameraComponent>(cameraEntity);
    const auto* worldTransform = world.Get<WorldTransformComponent>(cameraEntity);
    if (camera == nullptr || worldTransform == nullptr || !IsEntityActive(world, cameraEntity))
        return false;

    outView = renderer::RenderView{};
    outView.view = worldTransform->matrix.InverseAffine();
    outView.cameraPosition = ExtractCameraPosition(*worldTransform);
    outView.cameraForward = ExtractCameraForward(*worldTransform);
    outView.ambientColor = options.ambientColor;
    outView.ambientIntensity = options.ambientIntensity;
    outView.projection = BuildProjectionMatrix(*camera, viewportWidth, viewportHeight,
                                               outView.nearPlane, outView.farPlane);
    return true;
}

bool BuildPrimaryRenderView(const ecs::World& world,
                            uint32_t viewportWidth,
                            uint32_t viewportHeight,
                            renderer::RenderView& outView,
                            const CameraBuildOptions& options) noexcept
{
    const EntityID cameraEntity = FindPrimaryCameraEntity(world);
    if (!cameraEntity.IsValid())
        return false;

    return BuildRenderViewFromCamera(world, cameraEntity, viewportWidth, viewportHeight,
                                     outView, options);
}

} // namespace engine::addons::camera
