#pragma once
// =============================================================================
// KROM Engine - addons/prefab/PrefabSerialization.hpp
// =============================================================================
#include "ecs/ComponentMeta.hpp"

namespace engine::serialization {
class SceneSerializer;
class SceneDeserializer; // definiert in SceneSerializer.hpp
}

namespace engine::addons::prefab {

void RegisterPrefabSerializationHandlers(
    serialization::SceneSerializer& serializer);

void RegisterPrefabDeserializationHandlers(
    serialization::SceneDeserializer& deserializer);

void UnregisterPrefabSerializationHandlers(
    serialization::SceneSerializer& serializer);

void UnregisterPrefabDeserializationHandlers(
    serialization::SceneDeserializer& deserializer);

// Registriert Component-Meta + beide Serialisierungs-Handler auf einmal.
void RegisterPrefabAddon(
    ecs::ComponentMetaRegistry*       components,
    serialization::SceneSerializer*   serializer,
    serialization::SceneDeserializer* deserializer);

} // namespace engine::addons::prefab
