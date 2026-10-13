#pragma once

#include "renderer/RendererTypes.hpp"
#include "renderer/IDevice.hpp"
#include "rendergraph/CompiledFrame.hpp"
#include <algorithm>
#include <unordered_map>
#include <vector>

namespace engine::renderer {

struct GraphSubmissionMappingDesc
{
    uint32_t passIndex = UINT32_MAX;
    uint32_t submissionId = UINT32_MAX;
    QueueType queue = QueueType::Graphics;
};

struct CommandSubmissionPlan
{
    std::vector<CommandSubmissionDesc> submissions;
    std::vector<InterQueueDependencyDesc> dependencies;
    std::vector<GraphSubmissionMappingDesc> graphMappings;

    void Reset() { submissions.clear(); dependencies.clear(); graphMappings.clear(); }
    void Add(const CommandSubmissionDesc& desc) { submissions.push_back(desc); }
    void AddDependency(const InterQueueDependencyDesc& desc) { dependencies.push_back(desc); }
    void AddGraphMapping(const GraphSubmissionMappingDesc& desc) { graphMappings.push_back(desc); }
    [[nodiscard]] bool Empty() const noexcept { return submissions.empty(); }
};

inline CommandSubmissionPlan BuildSingleQueueFrameSubmissionPlan()
{
    CommandSubmissionPlan plan;
    plan.Add(CommandSubmissionDesc{QueueType::Graphics, true, true, 0u, 0u, false, false});
    return plan;
}

[[nodiscard]] inline QueueType ResolveMaterializedQueue(const IDevice& device, QueueType requested)
{
    const QueueCapabilities requestedCaps = device.GetQueueCapabilities(requested);
    if (requestedCaps.supported)
        return requested;
    return QueueType::Graphics;
}

inline CommandSubmissionPlan BuildDefaultFrameSubmissionPlan(const IDevice& device,
                                                             const rendergraph::CompiledFrame& frame)
{
    if (frame.passes.empty())
        return BuildSingleQueueFrameSubmissionPlan();

    CommandSubmissionPlan plan;
    std::unordered_map<uint32_t, uint32_t> passIndexToMaterializedSubmission;
    std::unordered_map<uint32_t, QueueType> passIndexToMaterializedQueue;

    bool havePreviousQueue = false;
    QueueType previousQueue = QueueType::Graphics;
    uint32_t nextSubmissionId = 0u;

    for (const auto& pass : frame.passes)
    {
        const QueueType queue = ResolveMaterializedQueue(device, pass.executionQueue);
        if (!havePreviousQueue)
        {
            previousQueue = queue;
            havePreviousQueue = true;
        }
        else if (queue != previousQueue)
        {
            ++nextSubmissionId;
            previousQueue = queue;
        }

        const uint32_t materializedSubmissionId = nextSubmissionId;
        passIndexToMaterializedSubmission[pass.passIndex] = materializedSubmissionId;
        passIndexToMaterializedQueue[pass.passIndex] = queue;
        plan.AddGraphMapping(GraphSubmissionMappingDesc{pass.passIndex, materializedSubmissionId, queue});

        if (plan.submissions.empty() || plan.submissions.back().submissionId != materializedSubmissionId)
        {
            const bool isGraphics = queue == QueueType::Graphics;
            plan.Add(CommandSubmissionDesc{
                queue,
                plan.submissions.empty() && isGraphics,
                false,
                materializedSubmissionId,
                0u,
                false,
                false
            });
        }
    }

    for (const auto& dependency : frame.interQueueDependencies)
    {
        const auto producerSubmissionIt = passIndexToMaterializedSubmission.find(dependency.producerPassIndex);
        const auto consumerSubmissionIt = passIndexToMaterializedSubmission.find(dependency.consumerPassIndex);
        const auto producerQueueIt = passIndexToMaterializedQueue.find(dependency.producerPassIndex);
        const auto consumerQueueIt = passIndexToMaterializedQueue.find(dependency.consumerPassIndex);
        if (producerSubmissionIt == passIndexToMaterializedSubmission.end() ||
            consumerSubmissionIt == passIndexToMaterializedSubmission.end() ||
            producerQueueIt == passIndexToMaterializedQueue.end() ||
            consumerQueueIt == passIndexToMaterializedQueue.end())
            continue;

        const uint32_t producerSubmissionId = producerSubmissionIt->second;
        const uint32_t consumerSubmissionId = consumerSubmissionIt->second;
        const QueueType producerQueue = producerQueueIt->second;
        const QueueType consumerQueue = consumerQueueIt->second;
        if (producerSubmissionId == consumerSubmissionId)
            continue;

        plan.AddDependency(InterQueueDependencyDesc{
            producerSubmissionId,
            producerQueue,
            consumerSubmissionId,
            consumerQueue,
            dependency.primitive,
            dependency.scope,
            dependency.ownershipTransferPoint,
            true,
            producerQueue != consumerQueue
        });
    }

    for (size_t i = plan.submissions.size(); i-- > 0u; )
    {
        if (plan.submissions[i].queue == QueueType::Graphics)
        {
            plan.submissions[i].signalSwapchainReady = true;
            break;
        }
    }

    for (const InterQueueDependencyDesc& dependency : plan.dependencies)
    {
        if (dependency.producerSubmissionId < plan.submissions.size())
            plan.submissions[dependency.producerSubmissionId].exportsQueueSignal = true;
        if (dependency.consumerSubmissionId < plan.submissions.size())
            plan.submissions[dependency.consumerSubmissionId].requiresQueueOwnership |= dependency.requiresOwnershipTransfer;
    }

    return plan;
}

} // namespace engine::renderer
