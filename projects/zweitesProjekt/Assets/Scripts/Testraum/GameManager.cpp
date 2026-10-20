#include "GameManager.hpp"

#include "core/Debug.hpp"

void GameManager::OnStart(engine::script::IScriptContext& ctx)
{
    // Startzustand: alle Lampen aus, unabhaengig davon, wie die Szene sie speichert.
    for (int i = 1; i <= required; ++i)
        SetLamp(ctx, i, false);
    engine::Debug::Log("Testraum: Start - sammle %d Objekte (P = Pause, R = Neustart)", required);
}

void GameManager::OnUpdate(engine::script::IScriptContext& ctx)
{
    if (!finished)
        elapsed += ctx.DeltaTime();
}

void GameManager::OnEvent(engine::script::IScriptContext& ctx, const engine::script::ScriptEvent& event)
{
    if (event.name != "Collected" || finished)
        return;

    ++collected;
    SetLamp(ctx, collected, true);
    ctx.PlayOneShot(collectSound);
    engine::Debug::Log("Testraum: eingesammelt %d/%d", collected, required);

    if (collected >= required)
    {
        finished = true;
        ctx.SendEvent(exitDoor, "Open");
        ctx.PlayOneShot(unlockSound);
        engine::Debug::Log("Testraum: Ausgang offen nach %.1f s", elapsed);
    }
}

void GameManager::SetLamp(engine::script::IScriptContext& ctx, int number, bool on)
{
    const std::string base = lampPrefix + " " + std::to_string(number);
    if (auto* lampOn = ctx.Get<engine::ActiveComponent>(ctx.FindEntity(base + " An")))
        lampOn->active = on;
    if (auto* lampOff = ctx.Get<engine::ActiveComponent>(ctx.FindEntity(base + " Aus")))
        lampOff->active = !on;
}
