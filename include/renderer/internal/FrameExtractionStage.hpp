#pragma once

#include "renderer/FeatureRegistry.hpp"
#include "renderer/RenderFrameTypes.hpp"

namespace engine::renderer {

struct FrameExtractionStageContext
{
    const ecs::World& world;
    const std::vector<const ISceneExtractionStep*>& extractionSteps;
    RenderWorld& renderWorld;
};

class FrameExtractionStage
{
public:
    [[nodiscard]] bool Execute(const FrameExtractionStageContext& context,
                               FrameExtractionStageResult& result) const;
};

} // namespace engine::renderer
