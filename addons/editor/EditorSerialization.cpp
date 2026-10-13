#include "addons/editor/EditorSerialization.hpp"

namespace engine::renderer::addons::editor {

void RegisterEditorSerializationHandlers(serialization::SceneSerializer& serializer)
{
    serializer.RegisterSerializer<EditorHierarchyComponent>(
        [](serialization::JsonWriter& w, const EditorHierarchyComponent& c) {
            w.BeginObject();
            w.WriteString("type", "EditorHierarchyComponent");
            w.WriteUint("order", c.order);
            w.EndObject();
        });

    // Kein Payload nötig — Anwesenheit der Komponente ist das Flag.
    serializer.RegisterSerializer<EditorPersistentComponent>(
        [](serialization::JsonWriter& w, const EditorPersistentComponent&) {
            w.BeginObject();
            w.WriteString("type", "EditorPersistentComponent");
            w.EndObject();
        });
}

void RegisterEditorDeserializationHandlers(serialization::SceneDeserializer& deserializer)
{
    deserializer.RegisterHandler<EditorHierarchyComponent>(
        [](const serialization::JsonValue& v, ecs::World& w, EntityID id) {
            EditorHierarchyComponent component{};
            if (const auto* order = v.Get("order"))
                component.order = order->AsUint();
            w.Add<EditorHierarchyComponent>(id, component);
        });

    deserializer.RegisterHandler<EditorPersistentComponent>(
        [](const serialization::JsonValue&, ecs::World& w, EntityID id) {
            w.Add<EditorPersistentComponent>(id, EditorPersistentComponent{});
        });
}

} // namespace engine::renderer::addons::editor
