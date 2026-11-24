#include "RotorSpin.hpp"

void RotorSpin::OnUpdate(engine::script::IScriptContext& ctx)
{
    const float step = speedDegPerSecond * ctx.DeltaTime();
    ctx.RotateEulerDeg(axis.x * step, axis.y * step, axis.z * step, engine::script::Space::Local);
}
