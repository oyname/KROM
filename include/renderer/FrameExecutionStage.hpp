#pragma once

#include "events/EventBus.hpp"
#include "renderer/CommandSubmissionPlan.hpp"
#include "platform/IPlatformTiming.hpp"
#include "renderer/GpuResourceRuntime.hpp"
#include "renderer/RenderFrameTypes.hpp"
#include "renderer/RenderWorld.hpp"
#include "renderer/ShaderBindingModel.hpp"
#include "rendergraph/CompiledFrame.hpp"

namespace engine::renderer {

struct FrameExecutionStageContext
{
    IDevice& device;
    ISwapchain& swapchain;
    ICommandList& graphicsCommandList;
    ICommandList* computeCommandList = nullptr;
    ICommandList* transferCommandList = nullptr;
    IFence* frameFence = nullptr;
    GpuResourceRuntime& gpuRuntime;
    RenderWorld& renderWorld;
    const platform::IPlatformTiming& timing;
    BufferHandle perFrameCB;
    const rendergraph::RenderGraph& renderGraph;
    const rendergraph::CompiledFrame& compiledFrame;
    uint64_t nextFenceValue = 0u;
    bool presentVsync = true;
    CommandSubmissionPlan submissionPlan{};
};

class FrameExecutionStage
{
public:
    [[nodiscard]] bool Execute(const FrameExecutionStageContext& context,
                               FrameExecutionStageResult& result) const;
};

} // namespace engine::renderer
