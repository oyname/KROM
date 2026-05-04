#include "addons/lighting/LightingSerialization.hpp"

#include "addons/lighting/LightingComponents.hpp"

namespace engine::addons::lighting {

void RegisterLightingSerializationHandlers(serialization::SceneSerializer& serializer)
{
    serializer.RegisterSerializer<LightComponent>([](serialization::JsonWriter& w, const LightComponent& c) {
        w.BeginObject();
        w.WriteString("type", "LightComponent");
        w.WriteUint("lightType", static_cast<uint32_t>(c.type));
        w.WriteVec3("color", c.color);
        w.WriteFloat("intensity", c.intensity);
        w.WriteFloat("range", c.range);
        w.WriteBool("castShadows", c.castShadows);
        w.EndObject();
    });
}

void RegisterLightingDeserializationHandlers(serialization::SceneDeserializer& deserializer)
{
    deserializer.RegisterHandler<LightComponent>([](const serialization::JsonValue& v, ecs::World& w, EntityID id) {
        LightComponent lc{};
        if (const auto* t = v.Get("lightType")) lc.type = static_cast<LightType>(t->AsUint());
        if (const auto* c = v.Get("color")) lc.color = c->AsVec3();
        if (const auto* i = v.Get("intensity")) lc.intensity = i->AsFloat();
        if (const auto* r = v.Get("range")) lc.range = r->AsFloat();
        if (const auto* s = v.Get("castShadows")) lc.castShadows = s->AsBool();
        w.Add<LightComponent>(id, lc);
    });
}

void UnregisterLightingSerializationHandlers(serialization::SceneSerializer& serializer)
{
    serializer.UnregisterSerializer<LightComponent>();
}

void UnregisterLightingDeserializationHandlers(serialization::SceneDeserializer& deserializer)
{
    deserializer.UnregisterHandler("LightComponent");
}

void RegisterLightingAddon(ecs::ComponentMetaRegistry* components,
                            serialization::SceneSerializer* serializer,
                            serialization::SceneDeserializer* deserializer)
{
    if (components)  RegisterLightingComponents(*components);
    if (serializer)  RegisterLightingSerializationHandlers(*serializer);
    if (deserializer) RegisterLightingDeserializationHandlers(*deserializer);
}

void UnregisterLightingAddon(ecs::ComponentMetaRegistry* components,
                              serialization::SceneSerializer* serializer,
                              serialization::SceneDeserializer* deserializer)
{
    if (deserializer) UnregisterLightingDeserializationHandlers(*deserializer);
    if (serializer)  UnregisterLightingSerializationHandlers(*serializer);
    if (components)  components->Unregister<LightComponent>();
}

} // namespace engine::addons::lighting
