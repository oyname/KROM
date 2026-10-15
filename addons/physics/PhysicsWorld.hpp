#pragma once
// =============================================================================
// KROM Engine - addons/physics/PhysicsWorld.hpp
// Physik-Simulation des Physics-Addons (intern Jolt, nach aussen Jolt-frei).
//
// Pro Frame, nach dem Script-Update und vor dem TransformSystem:
//   Update(world, dt) = Sync(world) + Step(dt) + WriteBack(world)
//
//   • Sync      legt Koerper an/entfernt sie, uebernimmt Form-Aenderungen,
//               kinematische/statische Bewegungen und Teleports (Transform
//               wurde von aussen geaendert).
//   • Step      fester Zeitschritt (Akkumulator, max. maxSubSteps pro Frame).
//   • WriteBack schreibt Position/Rotation dynamischer Koerper in die
//               TransformComponent der Rigidbody-Entity (lokal, relativ zum
//               Parent) und markiert sie dirty.
//
// Zuordnung (siehe PhysicsComponents.hpp): oberste aktive RigidBodyComponent
// einer Hierarchie besitzt alle Collider darunter; Collider ohne Rigidbody in
// der Hierarchie werden einzeln zu statischen Koerpern.
// =============================================================================
#include "core/Math.hpp"
#include "core/Types.hpp"
#include "ecs/World.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>

namespace engine::physics {

struct PhysicsSettings
{
    math::Vec3 gravity{ 0.f, -9.81f, 0.f };
    float      fixedTimeStep = 1.f / 60.f;
    uint32_t   maxSubSteps   = 4u;      // pro Frame; Rest der Zeit wird verworfen
    uint32_t   maxBodies     = 16384u;
    uint32_t   workerThreads = 0u;      // 0 = automatisch (max. 4)
};

class PhysicsWorld
{
public:
    PhysicsWorld();
    ~PhysicsWorld();
    PhysicsWorld(const PhysicsWorld&)            = delete;
    PhysicsWorld& operator=(const PhysicsWorld&) = delete;

    bool Initialize(const PhysicsSettings& settings = {});
    void Shutdown();
    [[nodiscard]] bool IsInitialized() const noexcept;

    void Update(ecs::World& world, float dt);
    void Sync(ecs::World& world);
    // Liefert die Anzahl ausgefuehrter fester Schritte.
    uint32_t Step(float dt);
    void WriteBack(ecs::World& world);

    // Entfernt alle Koerper (z.B. bei Szenenwechsel).
    void Reset();

    // entity darf die Rigidbody-Entity oder eines ihrer Collider-Kinder sein.
    // Wirkt nur auf dynamische Koerper; false, wenn es keinen gibt.
    bool AddForce(EntityID entity, const math::Vec3& force);
    bool AddImpulse(EntityID entity, const math::Vec3& impulse);
    bool SetLinearVelocity(EntityID entity, const math::Vec3& velocity);
    bool GetLinearVelocity(EntityID entity, math::Vec3& outVelocity) const;

    // Aktuelle Pose des Koerpers in Jolt (beliebiger Typ); false ohne Koerper.
    bool GetBodyPose(EntityID entity, math::Vec3& outPosition, math::Quat& outRotation) const;

    [[nodiscard]] size_t BodyCount() const noexcept;
    [[nodiscard]] bool   HasBody(EntityID entity) const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace engine::physics
