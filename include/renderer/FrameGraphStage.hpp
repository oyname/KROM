#pragma once

#include "events/EventBus.hpp"
#include "renderer/GpuResourceRuntime.hpp"
#include "renderer/FeatureRegistry.hpp"
#include "renderer/RenderFrameTypes.hpp"
#include "rendergraph/CompiledFrame.hpp"

namespace engine::renderer {

struct FrameGraphStageContext
{
    uint32_t viewportWidth = 0u;
    uint32_t viewportHeight = 0u;
    RenderTargetHandle backbufferRT;
    TextureHandle backbufferTex;
    const RenderQueue& renderQueue;
    const IRenderPipeline* activePipeline = nullptr;
    ShaderRuntime& shaderRuntime;
    const MaterialSystem& materials;
    const rendergraph::FramePipelineCallbacks& callbacks;
    events::EventBus* eventBus = nullptr;
    GpuResourceRuntime& gpuRuntime;
    BufferHandle perFrameCB;
    MaterialHandle defaultTonemapMaterial;
    const MaterialSystem* tonemapMaterialSystem = nullptr;
};


class FrameGraphStage
{
public:
    [[nodiscard]] bool Execute(const FrameGraphStageContext& context,
                               FrameGraphStageResult& result) const;
};

} // namespace engine::renderer
