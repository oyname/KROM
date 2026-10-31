#pragma once
#include "serialization/SceneSerializer.hpp"

namespace engine {
struct ParticleEmitterComponent;
}

namespace engine::particles {

void RegisterParticleSerializationHandlers(serialization::SceneSerializer& serializer);
void UnregisterParticleSerializationHandlers(serialization::SceneSerializer& serializer);
void RegisterParticleDeserializationHandlers(serialization::SceneDeserializer& deserializer);
void UnregisterParticleDeserializationHandlers(serialization::SceneDeserializer& deserializer);

// Felder ins offene JSON-Objekt schreiben bzw. lesen (gemeinsam fuer Szene und Prefab).
void WriteParticleEmitterFields(serialization::JsonWriter& w, const ParticleEmitterComponent& c);
void ReadParticleEmitterFields(const serialization::JsonValue& v, ParticleEmitterComponent& c);

} // namespace engine::particles
