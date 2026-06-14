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

    [[nodiscard]] EntityID GetEntity() const noexcept { return m_entity; }
    void SetEntity(EntityID id) noexcept { m_entity = id; }

private:
    EntityID m_entity = NULL_ENTITY;
};

} // namespace engine::script
