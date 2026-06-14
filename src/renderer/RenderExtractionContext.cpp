#include "renderer/RenderExtractionContext.hpp"

#include "renderer/RenderSceneSnapshot.hpp"

namespace engine::renderer {

SceneExtractionContext::SceneExtractionContext(const ecs::World& w,
                                               RenderSceneSnapshot& snap,
                                               const RenderView* viewIn,
                                               jobs::JobSystem* js) noexcept
    : world(w)
    , snapshot(&snap)
    , extractionView(snap.GetExtractionView())
    , view(viewIn)
    , jobSystem(js)
{
}

SceneExtractionContext::SceneExtractionContext(const ecs::World& w,
                                               RenderExtractionView extraction,
                                               const RenderView* viewIn,
                                               jobs::JobSystem* js) noexcept
    : world(w)
    , snapshot(nullptr)
    , extractionView(extraction)
    , view(viewIn)
    , jobSystem(js)
{
}

FrameConstantsContributionContext::FrameConstantsContributionContext(IDevice* deviceIn,
                                                                     const math::Mat4& projectionAdjustment,
                                                                     const math::Mat4& shadowAdjustment,
                                                                     uint32_t viewportWidthIn,
                                                                     uint32_t viewportHeightIn,
                                                                     const RenderView& viewIn,
                                                                     const platform::IPlatformTiming& timingIn,
                                                                     const RenderSceneSnapshot& snapshotIn)
    : device(deviceIn)
    , projectionClipSpaceAdjustment(projectionAdjustment)
    , shadowClipSpaceAdjustment(shadowAdjustment)
    , viewportWidth(viewportWidthIn)
    , viewportHeight(viewportHeightIn)
    , view(viewIn)
    , timing(timingIn)
    , snapshot(&snapshotIn)
    , frameData(snapshotIn.GetFrameDataView())
    , renderQueue(&snapshotIn.GetQueue())
{
}

FrameConstantsContributionContext::FrameConstantsContributionContext(IDevice* deviceIn,
                                                                     const math::Mat4& projectionAdjustment,
                                                                     const math::Mat4& shadowAdjustment,
                                                                     uint32_t viewportWidthIn,
                                                                     uint32_t viewportHeightIn,
                                                                     const RenderView& viewIn,
                                                                     const platform::IPlatformTiming& timingIn,
                                                                     RenderFrameDataView frameDataIn,
                                                                     const RenderQueue* renderQueueIn,
                                                                     const RenderSceneSnapshot* snapshotIn)
    : device(deviceIn)
    , projectionClipSpaceAdjustment(projectionAdjustment)
    , shadowClipSpaceAdjustment(shadowAdjustment)
    , viewportWidth(viewportWidthIn)
    , viewportHeight(viewportHeightIn)
    , view(viewIn)
    , timing(timingIn)
    , snapshot(snapshotIn)
    , frameData(frameDataIn)
    , renderQueue(renderQueueIn)
{
}

} // namespace engine::renderer
