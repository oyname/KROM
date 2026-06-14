#pragma once

#include "renderer/RenderExtractionContext.hpp"
#include "renderer/RenderSceneSnapshot.hpp"

namespace engine::jobs { class JobSystem; }
namespace engine::assets { class AssetRegistry; }

namespace engine::renderer {

struct FrameExtractionStageResult
{
    RenderSceneSnapshot snapshot{};
};

struct FrameExtractionStageContext
{
    const ecs::World& world;
    const std::vector<const ISceneExtractionStep*>& extractionSteps;
    RenderSceneSnapshot& snapshot;
    const RenderView*     view = nullptr;
    jobs::JobSystem*     jobSystem = nullptr;
    const assets::AssetRegistry* assetRegistry = nullptr;

    FrameExtractionStageContext(const ecs::World& worldIn,
                                const std::vector<const ISceneExtractionStep*>& extractionStepsIn,
                                RenderSceneSnapshot& snapshotIn,
                                const RenderView* viewIn = nullptr,
                                jobs::JobSystem* jsIn = nullptr,
                                const assets::AssetRegistry* assetRegistryIn = nullptr) noexcept
        : world(worldIn)
        , extractionSteps(extractionStepsIn)
        , snapshot(snapshotIn)
        , view(viewIn)
        , jobSystem(jsIn)
        , assetRegistry(assetRegistryIn)
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
