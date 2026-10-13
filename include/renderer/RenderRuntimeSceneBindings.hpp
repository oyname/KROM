#pragma once

#include "renderer/RenderFeatureDataViews.hpp"
#include "renderer/RenderWorldViews.hpp"

namespace engine::renderer {

struct FrameGraphSceneBindings
{
    const RenderQueue* renderQueue = nullptr;
    RenderFrameDataView frameData{};
    const FrameConstants* perFrameConstantsData = nullptr;
};

} // namespace engine::renderer
