#pragma once
#include "krom.h"

class Thruster final : public engine::script::ComponentScript
{
public:
    float force = 15.0f;   // N, knapp über der Gewichtskraft (9,81 N)
    bool  useFixed = true;

    void OnFixedUpdate(engine::script::IScriptContext& ctx) override
    {
        if (useFixed && ctx.KeyDown(Krom::Key::Space))
            ctx.AddForce({ 0.f, force, 0.f });
    }
    void OnUpdate(engine::script::IScriptContext& ctx) override
    {
        if (!useFixed && ctx.KeyDown(Krom::Key::Space))
            ctx.AddForce({ 0.f, force, 0.f });   // wird intern zu Impuls force*dt
    }
};
// KROM_SCRIPT(Thruster)
// KROM_FIELD(Thruster, force, Float)
// KROM_FIELD(Thruster, useFixed, Bool)
