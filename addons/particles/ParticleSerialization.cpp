#include "addons/particles/ParticleSerialization.hpp"
#include "addons/particles/ParticleComponents.hpp"

#include <algorithm>

namespace engine::particles {
namespace {

void ReadFloat(const serialization::JsonValue& v, const char* key, float& out)
{
    if (const auto* n = v.Get(key); n && n->IsNumber())
        out = n->AsFloat();
}

void ReadUint(const serialization::JsonValue& v, const char* key, uint32_t& out)
{
    if (const auto* n = v.Get(key); n && n->IsNumber() && n->AsFloat() >= 0.f)
        out = n->AsUint();
}

void ReadBool(const serialization::JsonValue& v, const char* key, bool& out)
{
    if (const auto* n = v.Get(key); n && n->IsBool())
        out = n->AsBool();
}

void ReadVec3(const serialization::JsonValue& v, const char* key, math::Vec3& out)
{
    if (const auto* n = v.Get(key); n && n->IsArray())
        out = n->AsVec3();
}

// Farbe als rgb-Array plus eigenem Alpha-Feld (der Writer kennt kein Vec4).
void WriteColor(serialization::JsonWriter& w, const char* key, const char* alphaKey, const math::Vec4& c)
{
    w.WriteVec3(key, math::Vec3{c.x, c.y, c.z});
    w.WriteFloat(alphaKey, c.w);
}

void ReadColor(const serialization::JsonValue& v, const char* key, const char* alphaKey, math::Vec4& out)
{
    math::Vec3 rgb{out.x, out.y, out.z};
    ReadVec3(v, key, rgb);
    out = math::Vec4{rgb, out.w};
    ReadFloat(v, alphaKey, out.w);
}

} // namespace

void WriteParticleEmitterFields(serialization::JsonWriter& w, const ParticleEmitterComponent& c)
{
    w.WriteUint ("maxParticles",     c.maxParticles);
    w.WriteFloat("emitRate",         c.emitRate);
    w.WriteUint ("burstCount",       c.burstCount);
    w.WriteFloat("particleLifetime", c.particleLifetime);
    w.WriteFloat("lifetimeRandom",   c.lifetimeRandom);
    w.WriteFloat("emitterRadius",    c.emitterRadius);
    w.WriteVec3 ("initialVelocity",  c.initialVelocity);
    w.WriteVec3 ("velocityRandom",   c.velocityRandom);
    w.WriteVec3 ("gravity",          c.gravity);
    w.WriteFloat("drag",             c.drag);
    w.WriteFloat("startSize",        c.startSize);
    w.WriteFloat("endSize",          c.endSize);
    WriteColor(w, "startColor", "startAlpha", c.startColor);
    WriteColor(w, "endColor",   "endAlpha",   c.endColor);
    w.WriteFloat("intensity",        c.intensity);
    w.WriteString("blendMode",       c.blendMode == ParticleBlendMode::Additive ? "additive" : "alpha");
    w.WriteString("texturePath",     c.texturePath);
    w.WriteUint ("flipbookColumns",  c.flipbookColumns);
    w.WriteUint ("flipbookRows",     c.flipbookRows);
    w.WriteBool ("flipbookRandomFrame", c.flipbookRandomFrame);
    w.WriteBool ("collideWithFloor", c.collideWithFloor);
    w.WriteFloat("floorHeight",      c.floorHeight);
    w.WriteBool ("floorRelative",    c.floorRelative);
    w.WriteFloat("bounce",           c.bounce);
    w.WriteFloat("floorFriction",    c.floorFriction);
    w.WriteBool ("dieOnFloor",       c.dieOnFloor);
    w.WriteBool ("looping",          c.looping);
    w.WriteFloat("duration",         c.duration);
    w.WriteBool ("playing",          c.playing);
}

void ReadParticleEmitterFields(const serialization::JsonValue& v, ParticleEmitterComponent& c)
{
    ReadUint (v, "maxParticles",     c.maxParticles);
    ReadFloat(v, "emitRate",         c.emitRate);
    ReadUint (v, "burstCount",       c.burstCount);
    ReadFloat(v, "particleLifetime", c.particleLifetime);
    ReadFloat(v, "lifetimeRandom",   c.lifetimeRandom);
    ReadFloat(v, "emitterRadius",    c.emitterRadius);
    ReadVec3 (v, "initialVelocity",  c.initialVelocity);
    ReadVec3 (v, "velocityRandom",   c.velocityRandom);
    ReadVec3 (v, "gravity",          c.gravity);
    ReadFloat(v, "drag",             c.drag);
    ReadFloat(v, "startSize",        c.startSize);
    ReadFloat(v, "endSize",          c.endSize);
    ReadColor(v, "startColor", "startAlpha", c.startColor);
    ReadColor(v, "endColor",   "endAlpha",   c.endColor);
    ReadFloat(v, "intensity",        c.intensity);
    if (const auto* n = v.Get("blendMode"); n && n->IsString())
        c.blendMode = n->AsString() == "additive" ? ParticleBlendMode::Additive : ParticleBlendMode::Alpha;
    if (const auto* n = v.Get("texturePath"); n && n->IsString())
        c.texturePath = n->AsString();
    ReadUint (v, "flipbookColumns",  c.flipbookColumns);
    ReadUint (v, "flipbookRows",     c.flipbookRows);
    c.flipbookColumns = std::max(c.flipbookColumns, 1u);
    c.flipbookRows    = std::max(c.flipbookRows, 1u);
    ReadBool (v, "flipbookRandomFrame", c.flipbookRandomFrame);
    ReadBool (v, "collideWithFloor", c.collideWithFloor);
    ReadFloat(v, "floorHeight",      c.floorHeight);
    ReadBool (v, "floorRelative",    c.floorRelative);
    ReadFloat(v, "bounce",           c.bounce);
    ReadFloat(v, "floorFriction",    c.floorFriction);
    ReadBool (v, "dieOnFloor",       c.dieOnFloor);
    ReadBool (v, "looping",          c.looping);
    ReadFloat(v, "duration",         c.duration);
    ReadBool (v, "playing",          c.playing);
}

void RegisterParticleSerializationHandlers(serialization::SceneSerializer& serializer)
{
    serializer.RegisterSerializer<ParticleEmitterComponent>(
        [](serialization::JsonWriter& w, const ParticleEmitterComponent& c)
    {
        w.BeginObject();
        w.WriteString("type", "ParticleEmitterComponent");
        WriteParticleEmitterFields(w, c);
        w.EndObject();
    });
}

void UnregisterParticleSerializationHandlers(serialization::SceneSerializer& serializer)
{
    serializer.UnregisterSerializer<ParticleEmitterComponent>();
}

void RegisterParticleDeserializationHandlers(serialization::SceneDeserializer& deserializer)
{
    deserializer.RegisterHandler("ParticleEmitterComponent",
        [](const serialization::JsonValue& v, ecs::World& w, EntityID id,
           const std::unordered_map<uint32_t, EntityID>&)
    {
        ParticleEmitterComponent c{};
        ReadParticleEmitterFields(v, c);
        w.Add<ParticleEmitterComponent>(id, c);
    });
}

void UnregisterParticleDeserializationHandlers(serialization::SceneDeserializer& deserializer)
{
    deserializer.UnregisterHandler("ParticleEmitterComponent");
}

} // namespace engine::particles
