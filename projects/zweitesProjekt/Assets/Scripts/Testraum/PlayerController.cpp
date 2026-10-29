#include "PlayerController.hpp"

#include <cmath>

void PlayerController::OnUpdate(engine::script::IScriptContext& ctx)
{
    using Krom::Key;
    using engine::math::Vec3;

    // Pause: keine Eingabe annehmen, damit nach dem Fortsetzen kein
    // vorgemerkter Lauf- oder Sprungbefehl nachwirkt.
    if (ctx.GetTimeScale() <= 0.0f)
    {
        ctx.SetMoveVelocity(Vec3{ 0.f, 0.f, 0.f });
        return;
    }

    // Kamera-Achsen auf den Boden projiziert; ohne Kamera gilt die Welt (-Z = vorn).
    Vec3 forward{ 0.f, 0.f, -1.f };
    if (camera.IsValid())
    {
        const Vec3 camForward = ctx.Forward(camera);
        const Vec3 flat{ camForward.x, 0.f, camForward.z };
        if (flat.LengthSq() > 1e-4f)
            forward = flat.Normalized();
    }
    const Vec3 right = Vec3::Cross(forward, Vec3{ 0.f, 1.f, 0.f });

    Vec3 move = forward * ctx.Axis(Key::S, Key::W) + right * ctx.Axis(Key::A, Key::D);
    if (move.LengthSq() > 1.f)
        move = move.Normalized();

    const float speed = walkSpeed * (ctx.KeyDown(Key::LeftShift) ? runMultiplier : 1.f);
    ctx.SetMoveVelocity(move * speed);

    if (move.LengthSq() > 1e-4f)
    {
        // Forward = -Z: Yaw so waehlen, dass -Z in Laufrichtung zeigt.
        const float yawDeg = engine::math::RAD_TO_DEG * std::atan2(-move.x, -move.z);
        ctx.SetRotationEulerDeg(0.f, yawDeg, 0.f);
    }

    if (ctx.KeyHit(Key::Space) && ctx.IsGrounded())
        ctx.Jump(jumpSpeed);
}
