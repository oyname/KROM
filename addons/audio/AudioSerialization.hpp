#pragma once
#include "serialization/SceneSerializer.hpp"

namespace engine {
struct AudioSourceComponent;
struct AudioListenerComponent;
}

namespace engine::audio {

void RegisterAudioSerializationHandlers(serialization::SceneSerializer& serializer);
void UnregisterAudioSerializationHandlers(serialization::SceneSerializer& serializer);
void RegisterAudioDeserializationHandlers(serialization::SceneDeserializer& deserializer);
void UnregisterAudioDeserializationHandlers(serialization::SceneDeserializer& deserializer);

// Felder ins offene JSON-Objekt schreiben bzw. lesen (gemeinsam fuer Szene und Prefab).
void WriteAudioSourceFields(serialization::JsonWriter& w, const AudioSourceComponent& c);
void ReadAudioSourceFields(const serialization::JsonValue& v, AudioSourceComponent& c);
void WriteAudioListenerFields(serialization::JsonWriter& w, const AudioListenerComponent& c);
void ReadAudioListenerFields(const serialization::JsonValue& v, AudioListenerComponent& c);

} // namespace engine::audio
