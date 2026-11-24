#include "HelicopterHover.hpp"

#include <cmath>

void HelicopterHover::OnUpdate(engine::script::IScriptContext& ctx)
{
    constexpr float kTwoPi = 6.28318530718f;

    // DeltaTime is scaled, so the helicopter holds still while the game is paused.
    phase += ctx.DeltaTime() * cyclesPerSecond * kTwoPi;

    // Main wave plus a slower, weaker one; the weights add up to 1, so the
    // offset never exceeds `height`.
    const float wave = 0.8f * std::sin(phase) + 0.2f * std::sin(phase * 0.37f);
    const float offset = wave * height;

    // Only the change since the last frame is applied, so other scripts
    // (e.g. HelicopterControl) can move the same entity.
    ctx.Translate(engine::math::Vec3{ 0.f, offset - lastOffset, 0.f }, engine::script::Space::World);
    lastOffset = offset;
}
