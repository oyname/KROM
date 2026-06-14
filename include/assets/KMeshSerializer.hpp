#pragma once
#include "assets/AssetRegistry.hpp"
#include <filesystem>
#include <vector>

namespace engine::assets {

// Returns the .kmesh cache path for a given source path.
// Example: assets/Dragon/drache.glb -> assets/Dragon/drache.kmesh
[[nodiscard]] std::filesystem::path KMeshCachePath(const std::filesystem::path& sourcePath);

// Writes submeshes to cachePath as interleaved binary.
// Returns false on write error or if submeshes have incompatible layouts.
bool KMeshSave(const std::filesystem::path& cachePath, const std::vector<SubMeshData>& submeshes);

// Loads submeshes from cachePath into outSubmeshes.
// Returns false if the file does not exist, magic/version mismatch, or read error.
[[nodiscard]] bool KMeshTryLoad(const std::filesystem::path& cachePath,
                                std::vector<SubMeshData>&    outSubmeshes);

} // namespace engine::assets
