#pragma once
// =============================================================================
// KROM Engine - assets/AssetImporter.hpp
// Formatneutrale Import-Schnittstelle.
//
// Core kennt nur AssetImporter und ImportedAssetBundle.
// Format-Addons (gltf, obj, fbx) implementieren IAssetImporter
// und schreiben in die standard Engine-Typen.
// =============================================================================
#include "assets/AssetRegistry.hpp"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace engine::assets {

struct ImportedSceneNode
{
    std::string name;
    int32_t     parentIndex = -1;
    int32_t     meshIndex   = -1;
    int32_t     skinIndex   = -1;
    math::Vec3  translation{0.f, 0.f, 0.f};
    math::Quat  rotation = math::Quat::Identity();
    math::Vec3  scale{1.f, 1.f, 1.f};
};

// Enthält alle aus einer Datei importierten Assets und ihre Beziehungen.
struct ImportedAssetBundle
{
    std::vector<MeshAsset>      meshes;
    std::vector<MaterialAsset>  materials;
    std::vector<SkeletonAsset>  skeletons;
    std::vector<AnimationClip>  animations;
    std::vector<ImportedSceneNode> nodes;

    // Parallel zu meshes: Index in skeletons wenn das Mesh geskinnt ist, -1 sonst.
    std::vector<int32_t>        meshSkinIndex;

    std::string error;

    [[nodiscard]] bool Ok() const noexcept { return error.empty(); }
};

// Formatneutrale Importer-Schnittstelle.
// Implementierungen liegen in Addons (addons/gltf, addons/obj, …).
class IAssetImporter
{
public:
    virtual ~IAssetImporter() = default;

    // Gibt true zurück wenn dieser Importer die Dateiendung verarbeiten kann.
    // extension ist normalisiert: ".gltf", ".glb", ".obj", etc.
    [[nodiscard]] virtual bool CanImport(std::string_view extension) const = 0;

    // Importiert die Datei und gibt ein Bundle zurück.
    // Bei Fehler: bundle.Ok() == false, bundle.error enthält Diagnose.
    [[nodiscard]] virtual ImportedAssetBundle Import(std::string_view path) = 0;
};

} // namespace engine::assets
