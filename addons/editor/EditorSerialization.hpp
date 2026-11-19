#pragma once

#include "addons/editor/EditorComponents.hpp"
#include "serialization/SceneSerializer.hpp"

namespace engine::renderer::addons::editor {

struct EditorState;

void RegisterEditorSerializationHandlers(serialization::SceneSerializer& serializer);
void RegisterEditorDeserializationHandlers(serialization::SceneDeserializer& deserializer);
std::string SerializeEditorHierarchyState(const EditorState& state);
void DeserializeEditorHierarchyState(const serialization::JsonValue& value, EditorState& state);

} // namespace engine::renderer::addons::editor
