#include "addons/lighting/LightingExtraction.hpp"

#include "addons/lighting/LightingComponents.hpp"
#include <algorithm>
#include <cmath>
#include <tuple>

namespace engine::addons::lighting {
namespace {

[[nodiscard]] bool IsEntityActive(const ecs::World& world, EntityID entity) noexcept
{
    const auto* active = world.Get<ActiveComponent>(entity);
    return active == nullptr || active->active;
}

[[nodiscard]] uint32_t GetLightTypePriority(ExtractedLightType type) noexcept
{
    switch (type)
    {
    case ExtractedLightType::Directional: return 0u;
    case ExtractedLightType::Spot:        return 1u;
    case ExtractedLightType::Point:       return 2u;
    default:                              return 3u;
    }
}

[[nodiscard]] bool PreferLight(const ExtractedLight& a, const ExtractedLight& b) noexcept
{
    const auto keyA = std::make_tuple(
        a.castShadows ? 0u : 1u,
        GetLightTypePriority(a.type),
        -a.intensity,
        -a.range,
        a.entity.value);
    const auto keyB = std::make_tuple(
        b.castShadows ? 0u : 1u,
        GetLightTypePriority(b.type),
        -b.intensity,
        -b.range,
        b.entity.value);
    return keyA < keyB;
}

} // namespace

void ExtractLights(const ecs::World& world, renderer::RenderSceneSnapshot& snapshot)
{
    ExtractLights(world, snapshot.GetWorld());
}

void ExtractLights(const ecs::World& world, renderer::RenderWorld& renderWorld)
{
    LightingFrameData& lighting =
        renderWorld.GetOrCreateFeatureData<LightingFrameData>("lighting.frame_data");
    lighting.lights.clear();
    lighting.extractedCount = 0u;
    lighting.packedCount = 0u;
    lighting.droppedCount = 0u;
    lighting.shadowCastingCount = 0u;

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
            light.castShadows = lc.castShadows && lc.shadowSettings.enabled;
            lighting.lights.push_back(light);
        });

    lighting.extractedCount = static_cast<uint32_t>(lighting.lights.size());
    for (const ExtractedLight& light : lighting.lights)
    {
        if (light.castShadows)
            ++lighting.shadowCastingCount;
    }

    std::stable_sort(lighting.lights.begin(), lighting.lights.end(), PreferLight);
}

} // namespace engine::addons::lighting
