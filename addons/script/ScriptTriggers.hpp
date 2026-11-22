#pragma once
#include "addons/script/ScriptList.hpp"
#include <set>
#include <utility>
#include <vector>

namespace engine::script {

// Detects entering/leaving trigger volumes (OBBComponent with isTrigger) and calls
// OnTriggerEnter/OnTriggerExit on the scripts of both sides.
//
// The other side are collision boxes (OBB without trigger) and mesh bounds, each
// merged to its hierarchy root; the trigger's own hierarchy does not count.
// The test uses world AABBs (rotated boxes are thus conservative) from the CURRENT
// transforms: Update() runs in the script phase after all OnUpdate calls and before
// the frame's TransformSystem::Update, so entities with a TransformComponent are
// evaluated from the local transforms of their parent chain
// (TransformSystem::ComputeCurrentWorldTransform, BoundsSystem::ComputeCurrentWorldBounds),
// and something a script moved in this OnUpdate enters/exits in this frame. Entities
// with only a WorldTransformComponent use the stored world transform. Reads only.
class ScriptTriggerSystem
{
public:
    void Update(ecs::World& world, const ScriptServices& services);

    // Vergisst alle aktiven Kontakte ohne Exit-Callbacks (z.B. bei Szenenwechsel).
    void Reset() noexcept { m_active.clear(); }

    [[nodiscard]] size_t ActiveContactCount() const noexcept { return m_active.size(); }

private:
    // (Trigger-Entity, Wurzel der Gegenseite)
    using Contact = std::pair<EntityID, EntityID>;
    struct ContactLess
    {
        bool operator()(const Contact& a, const Contact& b) const noexcept
        {
            return a.first.value != b.first.value ? a.first.value < b.first.value
                                                  : a.second.value < b.second.value;
        }
    };

    std::set<Contact, ContactLess> m_active;
};

// Stellt eine Physik-Kollision zwischen den Koerper-Entities a und b zu:
// OnCollisionEnter/Exit beim Script von a (other = Wurzel von b) und umgekehrt.
// Tote Entities werden uebersprungen.
void DispatchCollision(ecs::World& world, bool enter, EntityID a, EntityID b,
                       const ScriptServices& services);

// Stellt die in services.pendingEvents gesammelten Events zu (Ziel-Entity bzw.
// alle Scripts bei Broadcast). Events, die waehrend der Zustellung entstehen,
// bleiben in services.pendingEvents fuer den naechsten Aufruf.
void DispatchScriptEvents(ecs::World& world, const ScriptServices& services);

} // namespace engine::script
