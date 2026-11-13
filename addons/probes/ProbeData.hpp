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

// Probe id from a baked file name ("<16 hex digits>.kprobe"); 0 if it is not one.
[[nodiscard]] uint64_t ProbeIdFromFileName(const std::string& fileNameUtf8);

// Deletes .kprobe files in <assetsRoot>/Baked/<sceneName>/ that no probe uses:
// their id is neither in usedIds (the probes in the open world) nor written in
// any scene or prefab file (.json, .kscene, .prefab) under assetsRoot, so a
// probe of a scene that is not open keeps its file. Returns the deleted paths
// (UTF-8, relative to assetsRoot).
std::vector<std::string> DeleteUnusedProbeFiles(const std::string& assetsRootUtf8,
                                                const std::string& sceneNameUtf8,
                                                const std::vector<uint64_t>& usedIds);

} // namespace engine::addons::probes
