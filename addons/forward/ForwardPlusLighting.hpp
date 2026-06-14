#pragma once
// =============================================================================
// KROM Engine - addons/forward/ForwardPlusLighting.hpp
// CPU-seitiges Forward+ Tile-Culling: Typen und öffentliche Schnittstelle.
// Die Implementierung (BuildForwardPlusTileList, Buffer-Verwaltung) lebt in
// ForwardPlusLighting.cpp und ist nicht nach außen sichtbar.
// =============================================================================
#include "renderer/RendererTypes.hpp"
#include <cstdint>

namespace engine::renderer {
    class IDevice;
    struct FrameConstants;
    struct RenderFrameDataView;
}

namespace engine::renderer::addons::forward {

constexpr uint32_t kForwardPlusTileSize = 16u;

struct alignas(16) ForwardPlusTileHeaderGpu
{
    uint32_t offset    = 0u;
    uint32_t count     = 0u;
    uint32_t reserved0 = 0u;
    uint32_t reserved1 = 0u;
};
static_assert(sizeof(ForwardPlusTileHeaderGpu) == 16u,
              "ForwardPlus tile header must stay 16 bytes");

struct ForwardPlusTileMetrics
{
    uint32_t tileCount             = 0u;
    uint32_t minLightsPerTile      = 0u;
    uint32_t maxLightsPerTile      = 0u;
    float    avgLightsPerTile      = 0.0f;
    uint32_t totalLightAssignments = 0u;
};

struct ForwardPlusGpuResources
{
    BufferHandle tileHeaderBuffer = BufferHandle::Invalid();
    BufferHandle tileIndexBuffer  = BufferHandle::Invalid();
    BufferHandle computeParamsBuffer = BufferHandle::Invalid();
    // CpuWrite-Staging für per-Frame-Reset der Tile-Header (count/reserved0 → 0).
    // CopyBuffer in den main Command Buffer ersetzt UploadBufferData + vkQueueWaitIdle.
    BufferHandle resetStagingBuffer = BufferHandle::Invalid();
    uint32_t resetStagingCapacity   = 0u;
    ShaderHandle computeShader = ShaderHandle::Invalid();
    PipelineHandle computePipeline = PipelineHandle::Invalid();
    uint32_t tileHeaderCapacity   = 0u;
    uint32_t tileIndexCapacity    = 0u;
    uint32_t tileCountX           = 0u;
    uint32_t tileCountY           = 0u;
    uint32_t maxLightsPerTile     = 0u;
    ForwardPlusTileMetrics metrics{};
    bool initialized              = false;
};

void DestroyForwardPlusGpuResources(IDevice& device, ForwardPlusGpuResources& resources);

// Baut Tile-Listen auf der CPU, lädt Buffer hoch und schreibt Metriken in
// resources.metrics. Gibt false zurück wenn keine LightingFrameData vorhanden.
bool UpdateForwardPlusResources(IDevice& device,
                                RenderFrameDataView frameData,
                                const FrameConstants& frameConstants,
                                uint32_t viewportWidth,
                                uint32_t viewportHeight,
                                ForwardPlusGpuResources& resources);

bool EnsureForwardPlusComputePipeline(IDevice& device,
                                      ForwardPlusGpuResources& resources);

bool PrepareForwardPlusComputeResources(IDevice& device,
                                        RenderFrameDataView frameData,
                                        const FrameConstants& frameConstants,
                                        uint32_t viewportWidth,
                                        uint32_t viewportHeight,
                                        ForwardPlusGpuResources& resources);

} // namespace engine::renderer::addons::forward
