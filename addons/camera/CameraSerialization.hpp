#pragma once

#include "serialization/SceneSerializer.hpp"

namespace engine::addons::camera {

void RegisterCameraSerializationHandlers(serialization::SceneSerializer& serializer);
void RegisterCameraDeserializationHandlers(serialization::SceneDeserializer& deserializer);

} // namespace engine::addons::camera
