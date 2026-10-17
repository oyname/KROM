#pragma once
// =============================================================================
// KROM Engine - addons/script/IPhysicsService.hpp
// Schnittstelle, ueber die Scripts die Physik ansteuern. Bewusst ohne
// Abhaengigkeit zum Physics-Addon: die Runtime stellt eine Implementierung
// bereit (Adapter auf PhysicsWorld), Tests koennen einen Mock einsetzen.
// Ohne Physik ist ScriptServices::physics null und die Script-Helfer sind
// neutral (false / Null-Vektor).
// =============================================================================
#include "collision/SceneQueries.hpp"
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

    // Character Controller (CharacterControllerComponent). Standard: keine Figuren.
    virtual bool SetCharacterMove(EntityID entity, const math::Vec3& horizontalVelocity)
    { (void)entity; (void)horizontalVelocity; return false; }
    virtual bool CharacterJump(EntityID entity, float speed) { (void)entity; (void)speed; return false; }
    virtual bool IsCharacterGrounded(EntityID entity) const { (void)entity; return false; }

    // Strahl gegen die exakten Kollisionsformen; outHit.entity = Koerper-Entity.
    virtual bool Raycast(const math::Vec3& origin, const math::Vec3& direction, float maxDistance,
                         collision::RaycastHit& outHit, EntityID ignoreEntity) const
    { (void)origin; (void)direction; (void)maxDistance; (void)outHit; (void)ignoreEntity; return false; }
};

} // namespace engine::script
