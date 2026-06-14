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

[[nodiscard]] math::Vec3 ExtractCameraForward(const math::Mat4& worldMatrix) noexcept
{
    math::Vec3 forward = worldMatrix.TransformDirection({ 0.f, 0.f, -1.f }).Normalized();
    if (forward.LengthSq() < 1e-8f)
        forward = { 0.f, 0.f, -1.f };
    return forward;
}

struct ResolvedTransform
{
    math::Vec3 position{0.f, 0.f, 0.f};
    math::Quat rotation = math::Quat::Identity();
    math::Vec3 scale{1.f, 1.f, 1.f};
};

[[nodiscard]] bool ResolveLiveWorldTransform(const ecs::World& world,
                                             EntityID entity,
                                             ResolvedTransform& out,
                                             uint32_t depth = 0u) noexcept
{
    if (!world.IsAlive(entity) || depth > 1024u)
        return false;

    const auto* local = world.Get<TransformComponent>(entity);
    if (!local)
    {
        if (const auto* wt = world.Get<WorldTransformComponent>(entity))
        {
            out.position = wt->position;
            out.rotation = wt->rotation;
            out.scale = wt->scale;
            return true;
        }
        return false;
    }

    ResolvedTransform parent{};
    const auto* parentComponent = world.Get<ParentComponent>(entity);
    const bool hasParent =
        parentComponent &&
        parentComponent->parent.IsValid() &&
        ResolveLiveWorldTransform(world, parentComponent->parent, parent, depth + 1u);

    if (hasParent)
    {
        const math::Vec3 localOffset = local->inheritParentScale
            ? math::Vec3{
                parent.scale.x * local->localPosition.x,
                parent.scale.y * local->localPosition.y,
                parent.scale.z * local->localPosition.z
              }
            : local->localPosition;
        out.position = parent.position + parent.rotation.Rotate(localOffset);
        out.rotation = (parent.rotation * local->localRotation).Normalized();
        out.scale = local->inheritParentScale
            ? math::Vec3{
                parent.scale.x * local->localScale.x,
                parent.scale.y * local->localScale.y,
                parent.scale.z * local->localScale.z
              }
            : local->localScale;
    }
    else
    {
        out.position = local->localPosition;
        out.rotation = local->localRotation;
        out.scale = local->localScale;
    }
    return true;
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

    ResolvedTransform liveTransform{};
    if (!ResolveLiveWorldTransform(world, cameraEntity, liveTransform))
    {
        liveTransform.position = worldTransform->position;
        liveTransform.rotation = worldTransform->rotation;
        liveTransform.scale = worldTransform->scale;
    }

    const math::Vec3 cameraPosition = liveTransform.position;
    const math::Quat cameraRotation = liveTransform.rotation.Normalized();
    const math::Mat4 cameraWorldMatrix = math::Mat4::TRS(cameraPosition,
                                                         cameraRotation,
                                                         {1.f, 1.f, 1.f});

    outView = renderer::RenderView{};
    outView.view = cameraWorldMatrix.InverseAffine();
    outView.cameraPosition = cameraPosition;
    outView.cameraForward = ExtractCameraForward(cameraWorldMatrix);
    outView.ambientColor = options.ambientColor;
    outView.ambientIntensity = options.ambientIntensity;
    outView.projection = BuildProjectionMatrix(*camera, viewportWidth, viewportHeight,
                                               outView.nearPlane, outView.farPlane);
    outView.visibilityLayerMask         = camera->cullingMask;
    outView.backgroundMode = camera->backgroundMode;
    outView.clearColor     = camera->clearColor;
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
