#pragma once
#include "serialization/SceneSerializer.hpp"

namespace engine::physics {

void RegisterPhysicsSerializationHandlers(
    serialization::SceneSerializer& serializer);

void UnregisterPhysicsSerializationHandlers(
    serialization::SceneSerializer& serializer);

void RegisterPhysicsDeserializationHandlers(
    serialization::SceneDeserializer& deserializer);

void UnregisterPhysicsDeserializationHandlers(
    serialization::SceneDeserializer& deserializer);

} // namespace engine::physics
