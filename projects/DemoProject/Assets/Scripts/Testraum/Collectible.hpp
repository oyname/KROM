#pragma once
#include "krom.h"

// Sammelobjekt (OBBComponent mit isTrigger): dreht sich und schwebt. Betritt
// die Spielfigur den Trigger, meldet es "Collected" per Broadcast an den
// Spielablauf und zerstoert sich. Andere Objekte (Kisten, Plattformen)
// loesen nichts aus.
class Collectible final : public engine::script::ComponentScript
{
public:
    float spinDegPerSecond = 90.0f;
    float bobHeight = 0.15f;

    void OnStart(engine::script::IScriptContext& ctx) override;
    void OnUpdate(engine::script::IScriptContext& ctx) override;
    void OnTriggerEnter(engine::script::IScriptContext& ctx, engine::EntityID other) override;

private:
    engine::math::Vec3 basePosition{};
    float phase = 0.0f;
    bool  collected = false;
};

// KROM_SCRIPT(Collectible)
// KROM_FIELD(Collectible, spinDegPerSecond, Float)
// KROM_FIELD(Collectible, bobHeight, Float)
