#include "ThirdPersonCamera.hpp"

#include <algorithm>
#include <cmath>

void ThirdPersonCamera::OnStart(engine::script::IScriptContext& ctx)
{
    // Start hinter dem Ziel, in dessen Blickrichtung.
    if (target.IsValid())
    {
        const engine::math::Vec3 f = ctx.Forward(target);
        yawDeg = engine::math::RAD_TO_DEG * std::atan2(-f.x, -f.z);
    }
}

void ThirdPersonCamera::OnUpdate(engine::script::IScriptContext& ctx)
{
    using Krom::Key;

    if (!target.IsValid())
        return;

    const float dt = ctx.UnscaledDeltaTime();
    const engine::math::Vec2 mouse = ctx.MouseButtonDown(Krom::MouseButton::Right)
        ? ctx.MouseDelta() : engine::math::Vec2::Zero();

    yawDeg   -= ctx.Axis(Key::Left, Key::Right) * turnSpeedDegPerSecond * dt + mouse.x * mouseSensitivity;
    pitchDeg += ctx.Axis(Key::Down, Key::Up) * turnSpeedDegPerSecond * dt - mouse.y * mouseSensitivity;
    pitchDeg  = std::clamp(pitchDeg, -70.0f, 30.0f);
    distance  = std::clamp(distance - ctx.MouseScroll() * 0.5f, 2.0f, 12.0f);

    ctx.SetRotationEulerDeg(pitchDeg, yawDeg, 0.0f);
    const engine::math::Vec3 pivot = ctx.GetPosition(target) + engine::math::Vec3{ 0.f, pivotHeight, 0.f };
    ctx.SetPosition(pivot - ctx.Forward() * distance);
}
