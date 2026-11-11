#pragma once
// =============================================================================
// KROM Engine - addons/editor/EditorModelClips.hpp
// Clips of model files (*.glb/*.gltf) for the editor: the animation names come
// straight from the file's JSON (no import), the settings from "<model>.clips"
// (animation/ModelClipSettings.hpp). Asset browser, animator and animation
// window all read them through here.
// =============================================================================
#include "animation/ModelClipSettings.hpp"

#include <filesystem>
#include <string>
#include <vector>

namespace engine::assets { class AssetPipeline; class AssetRegistry; }

namespace engine::renderer::addons::editor {

// Animation names of a glTF/glb file as the importer names them (an unnamed
// animation is "animation"), in file order. Empty if the file has none or
// cannot be read.
[[nodiscard]] std::vector<std::string> ReadGltfAnimationNames(const std::filesystem::path& file);

struct ModelClipList
{
    std::string                        modelPath;   // relative to Assets
    animation::ModelClipSettings       settings;    // completed by SyncModelClipSettings
    std::vector<std::string>           sources;     // animation names in the model
    std::string                        error;       // settings file could not be read
    bool                               unsaved = false; // entries added that are not in the file yet

    [[nodiscard]] bool SourceExists(const std::string& source) const;
    [[nodiscard]] const animation::ModelClipEntry* FindByAssetPath(const std::string& assetPath) const;
};

// Clips of a model (path relative to the asset root). Adds an entry for every
// animation without one, in memory only (see PersistModelClips). Cached until
// the model or its settings change.
[[nodiscard]] const ModelClipList& GetModelClips(const std::filesystem::path& assetRoot, const std::string& modelPath);

// Writes the entries GetModelClips added, so their ids are stable. Call before a
// clip id is stored anywhere (animator, drag and drop, animation window); a model
// that is only looked at gets no settings file.
bool PersistModelClips(const std::filesystem::path& assetRoot, const std::string& modelPath);

// Writes "<model>.clips" and updates the cached list in place.
bool SaveModelClipSettings(const std::filesystem::path& assetRoot, const std::string& modelPath,
                           const animation::ModelClipSettings& settings);

// Makes the registry match the files: imports the model's animations (again,
// in place, when the model changed on disk) and registers "<model>#clip/<id>".
void RefreshModelClipAssets(assets::AssetPipeline& pipeline, assets::AssetRegistry& registry,
                            const std::string& modelPath, const animation::ModelClipSettings& settings);

// Label of a clip reference for the UI: the clip name for "<model>#clip/<id>",
// the animation name for the older "<model>#animation/<n>"; empty otherwise.
[[nodiscard]] std::string ModelClipDisplayName(const std::filesystem::path& assetRoot, const std::string& assetPath);

} // namespace engine::renderer::addons::editor
