#pragma once

#include "renderer/RendererTypes.hpp"
#include <array>
#include <cstdint>

namespace engine::renderer {

enum class NativeCommandListClass : uint8_t
{
    Graphics = 0,
    Compute,
    Copy,
};

struct CommandQueueRuntimeDesc
{
    QueueType queue = QueueType::Graphics;
    NativeCommandListClass nativeClass = NativeCommandListClass::Graphics;
    bool supported = false;
    bool dedicated = false;
    bool canPresent = false;
    bool supportsRenderPass = false;
    bool supportsDispatch = false;
    bool supportsCopy = false;
};

struct CommandAllocatorRuntimeDesc
{
    bool allocatorsAreFrameScoped = true;
    bool allocatorRecyclePerFrameFence = true;
    bool commandPoolsAreQueueScoped = true;
    bool commandListsResetWithAllocator = true;
    bool separateUploadCopyRecording = false;
};

enum class CommandListLifecycleReuseModel : uint8_t
{
    PerQueuePerFrameSlot = 0,
};

struct CommandListLifecycleRuntimeDesc
{
    CommandListLifecycleReuseModel reuseModel = CommandListLifecycleReuseModel::PerQueuePerFrameSlot;
    bool queueAffinityImmutable = true;
    bool allocatorCreatedPerQueueFrameSlot = true;
    bool commandListCreatedPerQueueFrameSlot = true;
    bool allocatorResetRequiresFenceCompletion = true;
    bool commandListResetRequiresAllocatorReset = true;
    bool queueSubmissionBoundToRecordingQueue = true;
    bool onePrimarySubmissionPerQueueFrameSlot = true;
    bool submittedListsRetirePerFrameFence = true;
};

struct RootBindingRuntimeDesc
{
    bool usesDescriptorTableModel = false;
    bool resourceTableBindPerCommandList = false;
    bool samplerTableBindPerCommandList = false;
    bool constantBufferRangesUseDynamicOffsets = false;
};

struct ComputePreparationRuntimeDesc
{
    ComputeRuntimeDesc runtime{};
    bool graphicsFirstV1 = true;
    bool computeCommandListsMaterialized = false;
    bool computePsoMaterialized = false;
    bool uavBarriersMaterialized = false;
    bool queueRoutingMaterialized = false;
    bool crossQueueSyncMaterialized = false;
};

struct MultiQueuePreparationRuntimeDesc
{
    bool preparedForCopyToGraphics = true;
    bool preparedForGraphicsToPresent = true;
    bool preparedForAsyncCompute = true;
    bool queueOwnershipTransfersPrepared = true;
    bool queueOwnershipTransfersMaterialized = false;
    bool interQueueDependenciesMaterialized = false;
};

struct CommandListRuntimeDesc
{
    std::array<CommandQueueRuntimeDesc, 3u> queues{};
    CommandAllocatorRuntimeDesc allocator{};
    CommandListLifecycleRuntimeDesc lifecycle{};
    RootBindingRuntimeDesc bindings{};
    ComputePreparationRuntimeDesc compute{};
    QueueSyncRuntimeDesc queueSync{};
    MultiQueuePreparationRuntimeDesc multiQueue{};
};

[[nodiscard]] inline CommandListRuntimeDesc BuildDefaultCommandListRuntimeDesc() noexcept
{
    CommandListRuntimeDesc desc{};
    desc.queues[0] = CommandQueueRuntimeDesc{ QueueType::Graphics, NativeCommandListClass::Graphics, true, false, true, true, false, true };
    desc.queues[1] = CommandQueueRuntimeDesc{ QueueType::Compute, NativeCommandListClass::Compute, false, false, false, false, true, true };
    desc.queues[2] = CommandQueueRuntimeDesc{ QueueType::Transfer, NativeCommandListClass::Copy, false, false, false, false, false, true };
    desc.allocator = CommandAllocatorRuntimeDesc{ true, true, true, true, false };
    desc.lifecycle = CommandListLifecycleRuntimeDesc{};
    desc.bindings = RootBindingRuntimeDesc{ false, false, false, false };
    desc.compute.runtime = ComputeRuntimeDesc{};
    desc.compute.graphicsFirstV1 = true;
    desc.compute.computeCommandListsMaterialized = false;
    desc.compute.computePsoMaterialized = false;
    desc.compute.uavBarriersMaterialized = false;
    desc.compute.queueRoutingMaterialized = false;
    desc.compute.crossQueueSyncMaterialized = false;
    desc.queueSync = QueueSyncRuntimeDesc{};
    desc.multiQueue = MultiQueuePreparationRuntimeDesc{};
    return desc;
}

} // namespace engine::renderer
