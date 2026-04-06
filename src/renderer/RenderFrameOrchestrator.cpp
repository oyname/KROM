#include "renderer/RenderFrameOrchestrator.hpp"
#include "core/Debug.hpp"
#include "jobs/TaskGraph.hpp"

namespace engine::renderer {
namespace {

FramePreparationStageContext MakePreparationContext(const RenderFrameOrchestratorContext& context)
{
    return FramePreparationStageContext{
        context.isOpenGLBackend,
        context.viewportWidth,
        context.viewportHeight,
        context.view,
        context.timing,
        context.materials,
        context.shaderRuntime,
        context.gpuRuntime,
        context.renderWorld
    };
}

bool ApplyExtractionResult(const RenderFrameOrchestratorContext& context,
                           const FrameExtractionStageResult& result)
{
    context.renderWorld.Clear();
    context.renderWorld.Extract(result.snapshot, context.materials);
    return true;
}

[[nodiscard]] jobs::TaskResult BuildPreparationTaskGraph(const RenderFrameOrchestratorContext& context,
                                                         FrameExtractionStage& extractionStage,
                                                         FramePreparationStage& preparationStage,
                                                         RenderFrameExecutionState& state)
{
    const FramePreparationStageContext preparationContext = MakePreparationContext(context);

    jobs::TaskGraph frameGraph;
    const auto tExtract = frameGraph.Add("Extract", {}, [&]() -> jobs::TaskResult {
        FrameExtractionStageContext extractionContext{
            context.world,
            context.featureRegistry.GetSceneExtractionSteps()
        };

        if (!extractionStage.Execute(extractionContext, state.extraction))
        {
            state.extractionStatus.MarkFailed("Extract failed");
            return jobs::TaskResult::Fail(state.extractionStatus.errorMessage.c_str());
        }

        ApplyExtractionResult(context, state.extraction);
        state.extractionStatus.MarkSucceeded();
        return jobs::TaskResult::Ok();
    });

    const auto tPrepareFrame = frameGraph.Add("PrepareFrame", {tExtract}, [&]() -> jobs::TaskResult {
        if (!state.extractionStatus.succeeded)
        {
            state.prepareFrameStatus.MarkFailed("PrepareFrame skipped because Extract failed");
            return jobs::TaskResult::Fail(state.prepareFrameStatus.errorMessage.c_str());
        }

        if (!preparationStage.PrepareFrameData(preparationContext, state.preparation))
        {
            state.prepareFrameStatus.MarkFailed("PrepareFrame failed");
            return jobs::TaskResult::Fail(state.prepareFrameStatus.errorMessage.c_str());
        }

        state.prepareFrameStatus.MarkSucceeded();
        return jobs::TaskResult::Ok();
    });

    frameGraph.Add("CollectShaderRequests", {tExtract}, [&]() -> jobs::TaskResult {
        if (!state.extractionStatus.succeeded)
        {
            state.collectShadersStatus.MarkFailed("CollectShaderRequests skipped because Extract failed");
            return jobs::TaskResult::Fail(state.collectShadersStatus.errorMessage.c_str());
        }

        if (!preparationStage.CollectShaderRequests(preparationContext, state.preparation))
        {
            state.collectShadersStatus.MarkFailed("CollectShaderRequests failed");
            return jobs::TaskResult::Fail(state.collectShadersStatus.errorMessage.c_str());
        }

        state.collectShadersStatus.MarkSucceeded();
        return jobs::TaskResult::Ok();
    });

    frameGraph.Add("CollectMaterialRequests", {tExtract}, [&]() -> jobs::TaskResult {
        if (!state.extractionStatus.succeeded)
        {
            state.collectMaterialsStatus.MarkFailed("CollectMaterialRequests skipped because Extract failed");
            return jobs::TaskResult::Fail(state.collectMaterialsStatus.errorMessage.c_str());
        }

        if (!preparationStage.CollectMaterialRequests(preparationContext, state.preparation))
        {
            state.collectMaterialsStatus.MarkFailed("CollectMaterialRequests failed");
            return jobs::TaskResult::Fail(state.collectMaterialsStatus.errorMessage.c_str());
        }

        state.collectMaterialsStatus.MarkSucceeded();
        return jobs::TaskResult::Ok();
    });

    const auto tBuildQueues = frameGraph.Add("BuildQueues", {tPrepareFrame}, [&]() -> jobs::TaskResult {
        if (!state.prepareFrameStatus.succeeded)
        {
            state.buildQueuesStatus.MarkFailed("BuildQueues skipped because PrepareFrame failed");
            return jobs::TaskResult::Fail(state.buildQueuesStatus.errorMessage.c_str());
        }

        if (!preparationStage.BuildRenderQueues(preparationContext, state.preparation))
        {
            state.buildQueuesStatus.MarkFailed("BuildQueues failed");
            return jobs::TaskResult::Fail(state.buildQueuesStatus.errorMessage.c_str());
        }

        state.buildQueuesStatus.MarkSucceeded();
        return jobs::TaskResult::Ok();
    });

    frameGraph.Add("CollectUploadRequests", {tBuildQueues}, [&]() -> jobs::TaskResult {
        if (!state.buildQueuesStatus.succeeded)
        {
            state.collectUploadsStatus.MarkFailed("CollectUploadRequests skipped because BuildQueues failed");
            return jobs::TaskResult::Fail(state.collectUploadsStatus.errorMessage.c_str());
        }

        if (!preparationStage.CollectUploadRequests(preparationContext, state.preparation))
        {
            state.collectUploadsStatus.MarkFailed("CollectUploadRequests failed");
            return jobs::TaskResult::Fail(state.collectUploadsStatus.errorMessage.c_str());
        }

        state.collectUploadsStatus.MarkSucceeded();
        return jobs::TaskResult::Ok();
    });

    const bool built = frameGraph.Build();
    if (!built)
    {
        Debug::LogError("RenderFrameOrchestrator: frame task graph build failed");
        return jobs::TaskResult::Fail("frame graph build failed");
    }

    return frameGraph.Execute(context.jobSystem);
}

[[nodiscard]] bool CommitPreparationOutputs(const RenderFrameOrchestratorContext& context,
                                            FramePreparationStage& preparationStage,
                                            RenderFrameExecutionState& state)
{
    const FramePreparationStageContext preparationContext = MakePreparationContext(context);

    if (!preparationStage.CommitShaderRequests(preparationContext, state.preparation))
    {
        state.commitShadersStatus.MarkFailed("CommitShaderRequests failed");
        return false;
    }
    state.commitShadersStatus.MarkSucceeded();

    if (!preparationStage.CommitMaterialRequests(preparationContext, state.preparation))
    {
        state.commitMaterialsStatus.MarkFailed("CommitMaterialRequests failed");
        return false;
    }
    state.commitMaterialsStatus.MarkSucceeded();

    if (!preparationStage.CommitUploads(preparationContext, state.preparation))
    {
        state.commitUploadsStatus.MarkFailed("CommitUploads failed");
        return false;
    }
    state.commitUploadsStatus.MarkSucceeded();
    return true;
}

[[nodiscard]] bool BuildFrameGraph(const RenderFrameOrchestratorContext& context,
                                   FrameGraphStage& graphStage,
                                   RenderFrameExecutionState& state)
{
    FrameGraphStageContext graphContext{
        context.viewportWidth,
        context.viewportHeight,
        context.swapchain.GetBackbufferRenderTarget(context.backbufferIndex),
        context.swapchain.GetBackbufferTexture(context.backbufferIndex),
        context.renderWorld.GetQueue(),
        context.featureRegistry.GetActiveRenderPipeline(),
        context.shaderRuntime,
        context.materials,
        context.callbacks,
        context.eventBus,
        context.gpuRuntime,
        state.preparation.perFrameCB,
        context.defaultTonemapMaterial,
        context.tonemapMaterialSystem
    };
    if (!graphStage.Execute(graphContext, state.graph))
    {
        state.buildGraphStatus.MarkFailed("BuildGraph failed");
        return false;
    }

    state.buildGraphStatus.MarkSucceeded();
    return true;
}

[[nodiscard]] bool ExecuteFrameGraph(const RenderFrameOrchestratorContext& context,
                                     FrameExecutionStage& executionStage,
                                     RenderFrameExecutionState& state)
{
    FrameExecutionStageContext executionContext{
        context.device,
        context.swapchain,
        context.commandList,
        context.frameFence,
        context.gpuRuntime,
        context.renderWorld,
        context.timing,
        state.preparation.perFrameCB,
        state.graph.renderGraph,
        state.graph.compiledFrame,
        context.nextFenceValue
    };
    if (!executionStage.Execute(executionContext, state.execution))
    {
        state.executeStatus.MarkFailed("Execute failed");
        return false;
    }

    state.executeStatus.MarkSucceeded();
    context.stats = state.execution.stats;
    context.nextFenceValue = state.execution.submittedFenceValue + 1u;
    return true;
}

} // namespace

bool RenderFrameOrchestrator::Execute(const RenderFrameOrchestratorContext& context,
                                      RenderFrameExecutionState& state) const
{
    if (context.eventBus)
    {
        context.eventBus->Publish(
            events::FrameBeginEvent{context.timing.GetDeltaSecondsF(), context.timing.GetFrameCount()});
    }

    const uint64_t completedFenceValue = context.frameFence ? context.frameFence->GetValue() : 0u;
    context.gpuRuntime.BeginFrame(completedFenceValue);
    context.device.BeginFrame();

    FrameExtractionStage extractionStage;
    FramePreparationStage preparationStage;
    FrameGraphStage graphStage;
    FrameExecutionStage executionStage;

    context.world.BeginReadPhase();

    const jobs::TaskResult graphResult = BuildPreparationTaskGraph(context, extractionStage, preparationStage, state);
    if (!graphResult.Succeeded())
    {
        Debug::LogError("RenderFrameOrchestrator: frame task graph execution failed%s%s",
                        graphResult.errorMessage ? ": " : "",
                        graphResult.errorMessage ? graphResult.errorMessage : "");
        context.world.EndReadPhase();
        return false;
    }

    if (!(state.collectShadersStatus.succeeded
          && state.collectMaterialsStatus.succeeded
          && state.collectUploadsStatus.succeeded))
    {
        Debug::LogError("RenderFrameOrchestrator: collect phase reported inconsistent success state");
        context.world.EndReadPhase();
        return false;
    }

    if (!CommitPreparationOutputs(context, preparationStage, state))
    {
        context.world.EndReadPhase();
        return false;
    }

    if (!BuildFrameGraph(context, graphStage, state))
    {
        Debug::LogError("RenderFrameOrchestrator: BuildGraph failed");
        context.world.EndReadPhase();
        return false;
    }

    if (!ExecuteFrameGraph(context, executionStage, state))
    {
        Debug::LogError("RenderFrameOrchestrator: Execute failed");
        context.world.EndReadPhase();
        return false;
    }

    if (context.eventBus)
    {
        context.eventBus->Publish(
            events::FrameEndEvent{context.timing.GetDeltaSecondsF(), context.timing.GetFrameCount()});
    }

    context.world.EndReadPhase();
    return state.Succeeded();
}

} // namespace engine::renderer
