#pragma once
#include <cstdint>
#include <span>
#include <vector>

namespace engine::addons::shadow {

// Packs the shadow views of all requests into one square atlas. Each request
// gets square tiles of one size (a power of two); requests may have different
// sizes, so a sun with 2048 cascades and point lights with 1024 faces share
// one atlas without the sun being cut to the size of the smallest tile.
struct ShadowAtlasLayoutRequest
{
    uint32_t desiredTileSize = 0u;   // settings.resolution, clamped to the atlas cap
    uint32_t viewCount = 0u;
    bool directional = false;        // shrunk last when the atlas is full
};

struct ShadowAtlasTile
{
    uint32_t x = 0u;
    uint32_t y = 0u;
};

struct ShadowAtlasLayout
{
    uint32_t atlasSize = 0u;                         // square, in texels; 0 = nothing to pack
    std::vector<uint32_t> tileSizes;                 // per request
    std::vector<std::vector<ShadowAtlasTile>> tiles; // per request, one per view
};

// Smallest atlas (steps of 1024 above 1024, powers of two below) that holds all
// tiles, at least minAtlasSize and at most maxAtlasSize. When the tiles do not
// fit at maxAtlasSize, the largest tile size is halved, local lights before
// directional ones, until they fit.
[[nodiscard]] ShadowAtlasLayout BuildShadowAtlasLayout(std::span<const ShadowAtlasLayoutRequest> requests,
                                                       uint32_t maxAtlasSize,
                                                       uint32_t minAtlasSize = 0u);

} // namespace engine::addons::shadow
