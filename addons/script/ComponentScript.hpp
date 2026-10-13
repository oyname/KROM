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

    [[nodiscard]] EntityID GetEntity() const noexcept { return m_entity; }
    void SetEntity(EntityID id) noexcept { m_entity = id; }

private:
    EntityID m_entity = NULL_ENTITY;
};

} // namespace engine::script
