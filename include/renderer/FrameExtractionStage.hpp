#pragma once

#include "renderer/FeatureRegistry.hpp"
#include "renderer/RenderFrameTypes.hpp"

namespace engine::jobs { class JobSystem; }

namespace engine::renderer {

struct FrameExtractionStageContext
{
    const ecs::World& world;
    const std::vector<const ISceneExtractionStep*>& extractionSteps;
    RenderSceneSnapshot* snapshot    = nullptr;
    RenderWorld*         renderWorld = nullptr;
    jobs::JobSystem*     jobSystem   = nullptr;

    FrameExtractionStageContext(const ecs::World& worldIn,
                                const std::vector<const ISceneExtractionStep*>& extractionStepsIn,
                                RenderSceneSnapshot& snapshotIn,
                                jobs::JobSystem* jsIn = nullptr) noexcept
        : world(worldIn)
        , extractionSteps(extractionStepsIn)
        , snapshot(&snapshotIn)
        , renderWorld(&snapshotIn.world)
        , jobSystem(jsIn)
    {
    }

    FrameExtractionStageContext(const ecs::World& worldIn,
                                const std::vector<const ISceneExtractionStep*>& extractionStepsIn,
                                RenderWorld& renderWorldIn,
                                jobs::JobSystem* jsIn = nullptr) noexcept
        : world(worldIn)
        , extractionSteps(extractionStepsIn)
        , snapshot(nullptr)
        , renderWorld(&renderWorldIn)
        , jobSystem(jsIn)
    {
    }
};

class FrameExtractionStage
{
public:
    [[nodiscard]] bool Execute(const FrameExtractionStageContext& context,
                               FrameExtractionStageResult& result) const;
};

} // namespace engine::renderer
