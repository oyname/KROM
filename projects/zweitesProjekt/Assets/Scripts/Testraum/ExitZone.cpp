#include "ExitZone.hpp"

#include "addons/physics/PhysicsComponents.hpp"
#include "core/Debug.hpp"

void ExitZone::OnTriggerEnter(engine::script::IScriptContext& ctx, engine::EntityID other)
{
    if (triggered || nextScene.empty() || !ctx.Has<engine::CharacterControllerComponent>(other))
        return;

    triggered = true;
    engine::Debug::Log("Testraum: Ausgang erreicht, lade %s", nextScene.c_str());
    ctx.LoadScene(nextScene);
}
