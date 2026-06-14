#pragma once

#include "serialization/SceneSerializer.hpp"

namespace engine::assets { class AssetPipeline; class AssetRegistry; }
namespace engine::ecs   { class World; }

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
                               ecs::World&             world);

// Combined addon entry points — call these from adapter Register()/Unregister().
void RegisterMeshRendererAddon(ecs::ComponentMetaRegistry* components,
                                serialization::SceneSerializer* serializer,
                                serialization::SceneDeserializer* deserializer);
void UnregisterMeshRendererAddon(ecs::ComponentMetaRegistry* components,
                                  serialization::SceneSerializer* serializer,
                                  serialization::SceneDeserializer* deserializer);

} // namespace engine::addons::mesh_renderer
