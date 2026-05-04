#include "renderer/RenderFrameOrchestrator.hpp"
#include "renderer/CommandSubmissionPlan.hpp"
#include "core/Debug.hpp"
#include <chrono>

namespace engine::renderer {
namespace {

FrameConstantStageContext MakeFrameConstantContext(const RenderFrameOrchestratorContext& context,
                                                     const RenderSceneSnapshot& snapshot,
                                                     uint32_t viewportWidth,
                                                     uint32_t viewportHeight)
{
    return FrameConstantStageContext{
        &context.device,
        context.device.GetClipSpaceAdjustment(),
        context.device.GetShadowClipSpaceAdjustment(),
        viewportWidth,
        viewportHeight,
        context.view,
        context.timing,
        context.shaderRuntime.GetEnvironmentState(),
        snapshot,
        context.featureRegistry.GetFrameConstantsContributors()
    };
}

FrameShaderStageContext MakeFrameShaderContext(const RenderFrameOrchestratorContext& context)
{
    return FrameShaderStageContext{
        context.materials,
        context.shaderRuntime
    };
}

FrameUploadStageContext MakeFrameUploadContext(const RenderFrameOrchestratorContext& context,
                                               RenderSceneSnapshot& snapshot,
                                               const FrameConstantsResult& frameData)
{
    return FrameUploadStageContext{
        snapshot,
        context.view,
        frameData,
        context.gpuRuntime,
        context.materials,
        context.shaderRuntime,
        context.renderPassRegistry,
        &context.jobSystem
    };
}

} // namespace

bool RenderFrameOrchestrator::Execute(const RenderFrameOrchestratorContext& context,
                                      RenderFrameExecutionState& state)
{
    state.execution.stats = {};

    if (context.eventBus)
    {
        context.eventBus->Publish(
            events::FrameBeginEvent{context.timing.GetDeltaSecondsF(), context.timing.GetFrameCount()});
    }

    context.device.BeginFrame();
    const uint64_t completedFenceValue = context.frameFence ? context.frameFence->GetValue() : 0u;
    context.gpuRuntime.BeginFrame(completedFenceValue, context.frameFence);

    const SwapchainFrameStatus swapchainStatus = context.swapchain.QueryFrameStatus();
    const uint32_t backbufferIndex = swapchainStatus.currentBackbufferIndex;
    const uint32_t viewportWidth = context.swapchain.GetWidth();
    const uint32_t viewportHeight = context.swapchain.GetHeight();
    const RenderTargetHandle backbufferRT = context.swapchain.GetBackbufferRenderTarget(backbufferIndex);
    const TextureHandle backbufferTex = context.swapchain.GetBackbufferTexture(backbufferIndex);
    const bool hasRenderableBackbuffer = swapchainStatus.hasRenderableBackbuffer && (backbufferRT.IsValid() || backbufferTex.IsValid());

    if (swapchainStatus.resizePending || viewportWidth == 0u || viewportHeight == 0u || !hasRenderableBackbuffer)
    {
        context.gpuRuntime.EndFrame(0u);
        context.device.EndFrame();
        if (context.eventBus)
        {
            context.eventBus->Publish(
                events::FrameEndEvent{context.timing.GetDeltaSecondsF(), context.timing.GetFrameCount()});
        }
        return true;
    }

    FrameExtractionStage extractionStage;
    FrameConstantStage frameConstantStage;
    FrameShaderStage frameShaderStage;
    FrameUploadStage frameUploadStage;
    FrameExecutionStage executionStage;

    context.world.BeginReadPhase();
    FrameExtractionStageContext extractionContext{
        context.world,
        context.featureRegistry.GetSceneExtractionSteps(),
        state.extraction.snapshot,
        &context.jobSystem
    };
    const FrameShaderStageContext frameShaderContext = MakeFrameShaderContext(context);
    if (!extractionStage.Execute(extractionContext, state.extraction))
    {
        state.extractionStatus.MarkFailed("Extract failed");
        context.world.EndReadPhase();
        return false;
    }
    state.extractionStatus.MarkSucceeded();
    context.world.EndReadPhase();

    const FrameConstantStageContext frameConstantContext =
        MakeFrameConstantContext(context, state.extraction.snapshot, viewportWidth, viewportHeight);

    // Safe multicore win: both collection steps are read-only over material/shader metadata.
    // PrepareFrameData is snapshot-only and independent from shader/material request collection.
    // World extraction/queue building remain serial because the current ECS read phase only
    // protects structural mutations, not arbitrary component writes.
    using Clock = std::chrono::steady_clock;

    context.jobSystem.ResetPeakActiveWorkers();
    const auto parallelStart = Clock::now();

    auto prepareFrameFuture = context.jobSystem.DispatchReturn([&frameConstantStage, &frameConstantContext, &state]() {
        const auto start = Clock::now();
        const bool ok = frameConstantStage.PrepareFrameData(frameConstantContext, state.frameConstants);
        const auto end = Clock::now();
        return std::pair<jobs::TaskResult, float>{
            ok ? jobs::TaskResult::Ok() : jobs::TaskResult::Fail("PrepareFrame failed"),
            std::chrono::duration<float, std::milli>(end - start).count()
        };
    });
    auto shaderFuture = context.jobSystem.DispatchReturn([&frameShaderStage, &frameShaderContext, &state]() {
        const auto start = Clock::now();
        const bool ok = frameShaderStage.CollectShaderRequests(frameShaderContext, state.shaderPrep);
        const auto end = Clock::now();
        return std::pair<jobs::TaskResult, float>{
            ok ? jobs::TaskResult::Ok() : jobs::TaskResult::Fail("CollectShaderRequests failed"),
            std::chrono::duration<float, std::milli>(end - start).count()
        };
    });
    auto materialFuture = context.jobSystem.DispatchReturn([&frameShaderStage, &frameShaderContext, &state]() {
        const auto start = Clock::now();
        const bool ok = frameShaderStage.CollectMaterialRequests(frameShaderContext, state.shaderPrep);
        const auto end = Clock::now();
        return std::pair<jobs::TaskResult, float>{
            ok ? jobs::TaskResult::Ok() : jobs::TaskResult::Fail("CollectMaterialRequests failed"),
            std::chrono::duration<float, std::milli>(end - start).count()
        };
    });

    const auto prepareFrameValue = prepareFrameFuture.get();
    const auto shaderCollectValue = shaderFuture.get();
    const auto materialCollectValue = materialFuture.get();
    const auto parallelEnd = Clock::now();

    state.execution.stats.prepareFrameMs = prepareFrameValue.value->second;
    state.execution.stats.collectShadersMs = shaderCollectValue.value->second;
    state.execution.stats.collectMaterialsMs = materialCollectValue.value->second;
    state.execution.stats.parallelSectionMs =
        std::chrono::duration<float, std::milli>(parallelEnd - parallelStart).count();
    state.execution.stats.peakActiveWorkers = context.jobSystem.PeakActiveWorkers();

    const jobs::TaskResult prepareFrameResult = prepareFrameValue.value->first;
    if (!prepareFrameResult.Succeeded())
    {
        state.prepareFrameStatus.MarkFailed("PrepareFrame failed");
        return false;
    }
    state.prepareFrameStatus.MarkSucceeded();

    const jobs::TaskResult shaderCollectResult = shaderCollectValue.value->first;
    if (!shaderCollectResult.Succeeded())
    {
        state.collectShadersStatus.MarkFailed("CollectShaderRequests failed");
        return false;
    }
    state.collectShadersStatus.MarkSucceeded();

    const jobs::TaskResult materialCollectResult = materialCollectValue.value->first;
    if (!materialCollectResult.Succeeded())
    {
        state.collectMaterialsStatus.MarkFailed("CollectMaterialRequests failed");
        return false;
    }
    state.collectMaterialsStatus.MarkSucceeded();

    const FrameUploadStageContext frameUploadContext =
        MakeFrameUploadContext(context, state.extraction.snapshot, state.frameConstants);

    const auto collectUploadsStart = Clock::now();
    if (!frameUploadStage.BuildRenderQueues(frameUploadContext))
    {
        state.buildQueuesStatus.MarkFailed("BuildQueues failed");
        return false;
    }
    state.buildQueuesStatus.MarkSucceeded();

    if (!frameUploadStage.CollectUploadRequests(frameUploadContext, state.upload))
    {
        state.collectUploadsStatus.MarkFailed("CollectUploadRequests failed");
        return false;
    }
    state.collectUploadsStatus.MarkSucceeded();
    const auto collectUploadsEnd = Clock::now();
    state.execution.stats.collectUploadsMs =
        std::chrono::duration<float, std::milli>(collectUploadsEnd - collectUploadsStart).count();

    if (!frameShaderStage.CommitShaderRequests(frameShaderContext, state.shaderPrep))
    {
        state.commitShadersStatus.MarkFailed("CommitShaderRequests failed");
        return false;
    }
    state.commitShadersStatus.MarkSucceeded();

    if (!frameShaderStage.CommitMaterialRequests(frameShaderContext, state.shaderPrep))
    {
        state.commitMaterialsStatus.MarkFailed("CommitMaterialRequests failed");
        return false;
    }
    state.commitMaterialsStatus.MarkSucceeded();

    const auto commitUploadsStart = Clock::now();
    if (!frameUploadStage.CommitUploads(frameUploadContext, state.upload))
    {
        state.commitUploadsStatus.MarkFailed("CommitUploads failed");
        return false;
    }
    state.commitUploadsStatus.MarkSucceeded();
    const auto commitUploadsEnd = Clock::now();
    state.execution.stats.commitUploadsMs =
        std::chrono::duration<float, std::milli>(commitUploadsEnd - commitUploadsStart).count();

    FrameGraphStageContext graphContext{
        context.device,
        viewportWidth,
        viewportHeight,
        backbufferRT,
        backbufferTex,
        &state.extraction.snapshot.world,
        state.extraction.snapshot.world.GetQueue(),
        context.featureRegistry.GetActiveRenderPipeline(),
        context.shaderRuntime,
        context.materials,
        context.callbacks,
        context.eventBus,
        context.gpuRuntime,
        state.upload.perFrameCB,
        &state.frameConstants.frameConstants,
        state.upload.perObjectArena,
        state.upload.perObjectStride,
        context.defaultTonemapMaterial,
        context.tonemapMaterialSystem
    };
    const auto buildGraphStart = Clock::now();
    if (!m_frameGraphStage.Execute(graphContext, state.graph))
    {
        state.buildGraphStatus.MarkFailed("BuildGraph failed");
        Debug::LogError("RenderFrameOrchestrator: BuildGraph failed");
        return false;
    }
    state.buildGraphStatus.MarkSucceeded();
    const auto buildGraphEnd = Clock::now();
    state.execution.stats.buildGraphMs =
        std::chrono::duration<float, std::milli>(buildGraphEnd - buildGraphStart).count();

    FrameExecutionStageContext executionContext{
        context.device,
        context.swapchain,
        context.graphicsCommandList,
        context.computeCommandList,
        context.transferCommandList,
        context.frameFence,
        context.gpuRuntime,
        state.extraction.snapshot,
        context.timing,
        state.upload.perFrameCB,
        state.upload.perObjectArena,
        state.upload.perObjectStride,
        *state.graph.renderGraph,
        state.graph.compiledFrame,
        state.graph.runtimeBindings,
        context.nextFenceValue,
        context.presentVsync,
        BuildDefaultFrameSubmissionPlan(context.device, state.graph.compiledFrame)
    };
    const auto executeStart = Clock::now();
    if (!executionStage.Execute(executionContext, state.execution))
    {
        state.executeStatus.MarkFailed("Execute failed");
        Debug::LogError("RenderFrameOrchestrator: Execute failed");
        return false;
    }
    const auto executeEnd = Clock::now();
    state.execution.stats.executeMs =
        std::chrono::duration<float, std::milli>(executeEnd - executeStart).count();

    state.executeStatus.MarkSucceeded();
    state.execution.stats.prepareFrameMs = state.execution.stats.prepareFrameMs;
    state.execution.stats.collectShadersMs = state.execution.stats.collectShadersMs;
    state.execution.stats.collectMaterialsMs = state.execution.stats.collectMaterialsMs;
    state.execution.stats.parallelSectionMs = state.execution.stats.parallelSectionMs;
    state.execution.stats.peakActiveWorkers = state.execution.stats.peakActiveWorkers;
    context.stats = state.execution.stats;
    context.nextFenceValue = state.execution.submittedFenceValue + 1u;

    if (context.eventBus)
    {
        context.eventBus->Publish(
            events::FrameEndEvent{context.timing.GetDeltaSecondsF(), context.timing.GetFrameCount()});
    }

    return state.Succeeded();
}

} // namespace engine::renderer
