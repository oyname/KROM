#include "addons/physics/PhysicsSerialization.hpp"
#include "addons/physics/PhysicsComponents.hpp"

#include <algorithm>

namespace engine::physics {
namespace {

void ReadFloat(const serialization::JsonValue& v, const char* key, float& out)
{
    if (const auto* n = v.Get(key); n && n->IsNumber())
        out = n->AsFloat();
}

void ReadBool(const serialization::JsonValue& v, const char* key, bool& out)
{
    if (const auto* n = v.Get(key); n && n->IsBool())
        out = n->AsBool();
}

void ReadVec3(const serialization::JsonValue& v, const char* key, math::Vec3& out)
{
    if (const auto* n = v.Get(key); n && n->IsArray() && n->arrayVal.size() >= 3u)
        out = n->AsVec3();
}

int ReadInt(const serialization::JsonValue& v, const char* key, int fallback)
{
    if (const auto* n = v.Get(key); n && n->IsNumber())
        return n->AsInt();
    return fallback;
}

} // namespace

void RegisterPhysicsSerializationHandlers(serialization::SceneSerializer& serializer)
{
    serializer.RegisterSerializer<RigidBodyComponent>(
        [](serialization::JsonWriter& w, const RigidBodyComponent& c)
    {
        w.BeginObject();
        w.WriteString("type", "RigidBodyComponent");
        w.WriteInt("bodyType",       static_cast<int>(c.type));
        w.WriteBool("enabled",       c.enabled);
        w.WriteFloat("mass",         c.mass);
        w.WriteFloat("friction",     c.friction);
        w.WriteFloat("restitution",  c.restitution);
        w.WriteFloat("linearDamping",  c.linearDamping);
        w.WriteFloat("angularDamping", c.angularDamping);
        w.WriteFloat("gravityScale", c.gravityScale);
        w.WriteBool("lockRotationX", c.lockRotationX);
        w.WriteBool("lockRotationY", c.lockRotationY);
        w.WriteBool("lockRotationZ", c.lockRotationZ);
        w.EndObject();
    });

    serializer.RegisterSerializer<SphereColliderComponent>(
        [](serialization::JsonWriter& w, const SphereColliderComponent& c)
    {
        w.BeginObject();
        w.WriteString("type", "SphereColliderComponent");
        w.WriteVec3("centerOffset", c.centerOffset);
        w.WriteFloat("radius",      c.radius);
        w.WriteBool("showInEditor", c.showInEditor);
        w.EndObject();
    });

    serializer.RegisterSerializer<CapsuleColliderComponent>(
        [](serialization::JsonWriter& w, const CapsuleColliderComponent& c)
    {
        w.BeginObject();
        w.WriteString("type", "CapsuleColliderComponent");
        w.WriteVec3("centerOffset", c.centerOffset);
        w.WriteFloat("radius",      c.radius);
        w.WriteFloat("height",      c.height);
        w.WriteInt("axis",          static_cast<int>(c.axis));
        w.WriteBool("showInEditor", c.showInEditor);
        w.EndObject();
    });
}

void UnregisterPhysicsSerializationHandlers(serialization::SceneSerializer& serializer)
{
    serializer.UnregisterSerializer<RigidBodyComponent>();
    serializer.UnregisterSerializer<SphereColliderComponent>();
    serializer.UnregisterSerializer<CapsuleColliderComponent>();
}

void RegisterPhysicsDeserializationHandlers(serialization::SceneDeserializer& deserializer)
{
    deserializer.RegisterHandler("RigidBodyComponent",
        [](const serialization::JsonValue& v, ecs::World& w, EntityID id,
           const std::unordered_map<uint32_t, EntityID>&)
    {
        RigidBodyComponent c{};
        const int type = std::clamp(ReadInt(v, "bodyType", static_cast<int>(c.type)), 0, 2);
        c.type = static_cast<RigidBodyType>(type);
        ReadBool (v, "enabled",        c.enabled);
        ReadFloat(v, "mass",           c.mass);
        ReadFloat(v, "friction",       c.friction);
        ReadFloat(v, "restitution",    c.restitution);
        ReadFloat(v, "linearDamping",  c.linearDamping);
        ReadFloat(v, "angularDamping", c.angularDamping);
        ReadFloat(v, "gravityScale",   c.gravityScale);
        ReadBool (v, "lockRotationX",  c.lockRotationX);
        ReadBool (v, "lockRotationY",  c.lockRotationY);
        ReadBool (v, "lockRotationZ",  c.lockRotationZ);
        w.Add<RigidBodyComponent>(id, c);
    });

    deserializer.RegisterHandler("SphereColliderComponent",
        [](const serialization::JsonValue& v, ecs::World& w, EntityID id,
           const std::unordered_map<uint32_t, EntityID>&)
    {
        SphereColliderComponent c{};
        ReadVec3 (v, "centerOffset", c.centerOffset);
        ReadFloat(v, "radius",       c.radius);
        ReadBool (v, "showInEditor", c.showInEditor);
        w.Add<SphereColliderComponent>(id, c);
    });

    deserializer.RegisterHandler("CapsuleColliderComponent",
        [](const serialization::JsonValue& v, ecs::World& w, EntityID id,
           const std::unordered_map<uint32_t, EntityID>&)
    {
        CapsuleColliderComponent c{};
        ReadVec3 (v, "centerOffset", c.centerOffset);
        ReadFloat(v, "radius",       c.radius);
        ReadFloat(v, "height",       c.height);
        c.axis = static_cast<CapsuleAxis>(std::clamp(ReadInt(v, "axis", 1), 0, 2));
        ReadBool (v, "showInEditor", c.showInEditor);
        w.Add<CapsuleColliderComponent>(id, c);
    });
}

void UnregisterPhysicsDeserializationHandlers(serialization::SceneDeserializer& deserializer)
{
    deserializer.UnregisterHandler("RigidBodyComponent");
    deserializer.UnregisterHandler("SphereColliderComponent");
    deserializer.UnregisterHandler("CapsuleColliderComponent");
}

} // namespace engine::physics
