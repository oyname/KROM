#pragma once

#include "serialization/SceneSerializer.hpp"

namespace engine::addons::mesh_renderer {

void RegisterMeshRendererSerializationHandlers(serialization::SceneSerializer& serializer);
void RegisterMeshRendererDeserializationHandlers(serialization::SceneDeserializer& deserializer);

} // namespace engine::addons::mesh_renderer
