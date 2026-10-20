#pragma once
#include "krom.h"

// Spielsteuerung: P schaltet die Pause um (TimeScale 0 haelt Physik,
// OnFixedUpdate und alle Scripts mit skalierter Zeit an), R laedt
// restartScene neu und setzt damit alle Objekte zurueck. Ist message
// gesetzt, steht sie gross in der Bildmitte (z. B. in der Zielszene).
class GameControls final : public engine::script::ComponentScript
{
public:
    std::string restartScene = "Scenes/Testraum.kscene";
    std::string message;

    void OnUpdate(engine::script::IScriptContext& ctx) override;
};

// KROM_SCRIPT(GameControls)
// KROM_FIELD(GameControls, restartScene, String)
// KROM_FIELD(GameControls, message, String)
