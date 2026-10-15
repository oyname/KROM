#pragma once
// =============================================================================
// KROM Engine - addons/physics/PhysicsComponents.hpp
// Physik-Komponenten des Physics-Addons. Bewusst ohne Jolt-Abhaengigkeit,
// damit Editor und Serialisierung sie ohne die Simulation nutzen koennen.
//
// Koerper und Collider:
//   • RigidBodyComponent macht eine Entity samt Hierarchie zu EINEM Koerper.
//     Alle Collider an ihr und an ihren Kindern (ohne eigenen Rigidbody)
//     werden zu einer gemeinsamen Form zusammengefasst.
//   • Collider ohne Rigidbody in der Hierarchie sind statische Hindernisse.
//   • Box-Collider ist das Core-OBBComponent (isTrigger = false); Kugel und
//     Kapsel liegen hier im Addon.
// =============================================================================
#include "core/Math.hpp"
#include "ecs/ComponentMeta.hpp"

#include <cstdint>

namespace engine {

enum class RigidBodyType : uint8_t
{
    Static    = 0,  // bewegt sich nie, blockiert nur
    Dynamic   = 1,  // simuliert: Schwerkraft, Kraefte, Kollisionen
    Kinematic = 2,  // folgt der Transform (Script/Animation), schiebt Dynamische weg
};

struct RigidBodyComponent
{
    RigidBodyType type = RigidBodyType::Dynamic;
    // false: Koerper samt Collidern nimmt nicht an der Physik teil.
    bool  enabled        = true;
    float mass           = 1.f;    // kg, nur Dynamic
    float friction       = 0.5f;
    float restitution    = 0.f;    // 0 = kein Abprallen, 1 = voll elastisch
    float linearDamping  = 0.05f;
    float angularDamping = 0.05f;
    float gravityScale   = 1.f;
    bool  lockRotationX  = false;
    bool  lockRotationY  = false;
    bool  lockRotationZ  = false;
};

struct SphereColliderComponent
{
    math::Vec3 centerOffset{ 0.f, 0.f, 0.f };
    float      radius       = 0.5f;   // bei ungleichmaessiger Skalierung: groesste Achse
    bool       showInEditor = false;
};

enum class CapsuleAxis : uint8_t { X = 0, Y = 1, Z = 2 };

struct CapsuleColliderComponent
{
    math::Vec3  centerOffset{ 0.f, 0.f, 0.f };
    float       radius       = 0.5f;
    float       height       = 2.f;   // Gesamthoehe inklusive beider Halbkugeln
    CapsuleAxis axis         = CapsuleAxis::Y;
    bool        showInEditor = false;
};

inline void RegisterPhysicsComponents(ecs::ComponentMetaRegistry& registry)
{
    using namespace ecs;
    RegisterComponent<RigidBodyComponent>(registry, "RigidBodyComponent");
    RegisterComponent<SphereColliderComponent>(registry, "SphereColliderComponent");
    RegisterComponent<CapsuleColliderComponent>(registry, "CapsuleColliderComponent");
}

} // namespace engine
