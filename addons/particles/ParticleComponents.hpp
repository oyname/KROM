#pragma once
// =============================================================================
// KROM Engine - addons/particles/ParticleComponents.hpp
// Partikel-Emitter des Partikel-Addons (nur Daten). Simulation: ParticleSystem,
// Darstellung: ParticleRenderer.
//
// Partikel entstehen am Weltursprung der Entity (plus emitterRadius) und
// fliegen danach unabhaengig vom Emitter im Weltraum. Farben in sRGB wie im
// Inspector, Alpha linear; intensity > 1 macht sie HDR-hell (Bloom).
// =============================================================================

#include "core/Math.hpp"
#include "ecs/ComponentMeta.hpp"

#include <cstdint>

namespace engine {

enum class ParticleBlendMode : uint8_t
{
    Alpha    = 0,   // Rauch, Staub: nach Tiefe sortiert, ueberdeckend
    Additive = 1,   // Feuer, Funken, Magie: addiert Licht, keine Sortierung
};

struct ParticleEmitterComponent
{
    uint32_t      maxParticles     = 1000u;
    float         emitRate         = 50.f;     // Partikel pro Sekunde
    uint32_t      burstCount       = 0u;       // einmalig beim Start
    float         particleLifetime = 2.f;      // Sekunden
    float         lifetimeRandom   = 0.2f;     // ± Anteil der Lebensdauer (0..1)
    float         emitterRadius    = 0.f;      // Startpunkt zufaellig in dieser Kugel
    math::Vec3    initialVelocity  { 0.f, 1.f, 0.f };     // in Emitter-Ausrichtung
    math::Vec3    velocityRandom   { 0.5f, 0.5f, 0.5f };  // ± je Achse
    math::Vec3    gravity          { 0.f, -9.81f, 0.f };  // Weltraum
    float         drag             = 0.f;      // Luftwiderstand pro Sekunde
    float         startSize        = 0.1f;     // Kantenlaenge in Metern
    float         endSize          = 0.0f;
    math::Vec4    startColor       { 1.f, 1.f, 1.f, 1.f };
    math::Vec4    endColor         { 1.f, 1.f, 1.f, 0.f };
    float         intensity        = 1.f;
    ParticleBlendMode blendMode    = ParticleBlendMode::Alpha;
    bool          looping          = true;
    float         duration         = 1.f;      // Emissionsdauer ohne looping
    bool          playing          = true;
    bool operator==(const ParticleEmitterComponent&) const = default;
};

inline void RegisterParticleComponents(ecs::ComponentMetaRegistry& registry)
{
    using namespace ecs;
    RegisterComponent<ParticleEmitterComponent>(registry, "ParticleEmitterComponent");
}

} // namespace engine
