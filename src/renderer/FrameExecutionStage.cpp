#include "renderer/FrameExecutionStage.hpp"

namespace engine::renderer {

bool FrameExecutionStage::Execute(const FrameExecutionStageContext& context,
                                       FrameExecutionStageResult& result) const
{
    context.commandList.Begin();
    if (context.perFrameCB.IsValid())
    {
        context.commandList.SetConstantBuffer(CBSlots::PerFrame,
                                              context.perFrameCB,
                                              ShaderStageMask::Vertex | ShaderStageMask::Fragment);
    }

    rendergraph::RGExecContext execContext;
    execContext.device = &context.device;
    execContext.cmd = &context.commandList;
    context.renderGraph.Execute(context.compiledFrame, execContext);
    context.commandList.End();
    context.commandList.Submit();

    result.submittedFenceValue = context.nextFenceValue;
    if (context.frameFence)
        context.frameFence->Signal(result.submittedFenceValue);

    context.gpuRuntime.ReleaseTransientTargets(context.compiledFrame, result.submittedFenceValue);
    context.gpuRuntime.EndFrame(result.submittedFenceValue);
    context.device.EndFrame();
    context.swapchain.Present(true);

    result.stats.frameIndex = context.timing.GetFrameCount();
    result.stats.totalProxyCount = context.renderWorld.TotalProxyCount();
    result.stats.visibleProxyCount = context.renderWorld.VisibleCount();
    result.stats.opaqueDraws = static_cast<uint32_t>(context.renderWorld.GetQueue().opaque.Size());
    result.stats.transparentDraws = static_cast<uint32_t>(context.renderWorld.GetQueue().transparent.Size());
    result.stats.shadowDraws = static_cast<uint32_t>(context.renderWorld.GetQueue().shadow.Size());
    result.stats.particleDraws = static_cast<uint32_t>(context.renderWorld.GetQueue().particles.Size());
    result.stats.backendDrawCalls = context.device.GetDrawCallCount();
    result.stats.graphPassCount = static_cast<uint32_t>(context.compiledFrame.passes.size());
    result.stats.graphTransitionCount = context.compiledFrame.barrierStats.finalTransitions;
    result.stats.pooledTransientTargets = context.gpuRuntime.GetStats().pooledTransientTargets;
    result.stats.uploadedBytes = context.gpuRuntime.GetStats().uploadedBytesThisFrame;

    return true;
}

} // namespace engine::renderer
