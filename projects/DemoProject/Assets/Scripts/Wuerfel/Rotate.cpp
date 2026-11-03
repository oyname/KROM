#include "Rotate.hpp"

void Rotate::OnStart(engine::script::IScriptContext&)
{
}

void Rotate::OnUpdate(engine::script::IScriptContext& ctx)
{
    ++spawnCount;
    ctx.RotateEulerDeg(0.0f, speedDegPerSecond * ctx.DeltaTime(), 0.0f, engine::script::Space::World);
}
