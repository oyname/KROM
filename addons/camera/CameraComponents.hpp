#pragma once

#include "ecs/ComponentMeta.hpp"
#include "renderer/BackgroundMode.hpp"
#include "renderer/RenderLayers.hpp"
#include <array>
#include <cstdint>

namespace engine {

enum class ProjectionType  : uint8_t { Perspective = 0, Orthographic = 1 };

struct CameraComponent
{
    ProjectionType       projection        = ProjectionType::Perspective;
    float                fovYDeg           = 60.f;
    float                nearPlane         = 0.1f;
    float                farPlane          = 1000.f;
    float                orthoSize         = 10.f;
    float                aspectRatio       = 16.f / 9.f;
    // Deprecated editor-only object-layer remnant. Cameras do not render
    // themselves; visibility is controlled through cullingMask.
    uint32_t             renderLayer       = 0u;
    uint32_t             cullingMask       = renderer::LAYER_ALL & ~renderer::LAYER_EDITOR_GIZMO & ~renderer::LAYER_EDITOR_GIZMO_DEPTH;
    bool                 isMainCamera      = false;

    BackgroundMode       backgroundMode    = BackgroundMode::ClearColor;
    std::array<float, 4> clearColor        = {0.f, 0.f, 0.f, 1.f};
};

inline void RegisterCameraComponents(ecs::ComponentMetaRegistry& registry)
{
    using namespace ecs;
    RegisterComponent<CameraComponent>(registry, "CameraComponent");
}

} // namespace engine
