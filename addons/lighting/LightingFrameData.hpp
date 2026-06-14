#pragma once

#include "renderer/RenderFeatureInterfaces.hpp"
#include "renderer/RendererTypes.hpp"
#include <cstdint>
#include <vector>

namespace engine::renderer {
struct RenderFrameDataView;
}

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
    struct GpuFrameResources
    {
        BufferHandle lightBuffer = BufferHandle::Invalid();
        uint32_t lightCount = 0u;
        uint32_t lightCapacity = 0u;
    };

    std::vector<ExtractedLight> lights;
    GpuFrameResources gpu;
    uint32_t extractedCount = 0u;
    uint32_t packedCount = 0u;
    uint32_t droppedCount = 0u;
    uint32_t shadowCastingCount = 0u;
};

struct alignas(16) GpuLightData
{
    float positionWS[4];
    float directionWS[4];
    float colorIntensity[4];
    float params[4];
};
static_assert(sizeof(GpuLightData) == 64u, "GpuLightData must be exactly 64 bytes");

// V1: 7 Lichter um 64 Byte für Shadow-VP-Matrix freizuhalten.
// Shadow-VP liegt bei kShadowVPOffset innerhalb von featurePayload.
static constexpr uint32_t kMaxLightsPerFrame    = 7u;
static constexpr uint32_t kLightingPayloadBytes = sizeof(GpuLightData) * kMaxLightsPerFrame; // 448
static constexpr uint32_t kShadowVPOffset       = kLightingPayloadBytes;                    // 448
static constexpr uint32_t kShadowVPBytes        = 64u; // float[16] = 1x mat4
static_assert(kLightingPayloadBytes + kShadowVPBytes == 512u,
              "Lighting + Shadow payload muss exakt featurePayload füllen");

[[nodiscard]] size_t GetExtractedLightCount(renderer::RenderFrameDataView frameData) noexcept;
[[nodiscard]] uint32_t GetPackedLightCount(renderer::RenderFrameDataView frameData) noexcept;
[[nodiscard]] uint32_t GetDroppedLightCount(renderer::RenderFrameDataView frameData) noexcept;
[[nodiscard]] uint32_t GetShadowCastingLightCount(renderer::RenderFrameDataView frameData) noexcept;
[[nodiscard]] BufferHandle GetLightBuffer(renderer::RenderFrameDataView frameData) noexcept;
[[nodiscard]] uint32_t GetLightBufferCount(renderer::RenderFrameDataView frameData) noexcept;
[[nodiscard]] renderer::FrameConstantsContributorPtr CreateLightingFrameConstantsContributor();

} // namespace engine::addons::lighting
