#include "addons/probes/ProbeSerialization.hpp"

#include <algorithm>
#include <charconv>
#include <cstdio>

namespace engine::addons::probes {
namespace {

// The JSON reader keeps numbers as double, which loses 64-bit ids; store hex.
std::string ProbeIdToString(uint64_t id)
{
    char buf[17]{};
    std::snprintf(buf, sizeof(buf), "%016llx", static_cast<unsigned long long>(id));
    return buf;
}

uint64_t ProbeIdFromString(const std::string& text)
{
    uint64_t id = 0u;
    std::from_chars(text.data(), text.data() + text.size(), id, 16);
    return id;
}

uint32_t SanitizeResolution(uint32_t r) noexcept
{
    if (r <= 64u) return 64u;
    if (r <= 128u) return 128u;
    return 256u;
}

} // namespace

void WriteProbeFields(serialization::JsonWriter& w, const ReflectionProbeComponent& c)
{
    w.WriteVec3("extents", c.extents);
    w.WriteFloat("intensity", c.intensity);
    w.WriteUint("resolution", c.resolution);
    w.WriteString("probeId", ProbeIdToString(c.probeId));
    w.WriteString("bakedData", c.bakedDataPath);
}

ReflectionProbeComponent ReadProbeFields(const serialization::JsonValue& v)
{
    ReflectionProbeComponent c{};
    if (const auto* e = v.Get("extents")) c.extents = e->AsVec3();
    if (const auto* i = v.Get("intensity")) c.intensity = i->AsFloat();
    if (const auto* r = v.Get("resolution")) c.resolution = r->AsUint();
    if (const auto* id = v.Get("probeId")) c.probeId = ProbeIdFromString(id->AsString());
    if (const auto* p = v.Get("bakedData")) c.bakedDataPath = p->AsString();

    c.extents.x = std::max(c.extents.x, 0.05f);
    c.extents.y = std::max(c.extents.y, 0.05f);
    c.extents.z = std::max(c.extents.z, 0.05f);
    c.intensity = std::max(c.intensity, 0.f);
    c.resolution = SanitizeResolution(c.resolution);
    return c;
}

void RegisterProbeSerializationHandlers(serialization::SceneSerializer& serializer)
{
    serializer.RegisterSerializer<ReflectionProbeComponent>(
        [](serialization::JsonWriter& w, const ReflectionProbeComponent& c) {
            w.BeginObject();
            w.WriteString("type", "ReflectionProbeComponent");
            WriteProbeFields(w, c);
            w.EndObject();
        });
}

void UnregisterProbeSerializationHandlers(serialization::SceneSerializer& serializer)
{
    serializer.UnregisterSerializer<ReflectionProbeComponent>();
}

void RegisterProbeDeserializationHandlers(serialization::SceneDeserializer& deserializer)
{
    // Named explicitly: the templated overload needs the component registered
    // in the world's meta registry first.
    deserializer.RegisterHandler("ReflectionProbeComponent",
        [](const serialization::JsonValue& v, ecs::World& w, EntityID id,
           const std::unordered_map<uint32_t, EntityID>&) {
            w.Add<ReflectionProbeComponent>(id, ReadProbeFields(v));
        });
}

void UnregisterProbeDeserializationHandlers(serialization::SceneDeserializer& deserializer)
{
    deserializer.UnregisterHandler("ReflectionProbeComponent");
}

} // namespace engine::addons::probes
