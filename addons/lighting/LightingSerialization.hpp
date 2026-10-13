#pragma once

#include "serialization/SceneSerializer.hpp"

namespace engine::addons::lighting {

void RegisterLightingSerializationHandlers(serialization::SceneSerializer& serializer);
void UnregisterLightingSerializationHandlers(serialization::SceneSerializer& serializer);
void RegisterLightingDeserializationHandlers(serialization::SceneDeserializer& deserializer);
void UnregisterLightingDeserializationHandlers(serialization::SceneDeserializer& deserializer);

void RegisterLightingAddon(ecs::ComponentMetaRegistry* components,
                            serialization::SceneSerializer* serializer,
                            serialization::SceneDeserializer* deserializer);
void UnregisterLightingAddon(ecs::ComponentMetaRegistry* components,
                              serialization::SceneSerializer* serializer,
                              serialization::SceneDeserializer* deserializer);

} // namespace engine::addons::lighting
