#pragma once
#include "krom.h"

// Haelt die Kamera mit festem Abstand am Ziel. Eine mitfahrende Kamera macht
// Ruckeln sichtbar, weil das Ziel ruhig im Bild steht und der Hintergrund wackelt.
class FollowCamera final : public engine::script::ComponentScript
{
public:
    engine::EntityID   target{};
    engine::math::Vec3 offset{ 0.f, 2.f, 10.f };

    void OnUpdate(engine::script::IScriptContext& ctx) override
    {
        if (target.IsValid())
            ctx.SetPosition(ctx.GetPosition(target) + offset);
    }
};

// KROM_SCRIPT(FollowCamera)
// KROM_FIELD(FollowCamera, target, Entity)
// KROM_FIELD(FollowCamera, offset, Vec3)
