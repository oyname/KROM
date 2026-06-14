#pragma once

#include "renderer/RenderRuntimeMaterialBindings.hpp"
#include "renderer/RenderRuntimeResourceBindings.hpp"
#include "renderer/RenderRuntimeSceneBindings.hpp"

namespace engine::renderer {

struct FrameGraphRuntimeBindings
{
    FrameGraphSceneBindings scene{};
    FrameGraphResourceBindings resources{};
    FrameGraphMaterialBindings material{};
};

} // namespace engine::renderer
