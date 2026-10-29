#pragma once
#include "krom.h"

// Ausgang (OBBComponent mit isTrigger): Betritt die Spielfigur den Bereich,
// wird nextScene geladen.
class ExitZone final : public engine::script::ComponentScript
{
public:
    std::string nextScene = "Scenes/Ziel.kscene";

    void OnTriggerEnter(engine::script::IScriptContext& ctx, engine::EntityID other) override;

private:
    bool triggered = false;
};

// KROM_SCRIPT(ExitZone)
// KROM_FIELD(ExitZone, nextScene, String)
