#pragma once

#include "platform/IPlatformTiming.hpp"
#include "renderer/FeatureRegistry.hpp"
#include "renderer/RenderFrameTypes.hpp"
#include "renderer/RenderWorld.hpp"

namespace engine::renderer {

struct FrameConstantStageContext
{
    math::Mat4 projectionClipSpaceAdjustment = math::Mat4::Identity();
    math::Mat4 shadowClipSpaceAdjustment = math::Mat4::Identity();
    uint32_t viewportWidth  = 0u;
    uint32_t viewportHeight = 0u;
    const RenderView& view;
    const platform::IPlatformTiming& timing;
    const RenderWorld& renderWorld;
    const std::vector<const IFrameConstantsContributor*>& contributors;

    FrameConstantStageContext(const math::Mat4& projectionAdjustment,
                              const math::Mat4& shadowAdjustment,
                              uint32_t viewportWidthIn,
                              uint32_t viewportHeightIn,
                              const RenderView& viewIn,
                              const platform::IPlatformTiming& timingIn,
                              const RenderWorld& renderWorldIn,
                              const std::vector<const IFrameConstantsContributor*>& contributorsIn)
        : projectionClipSpaceAdjustment(projectionAdjustment)
        , shadowClipSpaceAdjustment(shadowAdjustment)
        , viewportWidth(viewportWidthIn)
        , viewportHeight(viewportHeightIn)
        , view(viewIn)
        , timing(timingIn)
        , renderWorld(renderWorldIn)
        , contributors(contributorsIn)
    {
    }
};

class FrameConstantStage
{
public:
    [[nodiscard]] bool PrepareFrameData(const FrameConstantStageContext& context,
                                        FrameConstantsResult& result) const;
};

} // namespace engine::renderer
