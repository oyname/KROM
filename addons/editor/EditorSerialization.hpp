#pragma once

#include "addons/editor/EditorComponents.hpp"
#include "serialization/SceneSerializer.hpp"

namespace engine::renderer::addons::editor {

void RegisterEditorSerializationHandlers(serialization::SceneSerializer& serializer);
void RegisterEditorDeserializationHandlers(serialization::SceneDeserializer& deserializer);

} // namespace engine::renderer::addons::editor
