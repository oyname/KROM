#pragma once

#include "serialization/SceneSerializer.hpp"

namespace engine::addons::camera {

void RegisterCameraSerializationHandlers(serialization::SceneSerializer& serializer);
void UnregisterCameraSerializationHandlers(serialization::SceneSerializer& serializer);
void RegisterCameraDeserializationHandlers(serialization::SceneDeserializer& deserializer);
void UnregisterCameraDeserializationHandlers(serialization::SceneDeserializer& deserializer);

void RegisterCameraAddon(ecs::ComponentMetaRegistry* components,
                          serialization::SceneSerializer* serializer,
                          serialization::SceneDeserializer* deserializer);
void UnregisterCameraAddon(ecs::ComponentMetaRegistry* components,
                            serialization::SceneSerializer* serializer,
                            serialization::SceneDeserializer* deserializer);

} // namespace engine::addons::camera
