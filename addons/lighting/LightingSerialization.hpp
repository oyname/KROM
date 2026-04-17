#pragma once

#include "serialization/SceneSerializer.hpp"

namespace engine::addons::lighting {

void RegisterLightingSerializationHandlers(serialization::SceneSerializer& serializer);
void RegisterLightingDeserializationHandlers(serialization::SceneDeserializer& deserializer);

} // namespace engine::addons::lighting
