#pragma once
// =============================================================================
// KROM Engine - renderer/IBLTypes.hpp
// CPU-side data structures for precomputed IBL cube map data.
//
// IBLBakedData is the canonical in-memory representation after baking or after
// deserializing a .iblcache file. It stores all faces in RGBA16F packed as
// raw bytes so it can be uploaded directly via IDevice::UploadTextureData.
//
// Memory layout (per cube):
//   6 faces stored contiguously, face-major order (face 0..5).
//   Prefilter data is mip-major: [mip0: 6 faces][mip1: 6 faces]...
//   Each face of mip m has (prefilterBaseSize >> m) * (prefilterBaseSize >> m)
//   pixels, each pixel being 4 x uint16_t (RGBA16F).
// =============================================================================
#include <cstdint>
#include <vector>

namespace engine::renderer {

struct IBLBakedData
{
    uint32_t environmentSize   = 0u;  // cube face edge in pixels (e.g. 512)
    uint32_t irradianceSize    = 0u;  // cube face edge in pixels (e.g. 64)
    uint32_t prefilterBaseSize = 0u;  // cube face edge at mip 0 (e.g. 128)
    uint32_t prefilterMipCount = 0u;

    // RGBA16F bytes: 6 * size * size * 4 * sizeof(uint16_t)
    std::vector<uint8_t> environmentData;
    std::vector<uint8_t> irradianceData;
    // [mip0: 6*base^2*8 bytes][mip1: 6*(base/2)^2*8 bytes]...
    std::vector<uint8_t> prefilterData;

    [[nodiscard]] bool IsValid() const noexcept
    {
        return environmentSize   > 0u
            && irradianceSize    > 0u
            && prefilterBaseSize > 0u
            && prefilterMipCount > 0u
            && !environmentData.empty()
            && !irradianceData.empty()
            && !prefilterData.empty();
    }
};

} // namespace engine::renderer
