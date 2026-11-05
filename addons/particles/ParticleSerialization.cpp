#include "addons/particles/ParticleSerialization.hpp"
#include "addons/particles/ParticleComponents.hpp"

#include <algorithm>
#include <limits>
#include <cmath>

namespace engine::particles {
namespace {

void ReadFloat(const serialization::JsonValue& v, const char* key, float& out)
{
    if (const auto* n = v.Get(key); n && n->IsNumber())
        out = n->AsFloat();
}

void ReadUint(const serialization::JsonValue& v, const char* key, uint32_t& out)
{
    if (const auto* n = v.Get(key); n && n->IsNumber() && std::isfinite(n->numVal) &&
        n->numVal >= 0. && n->numVal <= std::numeric_limits<uint32_t>::max() && std::floor(n->numVal) == n->numVal)
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

const char* FrameAnimationName(ParticleFrameAnimation mode)
{
    switch (mode)
    {
    case ParticleFrameAnimation::Rate: return "rate";
    case ParticleFrameAnimation::Fixed: return "fixed";
    case ParticleFrameAnimation::Random: return "random";
    default: return "overLifetime";
    }
}

void ReadFrameAnimation(const serialization::JsonValue& value, ParticleEmitterComponent& emitter)
{
    emitter.frameAnimation = ParticleFrameAnimation::OverLifetime;
    if (const auto* mode = value.Get("frameAnimation"))
    {
        if (mode->IsString())
        {
            if (mode->AsString() == "rate") emitter.frameAnimation = ParticleFrameAnimation::Rate;
            else if (mode->AsString() == "fixed") emitter.frameAnimation = ParticleFrameAnimation::Fixed;
            else if (mode->AsString() == "random") emitter.frameAnimation = ParticleFrameAnimation::Random;
        }
    }
    else if (const auto* legacy = value.Get("flipbookRandomFrame"); legacy && legacy->IsBool() && legacy->AsBool())
        emitter.frameAnimation = ParticleFrameAnimation::Random;
    emitter.frameFirst = 0u;
    emitter.frameLast = kLastParticleFrame;
    emitter.frameFps = 12.f;
    emitter.frameLoop = true;
    emitter.frameRandomStart = false;
    ReadUint(value, "frameFirst", emitter.frameFirst);
    ReadUint(value, "frameLast", emitter.frameLast);
    ReadFloat(value, "frameFps", emitter.frameFps);
    ReadBool(value, "frameLoop", emitter.frameLoop);
    ReadBool(value, "frameRandomStart", emitter.frameRandomStart);
    if (!std::isfinite(emitter.frameFps) || emitter.frameFps <= 0.f) emitter.frameFps = 12.f;
    emitter.clips.clear();
    if (const auto* clips = value.Get("clips"); clips && clips->IsArray())
    {
        for (const auto& source : clips->arrayVal)
        {
            if (!source.IsObject()) continue;
            ParticleClip clip;
            if (const auto* name = source.Get("name"); name && name->IsString()) clip.name = name->AsString();
            ReadUint(source, "first", clip.first);
            ReadUint(source, "last", clip.last);
            if (const auto* timing = source.Get("timing"); timing && timing->IsString() && timing->AsString() == "overLifetime")
                clip.timing = ParticleClipTiming::OverLifetime;
            ReadFloat(source, "fps", clip.fps);
            ReadBool(source, "loop", clip.loop);
            if (!std::isfinite(clip.fps) || clip.fps <= 0.f) clip.fps = 12.f;
            emitter.clips.push_back(std::move(clip));
        }
    }
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
    w.WriteString("frameAnimation", FrameAnimationName(c.frameAnimation));
    w.WriteUint("frameFirst", c.frameFirst);
    w.WriteUint("frameLast", c.frameLast);
    w.WriteFloat("frameFps", c.frameFps);
    w.WriteBool("frameLoop", c.frameLoop);
    w.WriteBool("frameRandomStart", c.frameRandomStart);
    if (!c.clips.empty())
    {
        w.BeginArray("clips");
        for (const auto& clip : c.clips)
        {
            w.BeginObject();
            w.WriteString("name", clip.name);
            w.WriteUint("first", clip.first);
            w.WriteUint("last", clip.last);
            w.WriteString("timing", clip.timing == ParticleClipTiming::OverLifetime ? "overLifetime" : "rate");
            w.WriteFloat("fps", clip.fps);
            w.WriteBool("loop", clip.loop);
            w.EndObject();
        }
        w.EndArray();
    }
    if (!c.frames.empty())
    {
        w.BeginArray("frames");
        for (const auto& frame : c.frames)
        {
            w.BeginObject();
            w.WriteUint("x", frame.x);
            w.WriteUint("y", frame.y);
            w.WriteUint("width", frame.width);
            w.WriteUint("height", frame.height);
            w.WriteFloat("pivotX", frame.pivotX);
            w.WriteFloat("pivotY", frame.pivotY);
            w.EndObject();
        }
        w.EndArray();
    }
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
    if (uint64_t(c.flipbookColumns) * c.flipbookRows > std::numeric_limits<uint32_t>::max())
        c.flipbookColumns = c.flipbookRows = 1u;
    ReadFrameAnimation(v, c);
    c.frames.clear();
    if (const auto* list = v.Get("frames"); list && list->IsArray())
    {
        std::vector<ParticleFrame> frames;
        for (const auto& value : list->arrayVal)
        {
            ParticleFrame frame;
            bool valid = value.IsObject();
            for (const auto* key : {"x", "y", "width", "height"})
            {
                const auto* number = value.Get(key);
                valid = valid && number && number->IsNumber() && std::isfinite(number->numVal) &&
                    number->numVal >= 0. && number->numVal <= std::numeric_limits<uint32_t>::max() &&
                    std::floor(number->numVal) == number->numVal;
            }
            ReadUint(value, "x", frame.x);
            ReadUint(value, "y", frame.y);
            ReadUint(value, "width", frame.width);
            ReadUint(value, "height", frame.height);
            ReadFloat(value, "pivotX", frame.pivotX);
            ReadFloat(value, "pivotY", frame.pivotY);
            if (!valid || !frame.IsValid())
            {
                frames.clear();
                break;
            }
            frames.push_back(frame);
        }
        c.frames = std::move(frames);
    }
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
