#pragma once
// =============================================================================
// KROM Engine - addons/postprocess/PostProcessSerialization.hpp
// JSON fields of PostProcessVolumeComponent for scenes (editor .json and .kscene)
// and prefabs. "overrides" lists the overridden fields by name; a missing name
// means no override. All values are stored, so unticking keeps them.
// =============================================================================
#include "addons/postprocess/PostProcessVolumeComponent.hpp"
#include "serialization/SceneSerializer.hpp"

namespace engine::addons::postprocess {

void WritePostProcessVolumeFields(serialization::JsonWriter& w, const PostProcessVolumeComponent& c);
[[nodiscard]] PostProcessVolumeComponent ReadPostProcessVolumeFields(const serialization::JsonValue& v);

void RegisterPostProcessSerializationHandlers(serialization::SceneSerializer& serializer);
void UnregisterPostProcessSerializationHandlers(serialization::SceneSerializer& serializer);
void RegisterPostProcessDeserializationHandlers(serialization::SceneDeserializer& deserializer);
void UnregisterPostProcessDeserializationHandlers(serialization::SceneDeserializer& deserializer);

} // namespace engine::addons::postprocess
