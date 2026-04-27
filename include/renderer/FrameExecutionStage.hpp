#pragma once

#include "events/EventBus.hpp"
#include "renderer/CommandSubmissionPlan.hpp"
#include "platform/IPlatformTiming.hpp"
#include "renderer/GpuResourceRuntime.hpp"
#include "renderer/RenderFrameTypes.hpp"
#include "renderer/RenderWorld.hpp"
#include "renderer/ShaderBindingModel.hpp"
#include "rendergraph/CompiledFrame.hpp"
#include <memory>

namespace engine::renderer {

struct FrameGraphRuntimeBindings;

struct FrameExecutionStageContext
{
    IDevice& device;
    ISwapchain& swapchain;
    ICommandList& graphicsCommandList;
    ICommandList* computeCommandList  = nullptr;
    ICommandList* transferCommandList = nullptr;
    IFence* frameFence                = nullptr;
    GpuResourceRuntime& gpuRuntime;
    RenderSceneSnapshot& snapshot;
    const platform::IPlatformTiming& timing;
    BufferHandle perFrameCB;
    BufferHandle perObjectArena;
    uint32_t perObjectStride = 0u;
    const rendergraph::RenderGraph& renderGraph;
    const rendergraph::CompiledFrame& compiledFrame;
    std::shared_ptr<FrameGraphRuntimeBindings> runtimeBindings;
    uint64_t nextFenceValue = 0u;
    bool presentVsync       = true;
    CommandSubmissionPlan submissionPlan{};
};

class FrameExecutionStage
{
public:
    [[nodiscard]] bool Execute(const FrameExecutionStageContext& context,
                               FrameExecutionStageResult& result) const;
};

} // namespace engine::renderer
