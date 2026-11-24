#pragma once
#include "krom.h"

// Bodenschalter (OBBComponent mit isTrigger): Solange etwas darauf steht
// (Spieler oder Kiste), ist die Tuer offen. Klickt beim Druecken und sinkt
// sichtbar ein. Optional spruehen dabei Funken (Partikel-Emitter sparks), und
// beim Druecken steigt eine Staubwolke auf (Effekt-Prefab per SpawnEffect,
// raeumt sich selbst auf).
class PressurePlate final : public engine::script::ComponentScript
{
public:
    engine::EntityID door{};
    engine::EntityID sparks{};   // Partikel-Emitter, laeuft solange gedrueckt
    std::string clickSound = "Audio/Testraum/click.wav";
    KROM_PREFAB std::string dustEffect = "Prefabs/Testraum/Staubwolke.prefab";   // leer = kein Staub
    float pressDepth = 0.04f;

    void OnStart(engine::script::IScriptContext& ctx) override;
    void OnTriggerEnter(engine::script::IScriptContext& ctx, engine::EntityID other) override;
    void OnTriggerExit(engine::script::IScriptContext& ctx, engine::EntityID other) override;

private:
    engine::math::Vec3 restPosition{};
    int occupants = 0;
};
