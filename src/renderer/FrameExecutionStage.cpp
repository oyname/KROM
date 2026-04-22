#include "renderer/CommandSubmissionPlan.hpp"
#include "renderer/FrameExecutionStage.hpp"
#include "renderer/RenderPassRegistry.hpp"
#include "core/Debug.hpp"
#include <algorithm>
#include <unordered_map>
#include <vector>

namespace engine::renderer {
namespace {

[[nodiscard]] const char* QueueName(QueueType queue) noexcept
{
    switch (queue)
    {
    case QueueType::Graphics: return "Graphics";
    case QueueType::Compute: return "Compute";
    case QueueType::Transfer: return "Transfer";
    default: return "Unknown";
    }
}

[[nodiscard]] ICommandList* ResolveCommandList(const FrameExecutionStageContext& context, QueueType queue)
{
    switch (queue)
    {
    case QueueType::Graphics: return &context.graphicsCommandList;
    case QueueType::Compute:  return context.computeCommandList ? context.computeCommandList : &context.graphicsCommandList;
    case QueueType::Transfer: return context.transferCommandList ? context.transferCommandList : &context.graphicsCommandList;
    default:                  return &context.graphicsCommandList;
    }
}

[[nodiscard]] QueueType ResolveQueueFallback(const FrameExecutionStageContext& context, QueueType requested)
{
    const QueueCapabilities capabilities = context.device.GetQueueCapabilities(requested);
    if (capabilities.supported)
        return requested;
    return QueueType::Graphics;
}

[[nodiscard]] const CommandSubmissionPlan* ResolveSubmissionPlan(const FrameExecutionStageContext& context,
                                                                 CommandSubmissionPlan& fallbackPlan)
{
    if (!context.submissionPlan.Empty())
        return &context.submissionPlan;

    fallbackPlan = BuildDefaultFrameSubmissionPlan(context.device, context.compiledFrame);
    return &fallbackPlan;
}

[[nodiscard]] bool ShouldUseSeparateUploadSubmission(const CommandSubmissionPlan& plan,
                                                      QueueType uploadQueue)
{
    if (plan.submissions.empty())
        return true;
    return plan.submissions.front().queue != uploadQueue;
}

void PrependUploadSubmission(const FrameExecutionStageContext& context,
                             CommandSubmissionPlan& plan,
                             QueueType uploadQueue)
{
    for (auto& submission : plan.submissions)
        ++submission.submissionId;
    for (auto& mapping : plan.graphMappings)
        ++mapping.submissionId;
    for (auto& dependency : plan.dependencies)
    {
        ++dependency.producerSubmissionId;
        ++dependency.consumerSubmissionId;
    }

    plan.submissions.insert(plan.submissions.begin(), CommandSubmissionDesc{
        uploadQueue,
        false,
        false,
        0u,
        0u,
        false,
        true
    });

    if (!plan.submissions.empty())
    {
        for (const auto& mapped : plan.graphMappings)
        {
            if (mapped.submissionId != 1u)
                continue;
            plan.dependencies.push_back(InterQueueDependencyDesc{
                0u,
                uploadQueue,
                1u,
                mapped.queue,
                QueueSyncPrimitive::Fence,
                QueueDependencyScope::Submission,
                QueueOwnershipTransferPoint::AfterSubmit,
                true,
                uploadQueue != mapped.queue
            });
            break;
        }
    }
}


[[nodiscard]] bool RequiresDeferredConsumerTransition(const GpuResourceRuntime::PendingBufferUpload& upload) noexcept
{
    switch (upload.destinationStateAfterCopy)
    {
    case ResourceState::CopyDest:
    case ResourceState::CopySource:
    case ResourceState::Common:
    case ResourceState::Unknown:
        return false;
    default:
        return true;
    }
}

[[nodiscard]] bool IsTransferCompatibleState(ResourceState state) noexcept
{
    switch (state)
    {
    case ResourceState::Unknown:
    case ResourceState::Common:
    case ResourceState::CopySource:
    case ResourceState::CopyDest:
        return true;
    default:
        return false;
    }
}

[[nodiscard]] bool QueueSupportsState(QueueType queue, ResourceState state) noexcept
{
    switch (queue)
    {
    case QueueType::Graphics:
        return true;
    case QueueType::Compute:
        switch (state)
        {
        case ResourceState::Unknown:
        case ResourceState::Common:
        case ResourceState::ConstantBuffer:
        case ResourceState::UnorderedAccess:
        case ResourceState::ShaderRead:
        case ResourceState::CopySource:
        case ResourceState::CopyDest:
        case ResourceState::IndirectArg:
            return true;
        default:
            return false;
        }
    case QueueType::Transfer:
        return IsTransferCompatibleState(state);
    default:
        return false;
    }
}

[[nodiscard]] QueueType ResolveUploadQueue(const FrameExecutionStageContext& context,
                                           const CommandSubmissionPlan&,
                                           bool hasPendingUploads)
{
    QueueType preferred = ResolveQueueFallback(context, context.device.GetPreferredUploadQueue());
    if (!hasPendingUploads)
        return preferred;
    if (preferred == QueueType::Graphics)
        return preferred;

    const auto& uploads = context.gpuRuntime.GetPendingBufferUploads();
    for (const auto& upload : uploads)
    {
        if (!IsTransferCompatibleState(upload.destinationStateBeforeCopy))
            return QueueType::Graphics;
    }

    return preferred;
}

struct DeferredBufferTransition
{
    BufferHandle buffer{};
    ResourceState before = ResourceState::Unknown;
    ResourceState after = ResourceState::Unknown;
    uint32_t submissionId = UINT32_MAX;
};

[[nodiscard]] uint32_t ResolveConsumerSubmissionId(const CommandSubmissionPlan& plan,
                                                   uint32_t producerSubmissionId,
                                                   QueueType producerQueue,
                                                   ResourceState state) noexcept
{
    for (const InterQueueDependencyDesc& dependency : plan.dependencies)
    {
        if (dependency.producerSubmissionId != producerSubmissionId)
            continue;
        if (!QueueSupportsState(dependency.consumerQueue, state))
            continue;
        return dependency.consumerSubmissionId;
    }

    for (const CommandSubmissionDesc& submission : plan.submissions)
    {
        if (submission.submissionId == producerSubmissionId)
            continue;
        if (!QueueSupportsState(submission.queue, state))
            continue;
        return submission.submissionId;
    }

    for (const CommandSubmissionDesc& submission : plan.submissions)
    {
        if (submission.queue == producerQueue)
            continue;
        if (!QueueSupportsState(submission.queue, state))
            continue;
        return submission.submissionId;
    }

    return producerSubmissionId;
}

[[nodiscard]] bool ValidateSubmissionPlan(const FrameExecutionStageContext& context,
                                          const CommandSubmissionPlan& plan)
{
    if (plan.submissions.empty())
    {
        Debug::LogError("FrameExecutionStage: submission plan has no submissions");
        return false;
    }

    for (const auto& submission : plan.submissions)
    {
        ICommandList* cmd = ResolveCommandList(context, submission.queue);
        if (!cmd)
        {
            Debug::LogError("FrameExecutionStage: no command list available for submission %u queue %u",
                            submission.submissionId,
                            static_cast<unsigned>(submission.queue));
            return false;
        }
    }

    for (const auto& mapping : plan.graphMappings)
    {
        if (mapping.submissionId >= plan.submissions.size())
        {
            Debug::LogError("FrameExecutionStage: graph mapping references invalid submission %u", mapping.submissionId);
            return false;
        }
    }

    return true;
}

void ApplyTransition(ICommandList& cmd, const rendergraph::CompiledTransition& transition)
{
    if (transition.texture.IsValid())
        cmd.TransitionResource(transition.texture, transition.before, transition.after);
    else if (transition.renderTarget.IsValid())
        cmd.TransitionRenderTarget(transition.renderTarget, transition.before, transition.after);
    else if (transition.buffer.IsValid())
        cmd.TransitionResource(transition.buffer, transition.before, transition.after);
}

} // namespace

bool FrameExecutionStage::Execute(const FrameExecutionStageContext& context,
                                  FrameExecutionStageResult& result) const
{
    CommandSubmissionPlan fallbackPlan{};
    const CommandSubmissionPlan* resolvedPlan = ResolveSubmissionPlan(context, fallbackPlan);
    if (!resolvedPlan)
        return false;

    CommandSubmissionPlan effectivePlan = *resolvedPlan;
    const QueueType uploadQueue = ResolveUploadQueue(context, effectivePlan, context.gpuRuntime.HasPendingUploads());
    const bool hasPendingUploads = context.gpuRuntime.HasPendingUploads();
    const bool separateUploadSubmission = hasPendingUploads && ShouldUseSeparateUploadSubmission(effectivePlan, uploadQueue);
    if (separateUploadSubmission)
        PrependUploadSubmission(context, effectivePlan, uploadQueue);

    if (!ValidateSubmissionPlan(context, effectivePlan))
        return false;

    const CommandListRuntimeDesc runtimeDesc = context.device.GetCommandListRuntime();
    const bool useInterQueueSemaphores = runtimeDesc.queueSync.interQueueSemaphoreSignalSupported &&
                                         runtimeDesc.queueSync.interQueueSemaphoreWaitSupported;

    for (CommandSubmissionDesc& mutableSubmission : effectivePlan.submissions)
        mutableSubmission.waitSubmissionIds.clear();
    for (InterQueueDependencyDesc& dependency : effectivePlan.dependencies)
    {
        if (dependency.producerSubmissionId == UINT32_MAX || dependency.consumerSubmissionId == UINT32_MAX)
            continue;
        if (dependency.producerSubmissionId == dependency.consumerSubmissionId)
            continue;
        if (!dependency.requiresQueueWait && dependency.scope == QueueDependencyScope::None)
            continue;

        auto submissionIt = std::find_if(effectivePlan.submissions.begin(),
                                         effectivePlan.submissions.end(),
                                         [&](const CommandSubmissionDesc& desc)
                                         {
                                             return desc.submissionId == dependency.consumerSubmissionId;
                                         });
        if (submissionIt == effectivePlan.submissions.end())
            continue;

        if (useInterQueueSemaphores)
        {
            dependency.primitive = QueueSyncPrimitive::Semaphore;
            auto& waits = submissionIt->waitSubmissionIds;
            if (std::find(waits.begin(), waits.end(), dependency.producerSubmissionId) == waits.end())
                waits.push_back(dependency.producerSubmissionId);
        }
        else
        {
            dependency.primitive = QueueSyncPrimitive::Fence;
        }
    }

    std::unordered_map<uint32_t, std::vector<const rendergraph::CompiledPassEntry*>> passesPerSubmission;
    std::unordered_map<uint32_t, uint64_t> submittedFencePerSubmission;
    std::vector<DeferredBufferTransition> deferredUploadTransitions;

    for (const rendergraph::CompiledPassEntry& pass : context.compiledFrame.passes)
    {
        uint32_t submissionId = pass.submissionId;
        QueueType queue = pass.executionQueue;
        for (const GraphSubmissionMappingDesc& mapping : effectivePlan.graphMappings)
        {
            if (mapping.passIndex != pass.passIndex)
                continue;
            submissionId = mapping.submissionId;
            queue = mapping.queue;
            break;
        }

        if (queue != pass.executionQueue)
        {
            Debug::LogVerbose("FrameExecutionStage: pass '%s' routed %s -> %s (submission=%u)",
                       pass.debugName.c_str(),
                       QueueName(pass.executionQueue),
                       QueueName(queue),
                       submissionId);
        }
        passesPerSubmission[submissionId].push_back(&pass);
    }

    uint64_t maxSubmittedFenceValue = 0u;
    bool uploadSubmissionExecuted = false;

    for (const CommandSubmissionDesc& submission : effectivePlan.submissions)
    {
        if (!useInterQueueSemaphores)
        {
            for (const InterQueueDependencyDesc& dependency : effectivePlan.dependencies)
            {
                if (dependency.consumerSubmissionId != submission.submissionId)
                    continue;
                const auto submittedFenceIt = submittedFencePerSubmission.find(dependency.producerSubmissionId);
                if (submittedFenceIt == submittedFencePerSubmission.end())
                    continue;

                if (context.frameFence)
                {
                    Debug::LogVerbose("FrameExecutionStage: wait submission=%u queue=%s <- submission=%u queue=%s fence=%llu",
                               submission.submissionId,
                               QueueName(submission.queue),
                               dependency.producerSubmissionId,
                               QueueName(dependency.producerQueue),
                               static_cast<unsigned long long>(submittedFenceIt->second));
                    context.frameFence->Wait(submittedFenceIt->second);
                }
            }
        }

        ICommandList* commandList = ResolveCommandList(context, submission.queue);
        if (!commandList)
            return false;

        commandList->Begin();
        if (context.perFrameCB.IsValid())
        {
            ShaderStageMask perFrameStages = ShaderStageMask::None;
            if (submission.queue == QueueType::Graphics)
                perFrameStages = ShaderStageMask::Vertex | ShaderStageMask::Fragment;
            else if (submission.queue == QueueType::Compute)
                perFrameStages = ShaderStageMask::Compute;

            if (perFrameStages != ShaderStageMask::None)
            {
                commandList->SetConstantBuffer(CBSlots::PerFrame,
                                               context.perFrameCB,
                                               perFrameStages);
            }
        }

        const bool executeUploadsInThisSubmission = hasPendingUploads && !uploadSubmissionExecuted &&
            ((separateUploadSubmission && submission.submissionId == 0u) ||
             (!separateUploadSubmission && submission.queue == uploadQueue));
        if (executeUploadsInThisSubmission)
        {
            const auto& uploads = context.gpuRuntime.GetPendingBufferUploads();
            Debug::LogVerbose("FrameExecutionStage: upload submission queue=%s copies=%zu separate=%u",
                       QueueName(submission.queue),
                       uploads.size(),
                       separateUploadSubmission ? 1u : 0u);
            for (const auto& upload : uploads)
            {
                commandList->TransitionResource(upload.destinationBuffer,
                                                upload.destinationStateBeforeCopy,
                                                ResourceState::CopyDest);
                commandList->CopyBuffer(upload.destinationBuffer,
                                        upload.destinationOffset,
                                        upload.stagingBuffer,
                                        upload.sourceOffset,
                                        upload.byteSize);

                const bool deferConsumerTransition = separateUploadSubmission &&
                                                     submission.queue != QueueType::Graphics &&
                                                     RequiresDeferredConsumerTransition(upload);
                if (deferConsumerTransition)
                {
                    const uint32_t consumerSubmissionId = ResolveConsumerSubmissionId(effectivePlan,
                                                                                      submission.submissionId,
                                                                                      submission.queue,
                                                                                      upload.destinationStateAfterCopy);
                    deferredUploadTransitions.push_back(DeferredBufferTransition{
                        upload.destinationBuffer,
                        ResourceState::CopyDest,
                        upload.destinationStateAfterCopy,
                        consumerSubmissionId
                    });
                    Debug::LogVerbose("FrameExecutionStage: defer upload transition '%s' %s -> submission=%u",
                               upload.debugName.c_str(),
                               QueueName(submission.queue),
                               consumerSubmissionId);
                }
                else if (upload.destinationStateAfterCopy != ResourceState::CopyDest)
                {
                    commandList->TransitionResource(upload.destinationBuffer,
                                                    ResourceState::CopyDest,
                                                    upload.destinationStateAfterCopy);
                }
                Debug::LogVerbose("FrameExecutionStage: upload '%s' bytes=%llu queue=%s dst=0x%x",
                           upload.debugName.c_str(),
                           static_cast<unsigned long long>(upload.byteSize),
                           QueueName(submission.queue),
                           upload.destinationBuffer.value);
            }
            uploadSubmissionExecuted = true;
        }

        for (const DeferredBufferTransition& deferredTransition : deferredUploadTransitions)
        {
            if (deferredTransition.submissionId != submission.submissionId)
                continue;
            commandList->TransitionResource(deferredTransition.buffer,
                                            deferredTransition.before,
                                            deferredTransition.after);
        }

        rendergraph::RGExecContext execContext;
        execContext.device = &context.device;
        execContext.cmd = commandList;
        execContext.resources = &context.compiledFrame.resources;

        const auto submissionPassesIt = passesPerSubmission.find(submission.submissionId);
        if (submissionPassesIt != passesPerSubmission.end())
        {
            Debug::LogVerbose("FrameExecutionStage: submit %u queue=%s passCount=%zu",
                       submission.submissionId,
                       QueueName(submission.queue),
                       submissionPassesIt->second.size());
            for (const rendergraph::CompiledPassEntry* pass : submissionPassesIt->second)
            {
                if (!pass || pass->passIndex >= context.renderGraph.GetPasses().size())
                    continue;
                const auto& graphPass = context.renderGraph.GetPasses()[pass->passIndex];
                if (!graphPass.enabled || !graphPass.executeFn)
                    continue;

                Debug::LogVerbose("FrameExecutionStage: execute pass '%s' queue=%s submission=%u",
                           pass->debugName.c_str(),
                           QueueName(submission.queue),
                           submission.submissionId);
                for (const rendergraph::CompiledTransition& transition : pass->beginTransitions)
                    ApplyTransition(*execContext.cmd, transition);
                graphPass.executeFn(execContext);
                for (const rendergraph::CompiledTransition& transition : pass->endTransitions)
                    ApplyTransition(*execContext.cmd, transition);
            }
        }

        commandList->End();
        commandList->Submit(submission);

        const uint64_t actualSubmittedFenceValue = commandList->GetLastSubmittedFenceValue();
        const uint64_t submissionFenceValue = actualSubmittedFenceValue != 0u ? actualSubmittedFenceValue : context.nextFenceValue + submission.submissionId;
        submittedFencePerSubmission[submission.submissionId] = submissionFenceValue;
        maxSubmittedFenceValue = std::max(maxSubmittedFenceValue, submissionFenceValue);
        if (context.frameFence)
            context.frameFence->Signal(submissionFenceValue);

        Debug::LogVerbose("FrameExecutionStage: submitted %u queue=%s fence=%llu",
                   submission.submissionId,
                   QueueName(submission.queue),
                   static_cast<unsigned long long>(submissionFenceValue));
    }

    if (hasPendingUploads)
        context.gpuRuntime.ClearPendingUploads();

    result.submittedFenceValue = maxSubmittedFenceValue != 0u ? maxSubmittedFenceValue : context.nextFenceValue;
    context.gpuRuntime.ReleaseTransientTargets(context.compiledFrame, result.submittedFenceValue);
    context.gpuRuntime.EndFrame(result.submittedFenceValue);
    context.device.EndFrame();
    context.swapchain.Present(context.presentVsync);

    auto queueCount = [&](RenderPassID passId) -> uint32_t
    {
        const DrawList* list = context.renderWorld.GetQueue().FindList(passId);
        return list ? static_cast<uint32_t>(list->Size()) : 0u;
    };

    result.stats.frameIndex = context.timing.GetFrameCount();
    result.stats.totalProxyCount = context.renderWorld.TotalProxyCount();
    result.stats.visibleProxyCount = context.renderWorld.VisibleCount();
    result.stats.opaqueDraws = queueCount(StandardRenderPasses::Opaque());
    result.stats.transparentDraws = queueCount(StandardRenderPasses::Transparent());
    result.stats.shadowDraws = queueCount(StandardRenderPasses::Shadow());
    result.stats.backendDrawCalls = context.device.GetDrawCallCount();
    result.stats.graphPassCount = static_cast<uint32_t>(context.compiledFrame.passes.size());
    result.stats.graphTransitionCount = context.compiledFrame.barrierStats.finalTransitions;
    result.stats.pooledTransientTargets = context.gpuRuntime.GetStats().pooledTransientTargets;
    result.stats.uploadedBytes = context.gpuRuntime.GetStats().uploadedBytesThisFrame;

    return true;
}

} // namespace engine::renderer
