#include "CameraController.hpp"

#include <algorithm>
#include <cmath>

namespace {

engine::math::Vec3 EulerDegFromQuat(const engine::math::Quat& q)
{
    const float sinP = 2.0f * (q.w * q.x - q.z * q.y);
    const float pitch = std::abs(sinP) >= 0.9999f
        ? std::copysign(90.0f, sinP)
        : engine::math::RAD_TO_DEG * std::asin(std::clamp(sinP, -1.0f, 1.0f));
    const float yaw = engine::math::RAD_TO_DEG *
        std::atan2(2.0f * (q.w * q.y + q.x * q.z),
                   1.0f - 2.0f * (q.y * q.y + q.x * q.x));
    const float roll = engine::math::RAD_TO_DEG *
        std::atan2(2.0f * (q.w * q.z + q.x * q.y),
                   1.0f - 2.0f * (q.x * q.x + q.z * q.z));
    return {pitch, yaw, roll};
}

} // namespace

void CameraController::OnStart(engine::script::IScriptContext& ctx)
{
    const engine::math::Vec3 euler = EulerDegFromQuat(ctx.GetRotation());
    pitchDeg = euler.x;
    yawDeg = euler.y;
}

void CameraController::OnUpdate(engine::script::IScriptContext& ctx)
{
    using Krom::Key;

    constexpr float kMaxDt = 1.0f / 60.0f;
    const float boost = ctx.KeyDown(Key::LeftShift) ? fastMultiplier : 1.0f;
    const float dt = std::min(ctx.DeltaTime(), kMaxDt);
    const float move = moveSpeed * boost * dt;

    const engine::math::Vec3 input{
        ctx.Axis(Key::A, Key::D),
        ctx.Axis(Key::Q, Key::E),
        -ctx.Axis(Key::S, Key::W)};
    if (input.LengthSq() > 0.0f)
        ctx.Translate(input * move, engine::script::Space::Local);

    const bool mouseLook = ctx.MouseButtonDown(Krom::MouseButton::Right);
    const engine::math::Vec2 mouse = mouseLook ? ctx.MouseDelta() : engine::math::Vec2::Zero();

    yawDeg += ctx.Axis(Key::Left, Key::Right) * turnSpeedDegPerSecond * dt - mouse.x * mouseSensitivity;
    pitchDeg -= ctx.Axis(Key::Down, Key::Up) * turnSpeedDegPerSecond * dt + mouse.y * mouseSensitivity;
    pitchDeg = std::clamp(pitchDeg, -89.0f, 89.0f);

    ctx.SetRotationEulerDeg(pitchDeg, yawDeg, 0.0f);
}
