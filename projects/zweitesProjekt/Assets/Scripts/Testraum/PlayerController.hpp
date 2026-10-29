#pragma once
#include "krom.h"

// Spielfigur fuer den Testraum (braucht CharacterControllerComponent).
// WASD laeuft relativ zur Blickrichtung der Kamera, Shift rennt, Leertaste
// springt. Die Figur dreht sich in Laufrichtung.
class PlayerController final : public engine::script::ComponentScript
{
public:
    engine::EntityID camera{};
    float walkSpeed = 4.0f;
    float runMultiplier = 1.8f;
    float jumpSpeed = 5.0f;

    void OnUpdate(engine::script::IScriptContext& ctx) override;
};

// KROM_SCRIPT(PlayerController)
// KROM_FIELD(PlayerController, camera, Entity)
// KROM_FIELD(PlayerController, walkSpeed, Float)
// KROM_FIELD(PlayerController, runMultiplier, Float)
// KROM_FIELD(PlayerController, jumpSpeed, Float)
