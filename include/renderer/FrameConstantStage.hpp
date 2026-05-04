#pragma once

#include "renderer/Environment.hpp"
#include "platform/IPlatformTiming.hpp"
#include "renderer/FeatureRegistry.hpp"
#include "renderer/RenderFrameTypes.hpp"
#include "renderer/RenderWorld.hpp"

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
    const RenderWorld* renderWorld = nullptr;
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
                              const std::vector<const IFrameConstantsContributor*>& contributorsIn)
        : device(deviceIn)
        , projectionClipSpaceAdjustment(projectionAdjustment)
        , shadowClipSpaceAdjustment(shadowAdjustment)
        , viewportWidth(viewportWidthIn)
        , viewportHeight(viewportHeightIn)
        , view(viewIn)
        , timing(timingIn)
        , environmentState(environmentStateIn)
        , snapshot(&snapshotIn)
        , renderWorld(&snapshotIn.GetWorld())
        , contributors(contributorsIn)
    {
    }

    FrameConstantStageContext(IDevice* deviceIn,
                              const math::Mat4& projectionAdjustment,
                              const math::Mat4& shadowAdjustment,
                              uint32_t viewportWidthIn,
                              uint32_t viewportHeightIn,
                              const RenderView& viewIn,
                              const platform::IPlatformTiming& timingIn,
                              const EnvironmentRuntimeState& environmentStateIn,
                              const RenderWorld& renderWorldIn,
                              const std::vector<const IFrameConstantsContributor*>& contributorsIn)
        : device(deviceIn)
        , projectionClipSpaceAdjustment(projectionAdjustment)
        , shadowClipSpaceAdjustment(shadowAdjustment)
        , viewportWidth(viewportWidthIn)
        , viewportHeight(viewportHeightIn)
        , view(viewIn)
        , timing(timingIn)
        , environmentState(environmentStateIn)
        , snapshot(nullptr)
        , renderWorld(&renderWorldIn)
        , contributors(contributorsIn)
    {
    }

    FrameConstantStageContext(const math::Mat4& projectionAdjustment,
                              const math::Mat4& shadowAdjustment,
                              uint32_t viewportWidthIn,
                              uint32_t viewportHeightIn,
                              const RenderView& viewIn,
                              const platform::IPlatformTiming& timingIn,
                              const EnvironmentRuntimeState& environmentStateIn,
                              const RenderSceneSnapshot& snapshotIn,
                              const std::vector<const IFrameConstantsContributor*>& contributorsIn)
        : FrameConstantStageContext(nullptr,
                                    projectionAdjustment,
                                    shadowAdjustment,
                                    viewportWidthIn,
                                    viewportHeightIn,
                                    viewIn,
                                    timingIn,
                                    environmentStateIn,
                                    snapshotIn,
                                    contributorsIn)
    {
    }

    FrameConstantStageContext(const math::Mat4& projectionAdjustment,
                              const math::Mat4& shadowAdjustment,
                              uint32_t viewportWidthIn,
                              uint32_t viewportHeightIn,
                              const RenderView& viewIn,
                              const platform::IPlatformTiming& timingIn,
                              const EnvironmentRuntimeState& environmentStateIn,
                              const RenderWorld& renderWorldIn,
                              const std::vector<const IFrameConstantsContributor*>& contributorsIn)
        : FrameConstantStageContext(nullptr,
                                    projectionAdjustment,
                                    shadowAdjustment,
                                    viewportWidthIn,
                                    viewportHeightIn,
                                    viewIn,
                                    timingIn,
                                    environmentStateIn,
                                    renderWorldIn,
                                    contributorsIn)
    {
    }

    [[nodiscard]] const RenderWorld& GetRenderWorld() const noexcept
    {
        return *renderWorld;
    }
};

class FrameConstantStage
{
public:
    [[nodiscard]] bool PrepareFrameData(const FrameConstantStageContext& context,
                                        FrameConstantsResult& result) const;
};

} // namespace engine::renderer
