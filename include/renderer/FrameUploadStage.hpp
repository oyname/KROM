#pragma once

#include "renderer/GpuResourceRuntime.hpp"
#include "renderer/RenderPassRegistry.hpp"
#include "renderer/RenderFrameTypes.hpp"
#include "renderer/RenderSceneSnapshot.hpp"
#include "renderer/ShaderRuntime.hpp"

namespace engine::jobs { class JobSystem; }

namespace engine::renderer {

class MaterialSystem;

struct FrameUploadResult
{
    BufferHandle perFrameCB;
    BufferHandle perObjectArena;
    uint32_t perObjectStride = 0u;
    std::vector<GpuResourceRuntime::MeshUploadRequest> meshUploadRequests;
};

struct FrameUploadStageContext
{
    RenderSceneSnapshot& snapshot;
    const RenderView& view;
    const FrameConstantsResult& frameData;
    GpuResourceRuntime& gpuRuntime;
    const MaterialSystem& materials;
    ShaderRuntime& shaderRuntime;
    const RenderPassRegistry& renderPassRegistry;
    jobs::JobSystem* jobSystem = nullptr;
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
