#include "HelicopterControl.hpp"

#include <algorithm>
#include <cmath>

namespace
{
    // Damped spring towards `target`. Slightly underdamped, so the value
    // overshoots a little and then settles - the "stabilising" swing.
    void Spring(float& value, float& rate, float target, float dt)
    {
        constexpr float kStiffness = 30.0f;
        constexpr float kDamping = 7.0f;
        rate += (kStiffness * (target - value) - kDamping * rate) * dt;
        value += rate * dt;
    }
}

void HelicopterControl::OnStart(engine::script::IScriptContext& ctx)
{
    startRotation = ctx.GetRotation();
}

void HelicopterControl::OnUpdate(engine::script::IScriptContext& ctx)
{
    using Krom::Key;
    using engine::math::Quat;
    using engine::math::Vec3;

    // Capped so a long frame (e.g. a loading hitch) cannot blow up the spring.
    const float dt = std::min(ctx.DeltaTime(), 0.05f);
    if (dt <= 0.0f)
        return;

    const float forwardInput = std::clamp(ctx.Axis(Key::Down, Key::Up) + ctx.Axis(Key::S, Key::W), -1.0f, 1.0f);
    const float sideInput    = ctx.Axis(Key::A, Key::D);
    const float climbInput   = ctx.Axis(Key::E, Key::Q);
    const float turnInput    = ctx.Axis(Key::Right, Key::Left);   // positive yaw turns left

    // The turn rate eases in and out instead of starting and stopping abruptly.
    turnRate += (turnInput * turnSpeedDegPerSecond - turnRate) * (1.0f - std::exp(-6.0f * dt));
    yawDeg += turnRate * dt;

    // Nose and right-hand side in the model's own axes.
    Vec3 nose{ noseAxis.x, 0.f, noseAxis.z };
    nose = nose.LengthSq() > 1e-6f ? nose.Normalized() : Vec3{ -1.f, 0.f, 0.f };
    const Vec3 side = Vec3::Cross(nose, Vec3::Up());

    // Level orientation (heading only) gives the flight directions.
    const Quat heading = Quat::FromAxisAngleDeg(Vec3::Up(), yawDeg) * startRotation;
    const Vec3 forwardDir = heading.Rotate(nose);
    const Vec3 rightDir   = heading.Rotate(side);

    const Vec3 targetVelocity = forwardDir * (forwardInput * speed)
                              + rightDir * (sideInput * speed)
                              + Vec3::Up() * (climbInput * climbSpeed);
    velocity += (targetVelocity - velocity) * (1.0f - std::exp(-acceleration * dt));
    ctx.Translate(velocity * dt, engine::script::Space::World);

    // Lean into the movement, level out when the keys are released.
    Spring(pitchDeg, pitchRate, forwardInput * maxTiltDeg, dt);
    // Turning banks into the curve as well (left turn = left side down).
    const float bankInput = std::clamp(sideInput - turnInput, -1.0f, 1.0f);
    Spring(bankDeg, bankRate, bankInput * maxTiltDeg, dt);

    const Quat tilt = Quat::FromAxisAngleDeg(side, -pitchDeg) * Quat::FromAxisAngleDeg(nose, bankDeg);
    ctx.SetRotation((heading * tilt).Normalized());
}
