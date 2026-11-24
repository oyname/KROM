#pragma once
#include "krom.h"

// Chase camera for the helicopter. It keeps the view that was set up in the
// editor: the offset between camera and target at start is the offset it
// holds while playing. When the target turns, the camera swings around with
// it; it follows with a short delay and always looks at the target.
// The target's leaning and bobbing are not copied, so they stay visible.
class HelicopterCamera final : public engine::script::ComponentScript
{
public:
    engine::EntityID target{};
    float followSpeed = 4.0f;    // higher = camera sticks closer to its spot
    float lookHeight = 0.0f;     // aims this far above the target's origin

    void OnStart(engine::script::IScriptContext& ctx) override;
    void OnUpdate(engine::script::IScriptContext& ctx) override;

private:
    [[nodiscard]] float TargetHeadingDeg(engine::script::IScriptContext& ctx);

    engine::math::Vec3 startOffset{};
    float startHeadingDeg = 0.0f;
};
