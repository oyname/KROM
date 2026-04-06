#pragma once

#include "events/EventBus.hpp"
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
    ICommandList& commandList;
    IFence* frameFence = nullptr;
    GpuResourceRuntime& gpuRuntime;
    RenderWorld& renderWorld;
    const platform::IPlatformTiming& timing;
    BufferHandle perFrameCB;
    rendergraph::RenderGraph& renderGraph;
    const rendergraph::CompiledFrame& compiledFrame;
    uint64_t nextFenceValue = 0u;
};

class FrameExecutionStage
{
public:
    [[nodiscard]] bool Execute(const FrameExecutionStageContext& context,
                               FrameExecutionStageResult& result) const;
};

} // namespace engine::renderer
