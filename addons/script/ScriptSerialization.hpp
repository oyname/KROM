#pragma once
#include "serialization/SceneSerializer.hpp"

namespace engine::script { class ScriptRegistry; }

namespace engine::script {

void RegisterScriptSerializationHandlers(
    serialization::SceneSerializer& serializer);

void UnregisterScriptSerializationHandlers(
    serialization::SceneSerializer& serializer);

// registry kann null sein — dann werden Klassennamen ohne Live-Instanz gespeichert
// (fuer Editor-Inspektor / Undo-Redo).
void RegisterScriptDeserializationHandlers(
    serialization::SceneDeserializer& deserializer,
    ScriptRegistry*                   registry);

void UnregisterScriptDeserializationHandlers(
    serialization::SceneDeserializer& deserializer);

void ResolveScriptEntityReferences(
    ecs::World& world,
    const ScriptRegistry& registry);

} // namespace engine::script
