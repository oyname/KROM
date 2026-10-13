#pragma once
// =============================================================================
// KROM Engine - renderer/IBLBaker.hpp
//
// Import / Bake layer for Image-Based Lighting.
// Owns all CPU-side IBL math: equirect → cube, irradiance convolution,
// GGX prefilter mip chain. Returns RGBA16F-packed IBLBakedData ready for
// IBLCacheSerializer::Write or IBLResourceLoader::Upload.
//
// No IDevice interaction. Fully backend-neutral.
// =============================================================================
#include "renderer/IBLTypes.hpp"
#include "renderer/Environment.hpp"
#include "assets/AssetRegistry.hpp"
#include <cstdint>

// Forward declaration — avoids pulling the full JobSystem header into every
// translation unit that includes IBLBaker.hpp.
namespace engine::jobs { class JobSystem; }

namespace engine::renderer {

struct IBLBakeParams
{
    uint32_t environmentSize    = 512u;
    uint32_t irradianceSize     = 64u;
    uint32_t irradianceSamples  = 64u;
    uint32_t prefilterBaseSize  = 128u;
    uint32_t prefilterMipCount  = kIBLPrefilterMipCount;  // shared constant (= 6)
    uint32_t prefilterSamples   = 128u;
    uint32_t brdfLutSize        = 256u;
    uint32_t brdfLutSamples     = 256u;

    // Optional: provide an initialized JobSystem to parallelize expensive bake
    // steps (environment conversion, irradiance convolution, prefilter mip chain). null → single-threaded.
    jobs::JobSystem* jobSystem  = nullptr;
};

class IBLBaker
{
public:
    // -------------------------------------------------------------------------
    // Hashing — deterministic 64-bit source identifiers for cache key.
    // -------------------------------------------------------------------------

    // Hash of raw asset pixel bytes + intensity (FNV-1a chained).
    [[nodiscard]] static uint64_t HashTextureSource(
        const assets::TextureAsset& asset, float intensity) noexcept;

    // Hash of each ProceduralSkyDesc field individually in a fixed order.
    // Does NOT hash raw struct bytes — padding-safe and layout-stable.
    [[nodiscard]] static uint64_t HashProceduralSky(
        const ProceduralSkyDesc& sky, float intensity) noexcept;

    // -------------------------------------------------------------------------
    // Baking — CPU convolution → RGBA16F-packed IBLBakedData.
    // Returns IsValid()==false on error.
    // Pass params.jobSystem != nullptr to enable multi-threaded baking.
    // -------------------------------------------------------------------------

    [[nodiscard]] static IBLBakedData BakeFromTexture(
        const assets::TextureAsset& asset,
        float                       intensity,
        const IBLBakeParams&        params = {});

    [[nodiscard]] static IBLBakedData BakeFromProceduralSky(
        const ProceduralSkyDesc& sky,
        float                    intensity,
        const IBLBakeParams&     params = {});
};

} // namespace engine::renderer
