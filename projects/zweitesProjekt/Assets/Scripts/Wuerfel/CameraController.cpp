#include "CameraController.hpp"

#include <algorithm>
#include <cmath>

namespace {

float InputAxis(Krom::Key negative, Krom::Key positive)
{
    float value = 0.0f;
    if (Krom::KeyDown(negative)) value -= 1.0f;
    if (Krom::KeyDown(positive)) value += 1.0f;
    return value;
}

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

void CameraController::OnStart(engine::script::IScriptContext&)
{
    auto& world = Krom::GetWorld();
    const engine::TransformComponent* transform =
        world.Get<engine::TransformComponent>(GetEntity());
    if (!transform)
        return;

    const engine::math::Vec3 euler = EulerDegFromQuat(transform->localRotation);
    pitchDeg = euler.x;
    yawDeg = euler.y;
}

void CameraController::OnUpdate(engine::script::IScriptContext& ctx)
{
    const LPENTITY entity = GetEntity();
    if (!entity.IsValid())
        return;

    auto& world = Krom::GetWorld();
    engine::TransformComponent* transform =
        world.Get<engine::TransformComponent>(entity);
    if (!transform)
        return;

    constexpr float kMaxDt = 1.0f / 60.0f;
    const float boost = Krom::KeyDown(Krom::Key::LeftShift) ? fastMultiplier : 1.0f;
    const float dt = std::min(ctx.DeltaTime(), kMaxDt);
    const float move = moveSpeed * boost * dt;

    const float forwardAxis = InputAxis(Krom::Key::S, Krom::Key::W);
    const float strafeAxis = InputAxis(Krom::Key::A, Krom::Key::D);
    const float verticalAxis = InputAxis(Krom::Key::Q, Krom::Key::E);

    if (forwardAxis != 0.0f)
        transform->localPosition += transform->localRotation.Rotate({0.0f, 0.0f, -forwardAxis * move});
    if (strafeAxis != 0.0f)
        transform->localPosition += transform->localRotation.Rotate({strafeAxis * move, 0.0f, 0.0f});
    if (verticalAxis != 0.0f)
        transform->localPosition += transform->localRotation.Rotate({0.0f, verticalAxis * move, 0.0f});

    const float pitchAxis = InputAxis(Krom::Key::Down, Krom::Key::Up);
    const float yawAxis = InputAxis(Krom::Key::Left, Krom::Key::Right);

    const bool mouseLook = Krom::MouseButtonDown(Krom::MouseButton::Right);
    const float mouseDX = mouseLook ? static_cast<float>(Krom::MouseDeltaX()) : 0.0f;
    const float mouseDY = mouseLook ? static_cast<float>(Krom::MouseDeltaY()) : 0.0f;

    yawDeg -= yawAxis * turnSpeedDegPerSecond * dt + mouseDX * mouseSensitivity;
    pitchDeg += pitchAxis * turnSpeedDegPerSecond * dt + mouseDY * mouseSensitivity;
    pitchDeg = std::clamp(pitchDeg, -89.0f, 89.0f);

    transform->SetEulerDeg(pitchDeg, yawDeg, 0.0f);
    ++transform->localVersion;
}
