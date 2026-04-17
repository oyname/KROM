#pragma once

#include "renderer/FeatureRegistry.hpp"
#include "renderer/RenderWorld.hpp"
#include <cstdint>
#include <vector>

namespace engine::addons::lighting {

enum class ExtractedLightType : uint8_t
{
    Directional = 0,
    Point = 1,
    Spot = 2,
};

struct ExtractedLight
{
    EntityID entity;
    ExtractedLightType type = ExtractedLightType::Point;
    math::Vec3 positionWorld;
    math::Vec3 directionWorld;
    math::Vec3 color;
    float intensity   = 1.f;
    float range       = 10.f;
    float spotInner   = 0.f;
    float spotOuter   = 0.f;
    bool castShadows  = false;
};

struct LightingFrameData
{
    std::vector<ExtractedLight> lights;
};

struct alignas(16) GpuLightData
{
    float positionWS[4];
    float directionWS[4];
    float colorIntensity[4];
    float params[4];
};
static_assert(sizeof(GpuLightData) == 64u, "GpuLightData must be exactly 64 bytes");

static constexpr uint32_t kMaxLightsPerFrame = 8u;
static constexpr uint32_t kLightingPayloadBytes = sizeof(GpuLightData) * kMaxLightsPerFrame;
static_assert(kLightingPayloadBytes <= renderer::kFrameFeaturePayloadBytes,
              "Lighting payload exceeds FrameConstants feature payload budget");

[[nodiscard]] size_t GetExtractedLightCount(const renderer::RenderWorld& renderWorld) noexcept;
[[nodiscard]] renderer::FrameConstantsContributorPtr CreateLightingFrameConstantsContributor();

} // namespace engine::addons::lighting
