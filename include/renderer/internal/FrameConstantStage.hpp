#pragma once

#include "platform/IPlatformTiming.hpp"
#include "renderer/FeatureRegistry.hpp"
#include "renderer/RenderFrameTypes.hpp"
#include "renderer/RenderWorld.hpp"

namespace engine::renderer {

struct FrameConstantStageContext
{
    math::Mat4 clipSpaceAdjustment = math::Mat4::Identity();
    uint32_t viewportWidth  = 0u;
    uint32_t viewportHeight = 0u;
    const RenderView& view;
    const platform::IPlatformTiming& timing;
    const RenderWorld& renderWorld;
    const std::vector<const IFrameConstantsContributor*>& contributors;
};

class FrameConstantStage
{
public:
    [[nodiscard]] bool PrepareFrameData(const FrameConstantStageContext& context,
                                        FrameConstantsResult& result) const;
};

} // namespace engine::renderer
