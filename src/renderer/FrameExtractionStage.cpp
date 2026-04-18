#include "renderer/internal/FrameExtractionStage.hpp"
#include "core/Debug.hpp"

namespace engine::renderer {

bool FrameExtractionStage::Execute(const FrameExtractionStageContext& context,
                                   FrameExtractionStageResult& /*result*/) const
{
    context.renderWorld.Clear();

    if (context.extractionSteps.empty())
    {
        Debug::LogWarning("FrameExtractionStage: no extraction steps registered");
        return true;
    }

    for (const ISceneExtractionStep* step : context.extractionSteps)
    {
        if (!step)
        {
            Debug::LogError("FrameExtractionStage: encountered null extraction step");
            return false;
        }
        step->Extract(context.world, context.renderWorld);
    }

    Debug::LogVerbose("FrameExtractionStage: %u proxies extracted",
        context.renderWorld.TotalProxyCount());

    return true;
}

} // namespace engine::renderer
