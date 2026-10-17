#pragma once
#include "addons/script/IScriptContext.hpp"

namespace engine::script {

class ComponentScript
{
public:
    virtual ~ComponentScript() = default;

    virtual void OnStart  (IScriptContext&) {}
    virtual void OnUpdate (IScriptContext&) {}
    virtual void OnDestroy(IScriptContext&) {}

    // Trigger (OBBComponent mit isTrigger): wird auf beiden Seiten aufgerufen —
    // beim Script des Triggers und beim Script des eintretenden Objekts.
    // 'other' ist jeweils die Hierarchie-Wurzel der Gegenseite.
    virtual void OnTriggerEnter(IScriptContext&, EntityID /*other*/) {}
    virtual void OnTriggerExit (IScriptContext&, EntityID /*other*/) {}

    // Physik-Kollision (Physics-Addon): beginnt bzw. endet die Beruehrung zweier
    // Koerper, wird auf beiden Seiten aufgerufen — beim Script der Rigidbody-
    // Entity (bzw. der statischen Collider-Entity) jedes Koerpers. 'other' ist
    // die Hierarchie-Wurzel der Gegenseite. Zugestellt im Frame nach dem
    // Physikschritt, waehrend der Script-Phase (Destroy/LoadScene wirken normal).
    virtual void OnCollisionEnter(IScriptContext&, EntityID /*other*/) {}
    virtual void OnCollisionExit (IScriptContext&, EntityID /*other*/) {}

    // Event eines anderen Scripts (ctx.SendEvent / ctx.BroadcastEvent).
    virtual void OnEvent(IScriptContext&, const ScriptEvent& /*event*/) {}

    [[nodiscard]] EntityID GetEntity() const noexcept { return m_entity; }
    void SetEntity(EntityID id) noexcept { m_entity = id; }

private:
    EntityID m_entity = NULL_ENTITY;
};

} // namespace engine::script
