#pragma once
// =============================================================================
// KROM Engine - addons/particles/ParticleComponents.hpp
// Partikel-Emitter des Partikel-Addons (nur Daten). Simulation: ParticleSystem,
// Darstellung: ParticleRenderer.
//
// Partikel entstehen am Weltursprung der Entity (plus emitterRadius) und
// fliegen danach unabhaengig vom Emitter im Weltraum. Farben in sRGB wie im
// Inspector, Alpha linear; intensity > 1 macht sie HDR-hell (Bloom).
// Ein nicht spielender Emitter (playing = false) eignet sich als Vorlage fuer
// Script-Effekte an beliebiger Position (IScriptContext::EmitParticlesAt).
// =============================================================================

#include "core/Math.hpp"
#include "ecs/ComponentMeta.hpp"

#include <cstdint>
#include <string>

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
    // Bild relativ zum Asset-Root; leer = weiche Scheibe. Farbe multipliziert.
    std::string   texturePath;
    // Flipbook: Bild in Spalten x Zeilen Einzelbilder (Zeile fuer Zeile).
    // Animiert ueber die Lebensdauer oder (randomFrame) ein festes Zufallsbild.
    uint32_t      flipbookColumns  = 1u;
    uint32_t      flipbookRows     = 1u;
    bool          flipbookRandomFrame = false;
    // Bodenkollision mit einer waagrechten Ebene (billig, wie Unity "Planes").
    // floorRelative: floorHeight ist der Abstand zur Emitter-Hoehe, sonst Welt-Y.
    bool          collideWithFloor = false;
    float         floorHeight      = 0.f;
    bool          floorRelative    = true;
    float         bounce           = 0.4f;   // 0 = liegen bleiben, 1 = voll zurueck
    float         floorFriction    = 0.2f;   // Anteil seitlicher Geschwindigkeit, der je Aufprall verloren geht
    bool          dieOnFloor       = false;  // beim Aufprall verschwinden (z. B. Regen)
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
