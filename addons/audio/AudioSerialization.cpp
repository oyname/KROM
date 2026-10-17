#include "addons/audio/AudioSerialization.hpp"
#include "addons/audio/AudioComponents.hpp"

namespace engine::audio {
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

} // namespace

void WriteAudioSourceFields(serialization::JsonWriter& w, const AudioSourceComponent& c)
{
    w.WriteString("clipPath",   c.clipPath);
    w.WriteFloat("volume",      c.volume);
    w.WriteFloat("pitch",       c.pitch);
    w.WriteBool("loop",         c.loop);
    w.WriteBool("playOnStart",  c.playOnStart);
    w.WriteBool("spatial",      c.spatial);
    w.WriteFloat("minDistance", c.minDistance);
    w.WriteFloat("maxDistance", c.maxDistance);
}

void ReadAudioSourceFields(const serialization::JsonValue& v, AudioSourceComponent& c)
{
    if (const auto* n = v.Get("clipPath"); n && n->IsString())
        c.clipPath = n->AsString();
    ReadFloat(v, "volume",      c.volume);
    ReadFloat(v, "pitch",       c.pitch);
    ReadBool (v, "loop",        c.loop);
    ReadBool (v, "playOnStart", c.playOnStart);
    ReadBool (v, "spatial",     c.spatial);
    ReadFloat(v, "minDistance", c.minDistance);
    ReadFloat(v, "maxDistance", c.maxDistance);
}

void WriteAudioListenerFields(serialization::JsonWriter& w, const AudioListenerComponent& c)
{
    w.WriteFloat("volume", c.volume);
}

void ReadAudioListenerFields(const serialization::JsonValue& v, AudioListenerComponent& c)
{
    ReadFloat(v, "volume", c.volume);
}

void RegisterAudioSerializationHandlers(serialization::SceneSerializer& serializer)
{
    serializer.RegisterSerializer<AudioSourceComponent>(
        [](serialization::JsonWriter& w, const AudioSourceComponent& c)
    {
        w.BeginObject();
        w.WriteString("type", "AudioSourceComponent");
        WriteAudioSourceFields(w, c);
        w.EndObject();
    });
    serializer.RegisterSerializer<AudioListenerComponent>(
        [](serialization::JsonWriter& w, const AudioListenerComponent& c)
    {
        w.BeginObject();
        w.WriteString("type", "AudioListenerComponent");
        WriteAudioListenerFields(w, c);
        w.EndObject();
    });
}

void UnregisterAudioSerializationHandlers(serialization::SceneSerializer& serializer)
{
    serializer.UnregisterSerializer<AudioSourceComponent>();
    serializer.UnregisterSerializer<AudioListenerComponent>();
}

void RegisterAudioDeserializationHandlers(serialization::SceneDeserializer& deserializer)
{
    deserializer.RegisterHandler("AudioSourceComponent",
        [](const serialization::JsonValue& v, ecs::World& w, EntityID id,
           const std::unordered_map<uint32_t, EntityID>&)
    {
        AudioSourceComponent c{};
        ReadAudioSourceFields(v, c);
        w.Add<AudioSourceComponent>(id, std::move(c));
    });
    deserializer.RegisterHandler("AudioListenerComponent",
        [](const serialization::JsonValue& v, ecs::World& w, EntityID id,
           const std::unordered_map<uint32_t, EntityID>&)
    {
        AudioListenerComponent c{};
        ReadAudioListenerFields(v, c);
        w.Add<AudioListenerComponent>(id, c);
    });
}

void UnregisterAudioDeserializationHandlers(serialization::SceneDeserializer& deserializer)
{
    deserializer.UnregisterHandler("AudioSourceComponent");
    deserializer.UnregisterHandler("AudioListenerComponent");
}

} // namespace engine::audio
