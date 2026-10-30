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
#include <unordered_map>
#include <vector>

namespace engine::particles {

struct Particle
{
    math::Vec3 position{};
    math::Vec3 velocity{};
    float age      = 0.f;
    float lifetime = 1.f;
};

// Ein Partikel fuer den Renderer: Weltposition, Kantenlaenge, lineare Farbe
// (bereits mit intensity multipliziert).
struct ParticleBillboard
{
    math::Vec3 position{};
    float      size = 0.f;
    math::Vec4 color{};
};

class ParticleSystem
{
public:
    explicit ParticleSystem(uint32_t seed = 0x2545F491u) noexcept;

    void Update(const ecs::World& world, float dt);

    // Vergisst alle Emitter und Partikel (z.B. bei Szenenwechsel).
    void Clear() noexcept;
    // Startet einen Emitter neu: Partikel weg, Zeit 0, Burst erneut.
    void Restart(EntityID entity);

    [[nodiscard]] size_t   EmitterCount() const noexcept { return m_emitters.size(); }
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
        bool  seen            = false;   // in diesem Update noch vorhanden
    };

    void Spawn(EmitterState& state, const WorldTransformComponent& transform);
    float Random01() noexcept;           // [0, 1)
    float RandomSigned() noexcept;       // [-1, 1)

    std::unordered_map<EntityID, EmitterState> m_emitters;
    uint32_t m_rng;
};

// sRGB → linear fuer eine Farbkomponente (Alpha bleibt linear).
[[nodiscard]] float SrgbToLinear(float c) noexcept;

} // namespace engine::particles
