#pragma once

#include "renderer/GpuResourceRuntime.hpp"
#include "renderer/RenderPassRegistry.hpp"
#include "renderer/RenderFrameTypes.hpp"
#include "renderer/RenderWorld.hpp"
#include "renderer/ShaderRuntime.hpp"

namespace engine::renderer {

class MaterialSystem;

struct FrameUploadStageContext
{
    RenderWorld& renderWorld;
    const RenderView& view;
    const FrameConstantsResult& frameData;
    GpuResourceRuntime& gpuRuntime;
    const MaterialSystem& materials;
    ShaderRuntime& shaderRuntime;
    const RenderPassRegistry& renderPassRegistry;
};

class FrameUploadStage
{
public:
    [[nodiscard]] bool BuildRenderQueues(const FrameUploadStageContext& context) const;
    [[nodiscard]] bool CollectUploadRequests(const FrameUploadStageContext& context,
                                             FrameUploadResult& result) const;
    [[nodiscard]] bool CommitUploads(const FrameUploadStageContext& context,
                                     FrameUploadResult& result) const;
};

} // namespace engine::renderer
