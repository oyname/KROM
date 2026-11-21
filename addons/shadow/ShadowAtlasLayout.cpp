#include "addons/shadow/ShadowAtlasLayout.hpp"
#include <algorithm>
#include <numeric>

namespace engine::addons::shadow {
namespace {

constexpr uint32_t kMinTileSize = 64u;

[[nodiscard]] uint32_t FloorPowerOfTwo(uint32_t value) noexcept
{
    uint32_t result = 1u;
    while (result <= value / 2u)
        result *= 2u;
    return result;
}

struct FreeRect
{
    uint32_t x = 0u;
    uint32_t y = 0u;
    uint32_t w = 0u;
    uint32_t h = 0u;
};

// Guillotine packing of square tiles, largest first. Returns false if a tile
// does not fit into the atlas.
[[nodiscard]] bool PackTiles(uint32_t atlasSize,
                             std::span<const ShadowAtlasLayoutRequest> requests,
                             const std::vector<uint32_t>& tileSizes,
                             std::vector<std::vector<ShadowAtlasTile>>& outTiles)
{
    std::vector<size_t> order(requests.size());
    std::iota(order.begin(), order.end(), size_t{0});
    std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) {
        return tileSizes[a] > tileSizes[b];
    });

    outTiles.assign(requests.size(), {});
    std::vector<FreeRect> freeRects{ FreeRect{0u, 0u, atlasSize, atlasSize} };
    for (size_t requestIndex : order)
    {
        const uint32_t size = tileSizes[requestIndex];
        outTiles[requestIndex].reserve(requests[requestIndex].viewCount);
        for (uint32_t view = 0u; view < requests[requestIndex].viewCount; ++view)
        {
            // Best short-side fit keeps large free areas for the next tiles.
            size_t best = freeRects.size();
            uint32_t bestLeftover = UINT32_MAX;
            for (size_t i = 0; i < freeRects.size(); ++i)
            {
                const FreeRect& r = freeRects[i];
                if (r.w < size || r.h < size)
                    continue;
                const uint32_t leftover = std::min(r.w - size, r.h - size);
                if (leftover < bestLeftover)
                {
                    bestLeftover = leftover;
                    best = i;
                }
            }
            if (best == freeRects.size())
                return false;

            const FreeRect r = freeRects[best];
            freeRects.erase(freeRects.begin() + static_cast<std::ptrdiff_t>(best));
            outTiles[requestIndex].push_back(ShadowAtlasTile{r.x, r.y});
            // Split the rest along the longer leftover side.
            const uint32_t restW = r.w - size;
            const uint32_t restH = r.h - size;
            if (restW > restH)
            {
                if (restW > 0u) freeRects.push_back({r.x + size, r.y, restW, r.h});
                if (restH > 0u) freeRects.push_back({r.x, r.y + size, size, restH});
            }
            else
            {
                if (restH > 0u) freeRects.push_back({r.x, r.y + size, r.w, restH});
                if (restW > 0u) freeRects.push_back({r.x + size, r.y, restW, size});
            }
        }
    }
    return true;
}

[[nodiscard]] uint32_t NextAtlasSize(uint32_t size) noexcept
{
    return size < 1024u ? size * 2u : size + 1024u;
}

} // namespace

ShadowAtlasLayout BuildShadowAtlasLayout(std::span<const ShadowAtlasLayoutRequest> requests,
                                         uint32_t maxAtlasSize,
                                         uint32_t minAtlasSize)
{
    ShadowAtlasLayout layout{};
    const uint32_t cap = std::max(maxAtlasSize, kMinTileSize);
    std::vector<uint32_t> tileSizes(requests.size(), 0u);
    bool anyView = false;
    for (size_t i = 0; i < requests.size(); ++i)
    {
        const uint32_t desired = std::clamp(requests[i].desiredTileSize, kMinTileSize, cap);
        tileSizes[i] = FloorPowerOfTwo(desired);
        anyView = anyView || requests[i].viewCount > 0u;
    }
    if (!anyView)
        return layout;

    for (;;)
    {
        uint32_t largest = 0u;
        for (size_t i = 0; i < requests.size(); ++i)
            if (requests[i].viewCount > 0u)
                largest = std::max(largest, tileSizes[i]);

        uint32_t size = std::min(cap, std::max(largest, minAtlasSize));
        for (;;)
        {
            if (PackTiles(size, requests, tileSizes, layout.tiles))
            {
                layout.atlasSize = size;
                layout.tileSizes = tileSizes;
                return layout;
            }
            if (size >= cap)
                break;
            size = std::min(cap, NextAtlasSize(size));
        }

        // Does not fit at the cap: halve one tile size. The sun has priority:
        // local lights shrink while they are larger than a quarter of the
        // largest directional tile; then the largest tile shrinks. Among equal
        // sizes the last request shrinks first.
        auto largestOf = [&](bool directional, size_t& index) {
            uint32_t best = 0u;
            index = requests.size();
            for (size_t i = requests.size(); i-- > 0u;)
            {
                if (requests[i].viewCount == 0u || requests[i].directional != directional)
                    continue;
                if (tileSizes[i] > best && tileSizes[i] > kMinTileSize)
                {
                    best = tileSizes[i];
                    index = i;
                }
            }
            return best;
        };
        size_t localIndex = requests.size();
        size_t directionalIndex = requests.size();
        const uint32_t localSize = largestOf(false, localIndex);
        const uint32_t directionalSize = largestOf(true, directionalIndex);
        size_t victim = requests.size();
        if (localIndex != requests.size() && (directionalIndex == requests.size() || localSize > directionalSize / 4u))
            victim = localIndex;
        else if (directionalIndex != requests.size())
            victim = directionalIndex;
        else
            victim = localIndex;
        if (victim == requests.size())
        {
            // Every tile is at the minimum and still too many: give up.
            layout = {};
            return layout;
        }
        tileSizes[victim] /= 2u;
    }
}

} // namespace engine::addons::shadow
