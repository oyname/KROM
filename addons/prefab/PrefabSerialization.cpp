// =============================================================================
// KROM Engine - addons/prefab/PrefabSerialization.cpp
// =============================================================================
#include "PrefabSerialization.hpp"
#include "PrefabInstanceComponent.hpp"

#include "ecs/World.hpp"
#include "serialization/SceneSerializer.hpp"

namespace engine::addons::prefab {

void RegisterPrefabSerializationHandlers(serialization::SceneSerializer& serializer)
{
    serializer.RegisterSerializer<PrefabInstanceComponent>(
        [](serialization::JsonWriter& w, const PrefabInstanceComponent& c)
        {
            w.BeginObject();
            w.WriteString("type", "PrefabInstanceComponent");
            w.WriteString("prefabAssetPath", c.prefabAssetPath);
            w.WriteBool("overrideTransform", c.overrideTransform);
            w.EndObject();
        });
}

void RegisterPrefabDeserializationHandlers(serialization::SceneDeserializer& deserializer)
{
    deserializer.RegisterHandler<PrefabInstanceComponent>(
        [](const serialization::JsonValue& v, ecs::World& w, EntityID id)
        {
            PrefabInstanceComponent c;
            if (const auto* p = v.Get("prefabAssetPath"))
                c.prefabAssetPath = p->AsString();
            if (const auto* b = v.Get("overrideTransform"))
                c.overrideTransform = b->AsBool();
            w.Add<PrefabInstanceComponent>(id, c);
        });
}

void UnregisterPrefabSerializationHandlers(serialization::SceneSerializer& serializer)
{
    serializer.UnregisterSerializer<PrefabInstanceComponent>();
}

void UnregisterPrefabDeserializationHandlers(serialization::SceneDeserializer& deserializer)
{
    deserializer.UnregisterHandler("PrefabInstanceComponent");
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
