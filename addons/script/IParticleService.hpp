#pragma once
// =============================================================================
// KROM Engine - addons/script/IParticleService.hpp
// Schnittstelle, ueber die Scripts Partikel-Emitter steuern. Ohne Abhaengigkeit
// zum Partikel-Addon: die Runtime stellt einen Adapter auf ParticleSystem bereit.
// Ohne Partikel ist ScriptServices::particles null und die Helfer sind neutral.
// =============================================================================
#include "core/Math.hpp"
#include "core/Types.hpp"

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
};

} // namespace engine::script
