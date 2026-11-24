#include "HelicopterCamera.hpp"

#include <algorithm>
#include <cmath>

void HelicopterCamera::OnStart(engine::script::IScriptContext& ctx)
{
    if (!target.IsValid())
        return;
    startOffset = ctx.GetPosition() - ctx.GetWorldPosition(target);
    startHeadingDeg = TargetHeadingDeg(ctx);
}

void HelicopterCamera::OnUpdate(engine::script::IScriptContext& ctx)
{
    using engine::math::Quat;
    using engine::math::Vec3;

    if (!target.IsValid())
        return;

    const Vec3 targetPosition = ctx.GetWorldPosition(target);

    // Swing the start offset around with the target's heading.
    const float turnedDeg = TargetHeadingDeg(ctx) - startHeadingDeg;
    const Vec3 wanted = targetPosition + Quat::FromAxisAngleDeg(Vec3::Up(), turnedDeg).Rotate(startOffset);

    const float blend = 1.0f - std::exp(-followSpeed * ctx.DeltaTime());
    const Vec3 position = Vec3::Lerp(ctx.GetPosition(), wanted, blend);
    ctx.SetPosition(position);

    // Look at the target (camera forward is -Z).
    const Vec3 toTarget = targetPosition + Vec3{ 0.f, lookHeight, 0.f } - position;
    if (toTarget.LengthSq() < 1e-6f)
        return;
    const Vec3 dir = toTarget.Normalized();
    const float yawDeg = engine::math::RAD_TO_DEG * std::atan2(-dir.x, -dir.z);
    const float pitchDeg = engine::math::RAD_TO_DEG * std::asin(std::clamp(dir.y, -1.0f, 1.0f));
    ctx.SetRotationEulerDeg(pitchDeg, yawDeg, 0.0f);
}

float HelicopterCamera::TargetHeadingDeg(engine::script::IScriptContext& ctx)
{
    // Heading from the target's horizontal axes only, so its leaning does not
    // swing the camera. Whichever of X and Z lies flatter is used.
    const engine::math::Quat rotation = ctx.GetRotation(target);
    const engine::math::Vec3 x = rotation.Rotate(engine::math::Vec3{ 1.f, 0.f, 0.f });
    const engine::math::Vec3 z = rotation.Rotate(engine::math::Vec3{ 0.f, 0.f, 1.f });
    if (std::abs(x.y) <= std::abs(z.y))
        return engine::math::RAD_TO_DEG * std::atan2(-x.x, -x.z);
    return engine::math::RAD_TO_DEG * std::atan2(-z.x, -z.z) + 90.0f;
}
