#pragma once
// =============================================================================
// KROM Engine - addons/probes/ProbeData.hpp
//
// Baked probe data and the .kprobe file format (binary, little endian):
//   char[4]  magic "KPRB"
//   uint32   formatVersion (kProbeFileVersion)
//   uint32   bakeVersion   (kProbeBakeVersion)
//   uint64   inputHash
//   float[3] capture position, float[3] extents
//   uint32   irradianceSize, prefilterBaseSize, prefilterMipCount
//   uint64   irradiance byte count, then RGBA16F cube (6 faces)
//   uint64   prefilter byte count, then RGBA16F cube mips (mip-major, 6 faces each)
// =============================================================================
#include "core/Math.hpp"
#include <cstdint>
#include <string>
#include <vector>

namespace engine::addons::probes {

inline constexpr uint32_t kProbeFileVersion = 1u;
// Increment when the capture or convolution changes; older files are ignored.
inline constexpr uint32_t kProbeBakeVersion = 1u;

struct ProbeBakedData
{
    uint64_t   inputHash = 0u;
    math::Vec3 capturePosition{ 0.f, 0.f, 0.f };
    math::Vec3 extents{ 0.f, 0.f, 0.f };
    uint32_t   irradianceSize = 0u;
    uint32_t   prefilterBaseSize = 0u;
    uint32_t   prefilterMipCount = 0u;
    std::vector<uint8_t> irradianceData;
    std::vector<uint8_t> prefilterData;

    [[nodiscard]] bool IsValid() const noexcept;
};

// path is UTF-8. Creates parent directories.
[[nodiscard]] bool WriteProbeFile(const std::string& path, const ProbeBakedData& data,
                                  std::string* outError = nullptr);
// Reads only the header; false if missing or of another format/bake version.
[[nodiscard]] bool ReadProbeFileInputHash(const std::string& path, uint64_t& outInputHash);
// Fails on wrong magic, format or bake version, or size mismatch.
[[nodiscard]] bool ReadProbeFile(const std::string& path, ProbeBakedData& outData,
                                 std::string* outError = nullptr);

} // namespace engine::addons::probes
