#pragma once
// =============================================================================
// KROM Engine - renderer/IBLCacheSerializer.hpp
//
// Binary .iblcache cache system for precomputed IBL data.
//
// File layout:
//   [FileHeader — 64 bytes]
//   [environment cube RGBA16F data]
//   [irradiance cube RGBA16F data]
//   [prefilter cube RGBA16F data, mip-major]
//
// Cache is considered valid only when ALL of the following match:
//   - magic number
//   - file format version (kFileVersion)
//   - bake algorithm version (kBakeVersion)
//   - source hash (encodes source pixels + intensity)
//   - cube sizes and mip count
// =============================================================================
#include "renderer/IBLTypes.hpp"
#include <cstdint>
#include <string>

namespace engine::renderer {

struct IBLCacheKey
{
    uint64_t sourceHash      = 0u;
    uint32_t environmentSize = 0u;
    uint32_t irradianceSize  = 0u;
    uint32_t prefilterSize   = 0u;
    uint32_t mipCount        = 0u;
};

class IBLCacheSerializer
{
public:
    // Increment when the baking algorithm changes to force cache regeneration.
    static constexpr uint32_t kBakeVersion = 5u;

    // Returns true if the file at path exists and its header matches key + kBakeVersion.
    [[nodiscard]] static bool IsValid(const std::string& path,
                                      const IBLCacheKey& key) noexcept;

    // Deserialize from file into outData. Returns false on any read or format error.
    [[nodiscard]] static bool Read(const std::string& path, IBLBakedData& outData);

    // Serialize data to file. Creates parent directories if needed.
    // Returns false on write error.
    [[nodiscard]] static bool Write(const std::string& path,
                                    const IBLBakedData& data,
                                    const IBLCacheKey& key);

    // Opportunistic maintenance: remove malformed or version-incompatible
    // .iblcache files from the given directory. Does NOT delete valid cache
    // files for other sources. Returns the number of deleted files.
    [[nodiscard]] static uint32_t CleanupDirectory(const std::string& directory) noexcept;
};

} // namespace engine::renderer
