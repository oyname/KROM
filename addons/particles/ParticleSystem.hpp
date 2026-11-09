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
#include "addons/particles/ParticleAnimation.hpp"
#include "core/Math.hpp"
#include "core/Types.hpp"
#include "ecs/Components.hpp"
#include "ecs/World.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <memory>
#include <optional>
#include <span>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace engine::particles {

struct ParticleLook
{
    std::string texture;
    uint32_t columns = 1u, rows = 1u;
    std::vector<ParticleFrame> frames;
    ParticleFrameSettings animation;
    std::vector<ParticleClip> clips;
    [[nodiscard]] uint32_t FrameCount() const noexcept;
};

enum class FrameMode : uint8_t { Emitter, Fixed, Rate, OverLifetime, Manual };

struct Particle
{
    math::Vec3 position{};
    math::Vec3 velocity{};
    float age      = 0.f;
    float lifetime = 1.f;
    uint32_t frame = 0u;
    ParticleID id{};
    std::shared_ptr<const ParticleLook> look;
    math::Vec2 sizeOverride{};
    uint32_t firstFrame = 0u, lastFrame = 0u;
    double fps = 0., animTime = 0., animLength = 0.;
    FrameMode frameMode = FrameMode::Emitter;
    bool loop = false, paused = false, motion = true;
    uint32_t randomFrameIndex = 0u;
};

// Ein Partikel fuer den Renderer: Weltposition, Kantenlaenge, lineare Farbe
// (bereits mit intensity multipliziert), Textur und Bildausschnitt.
struct ParticleBillboard
{
    math::Vec3 position{};
    math::Vec2 size{};
    math::Vec2 pivot{0.5f, 0.5f};
    math::Vec4 color{};
    // Asset-Pfad der Textur (zeigt in die Emitter-Daten, gueltig bis zum
    // naechsten Update); nullptr = weiche Scheibe.
    const std::string* texture = nullptr;
    math::Vec4 uvRect{0.f, 0.f, 1.f, 1.f};   // u0, v0, u1, v1
    std::shared_ptr<const ParticleLook> look;
    uint32_t frame = 0u;
    bool gridAspect = false;
};

class ParticleSystem
{
public:
    explicit ParticleSystem(uint32_t seed = 0x2545F491u) noexcept;
    ParticleSystem(const ParticleSystem&) = delete;
    ParticleSystem& operator=(const ParticleSystem&) = delete;

    ParticleID SpawnParticle(const ecs::World& world, EntityID emitter, const math::Vec3& position);
    bool SetParticlePosition(ParticleID id, const math::Vec3& position);
    bool SetParticleVelocity(ParticleID id, const math::Vec3& velocity);
    bool SetParticleMotionEnabled(ParticleID id, bool enabled);
    bool SetParticleSize(ParticleID id, const math::Vec2& size);
    bool SetParticleFrame(ParticleID id, uint32_t frame);
    bool PlayParticleFrames(ParticleID id, uint32_t first, uint32_t last, float fps, bool loop = false);
    bool PlayParticleFramesOverLifetime(ParticleID id, uint32_t first, uint32_t last);
    bool PlayParticleClip(ParticleID id, std::string_view name);
    bool SetParticleFrameProgress(ParticleID id, uint32_t first, uint32_t last, float progress);
    bool SetParticleAnimationPaused(ParticleID id, bool paused);
    [[nodiscard]] std::optional<uint32_t> GetParticleFrame(ParticleID id) const;
    [[nodiscard]] bool IsParticleAlive(ParticleID id) const;
    [[nodiscard]] EntityID ParticleEmitter(ParticleID id) const;
    bool RemoveParticle(ParticleID id);

    // only != nullptr: nur diese Emitter simulieren; alle anderen werden samt
    // Partikeln verworfen und starten neu, sobald sie wieder dabei sind
    // (Editor: nur die Auswahl spielt ab).
    void Update(const ecs::World& world, float dt,
                const std::unordered_set<EntityID>* only = nullptr);

    // Vergisst alle Emitter und Partikel (z.B. bei Szenenwechsel).
    void Clear() noexcept;
    // Startet einen Emitter neu: Partikel weg, Zeit 0, Prewarm und Burst erneut.
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

    // Emitter fertig: keine Partikel mehr und es kommen keine neuen (aus oder
    // Emissionsdauer ohne Schleife vorbei). false, solange er noch nicht simuliert wurde.
    [[nodiscard]] bool IsFinished(EntityID entity) const noexcept;
    // Wurzeln mit ParticleEffectAutoDestroyComponent, deren Emitter (samt Kindern)
    // alle fertig sind - zum Entfernen durch die Runtime.
    void CollectFinishedEffects(const ecs::World& world, std::vector<EntityID>& out) const;

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
        EntityID entity = NULL_ENTITY;
        std::shared_ptr<const ParticleLook> look;
    };

    struct Slot
    {
        EmitterState* pool = nullptr;
        size_t index = 0u;
        uint32_t generation = 1u;
    };
    friend struct ParticleSystemTestAccess;
    [[nodiscard]] Slot* FindSlot(ParticleID id);
    [[nodiscard]] const Slot* FindSlot(ParticleID id) const;
    void RemoveAt(EmitterState& state, size_t index);
    void ClearPool(EmitterState& state);
    static void RefreshLook(EmitterState& state);
    ParticleID Spawn(EmitterState& state, const WorldTransformComponent& transform, bool exactPosition = false);
    void SpawnMany(EmitterState& state, const WorldTransformComponent& transform, uint32_t count);
    void EmitParticles(EmitterState& state, const WorldTransformComponent& transform, float dt);
    void Emit(EmitterState& state, const WorldTransformComponent& transform, float dt);
    void Prewarm(EmitterState& state, const WorldTransformComponent& transform);
    void Simulate(EmitterState& state, float dt, size_t firstParticle = 0u);
    static void UpdateFrame(Particle& particle);
    static math::Vec2 ParticleSize(const Particle& particle, const ParticleEmitterComponent& params);
    static float FloorOffset(const Particle& particle, const ParticleEmitterComponent& params);
    void LiftFromFloor(Slot& slot);
    float Random01() noexcept;           // [0, 1)
    float RandomSigned() noexcept;       // [-1, 1)
    math::Vec3 SampleSpawnOffset(const ParticleEmitterComponent& params) noexcept;

    std::unordered_map<EntityID, EmitterState> m_emitters;
    std::unordered_map<EntityID, uint32_t> m_pendingBursts;
    std::vector<std::unique_ptr<EmitterState>> m_detached;
    std::vector<Slot> m_slots;
    std::vector<uint32_t> m_freeSlots;
    uint32_t m_system;
    uint32_t m_rng;
};

// sRGB → linear fuer eine Farbkomponente (Alpha bleibt linear).
[[nodiscard]] float SrgbToLinear(float c) noexcept;

} // namespace engine::particles
