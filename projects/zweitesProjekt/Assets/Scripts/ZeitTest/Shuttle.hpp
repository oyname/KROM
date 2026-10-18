#pragma once
#include "krom.h"

// Fliegt mit konstanter Geschwindigkeit auf der X-Achse hin und her.
// Braucht einen dynamischen Rigidbody mit Schwerkraft-Skala 0 und Daempfung 0.
// Gut, um die Interpolation zu sehen (Taste I in TimeControl).
class Shuttle final : public engine::script::ComponentScript
{
public:
    float speed = 8.0f;    // m/s
    float range = 10.0f;   // Umkehrpunkte bei x = -range / +range

    void OnStart(engine::script::IScriptContext& ctx) override
    {
        ctx.SetVelocity({ speed, 0.f, 0.f });
    }

    void OnFixedUpdate(engine::script::IScriptContext& ctx) override
    {
        const float x = ctx.GetPosition().x;   // hier die echte Physik-Pose
        if (x > range)
            ctx.SetVelocity({ -speed, 0.f, 0.f });
        else if (x < -range)
            ctx.SetVelocity({ speed, 0.f, 0.f });
    }
};

// KROM_SCRIPT(Shuttle)
// KROM_FIELD(Shuttle, speed, Float)
// KROM_FIELD(Shuttle, range, Float)
