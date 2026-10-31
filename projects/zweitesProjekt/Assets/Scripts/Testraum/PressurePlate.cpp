#include "PressurePlate.hpp"

#include "core/Debug.hpp"

void PressurePlate::OnStart(engine::script::IScriptContext& ctx)
{
    restPosition = ctx.GetPosition();
}

void PressurePlate::OnTriggerEnter(engine::script::IScriptContext& ctx, engine::EntityID other)
{
    (void)other;
    if (++occupants != 1)
        return;

    ctx.SetPosition(restPosition - engine::math::Vec3{ 0.f, pressDepth, 0.f });
    ctx.PlayOneShotAt(clickSound, restPosition);
    ctx.SendEvent(door, "Open");
    if (sparks.IsValid())
        ctx.PlayParticles(sparks);   // von vorn: Burst + laufende Funken
    if (!dustEffect.empty())
        ctx.SpawnEffect(dustEffect, restPosition);   // einmalig, ohne Emitter in der Szene
    engine::Debug::Log("Testraum: Bodenschalter gedrueckt");
}

void PressurePlate::OnTriggerExit(engine::script::IScriptContext& ctx, engine::EntityID other)
{
    (void)other;
    if (occupants == 0 || --occupants != 0)
        return;

    ctx.SetPosition(restPosition);
    ctx.PlayOneShotAt(clickSound, restPosition, 0.6f);
    ctx.SendEvent(door, "Close");
    if (sparks.IsValid())
        ctx.StopParticles(false, sparks);   // vorhandene Funken fliegen aus
    engine::Debug::Log("Testraum: Bodenschalter frei");
}
