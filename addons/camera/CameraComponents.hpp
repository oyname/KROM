#pragma once

#include "ecs/ComponentMeta.hpp"
#include <cstdint>

namespace engine {

enum class ProjectionType : uint8_t { Perspective = 0, Orthographic = 1 };

struct CameraComponent
{
    ProjectionType projection   = ProjectionType::Perspective;
    float          fovYDeg      = 60.f;
    float          nearPlane    = 0.1f;
    float          farPlane     = 1000.f;
    float          orthoSize    = 10.f;
    float          aspectRatio  = 16.f / 9.f;
    uint32_t       renderLayer  = 0u;
    bool           isMainCamera = false;
};

inline void RegisterCameraComponents()
{
    using namespace ecs;
    RegisterComponent<CameraComponent>("CameraComponent");
}

} // namespace engine
