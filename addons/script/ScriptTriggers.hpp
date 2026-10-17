#pragma once
#include "addons/script/ScriptList.hpp"
#include <set>
#include <utility>
#include <vector>

namespace engine::script {

// Erkennt Ein-/Austritte in Trigger-Volumen (OBBComponent mit isTrigger) und
// ruft OnTriggerEnter/OnTriggerExit auf den Scripts beider Seiten auf.
//
// Gegenseite sind Kollisions-Boxen (OBB ohne Trigger) und Mesh-Bounds, jeweils
// zusammengefasst auf ihre Hierarchie-Wurzel; die eigene Hierarchie des
// Triggers zaehlt nicht. Getestet wird mit Welt-AABBs (gedrehte Boxen also
// konservativ) auf Basis der zuletzt berechneten World-Transforms/Bounds.
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
