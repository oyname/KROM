#pragma once

#include "serialization/SceneSerializer.hpp"
#include "addons/camera/CameraComponents.hpp"

namespace engine::addons::camera {

void WriteCameraFields(serialization::JsonWriter&, const CameraComponent&);
CameraComponent ReadCameraFields(const serialization::JsonValue&);

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
