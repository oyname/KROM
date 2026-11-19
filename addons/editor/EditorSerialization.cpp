#include "addons/editor/EditorSerialization.hpp"
#include "addons/editor/EditorFeature.hpp"

namespace engine::renderer::addons::editor {

std::string SerializeEditorHierarchyState(const EditorState& state)
{
    serialization::JsonWriter out;
    out.BeginObject();
    for (const auto& [scene, entities] : state.hierarchyOpenStates)
    {
        out.BeginObject(scene);
        for (const auto& [guid, open] : entities)
            out.WriteBool(guid, open);
        out.EndObject();
    }
    out.EndObject();
    return out.GetString();
}

void DeserializeEditorHierarchyState(const serialization::JsonValue& value, EditorState& state)
{
    state.hierarchyOpenStates.clear();
    if (!value.IsObject())
        return;
    for (const auto& [scene, entities] : value.objectVal)
    {
        if (scene.empty() || !entities.IsObject())
            continue;
        for (const auto& [guid, open] : entities.objectVal)
            if (!guid.empty() && open.IsBool())
                state.hierarchyOpenStates[scene][guid] = open.AsBool();
    }
}

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
            w.SetComponent<EditorHierarchyComponent>(id, component);
        });

    deserializer.RegisterHandler<EditorPersistentComponent>(
        [](const serialization::JsonValue&, ecs::World& w, EntityID id) {
            w.SetComponent<EditorPersistentComponent>(id, EditorPersistentComponent{});
        });
}

} // namespace engine::renderer::addons::editor
