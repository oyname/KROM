#include "renderer/RenderFrameOrchestrator.hpp"
#include "renderer/CommandSubmissionPlan.hpp"
#include "core/Debug.hpp"

namespace engine::renderer {
namespace {

FrameConstantStageContext MakeFrameConstantContext(const RenderFrameOrchestratorContext& context,
                                                     uint32_t viewportWidth,
                                                     uint32_t viewportHeight)
{
    return FrameConstantStageContext{
        context.isOpenGLBackend,
        viewportWidth,
        viewportHeight,
        context.view,
        context.timing,
        context.renderWorld
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
                                               const FrameConstantsResult& frameData)
{
    return FrameUploadStageContext{
        context.renderWorld,
        context.view,
        frameData,
        context.gpuRuntime,
        context.materials,
        context.shaderRuntime
    };
}

} // namespace

void RenderFrameOrchestrator::EnsurePreparationTaskGraphBuilt()
{
    if (m_preparationTaskGraphBuilt)
        return;

    m_preparationTaskGraph.Clear();
    m_extractTask = m_preparationTaskGraph.Add("Extract", {}, []() { return jobs::TaskResult::Ok(); });
    m_prepareFrameTask = m_preparationTaskGraph.Add("PrepareFrame", {m_extractTask}, []() { return jobs::TaskResult::Ok(); });
    m_collectShadersTask = m_preparationTaskGraph.Add("CollectShaderRequests", {m_extractTask}, []() { return jobs::TaskResult::Ok(); });
    m_collectMaterialsTask = m_preparationTaskGraph.Add("CollectMaterialRequests", {m_extractTask}, []() { return jobs::TaskResult::Ok(); });
    m_buildQueuesTask = m_preparationTaskGraph.Add("BuildQueues", {m_prepareFrameTask}, []() { return jobs::TaskResult::Ok(); });
    m_collectUploadsTask = m_preparationTaskGraph.Add("CollectUploadRequests", {m_buildQueuesTask}, []() { return jobs::TaskResult::Ok(); });

    m_preparationTaskGraphBuilt = m_preparationTaskGraph.Build();
    if (m_preparationTaskGraphBuilt)
        Debug::Log("RenderFrameOrchestrator: Build - cached preparation task graph");
}

bool RenderFrameOrchestrator::Execute(const RenderFrameOrchestratorContext& context,
                                      RenderFrameExecutionState& state)
{
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
    const bool hadPreparationTaskGraph = m_preparationTaskGraphBuilt;
    EnsurePreparationTaskGraphBuilt();
    if (!m_preparationTaskGraphBuilt)
    {
        Debug::LogError("RenderFrameOrchestrator: cached preparation task graph build failed");
        context.world.EndReadPhase();
        return false;
    }

    FrameExtractionStageContext extractionContext{
        context.world,
        context.featureRegistry.GetSceneExtractionSteps(),
        context.renderWorld
    };
    const FrameConstantStageContext frameConstantContext = MakeFrameConstantContext(context, viewportWidth, viewportHeight);
    const FrameShaderStageContext frameShaderContext = MakeFrameShaderContext(context);
    const FrameUploadStageContext frameUploadContext = MakeFrameUploadContext(context, state.frameConstants);

    m_preparationTaskGraph.SetTaskFunction(m_extractTask, [&]() -> jobs::TaskResult {
        if (!extractionStage.Execute(extractionContext, state.extraction))
        {
            state.extractionStatus.MarkFailed("Extract failed");
            return jobs::TaskResult::Fail(state.extractionStatus.errorMessage.c_str());
        }
        state.extractionStatus.MarkSucceeded();
        return jobs::TaskResult::Ok();
    });
    m_preparationTaskGraph.SetTaskFunction(m_prepareFrameTask, [&]() -> jobs::TaskResult {
        if (!frameConstantStage.PrepareFrameData(frameConstantContext, state.frameConstants))
        {
            state.prepareFrameStatus.MarkFailed("PrepareFrame failed");
            return jobs::TaskResult::Fail(state.prepareFrameStatus.errorMessage.c_str());
        }
        state.prepareFrameStatus.MarkSucceeded();
        return jobs::TaskResult::Ok();
    });
    m_preparationTaskGraph.SetTaskFunction(m_collectShadersTask, [&]() -> jobs::TaskResult {
        if (!frameShaderStage.CollectShaderRequests(frameShaderContext, state.shaderPrep))
        {
            state.collectShadersStatus.MarkFailed("CollectShaderRequests failed");
            return jobs::TaskResult::Fail(state.collectShadersStatus.errorMessage.c_str());
        }
        state.collectShadersStatus.MarkSucceeded();
        return jobs::TaskResult::Ok();
    });
    m_preparationTaskGraph.SetTaskFunction(m_collectMaterialsTask, [&]() -> jobs::TaskResult {
        if (!frameShaderStage.CollectMaterialRequests(frameShaderContext, state.shaderPrep))
        {
            state.collectMaterialsStatus.MarkFailed("CollectMaterialRequests failed");
            return jobs::TaskResult::Fail(state.collectMaterialsStatus.errorMessage.c_str());
        }
        state.collectMaterialsStatus.MarkSucceeded();
        return jobs::TaskResult::Ok();
    });
    m_preparationTaskGraph.SetTaskFunction(m_buildQueuesTask, [&]() -> jobs::TaskResult {
        if (!frameUploadStage.BuildRenderQueues(frameUploadContext))
        {
            state.buildQueuesStatus.MarkFailed("BuildQueues failed");
            return jobs::TaskResult::Fail(state.buildQueuesStatus.errorMessage.c_str());
        }
        state.buildQueuesStatus.MarkSucceeded();
        return jobs::TaskResult::Ok();
    });
    m_preparationTaskGraph.SetTaskFunction(m_collectUploadsTask, [&]() -> jobs::TaskResult {
        if (!frameUploadStage.CollectUploadRequests(frameUploadContext, state.upload))
        {
            state.collectUploadsStatus.MarkFailed("CollectUploadRequests failed");
            return jobs::TaskResult::Fail(state.collectUploadsStatus.errorMessage.c_str());
        }
        state.collectUploadsStatus.MarkSucceeded();
        return jobs::TaskResult::Ok();
    });

    const jobs::TaskResult preparationResult = m_preparationTaskGraph.Execute(context.jobSystem);
    if (!preparationResult.Succeeded())
    {
        Debug::LogError("RenderFrameOrchestrator: cached preparation task graph execution failed%s%s",
                        preparationResult.errorMessage ? ": " : "",
                        preparationResult.errorMessage ? preparationResult.errorMessage : "");
        context.world.EndReadPhase();
        return false;
    }

    if (hadPreparationTaskGraph)
        Debug::LogVerbose("RenderFrameOrchestrator: Reuse - cached preparation task graph");

    if (!frameShaderStage.CommitShaderRequests(frameShaderContext, state.shaderPrep))
    {
        state.commitShadersStatus.MarkFailed("CommitShaderRequests failed");
        context.world.EndReadPhase();
        return false;
    }
    state.commitShadersStatus.MarkSucceeded();

    if (!frameShaderStage.CommitMaterialRequests(frameShaderContext, state.shaderPrep))
    {
        state.commitMaterialsStatus.MarkFailed("CommitMaterialRequests failed");
        context.world.EndReadPhase();
        return false;
    }
    state.commitMaterialsStatus.MarkSucceeded();

    if (!frameUploadStage.CommitUploads(frameUploadContext, state.upload))
    {
        state.commitUploadsStatus.MarkFailed("CommitUploads failed");
        context.world.EndReadPhase();
        return false;
    }
    state.commitUploadsStatus.MarkSucceeded();

    FrameGraphStageContext graphContext{
        context.device,
        viewportWidth,
        viewportHeight,
        backbufferRT,
        backbufferTex,
        context.renderWorld.GetQueue(),
        context.featureRegistry.GetActiveRenderPipeline(),
        context.shaderRuntime,
        context.materials,
        context.callbacks,
        context.eventBus,
        context.gpuRuntime,
        state.upload.perFrameCB,
        state.upload.perObjectArena,
        state.upload.perObjectStride,
        context.defaultTonemapMaterial,
        context.tonemapMaterialSystem
    };
    if (!m_frameGraphStage.Execute(graphContext, state.graph))
    {
        state.buildGraphStatus.MarkFailed("BuildGraph failed");
        Debug::LogError("RenderFrameOrchestrator: BuildGraph failed");
        context.world.EndReadPhase();
        return false;
    }
    state.buildGraphStatus.MarkSucceeded();

    FrameExecutionStageContext executionContext{
        context.device,
        context.swapchain,
        context.graphicsCommandList,
        context.computeCommandList,
        context.transferCommandList,
        context.frameFence,
        context.gpuRuntime,
        context.renderWorld,
        context.timing,
        state.upload.perFrameCB,
        *state.graph.renderGraph,
        state.graph.compiledFrame,
        context.nextFenceValue,
        context.presentVsync,
        BuildDefaultFrameSubmissionPlan(context.device, state.graph.compiledFrame)
    };
    if (!executionStage.Execute(executionContext, state.execution))
    {
        state.executeStatus.MarkFailed("Execute failed");
        Debug::LogError("RenderFrameOrchestrator: Execute failed");
        context.world.EndReadPhase();
        return false;
    }

    state.executeStatus.MarkSucceeded();
    context.stats = state.execution.stats;
    context.nextFenceValue = state.execution.submittedFenceValue + 1u;

    if (context.eventBus)
    {
        context.eventBus->Publish(
            events::FrameEndEvent{context.timing.GetDeltaSecondsF(), context.timing.GetFrameCount()});
    }

    context.world.EndReadPhase();
    return state.Succeeded();
}

} // namespace engine::renderer
