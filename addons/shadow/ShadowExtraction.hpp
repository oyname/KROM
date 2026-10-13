#pragma once
#include "ecs/World.hpp"
#include "renderer/RenderExtractionContext.hpp"
#include <vector>

namespace engine::addons::shadow {

struct ShadowExtractionConfig
{
    uint32_t maxShadowLights = 16u;
    uint32_t maxShadowViews = 64u;
    uint32_t maxShadowAtlasResolution = 4096u;
    uint32_t hysteresisFrames = 3u;
};

struct ShadowExtractionTemporalState
{
    std::vector<std::pair<EntityID, uint32_t>> localShadowRetention;
    uint32_t stableAtlasViewCount = 0u;
};

// Baut einen gemeinsamen Shadow-Plan fuer alle relevanten Schattenlichter.
// Der aktuelle Renderpfad verwendet daraus einen ausgewaehlten Request, die
// CPU-Datenhaltung bleibt aber bereits lichttyp-uebergreifend.
void ExtractShadow(const renderer::SceneExtractionContext& context,
                   ShadowExtractionConfig config = {},
                   ShadowExtractionTemporalState* temporalState = nullptr);

} // namespace engine::addons::shadow
