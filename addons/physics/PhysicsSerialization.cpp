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

int ReadInt(const serialization::JsonValue& v, const char* key, int fallback)
{
    if (const auto* n = v.Get(key); n && n->IsNumber())
        return n->AsInt();
    return fallback;
}

} // namespace

// ─── Felder (gemeinsam fuer Szene und Prefab) ────────────────────────────────

void WriteRigidBodyFields(serialization::JsonWriter& w, const RigidBodyComponent& c)
{
    w.WriteInt("bodyType",         static_cast<int>(c.type));
    w.WriteBool("enabled",         c.enabled);
    w.WriteFloat("mass",           c.mass);
    w.WriteFloat("friction",       c.friction);
    w.WriteFloat("restitution",    c.restitution);
    w.WriteFloat("linearDamping",  c.linearDamping);
    w.WriteFloat("angularDamping", c.angularDamping);
    w.WriteFloat("gravityScale",   c.gravityScale);
    w.WriteBool("lockRotationX",   c.lockRotationX);
    w.WriteBool("lockRotationY",   c.lockRotationY);
    w.WriteBool("lockRotationZ",   c.lockRotationZ);
}

void ReadRigidBodyFields(const serialization::JsonValue& v, RigidBodyComponent& c)
{
    c.type = static_cast<RigidBodyType>(std::clamp(ReadInt(v, "bodyType", static_cast<int>(c.type)), 0, 2));
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
}

void WriteCharacterControllerFields(serialization::JsonWriter& w, const CharacterControllerComponent& c)
{
    w.WriteBool("enabled",       c.enabled);
    w.WriteFloat("radius",       c.radius);
    w.WriteFloat("height",       c.height);
    w.WriteFloat("maxSlopeDeg",  c.maxSlopeDeg);
    w.WriteFloat("stepHeight",   c.stepHeight);
    w.WriteFloat("mass",         c.mass);
    w.WriteFloat("gravityScale", c.gravityScale);
    w.WriteBool("showInEditor",  c.showInEditor);
}

void ReadCharacterControllerFields(const serialization::JsonValue& v, CharacterControllerComponent& c)
{
    ReadBool (v, "enabled",      c.enabled);
    ReadFloat(v, "radius",       c.radius);
    ReadFloat(v, "height",       c.height);
    ReadFloat(v, "maxSlopeDeg",  c.maxSlopeDeg);
    ReadFloat(v, "stepHeight",   c.stepHeight);
    ReadFloat(v, "mass",         c.mass);
    ReadFloat(v, "gravityScale", c.gravityScale);
    ReadBool (v, "showInEditor", c.showInEditor);
}

// ─── Szenen-Handler ──────────────────────────────────────────────────────────

void RegisterPhysicsSerializationHandlers(serialization::SceneSerializer& serializer)
{
    serializer.RegisterSerializer<RigidBodyComponent>(
        [](serialization::JsonWriter& w, const RigidBodyComponent& c)
    {
        w.BeginObject();
        w.WriteString("type", "RigidBodyComponent");
        WriteRigidBodyFields(w, c);
        w.EndObject();
    });

    serializer.RegisterSerializer<CharacterControllerComponent>(
        [](serialization::JsonWriter& w, const CharacterControllerComponent& c)
    {
        w.BeginObject();
        w.WriteString("type", "CharacterControllerComponent");
        WriteCharacterControllerFields(w, c);
        w.EndObject();
    });
}

void UnregisterPhysicsSerializationHandlers(serialization::SceneSerializer& serializer)
{
    serializer.UnregisterSerializer<RigidBodyComponent>();
    serializer.UnregisterSerializer<CharacterControllerComponent>();
}

void RegisterPhysicsDeserializationHandlers(serialization::SceneDeserializer& deserializer)
{
    deserializer.RegisterHandler("RigidBodyComponent",
        [](const serialization::JsonValue& v, ecs::World& w, EntityID id,
           const std::unordered_map<uint32_t, EntityID>&)
    {
        RigidBodyComponent c{};
        ReadRigidBodyFields(v, c);
        w.SetComponent<RigidBodyComponent>(id, c);
    });

    deserializer.RegisterHandler("CharacterControllerComponent",
        [](const serialization::JsonValue& v, ecs::World& w, EntityID id,
           const std::unordered_map<uint32_t, EntityID>&)
    {
        CharacterControllerComponent c{};
        ReadCharacterControllerFields(v, c);
        w.SetComponent<CharacterControllerComponent>(id, c);
    });
}

void UnregisterPhysicsDeserializationHandlers(serialization::SceneDeserializer& deserializer)
{
    deserializer.UnregisterHandler("RigidBodyComponent");
    deserializer.UnregisterHandler("CharacterControllerComponent");
}

} // namespace engine::physics
