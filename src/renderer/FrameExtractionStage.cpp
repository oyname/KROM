#include "renderer/FrameExtractionStage.hpp"
#include "core/Debug.hpp"

namespace engine::renderer {

bool FrameExtractionStage::Execute(const FrameExtractionStageContext& context,
                                   FrameExtractionStageResult& /*result*/) const
{
    context.snapshot.Clear();

    if (context.extractionSteps.empty())
    {
        Debug::LogWarning("FrameExtractionStage: no extraction steps registered");
        return true;
    }

    SceneExtractionContext stepCtx(context.world, context.snapshot, context.view, context.jobSystem);
    stepCtx.assetRegistry = context.assetRegistry;

    for (const ISceneExtractionStep* step : context.extractionSteps)
    {
        if (!step)
        {
            Debug::LogError("FrameExtractionStage: encountered null extraction step");
            return false;
        }
        step->Extract(stepCtx);
    }

    Debug::LogVerbose("FrameExtractionStage: %u proxies extracted",
        context.snapshot.TotalProxyCount());

    return true;
}

} // namespace engine::renderer
