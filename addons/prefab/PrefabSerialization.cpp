// =============================================================================
// KROM Engine - addons/prefab/PrefabSerialization.cpp
// =============================================================================
#include "PrefabSerialization.hpp"
#include "PrefabInstanceComponent.hpp"

#include "ecs/World.hpp"
#include "serialization/SceneSerializer.hpp"

namespace engine::addons::prefab {

void WritePrefabInstanceFields(serialization::JsonWriter& w, const PrefabInstanceComponent& c)
{
    w.WriteString("prefabAssetPath", c.prefabAssetPath);
    w.WriteBool("overrideTransform", c.overrideTransform);
    w.WriteString("baselinePrefabJson", c.baselinePrefabJson);
    w.BeginArray("bindings");
    for (const auto& binding : c.bindings)
    {
        w.BeginObject();
        w.WriteString("recordId", binding.recordId);
        w.WriteString("entityGuid", binding.entityGuid);
        w.BeginArray("overrides");
        for (const auto& path : binding.overriddenProperties) w.WriteString("", path);
        w.EndArray();
        w.EndObject();
    }
    w.EndArray();
}

PrefabInstanceComponent ReadPrefabInstanceFields(const serialization::JsonValue& v)
{
    PrefabInstanceComponent c;
    if (const auto* p = v.Get("prefabAssetPath")) c.prefabAssetPath = p->AsString();
    if (const auto* p = v.Get("overrideTransform")) c.overrideTransform = p->AsBool();
    if (const auto* p = v.Get("baselinePrefabJson")) c.baselinePrefabJson = p->AsString();
    if (const auto* bindings = v.Get("bindings"); bindings && bindings->IsArray())
        for (const auto& b : bindings->arrayVal)
            if (b.Get("recordId") && b.Get("entityGuid"))
            {
                PrefabInstanceComponent::Binding binding{b.Get("recordId")->AsString(), b.Get("entityGuid")->AsString()};
                if (const auto* paths = b.Get("overrides"); paths && paths->IsArray())
                    for (const auto& path : paths->arrayVal) binding.overriddenProperties.push_back(path.AsString());
                c.bindings.push_back(std::move(binding));
            }
    return c;
}

void RegisterPrefabSerializationHandlers(serialization::SceneSerializer& serializer)
{
    serializer.RegisterSerializer<PrefabInstanceComponent>(
        [](serialization::JsonWriter& w, const PrefabInstanceComponent& c)
        {
            w.BeginObject();
            w.WriteString("type", "PrefabInstanceComponent");
            WritePrefabInstanceFields(w, c);
            w.EndObject();
        });
    serializer.RegisterSerializer<PrefabSourceComponent>([](serialization::JsonWriter& w, const PrefabSourceComponent& c) {
        w.BeginObject(); w.WriteString("type", "PrefabSourceComponent");
        w.WriteString("recordId", c.recordId); w.EndObject();
    });
}

void RegisterPrefabDeserializationHandlers(serialization::SceneDeserializer& deserializer)
{
    deserializer.RegisterHandler<PrefabInstanceComponent>(
        [](const serialization::JsonValue& v, ecs::World& w, EntityID id)
        {
            w.Add<PrefabInstanceComponent>(id, ReadPrefabInstanceFields(v));
        });
    deserializer.RegisterHandler<PrefabSourceComponent>([](const serialization::JsonValue& v, ecs::World& w, EntityID id) {
        if (const auto* p = v.Get("recordId")) w.Add<PrefabSourceComponent>(id, PrefabSourceComponent{p->AsString()});
    });
}

void UnregisterPrefabSerializationHandlers(serialization::SceneSerializer& serializer)
{
    serializer.UnregisterSerializer<PrefabInstanceComponent>();
    serializer.UnregisterSerializer<PrefabSourceComponent>();
}

void UnregisterPrefabDeserializationHandlers(serialization::SceneDeserializer& deserializer)
{
    deserializer.UnregisterHandler("PrefabInstanceComponent");
    deserializer.UnregisterHandler("PrefabSourceComponent");
}

void RegisterPrefabAddon(
    ecs::ComponentMetaRegistry*       components,
    serialization::SceneSerializer*   serializer,
    serialization::SceneDeserializer* deserializer)
{
    if (components)   RegisterPrefabInstanceComponent(*components);
    if (serializer)   RegisterPrefabSerializationHandlers(*serializer);
    if (deserializer) RegisterPrefabDeserializationHandlers(*deserializer);
}

} // namespace engine::addons::prefab
