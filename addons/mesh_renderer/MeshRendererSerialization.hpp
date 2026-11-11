#pragma once

#include "serialization/SceneSerializer.hpp"
#include <span>

namespace engine::assets { class AssetPipeline; class AssetRegistry; }
namespace engine::ecs   { class World; }
namespace engine::animation { struct ModelClipSettings; }

namespace engine::addons::mesh_renderer {

void RegisterMeshRendererSerializationHandlers(serialization::SceneSerializer& serializer);
void UnregisterMeshRendererSerializationHandlers(serialization::SceneSerializer& serializer);
void RegisterMeshRendererDeserializationHandlers(serialization::SceneDeserializer& deserializer);
void UnregisterMeshRendererDeserializationHandlers(serialization::SceneDeserializer& deserializer);

// Laeuft nach LoadEditorScene / DeserializeFromJson und re-laedt alle Mesh-Assets
// anhand ihrer in MeshComponent.meshAssetPath gespeicherten Pfade.
// Fragment-Pfade ("model.glb#mesh/0") werden via ImportBundle re-importiert;
// direkte Pfade via LoadMesh.
void ResolveMeshAssetBindings(assets::AssetPipeline&  pipeline,
                               assets::AssetRegistry&  registry,
                               ecs::World&             world,
                               std::span<const EntityID> entities = {});

// Combined addon entry points — call these from adapter Register()/Unregister().
// Skeletons and animation clips of skinned meshes ("<model>#skeleton/<n>",
// "<model>#animation/<n>"): resolves SkinComponent / AnimationPlayerComponent
// handles from their asset paths, importing the model when needed. Runs as part
// of ResolveMeshAssetBindings, so every scene/snapshot/prefab load covers it.
void ResolveAnimationAssetBindings(assets::AssetPipeline&  pipeline,
                                   assets::AssetRegistry&  registry,
                                   ecs::World&             world,
                                   std::span<const EntityID> entities = {});

// Imports the model behind "<model>#<kind>/<n>" and registers all its skeletons
// and clips under "<model>#skeleton/<i>" / "<model>#animation/<i>" (the keys
// BuildPrefabFromImportedBundle uses), plus "<model>#clip/<id>" from the
// model's clip settings. False if the model cannot be imported.
// replaceExisting: skeletons and clips registered before are overwritten in
// place (the model changed on disk); otherwise they are kept.
bool ImportModelAnimationAssets(assets::AssetPipeline& pipeline,
                                assets::AssetRegistry& registry,
                                const std::string&     assetPath,
                                bool                   replaceExisting = false);

// Clip settings beside a model ("<model>.clips", see animation/ModelClipSettings.hpp).
// A missing file is not an error and yields empty settings; false only for an
// unreadable or invalid file (reason in *error).
bool ReadModelClipSettings(const assets::AssetPipeline&     pipeline,
                           const std::string&               modelPath,
                           ::engine::animation::ModelClipSettings&    out,
                           std::string*                     error = nullptr);

// Registers "<model>#clip/<id>" for every entry, built from the model's clips
// already registered under "<model>#animation/<i>". Clips registered before are
// replaced in place, so handles stay valid when the settings change.
void RegisterModelClips(assets::AssetRegistry&              registry,
                        const std::string&                  modelPath,
                        const ::engine::animation::ModelClipSettings& settings);

// "<base>#<fragment>" with base below the asset root -> relative base.
[[nodiscard]] std::string ToAssetRelativeFragmentPath(const assets::AssetPipeline& pipeline,
                                                      const std::string& assetPath);

void RegisterMeshRendererAddon(ecs::ComponentMetaRegistry* components,
                                serialization::SceneSerializer* serializer,
                                serialization::SceneDeserializer* deserializer);
void UnregisterMeshRendererAddon(ecs::ComponentMetaRegistry* components,
                                  serialization::SceneSerializer* serializer,
                                  serialization::SceneDeserializer* deserializer);

} // namespace engine::addons::mesh_renderer
