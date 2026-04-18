#pragma once

#include "events/EventBus.hpp"
#include "renderer/GpuResourceRuntime.hpp"
#include "renderer/FeatureRegistry.hpp"
#include "renderer/RenderFrameTypes.hpp"
#include "rendergraph/CompiledFrame.hpp"
#include <memory>

namespace engine::renderer {

// FrameGraphRuntimeBindings lebt in FeatureRegistry.hpp (Pipeline-Addon-Vertrag).

struct FrameGraphStageContext
{
    IDevice& device;
    uint32_t viewportWidth  = 0u;
    uint32_t viewportHeight = 0u;
    RenderTargetHandle backbufferRT;
    TextureHandle backbufferTex;
    const RenderQueue& renderQueue;
    const IRenderPipeline* activePipeline = nullptr;
    ShaderRuntime& shaderRuntime;
    const MaterialSystem& materials;
    const FramePipelineCallbacks& callbacks;
    events::EventBus* eventBus             = nullptr;
    GpuResourceRuntime& gpuRuntime;
    BufferHandle perFrameCB;
    BufferHandle perObjectArena;
    uint32_t     perObjectStride           = 0u;
    MaterialHandle defaultTonemapMaterial;
    const MaterialSystem* tonemapMaterialSystem = nullptr;
};

class FrameGraphStage
{
public:
    FrameGraphStage();

    [[nodiscard]] bool Execute(const FrameGraphStageContext& context,
                               FrameGraphStageResult& result);

private:
    struct StructuralCache
    {
        uint64_t structureKey                              = 0ull;
        rendergraph::RenderGraph renderGraph;
        rendergraph::RGResourceID backbufferResource       = rendergraph::RG_INVALID_RESOURCE;
        bool compileValid                                  = false;
    };

    [[nodiscard]] uint64_t ComputeStructureKey(const FrameGraphStageContext& context) const noexcept;
    void UpdateRuntimeBindings(const FrameGraphStageContext& context);

    std::shared_ptr<FrameGraphRuntimeBindings> m_runtimeBindings;
    StructuralCache m_cache;
};

} // namespace engine::renderer
