#pragma once

#include "addons/camera/CameraComponents.hpp"
#include "ecs/Components.hpp"
#include "ecs/World.hpp"
#include "renderer/RenderFrameTypes.hpp"

namespace engine::addons::camera {

struct CameraBuildOptions
{
    math::Vec3 ambientColor{ 0.03f, 0.03f, 0.03f };
    float      ambientIntensity = 1.f;
};

[[nodiscard]] EntityID FindPrimaryCameraEntity(const ecs::World& world) noexcept;

[[nodiscard]] bool BuildRenderViewFromCamera(const ecs::World& world,
                                             EntityID cameraEntity,
                                             uint32_t viewportWidth,
                                             uint32_t viewportHeight,
                                             renderer::RenderView& outView,
                                             const CameraBuildOptions& options = {}) noexcept;

[[nodiscard]] bool BuildPrimaryRenderView(const ecs::World& world,
                                          uint32_t viewportWidth,
                                          uint32_t viewportHeight,
                                          renderer::RenderView& outView,
                                          const CameraBuildOptions& options = {}) noexcept;

} // namespace engine::addons::camera
