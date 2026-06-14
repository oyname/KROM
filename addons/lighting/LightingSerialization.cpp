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
        w.WriteFloat("spotInnerDeg", c.spotInnerDeg);
        w.WriteFloat("spotOuterDeg", c.spotOuterDeg);
        w.WriteBool("castShadows", c.castShadows);
        w.WriteUint("layerMask", c.layerMask);
        w.WriteBool("shadowEnabled", c.shadowSettings.enabled);
        w.WriteUint("shadowFilter", static_cast<uint32_t>(c.shadowSettings.filter));
        w.WriteUint("shadowResolution", c.shadowSettings.resolution);
        w.WriteFloat("shadowBias", c.shadowSettings.bias);
        w.WriteFloat("shadowNormalBias", c.shadowSettings.normalBias);
        w.WriteFloat("shadowMaxDistance", c.shadowSettings.maxDistance);
        w.WriteFloat("shadowStrength", c.shadowSettings.strength);
        w.WriteUint("shadowCascadeCount", c.shadowSettings.cascadeCount);
        w.WriteFloat("shadowCascadeLambda", c.shadowSettings.cascadeLambda);
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
        if (const auto* si = v.Get("spotInnerDeg")) lc.spotInnerDeg = si->AsFloat();
        if (const auto* so = v.Get("spotOuterDeg")) lc.spotOuterDeg = so->AsFloat();
        if (const auto* s = v.Get("castShadows")) lc.castShadows = s->AsBool();
        if (const auto* lm = v.Get("layerMask")) lc.layerMask = lm->AsUint();
        if (const auto* se = v.Get("shadowEnabled")) lc.shadowSettings.enabled = se->AsBool();
        if (const auto* sf = v.Get("shadowFilter")) lc.shadowSettings.filter = static_cast<ShadowFilter>(sf->AsUint());
        if (const auto* sr = v.Get("shadowResolution")) lc.shadowSettings.resolution = sr->AsUint();
        if (const auto* sb = v.Get("shadowBias")) lc.shadowSettings.bias = sb->AsFloat();
        if (const auto* sn = v.Get("shadowNormalBias")) lc.shadowSettings.normalBias = sn->AsFloat();
        if (const auto* smd = v.Get("shadowMaxDistance")) lc.shadowSettings.maxDistance = smd->AsFloat();
        if (const auto* ss = v.Get("shadowStrength")) lc.shadowSettings.strength = ss->AsFloat();
        if (const auto* scc = v.Get("shadowCascadeCount")) lc.shadowSettings.cascadeCount = scc->AsUint();
        if (const auto* scl = v.Get("shadowCascadeLambda")) lc.shadowSettings.cascadeLambda = scl->AsFloat();
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
