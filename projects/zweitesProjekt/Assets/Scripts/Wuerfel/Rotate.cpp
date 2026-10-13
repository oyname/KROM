#include "Rotate.hpp"

void Rotate::OnStart(engine::script::IScriptContext&)
{
}

void Rotate::OnUpdate(engine::script::IScriptContext& ctx)
{
    auto& world = Krom::GetWorld();

    ++spawnCount;

    auto* transform = world.Get<engine::TransformComponent>(GetEntity());
    if (!transform)
        return;

    transform->RotateWorldEulerDeg(0.0f, speedDegPerSecond * ctx.DeltaTime(), 0.0f);
}
