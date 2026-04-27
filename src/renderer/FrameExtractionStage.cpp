#include "renderer/FrameExtractionStage.hpp"
#include "core/Debug.hpp"

namespace engine::renderer {

bool FrameExtractionStage::Execute(const FrameExtractionStageContext& context,
                                   FrameExtractionStageResult& /*result*/) const
{
    RenderSceneSnapshot localSnapshot{};
    RenderSceneSnapshot* targetSnapshot = context.snapshot;
    if (!targetSnapshot && !context.renderWorld)
    {
        Debug::LogError("FrameExtractionStage: no render target provided");
        return false;
    }

    if (targetSnapshot)
        targetSnapshot->Clear();
    else
        localSnapshot.Clear();

    if (context.extractionSteps.empty())
    {
        Debug::LogWarning("FrameExtractionStage: no extraction steps registered");
        return true;
    }

    SceneExtractionContext stepCtx =
        targetSnapshot
        ? SceneExtractionContext(context.world, *targetSnapshot, context.jobSystem)
        : SceneExtractionContext(context.world, localSnapshot,   context.jobSystem);

    for (const ISceneExtractionStep* step : context.extractionSteps)
    {
        if (!step)
        {
            Debug::LogError("FrameExtractionStage: encountered null extraction step");
            return false;
        }
        step->Extract(stepCtx);
    }

    if (!targetSnapshot && context.renderWorld)
        *context.renderWorld = std::move(localSnapshot.world);

    const RenderWorld* debugWorld = targetSnapshot ? &targetSnapshot->GetWorld() : context.renderWorld;
    Debug::LogVerbose("FrameExtractionStage: %u proxies extracted",
        debugWorld ? debugWorld->TotalProxyCount() : 0u);

    return true;
}

} // namespace engine::renderer
