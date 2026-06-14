#include "Move.hpp"

void Move::OnStart(engine::script::IScriptContext&)
{
}

void Move::OnUpdate(engine::script::IScriptContext& ctx)
{
    auto& world = Krom::GetWorld();

    engine::TransformComponent* transform = world.Get<engine::TransformComponent>(target);
    if (!transform)
        return;

    const float speed = speedPerSecond;
    transform->localPosition.z += speed * ctx.DeltaTime();

    transform->dirty = true;
    ++transform->localVersion;
}
