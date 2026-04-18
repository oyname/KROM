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

[[nodiscard]] math::Vec3 ExtractCameraPosition(const math::Mat4& worldMatrix) noexcept
{
    return {
        worldMatrix.m[3][0],
        worldMatrix.m[3][1],
        worldMatrix.m[3][2]
    };
}

[[nodiscard]] math::Vec3 ExtractCameraForward(const math::Mat4& worldMatrix) noexcept
{
    math::Vec3 forward = worldMatrix.TransformDirection({ 0.f, 0.f, -1.f }).Normalized();
    if (forward.LengthSq() < 1e-8f)
        forward = { 0.f, 0.f, -1.f };
    return forward;
}

[[nodiscard]] bool TryBuildEffectiveWorldMatrix(const ecs::World& world,
                                                EntityID entity,
                                                math::Mat4& outWorldMatrix,
                                                uint32_t depth = 0u) noexcept
{
    static constexpr uint32_t kMaxHierarchyDepth = 1024u;
    if (!entity.IsValid() || !world.IsAlive(entity) || depth >= kMaxHierarchyDepth)
        return false;

    const auto* localTransform = world.Get<TransformComponent>(entity);
    if (localTransform == nullptr)
    {
        const auto* worldTransform = world.Get<WorldTransformComponent>(entity);
        if (worldTransform == nullptr)
            return false;

        outWorldMatrix = worldTransform->matrix;
        return true;
    }

    const math::Mat4 localMatrix = math::Mat4::TRS(localTransform->localPosition,
                                                   localTransform->localRotation,
                                                   localTransform->localScale);

    const auto* parent = world.Get<ParentComponent>(entity);
    if (parent == nullptr || !parent->parent.IsValid() || !world.IsAlive(parent->parent))
    {
        outWorldMatrix = localMatrix;
        return true;
    }

    math::Mat4 parentWorld = math::Mat4::Identity();
    if (!TryBuildEffectiveWorldMatrix(world, parent->parent, parentWorld, depth + 1u))
        return false;

    outWorldMatrix = parentWorld * localMatrix;
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

    math::Mat4 cameraWorldMatrix = worldTransform->matrix;
    if (!TryBuildEffectiveWorldMatrix(world, cameraEntity, cameraWorldMatrix))
        return false;

    outView = renderer::RenderView{};
    outView.view = cameraWorldMatrix.InverseAffine();
    outView.cameraPosition = ExtractCameraPosition(cameraWorldMatrix);
    outView.cameraForward = ExtractCameraForward(cameraWorldMatrix);
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
