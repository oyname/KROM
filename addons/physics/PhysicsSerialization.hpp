#pragma once
#include "serialization/SceneSerializer.hpp"

namespace engine {
struct RigidBodyComponent;
struct SphereColliderComponent;
struct CapsuleColliderComponent;
}

namespace engine::physics {

void RegisterPhysicsSerializationHandlers(
    serialization::SceneSerializer& serializer);

void UnregisterPhysicsSerializationHandlers(
    serialization::SceneSerializer& serializer);

void RegisterPhysicsDeserializationHandlers(
    serialization::SceneDeserializer& deserializer);

void UnregisterPhysicsDeserializationHandlers(
    serialization::SceneDeserializer& deserializer);

// Einzelne Komponentenfelder ins aktuell offene JSON-Objekt schreiben bzw. aus
// einem Objekt lesen (fehlende Felder behalten ihren Wert). Gemeinsames Format
// fuer Szenen-Handler und Prefabs.
void WriteRigidBodyFields(serialization::JsonWriter& w, const RigidBodyComponent& c);
void ReadRigidBodyFields(const serialization::JsonValue& v, RigidBodyComponent& c);
void WriteSphereColliderFields(serialization::JsonWriter& w, const SphereColliderComponent& c);
void ReadSphereColliderFields(const serialization::JsonValue& v, SphereColliderComponent& c);
void WriteCapsuleColliderFields(serialization::JsonWriter& w, const CapsuleColliderComponent& c);
void ReadCapsuleColliderFields(const serialization::JsonValue& v, CapsuleColliderComponent& c);

} // namespace engine::physics
