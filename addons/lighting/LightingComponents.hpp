#pragma once

#include "addons/lighting/ShadowTypes.hpp"
#include "core/Math.hpp"
#include "ecs/ComponentMeta.hpp"
#include <cstdint>

namespace engine {

enum class LightType : uint8_t { Directional = 0, Point = 1, Spot = 2 };

struct LightComponent
{
    LightType      type           = LightType::Point;
    math::Vec3     color          { 1.f, 1.f, 1.f };
    float          intensity      = 1.f;
    float          range          = 10.f;
    float          spotInnerDeg   = 15.f;
    float          spotOuterDeg   = 30.f;
    bool           castShadows    = false;
    uint32_t       layerMask      = 0xFFFFFFFFu;
    ShadowSettings shadowSettings;
};

inline void RegisterLightingComponents(ecs::ComponentMetaRegistry& registry)
{
    using namespace ecs;
    RegisterComponent<LightComponent>(registry, "LightComponent");
}

} // namespace engine
