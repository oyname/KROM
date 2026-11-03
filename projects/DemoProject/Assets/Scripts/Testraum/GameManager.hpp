#pragma once
#include "krom.h"

// Spielablauf des Testraums: zaehlt eingesammelte Objekte ("Collected"),
// schaltet pro Objekt eine Lampe ein ("<lampPrefix> N An" aktiv,
// "<lampPrefix> N Aus" inaktiv) und oeffnet nach dem letzten die Ausgangstuer.
// Die Spielzeit laeuft mit skalierter Zeit, steht also in der Pause.
// HUD per Bildschirmtext: Zaehler "n/3" und Spielzeit oben links,
// Bedienhinweise unten, Erfolgsmeldung bzw. "Pause" in der Mitte.
class GameManager final : public engine::script::ComponentScript
{
public:
    engine::EntityID exitDoor{};
    int required = 3;
    std::string lampPrefix = "Lampe";
    std::string collectSound = "Audio/Testraum/collect.wav";
    std::string unlockSound = "Audio/Testraum/unlock.wav";

    void OnStart(engine::script::IScriptContext& ctx) override;
    void OnUpdate(engine::script::IScriptContext& ctx) override;
    void OnEvent(engine::script::IScriptContext& ctx, const engine::script::ScriptEvent& event) override;

private:
    void SetLamp(engine::script::IScriptContext& ctx, int number, bool on);
    void DrawHud(engine::script::IScriptContext& ctx);

    int   collected = 0;
    float elapsed = 0.0f;
    bool  finished = false;
};

// KROM_SCRIPT(GameManager)
// KROM_FIELD(GameManager, exitDoor, Entity)
// KROM_FIELD(GameManager, required, Int)
// KROM_FIELD(GameManager, lampPrefix, String)
// KROM_FIELD(GameManager, collectSound, String)
// KROM_FIELD(GameManager, unlockSound, String)
