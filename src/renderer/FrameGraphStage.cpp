#include "renderer/FrameGraphStage.hpp"
#include "core/Debug.hpp"

namespace engine::renderer {

bool FrameGraphStage::Execute(const FrameGraphStageContext& context,
                              FrameGraphStageResult& result) const
{
    if (!context.activePipeline)
    {
        Debug::LogError("FrameGraphStage: no active render pipeline registered");
        return false;
    }

    RenderPipelineBuildContext pipelineContext{
        result.renderGraph,
        context.viewportWidth,
        context.viewportHeight,
        context.backbufferRT,
        context.backbufferTex,
        context.renderQueue,
        context.shaderRuntime,
        context.materials,
        context.perFrameCB,
        context.defaultTonemapMaterial,
        context.tonemapMaterialSystem,
        context.eventBus,
        context.callbacks
    };
    RenderPipelineBuildResult pipelineResult{};
    if (!context.activePipeline->Build(pipelineContext, pipelineResult))
    {
        Debug::LogError("FrameGraphStage: render pipeline build failed: %s",
                        std::string(context.activePipeline->GetName()).c_str());
        return false;
    }

    context.gpuRuntime.AllocateTransientTargets(result.renderGraph);
    if (!result.renderGraph.Compile(result.compiledFrame))
        return false;

    return true;
}

} // namespace engine::renderer
