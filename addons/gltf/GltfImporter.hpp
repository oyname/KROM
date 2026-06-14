#pragma once
// =============================================================================
// KROM Engine - addons/gltf/GltfImporter.hpp
// IAssetImporter-Implementierung fuer .gltf und .glb (cgltf-basiert).
// =============================================================================
#include "assets/AssetImporter.hpp"

namespace engine::addons::gltf {

class GltfImporter : public assets::IAssetImporter
{
public:
    // Akzeptiert ".gltf" und ".glb".
    [[nodiscard]] bool CanImport(std::string_view extension) const override;

    // Importiert Meshes, Skeleton (skins[0]) und Animationen aus path.
    // Kein ECS, keine Registry: gibt reines ImportedAssetBundle zurueck.
    [[nodiscard]] assets::ImportedAssetBundle Import(std::string_view path) override;
};

} // namespace engine::addons::gltf
