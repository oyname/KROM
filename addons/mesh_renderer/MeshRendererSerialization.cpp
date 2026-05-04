#include "addons/mesh_renderer/MeshRendererSerialization.hpp"

#include "addons/mesh_renderer/MeshRendererComponents.hpp"

namespace engine::addons::mesh_renderer {

void RegisterMeshRendererSerializationHandlers(serialization::SceneSerializer& serializer)
{
    serializer.RegisterSerializer<MeshComponent>([](serialization::JsonWriter& w, const MeshComponent& c) {
        w.BeginObject();
        w.WriteString("type", "MeshComponent");
        w.WriteUint("meshHandle", c.mesh.value);
        w.WriteBool("castShadows", c.castShadows);
        w.WriteUint("layerMask", c.layerMask);
        w.EndObject();
    });

    serializer.RegisterSerializer<MaterialComponent>([](serialization::JsonWriter& w, const MaterialComponent& c) {
        w.BeginObject();
        w.WriteString("type", "MaterialComponent");
        w.WriteUint("materialHandle", c.material.value);
        w.WriteUint("submeshIndex", c.submeshIndex);
        w.EndObject();
    });
}

void RegisterMeshRendererDeserializationHandlers(serialization::SceneDeserializer& deserializer)
{
    deserializer.RegisterHandler<MeshComponent>([](const serialization::JsonValue& v, ecs::World& w, EntityID id) {
        MeshComponent mc{};
        if (const auto* h = v.Get("meshHandle")) mc.mesh = MeshHandle(h->AsUint());
        if (const auto* s = v.Get("castShadows")) mc.castShadows = s->AsBool();
        if (const auto* l = v.Get("layerMask")) mc.layerMask = l->AsUint();
        w.Add<MeshComponent>(id, mc);
    });

    deserializer.RegisterHandler<MaterialComponent>([](const serialization::JsonValue& v, ecs::World& w, EntityID id) {
        MaterialComponent mc{};
        if (const auto* h = v.Get("materialHandle")) mc.material = MaterialHandle(h->AsUint());
        if (const auto* s = v.Get("submeshIndex")) mc.submeshIndex = s->AsUint();
        w.Add<MaterialComponent>(id, mc);
    });
}

void UnregisterMeshRendererSerializationHandlers(serialization::SceneSerializer& serializer)
{
    serializer.UnregisterSerializer<MeshComponent>();
    serializer.UnregisterSerializer<MaterialComponent>();
}

void UnregisterMeshRendererDeserializationHandlers(serialization::SceneDeserializer& deserializer)
{
    deserializer.UnregisterHandler("MeshComponent");
    deserializer.UnregisterHandler("MaterialComponent");
}

void RegisterMeshRendererAddon(ecs::ComponentMetaRegistry* components,
                                serialization::SceneSerializer* serializer,
                                serialization::SceneDeserializer* deserializer)
{
    if (components)  RegisterMeshRendererComponents(*components);
    if (serializer)  RegisterMeshRendererSerializationHandlers(*serializer);
    if (deserializer) RegisterMeshRendererDeserializationHandlers(*deserializer);
}

void UnregisterMeshRendererAddon(ecs::ComponentMetaRegistry* components,
                                  serialization::SceneSerializer* serializer,
                                  serialization::SceneDeserializer* deserializer)
{
    if (deserializer) UnregisterMeshRendererDeserializationHandlers(*deserializer);
    if (serializer)  UnregisterMeshRendererSerializationHandlers(*serializer);
    if (components)  { components->Unregister<MeshComponent>(); components->Unregister<MaterialComponent>(); }
}

} // namespace engine::addons::mesh_renderer
