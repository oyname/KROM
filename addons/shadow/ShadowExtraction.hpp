#pragma once
#include "ecs/World.hpp"
#include "renderer/RenderExtractionContext.hpp"
#include <vector>

namespace engine::addons::shadow {

struct ShadowExtractionConfig
{
    uint32_t maxShadowLights = 16u;
    uint32_t maxShadowViews = 64u;
    // Square atlas cap in texels. 6144 (D32, about 150 MB) holds four 2048 sun
    // cascades next to the tiles of local lights; the atlas only grows that
    // large when the lights need it.
    uint32_t maxShadowAtlasResolution = 6144u;
    uint32_t hysteresisFrames = 3u;
};

// Frames the atlas must be larger than needed before it shrinks.
inline constexpr uint32_t kShadowAtlasShrinkDelayFrames = 120u;

struct ShadowExtractionTemporalState
{
    std::vector<std::pair<EntityID, uint32_t>> localShadowRetention;
    uint32_t stableAtlasSize = 0u;
    uint32_t framesAtlasTooLarge = 0u;
};

// Baut einen gemeinsamen Shadow-Plan fuer alle relevanten Schattenlichter.
// Der aktuelle Renderpfad verwendet daraus einen ausgewaehlten Request, die
// CPU-Datenhaltung bleibt aber bereits lichttyp-uebergreifend.
void ExtractShadow(const renderer::SceneExtractionContext& context,
                   ShadowExtractionConfig config = {},
                   ShadowExtractionTemporalState* temporalState = nullptr);

} // namespace engine::addons::shadow
