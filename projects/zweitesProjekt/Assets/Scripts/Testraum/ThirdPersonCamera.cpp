#include "ThirdPersonCamera.hpp"

#include "addons/physics/PhysicsComponents.hpp"

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
    // davor. Der Pivot liegt in der Kapsel der Figur; ein Strahl von dort
    // trifft sie bei Abstand 0. Deshalb beginnt er erst dort, wo er die
    // Kapsel (als Zylinder aus Radius und Hoehe genaehert) verlaesst.
    constexpr float kSkin = 0.2f;
    float start = 0.0f;
    if (const auto* cc = ctx.Get<engine::CharacterControllerComponent>(target))
    {
        constexpr float kMargin = 0.05f;
        const float horizontal = std::sqrt(back.x * back.x + back.z * back.z);
        float exitSide = horizontal > 1e-3f ? (cc->radius + kMargin) / horizontal : distance;
        float exitTop  = back.y > 1e-3f ? (cc->height + kMargin - pivotHeight) / back.y : distance;
        start = std::clamp(std::min(exitSide, exitTop), 0.0f, distance);
    }

    // Weitere Treffer auf die Figur (z.B. Kind-Collider) ueberspringen.
    const engine::EntityID targetRoot = ctx.GetRoot(target);
    for (int i = 0; i < 4 && start < distance; ++i)
    {
        engine::collision::RaycastHit hit{};
        if (!ctx.PhysicsRaycast(pivot + back * start, back, distance - start, hit))
            break;
        const float at = start + hit.distance;
        if (ctx.GetRoot(hit.entity) != targetRoot)
            return std::max(at - kSkin, minDistance);
        start = at + 0.25f;
    }
    return distance;
}
