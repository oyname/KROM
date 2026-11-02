#pragma once

#include "serialization/SceneSerializer.hpp"
#include "addons/lighting/LightingComponents.hpp"

namespace engine::addons::lighting {

void WriteLightingFields(serialization::JsonWriter&, const LightComponent&);
LightComponent ReadLightingFields(const serialization::JsonValue&);

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
