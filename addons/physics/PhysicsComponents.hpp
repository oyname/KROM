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
    bool operator==(const RigidBodyComponent&) const = default;
};

struct SphereColliderComponent
{
    math::Vec3 centerOffset{ 0.f, 0.f, 0.f };
    float      radius       = 0.5f;   // bei ungleichmaessiger Skalierung: groesste Achse
    bool       showInEditor = false;
    bool operator==(const SphereColliderComponent&) const = default;
};

enum class CapsuleAxis : uint8_t { X = 0, Y = 1, Z = 2 };

struct CapsuleColliderComponent
{
    math::Vec3  centerOffset{ 0.f, 0.f, 0.f };
    float       radius       = 0.5f;
    float       height       = 2.f;   // Gesamthoehe inklusive beider Halbkugeln
    CapsuleAxis axis         = CapsuleAxis::Y;
    bool        showInEditor = false;
    bool operator==(const CapsuleColliderComponent&) const = default;
};

// Spielfigur (Jolt CharacterVirtual): laeuft ueber Boden, Schraegen und Stufen,
// statt wie ein Rigidbody zu rollen oder zu kippen. Form ist eine aufrechte
// Kapsel mit dem Entity-Ursprung an den Fuessen. Gesteuert wird per Script
// (SetMoveVelocity / Jump); die Physik schreibt nur die Position zurueck,
// die Rotation (Blickrichtung) bleibt beim Script. Collider in der Hierarchie
// einer Figur werden ignoriert.
struct CharacterControllerComponent
{
    bool  enabled       = true;
    float radius        = 0.3f;
    float height        = 1.8f;    // Gesamthoehe inklusive beider Halbkugeln
    float maxSlopeDeg   = 50.f;    // steilere Flaechen gelten nicht als Boden
    float stepHeight    = 0.4f;    // Stufen bis zu dieser Hoehe werden erklommen
    float mass          = 70.f;    // kg, bestimmt wie stark Objekte weggeschoben werden
    float gravityScale  = 1.f;
    bool  showInEditor  = true;
    bool operator==(const CharacterControllerComponent&) const = default;
};

inline void RegisterPhysicsComponents(ecs::ComponentMetaRegistry& registry)
{
    using namespace ecs;
    RegisterComponent<RigidBodyComponent>(registry, "RigidBodyComponent");
    RegisterComponent<SphereColliderComponent>(registry, "SphereColliderComponent");
    RegisterComponent<CapsuleColliderComponent>(registry, "CapsuleColliderComponent");
    RegisterComponent<CharacterControllerComponent>(registry, "CharacterControllerComponent");
}

} // namespace engine
