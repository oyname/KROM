#pragma once
// =============================================================================
// KROM Engine - addons/particles/ParticleSystem.hpp
// CPU-Simulation des Partikel-Addons, ohne GPU testbar.
//
// Pro Frame nach dem TransformSystem: Update(world, dt) liest Emitter und
// WorldTransform (die World wird nicht veraendert), erzeugt, bewegt und
// entfernt Partikel. Der Zustand haengt an der EntityID; entfernte Emitter
// verschwinden samt Partikeln. CollectBillboards liefert die Partikel des
// Frames fuer den ParticleRenderer.
// =============================================================================

#include "addons/particles/ParticleComponents.hpp"
#include "core/Math.hpp"
#include "core/Types.hpp"
#include "ecs/Components.hpp"
#include "ecs/World.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace engine::particles {

struct Particle
{
    math::Vec3 position{};
    math::Vec3 velocity{};
    float age      = 0.f;
    float lifetime = 1.f;
    uint32_t frame = 0u;     // Zufallsbild bei flipbookRandomFrame
};

// Ein Partikel fuer den Renderer: Weltposition, Kantenlaenge, lineare Farbe
// (bereits mit intensity multipliziert), Textur und Bildausschnitt.
struct ParticleBillboard
{
    math::Vec3 position{};
    float      size = 0.f;
    math::Vec4 color{};
    // Asset-Pfad der Textur (zeigt in die Emitter-Daten, gueltig bis zum
    // naechsten Update); nullptr = weiche Scheibe.
    const std::string* texture = nullptr;
    math::Vec4 uvRect{0.f, 0.f, 1.f, 1.f};   // u0, v0, u1, v1
};

class ParticleSystem
{
public:
    explicit ParticleSystem(uint32_t seed = 0x2545F491u) noexcept;

    // only != nullptr: nur diese Emitter simulieren; alle anderen werden samt
    // Partikeln verworfen und starten neu, sobald sie wieder dabei sind
    // (Editor: nur die Auswahl spielt ab).
    void Update(const ecs::World& world, float dt,
                const std::unordered_set<EntityID>* only = nullptr);

    // Vergisst alle Emitter und Partikel (z.B. bei Szenenwechsel).
    void Clear() noexcept;
    // Startet einen Emitter neu: Partikel weg, Zeit 0, Burst erneut.
    void Restart(EntityID entity);
    // Entfernt die vorhandenen Partikel eines Emitters (z.B. beim harten Stopp).
    void ClearParticles(EntityID entity);
    // count Partikel beim naechsten Update am Emitter erzeugen, auch wenn er
    // nicht spielt (maxParticles gilt weiter).
    void Burst(EntityID entity, uint32_t count);
    // Einmaliger Effekt ohne Entity: count Partikel mit den Einstellungen von
    // params an position (Ausrichtung rotation). Lebt, bis alle Partikel weg sind.
    // Ein relativer Boden bezieht sich auf position.
    void EmitAt(const ParticleEmitterComponent& params, const math::Vec3& position,
                uint32_t count, const math::Quat& rotation = math::Quat::Identity());

    [[nodiscard]] size_t   EmitterCount() const noexcept { return m_emitters.size(); }
    [[nodiscard]] size_t   DetachedEffectCount() const noexcept { return m_detached.size(); }
    [[nodiscard]] uint32_t ParticleCount() const noexcept;
    [[nodiscard]] uint32_t ParticleCount(EntityID entity) const noexcept;
    [[nodiscard]] const std::vector<Particle>* Particles(EntityID entity) const noexcept;

    // alpha: von hinten nach vorn sortiert (Abstand zu cameraPosition).
    // additive: unsortiert. Beide Listen werden vorher geleert.
    void CollectBillboards(const math::Vec3& cameraPosition,
                           std::vector<ParticleBillboard>& alpha,
                           std::vector<ParticleBillboard>& additive) const;

private:
    struct EmitterState
    {
        ParticleEmitterComponent params{};
        std::vector<Particle> particles;
        float emitAccumulator = 0.f;
        float time            = 0.f;     // seit Start, fuer duration
        bool  burstDone       = false;
        float floorY          = 0.f;     // Welt-Hoehe der Bodenebene (collideWithFloor)
        bool  seen            = false;   // in diesem Update noch vorhanden
    };

    void Spawn(EmitterState& state, const WorldTransformComponent& transform);
    static void Simulate(EmitterState& state, float dt);
    float Random01() noexcept;           // [0, 1)
    float RandomSigned() noexcept;       // [-1, 1)

    std::unordered_map<EntityID, EmitterState> m_emitters;
    std::unordered_map<EntityID, uint32_t> m_pendingBursts;
    std::vector<EmitterState> m_detached;   // EmitAt-Effekte
    uint32_t m_rng;
};

// sRGB → linear fuer eine Farbkomponente (Alpha bleibt linear).
[[nodiscard]] float SrgbToLinear(float c) noexcept;

} // namespace engine::particles
