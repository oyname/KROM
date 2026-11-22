#pragma once
#include "addons/lighting/ShadowTypes.hpp"
#include "ecs/World.hpp"
#include "renderer/RenderExtractionContext.hpp"
#include <memory>
#include <vector>

namespace engine::addons::shadow {

// Shadow-map quality of the render quality level (project / player setting).
// When set, it replaces resolution and filter of every light: lights keep only
// scene values such as distance, cascades, bias and strength (as in Unreal).
// The application owns it and may change it between frames.
struct ShadowQualitySettings
{
    uint32_t     directionalResolution = 2048u;   // tile edge of each sun cascade
    uint32_t     localResolution = 1024u;         // tile edge of each point/spot view
    ShadowFilter filter = ShadowFilter::Tent5x5;
};

struct ShadowExtractionConfig
{
    uint32_t maxShadowLights = 16u;
    uint32_t maxShadowViews = 64u;
    // Square atlas cap in texels. 6144 (D32, about 150 MB) holds four 2048 sun
    // cascades next to the tiles of local lights; the atlas only grows that
    // large when the lights need it.
    uint32_t maxShadowAtlasResolution = 6144u;
    uint32_t hysteresisFrames = 3u;
    std::shared_ptr<const ShadowQualitySettings> quality;   // null: per-light values
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
