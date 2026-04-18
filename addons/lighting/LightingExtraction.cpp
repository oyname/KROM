#include "addons/lighting/LightingExtraction.hpp"

#include "addons/lighting/LightingComponents.hpp"
#include <cmath>

namespace engine::addons::lighting {
namespace {

[[nodiscard]] bool IsEntityActive(const ecs::World& world, EntityID entity) noexcept
{
    const auto* active = world.Get<ActiveComponent>(entity);
    return active == nullptr || active->active;
}

} // namespace

void ExtractLights(const ecs::World& world, renderer::RenderWorld& renderWorld)
{
    LightingFrameData& lighting =
        renderWorld.GetOrCreateFeatureData<LightingFrameData>("lighting.frame_data");
    lighting.lights.clear();

    world.View<WorldTransformComponent, LightComponent>(
        [&](EntityID id,
            const WorldTransformComponent& wt,
            const LightComponent& lc)
        {
            if (!IsEntityActive(world, id))
                return;

            ExtractedLight light{};
            light.entity = id;
            switch (lc.type)
            {
            case LightType::Directional: light.type = ExtractedLightType::Directional; break;
            case LightType::Point:       light.type = ExtractedLightType::Point; break;
            case LightType::Spot:        light.type = ExtractedLightType::Spot; break;
            }

            light.positionWorld = wt.matrix.TransformPoint(math::Vec3(0.f, 0.f, 0.f));
            light.directionWorld = wt.matrix.TransformDirection(math::Vec3(0.f, 0.f, -1.f)).Normalized();
            light.color = lc.color;
            light.intensity = lc.intensity;
            light.range = lc.range;
            light.spotInner = std::cos(lc.spotInnerDeg * math::DEG_TO_RAD);
            light.spotOuter = std::cos(lc.spotOuterDeg * math::DEG_TO_RAD);
            light.castShadows = lc.castShadows;
            lighting.lights.push_back(light);
        });
}

} // namespace engine::addons::lighting
