#pragma once
// =============================================================================
// KROM Engine - addons/script/IPhysicsService.hpp
// Schnittstelle, ueber die Scripts die Physik ansteuern. Bewusst ohne
// Abhaengigkeit zum Physics-Addon: die Runtime stellt eine Implementierung
// bereit (Adapter auf PhysicsWorld), Tests koennen einen Mock einsetzen.
// Ohne Physik ist ScriptServices::physics null und die Script-Helfer sind
// neutral (false / Null-Vektor).
// =============================================================================
#include "core/Math.hpp"
#include "core/Types.hpp"

namespace engine::script {

class IPhysicsService
{
public:
    virtual ~IPhysicsService() = default;

    // entity darf die Rigidbody-Entity oder eines ihrer Collider-Kinder sein.
    // Wirkt nur auf dynamische Koerper. Befehle fuer Koerper, die erst beim
    // naechsten Physik-Sync entstehen (z.B. frisch gespawnte Prefabs), werden
    // vorgemerkt. false = Entity hat einen nicht-dynamischen Koerper bzw. es
    // gibt keine aktive Physik.
    virtual bool AddForce(EntityID entity, const math::Vec3& force) = 0;
    virtual bool AddImpulse(EntityID entity, const math::Vec3& impulse) = 0;
    virtual bool SetLinearVelocity(EntityID entity, const math::Vec3& velocity) = 0;
    virtual bool GetLinearVelocity(EntityID entity, math::Vec3& outVelocity) const = 0;
};

} // namespace engine::script
