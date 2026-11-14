#pragma once

#include "serialization/SceneSerializer.hpp"
#include "addons/probes/ProbeComponents.hpp"

namespace engine::addons::probes {

void WriteProbeFields(serialization::JsonWriter&, const ReflectionProbeComponent&);
ReflectionProbeComponent ReadProbeFields(const serialization::JsonValue&);

void RegisterProbeSerializationHandlers(serialization::SceneSerializer& serializer);
void UnregisterProbeSerializationHandlers(serialization::SceneSerializer& serializer);
void RegisterProbeDeserializationHandlers(serialization::SceneDeserializer& deserializer);
void UnregisterProbeDeserializationHandlers(serialization::SceneDeserializer& deserializer);

} // namespace engine::addons::probes
