#pragma once
#include "krom.h"

// Bodenschalter (OBBComponent mit isTrigger): Solange etwas darauf steht
// (Spieler oder Kiste), ist die Tuer offen. Klickt beim Druecken und sinkt
// sichtbar ein.
class PressurePlate final : public engine::script::ComponentScript
{
public:
    engine::EntityID door{};
    std::string clickSound = "Audio/Testraum/click.wav";
    float pressDepth = 0.04f;

    void OnStart(engine::script::IScriptContext& ctx) override;
    void OnTriggerEnter(engine::script::IScriptContext& ctx, engine::EntityID other) override;
    void OnTriggerExit(engine::script::IScriptContext& ctx, engine::EntityID other) override;

private:
    engine::math::Vec3 restPosition{};
    int occupants = 0;
};

// KROM_SCRIPT(PressurePlate)
// KROM_FIELD(PressurePlate, door, Entity)
// KROM_FIELD(PressurePlate, clickSound, String)
// KROM_FIELD(PressurePlate, pressDepth, Float)
