#pragma once
// =============================================================================
// KROM Engine - addons/script/IParticleService.hpp
// Schnittstelle, ueber die Scripts Partikel-Emitter steuern. Ohne Abhaengigkeit
// zum Partikel-Addon: die Runtime stellt einen Adapter auf ParticleSystem bereit.
// Ohne Partikel ist ScriptServices::particles null und die Helfer sind neutral.
// =============================================================================
#include "core/Math.hpp"
#include "core/Types.hpp"
#include "addons/particles/ParticleTypes.hpp"
#include <optional>
#include <span>
#include <string>

#include <cstdint>

namespace engine::script {

class IParticleService
{
public:
    virtual ~IParticleService() = default;

    // Emitter (ParticleEmitterComponent) starten: von vorn, inkl. Burst.
    virtual bool Play(EntityID entity) = 0;
    // Emission anhalten; clear = vorhandene Partikel sofort entfernen,
    // sonst laufen sie bis zum Ende ihrer Lebensdauer weiter.
    virtual bool Stop(EntityID entity, bool clear) = 0;
    [[nodiscard]] virtual bool IsPlaying(EntityID entity) const = 0;
    // count Partikel sofort am Emitter erzeugen, auch wenn er nicht spielt.
    virtual bool Burst(EntityID entity, uint32_t count) = 0;
    // Einmaliger Effekt: count Partikel mit den Einstellungen des Emitters
    // templateEntity an position. Die Vorlage selbst muss nicht spielen.
    virtual bool EmitAt(EntityID templateEntity, const math::Vec3& position, uint32_t count) = 0;
    [[nodiscard]] virtual uint32_t ParticleCount(EntityID entity) const = 0;
    // root (samt Kindern) entfernen, sobald alle Emitter darunter fertig sind.
    virtual void DestroyWhenFinished(EntityID root) = 0;

    virtual bool SetEmitterTexture(EntityID emitter, const std::string& texturePath) = 0;
    virtual bool SetEmitterAtlasGrid(EntityID emitter, uint32_t columns, uint32_t rows) = 0;
    virtual bool SetEmitterFrames(EntityID emitter, std::span<const ParticleFrame> frames) = 0;
    virtual uint32_t GetEmitterFrameCount(EntityID emitter) = 0;
    virtual ParticleID SpawnParticle(EntityID emitter, const math::Vec3& position) = 0;
    virtual bool SetParticlePosition(ParticleID particle, const math::Vec3& position) = 0;
    virtual bool SetParticleVelocity(ParticleID particle, const math::Vec3& velocity) = 0;
    virtual bool SetParticleMotionEnabled(ParticleID particle, bool enabled) = 0;
    virtual bool SetParticleSize(ParticleID particle, const math::Vec2& size) = 0;
    virtual bool SetParticleFrame(ParticleID particle, uint32_t frame) = 0;
    virtual bool PlayParticleFrames(ParticleID particle, uint32_t firstFrame, uint32_t lastFrame, float fps, bool loop = false) = 0;
    virtual bool PlayParticleFramesOverLifetime(ParticleID particle, uint32_t firstFrame, uint32_t lastFrame) = 0;
    virtual bool SetParticleFrameProgress(ParticleID particle, uint32_t firstFrame, uint32_t lastFrame, float progress) = 0;
    virtual bool SetParticleAnimationPaused(ParticleID particle, bool paused) = 0;
    virtual std::optional<uint32_t> GetParticleFrame(ParticleID particle) = 0;
    virtual bool IsParticleAlive(ParticleID particle) = 0;
    virtual bool RemoveParticle(ParticleID particle) = 0;
    virtual bool PlayParticleClip(ParticleID particle, std::string_view name) = 0;
    virtual std::optional<ParticleClip> FindEmitterClip(EntityID emitter, std::string_view name) = 0;
};

} // namespace engine::script
