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
    const engine::math::Vec3 back = ctx.Forward() * -1.0f;
    ctx.SetPosition(pivot + back * CollisionDistance(ctx, pivot, back));
}

float ThirdPersonCamera::CollisionDistance(engine::script::IScriptContext& ctx,
                                           const engine::math::Vec3& pivot,
                                           const engine::math::Vec3& back)
{
    // Strahl vom Ziel zur Kamera: Liegt eine Wand dazwischen, rueckt die Kamera
    // davor. Treffer auf das Ziel selbst (Figur) werden uebersprungen.
    constexpr float kSkin = 0.2f;
    const engine::EntityID targetRoot = ctx.GetRoot(target);
    float start = 0.0f;
    for (int i = 0; i < 4 && start < distance; ++i)
    {
        engine::collision::RaycastHit hit{};
        if (!ctx.PhysicsRaycast(pivot + back * start, back, distance - start, hit))
            break;
        const float at = start + hit.distance;
        if (ctx.GetRoot(hit.entity) != targetRoot)
            return std::max(at - kSkin, minDistance);
        start = at + 0.05f;
    }
    return distance;
}
