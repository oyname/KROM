#include "Collectible.hpp"

#include "addons/physics/PhysicsComponents.hpp"

#include <cmath>

void Collectible::OnStart(engine::script::IScriptContext& ctx)
{
    basePosition = ctx.GetPosition();
}

void Collectible::OnUpdate(engine::script::IScriptContext& ctx)
{
    // DeltaTime ist skaliert: in der Pause steht das Objekt still.
    const float dt = ctx.DeltaTime();
    phase += dt;
    ctx.RotateEulerDeg(0.0f, spinDegPerSecond * dt, 0.0f, engine::script::Space::World);
    ctx.SetPosition(basePosition + engine::math::Vec3{ 0.f, std::sin(phase * 2.5f) * bobHeight, 0.f });
}

void Collectible::OnTriggerEnter(engine::script::IScriptContext& ctx, engine::EntityID other)
{
    if (collected || !ctx.Has<engine::CharacterControllerComponent>(other))
        return;

    collected = true;
    ctx.BroadcastEvent("Collected");
    ctx.Destroy();
}
