#include "HelicopterControl.hpp"
#include "addons/physics/PhysicsComponents.hpp"

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

bool HelicopterControl::HasDynamicBody(engine::script::IScriptContext& ctx)
{
    const auto* body = ctx.Get<engine::RigidBodyComponent>();
    return body && body->enabled && body->type == engine::RigidBodyType::Dynamic;
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
    const float turnInput    = ctx.Axis(Key::Right, Key::Left);   // positive yaw turns left
    climbInput               = ctx.Axis(Key::E, Key::Q);

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

    // With a dynamic body the physics moves it (OnFixedUpdate): the height
    // comes from the lift force, forward and sideways from this velocity.
    const bool physical = HasDynamicBody(ctx);
    const Vec3 targetVelocity = forwardDir * (forwardInput * speed)
                              + rightDir * (sideInput * speed)
                              + Vec3::Up() * (physical ? 0.0f : climbInput * climbSpeed);
    velocity += (targetVelocity - velocity) * (1.0f - std::exp(-acceleration * dt));
    if (physical)
        velocity.y = 0.0f;
    else
        ctx.Translate(velocity * dt, engine::script::Space::World);

    // Lean into the movement, level out when the keys are released.
    Spring(pitchDeg, pitchRate, forwardInput * maxTiltDeg, dt);
    // Turning banks into the curve as well (left turn = left side down).
    const float bankInput = std::clamp(sideInput - turnInput, -1.0f, 1.0f);
    Spring(bankDeg, bankRate, bankInput * maxTiltDeg, dt);

    const Quat tilt = Quat::FromAxisAngleDeg(side, -pitchDeg) * Quat::FromAxisAngleDeg(nose, bankDeg);
    ctx.SetRotation((heading * tilt).Normalized());
}

void HelicopterControl::OnFixedUpdate(engine::script::IScriptContext& ctx)
{
    const auto* body = ctx.Get<engine::RigidBodyComponent>();
    if (!body || !body->enabled || body->type != engine::RigidBodyType::Dynamic)
        return;

    // Weight of the helicopter: what the lift has to carry to hold the height.
    const float weight = std::max(body->mass, 0.0f) * gravity * body->gravityScale;

    // Lift = weight + whatever is needed to reach the wanted climb rate.
    // Positive extra lift climbs, negative (less than the weight) sinks.
    const float wantedClimbRate = climbInput * climbSpeed;
    const float climbRate = ctx.GetVelocity().y;

    // Forward and sideways: the body takes the flight velocity, the vertical
    // part stays with the physics.
    ctx.SetVelocity(engine::math::Vec3{ velocity.x, climbRate, velocity.z });

    const float extraLift = std::max(body->mass, 0.0f) * liftResponse * (wantedClimbRate - climbRate);
    const float lift = std::clamp(weight + extraLift, 0.0f, weight * std::max(maxLiftFactor, 1.0f));

    ctx.AddForce(engine::math::Vec3{ 0.f, lift, 0.f });
}
