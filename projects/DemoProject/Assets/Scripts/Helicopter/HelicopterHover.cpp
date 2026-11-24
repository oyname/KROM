#include "HelicopterHover.hpp"
#include "addons/physics/PhysicsComponents.hpp"

#include <cmath>

namespace
{
    constexpr float kTwoPi = 6.28318530718f;
    constexpr float kSlowWave = 0.37f;   // second wave runs at 37 % of the main one

    const engine::RigidBodyComponent* DynamicBody(engine::script::IScriptContext& ctx)
    {
        const auto* body = ctx.Get<engine::RigidBodyComponent>();
        return body && body->enabled && body->type == engine::RigidBodyType::Dynamic ? body : nullptr;
    }
}

void HelicopterHover::OnUpdate(engine::script::IScriptContext& ctx)
{
    if (DynamicBody(ctx))
        return;   // OnFixedUpdate pushes the body instead

    // DeltaTime is scaled, so the helicopter holds still while the game is paused.
    phase += ctx.DeltaTime() * cyclesPerSecond * kTwoPi;

    // Main wave plus a slower, weaker one; the weights add up to 1, so the
    // offset never exceeds `height`.
    const float wave = 0.8f * std::sin(phase) + 0.2f * std::sin(phase * kSlowWave);
    const float offset = wave * height;

    // Only the change since the last frame is applied, so other scripts
    // (e.g. HelicopterControl) can move the same entity.
    ctx.Translate(engine::math::Vec3{ 0.f, offset - lastOffset, 0.f }, engine::script::Space::World);
    lastOffset = offset;
}

void HelicopterHover::OnFixedUpdate(engine::script::IScriptContext& ctx)
{
    const auto* body = DynamicBody(ctx);
    if (!body)
        return;

    phase += ctx.FixedDeltaTime() * cyclesPerSecond * kTwoPi;

    // Second derivative of the offset above: the acceleration that makes the
    // body follow the wave. Force = mass * acceleration.
    const float omega = cyclesPerSecond * kTwoPi;
    const float acceleration = -height * omega * omega *
        (0.8f * std::sin(phase) + 0.2f * kSlowWave * kSlowWave * std::sin(phase * kSlowWave));
    ctx.AddForce(engine::math::Vec3{ 0.f, body->mass * acceleration, 0.f });
}
