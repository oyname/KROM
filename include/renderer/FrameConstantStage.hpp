#pragma once

#include "renderer/Environment.hpp"
#include "platform/IPlatformTiming.hpp"
#include "renderer/RenderExtractionContext.hpp"
#include "renderer/RenderFrameTypes.hpp"

namespace engine::renderer {

struct FrameConstantStageContext
{
    IDevice* device = nullptr;
    math::Mat4 projectionClipSpaceAdjustment = math::Mat4::Identity();
    math::Mat4 shadowClipSpaceAdjustment = math::Mat4::Identity();
    uint32_t viewportWidth  = 0u;
    uint32_t viewportHeight = 0u;
    const RenderView& view;
    const platform::IPlatformTiming& timing;
    EnvironmentRuntimeState environmentState{};
    const RenderSceneSnapshot* snapshot = nullptr;
    RenderFrameDataView frameData{};
    const RenderQueue* renderQueue = nullptr;
    const std::vector<const IFrameConstantsContributor*>& contributors;

    FrameConstantStageContext(IDevice* deviceIn,
                              const math::Mat4& projectionAdjustment,
                              const math::Mat4& shadowAdjustment,
                              uint32_t viewportWidthIn,
                              uint32_t viewportHeightIn,
                              const RenderView& viewIn,
                              const platform::IPlatformTiming& timingIn,
                              const EnvironmentRuntimeState& environmentStateIn,
                              const RenderSceneSnapshot& snapshotIn,
                              const std::vector<const IFrameConstantsContributor*>& contributorsIn);

    FrameConstantStageContext(const math::Mat4& projectionAdjustment,
                              const math::Mat4& shadowAdjustment,
                              uint32_t viewportWidthIn,
                              uint32_t viewportHeightIn,
                              const RenderView& viewIn,
                              const platform::IPlatformTiming& timingIn,
                              const EnvironmentRuntimeState& environmentStateIn,
                              const RenderSceneSnapshot& snapshotIn,
                              const std::vector<const IFrameConstantsContributor*>& contributorsIn);

    FrameConstantStageContext(IDevice* deviceIn,
                              const math::Mat4& projectionAdjustment,
                              const math::Mat4& shadowAdjustment,
                              uint32_t viewportWidthIn,
                              uint32_t viewportHeightIn,
                              const RenderView& viewIn,
                              const platform::IPlatformTiming& timingIn,
                              const EnvironmentRuntimeState& environmentStateIn,
                              RenderFrameDataView frameDataIn,
                              const RenderQueue* renderQueueIn,
                              const std::vector<const IFrameConstantsContributor*>& contributorsIn);

    FrameConstantStageContext(const math::Mat4& projectionAdjustment,
                              const math::Mat4& shadowAdjustment,
                              uint32_t viewportWidthIn,
                              uint32_t viewportHeightIn,
                              const RenderView& viewIn,
                              const platform::IPlatformTiming& timingIn,
                              const EnvironmentRuntimeState& environmentStateIn,
                              RenderFrameDataView frameDataIn,
                              const RenderQueue* renderQueueIn,
                              const std::vector<const IFrameConstantsContributor*>& contributorsIn);

};

class FrameConstantStage
{
public:
    [[nodiscard]] bool PrepareFrameData(const FrameConstantStageContext& context,
                                        FrameConstantsResult& result) const;
};

} // namespace engine::renderer
