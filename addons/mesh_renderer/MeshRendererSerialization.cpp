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

} // namespace engine::addons::mesh_renderer
