#pragma once
#include "krom.h"

// Schiebetuer (kinematischer Rigidbody): faehrt bei Event "Open" nach oben und
// bei "Close" nach closeDelay Sekunden wieder herunter. Spielt dabei die
// AudioSourceComponent der eigenen Entity ab.
class Door final : public engine::script::ComponentScript
{
public:
    float openHeight = 3.0f;
    float speed = 2.0f;
    float closeDelay = 2.0f;

    void OnStart(engine::script::IScriptContext& ctx) override;
    void OnUpdate(engine::script::IScriptContext& ctx) override;
    void OnEvent(engine::script::IScriptContext& ctx, const engine::script::ScriptEvent& event) override;

private:
    engine::math::Vec3 closedPosition{};
    bool  wantOpen = false;
    float closeTimer = -1.0f;   // < 0: kein Schliessen vorgemerkt
    float lift = 0.0f;          // aktuelle Hoehe ueber closedPosition
};

// KROM_SCRIPT(Door)
// KROM_FIELD(Door, openHeight, Float)
// KROM_FIELD(Door, speed, Float)
// KROM_FIELD(Door, closeDelay, Float)
