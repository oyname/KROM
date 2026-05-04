#pragma once

#include "serialization/SceneSerializer.hpp"

namespace engine::addons::mesh_renderer {

void RegisterMeshRendererSerializationHandlers(serialization::SceneSerializer& serializer);
void UnregisterMeshRendererSerializationHandlers(serialization::SceneSerializer& serializer);
void RegisterMeshRendererDeserializationHandlers(serialization::SceneDeserializer& deserializer);
void UnregisterMeshRendererDeserializationHandlers(serialization::SceneDeserializer& deserializer);

// Combined addon entry points — call these from adapter Register()/Unregister().
void RegisterMeshRendererAddon(ecs::ComponentMetaRegistry* components,
                                serialization::SceneSerializer* serializer,
                                serialization::SceneDeserializer* deserializer);
void UnregisterMeshRendererAddon(ecs::ComponentMetaRegistry* components,
                                  serialization::SceneSerializer* serializer,
                                  serialization::SceneDeserializer* deserializer);

} // namespace engine::addons::mesh_renderer
