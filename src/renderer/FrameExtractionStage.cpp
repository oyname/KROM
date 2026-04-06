#include "renderer/FrameExtractionStage.hpp"
#include "core/Debug.hpp"
#include "renderer/ECSExtractor.hpp"

namespace engine::renderer {

bool FrameExtractionStage::Execute(const FrameExtractionStageContext& context,
                                   FrameExtractionStageResult& result) const
{
    result.snapshot = {};
    ECSExtractor::BeginSnapshot(result.snapshot);

    for (const ISceneExtractionStep* step : context.extractionSteps)
    {
        if (!step)
        {
            Debug::LogError("FrameExtractionStage: encountered null extraction step");
            return false;
        }

        const size_t renderableOffset = result.snapshot.renderables.size();
        const size_t lightOffset = result.snapshot.lights.size();
        step->Extract(context.world, result.snapshot);
        result.snapshot.RecordContribution(step->GetName(),
                                           renderableOffset,
                                           lightOffset,
                                           result.snapshot.renderables.size() - renderableOffset,
                                           result.snapshot.lights.size() - lightOffset);
    }

    return true;
}

} // namespace engine::renderer
