#pragma once

#include "platform/IPlatformTiming.hpp"
#include "renderer/GpuResourceRuntime.hpp"
#include "renderer/RenderFrameTypes.hpp"
#include "renderer/RenderWorld.hpp"
#include "renderer/ShaderRuntime.hpp"
#include <vector>

namespace engine::renderer {

class MaterialSystem;

struct FramePreparationStageContext
{
    bool isOpenGLBackend = false;
    uint32_t viewportWidth = 0u;
    uint32_t viewportHeight = 0u;
    const RenderView& view;
    const platform::IPlatformTiming& timing;
    const MaterialSystem& materials;
    ShaderRuntime& shaderRuntime;
    GpuResourceRuntime& gpuRuntime;
    RenderWorld& renderWorld;
};


class FramePreparationStage
{
public:
    [[nodiscard]] bool PrepareFrameData(const FramePreparationStageContext& context,
                                        FramePreparationStageResult& result) const;
    [[nodiscard]] bool CollectShaderRequests(const FramePreparationStageContext& context,
                                             FramePreparationStageResult& result) const;
    [[nodiscard]] bool CollectMaterialRequests(const FramePreparationStageContext& context,
                                               FramePreparationStageResult& result) const;
    [[nodiscard]] bool BuildRenderQueues(const FramePreparationStageContext& context,
                                         const FramePreparationStageResult& result) const;
    [[nodiscard]] bool CollectUploadRequests(const FramePreparationStageContext& context,
                                             FramePreparationStageResult& result) const;

    [[nodiscard]] bool CommitShaderRequests(const FramePreparationStageContext& context,
                                            const FramePreparationStageResult& result) const;
    [[nodiscard]] bool CommitMaterialRequests(const FramePreparationStageContext& context,
                                              const FramePreparationStageResult& result) const;
    [[nodiscard]] bool CommitUploads(const FramePreparationStageContext& context,
                                     FramePreparationStageResult& result) const;

    [[nodiscard]] bool Execute(const FramePreparationStageContext& context,
                               FramePreparationStageResult& result) const;
};

} // namespace engine::renderer
