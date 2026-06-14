#include "MoveX.hpp"

void MoveX::OnStart(engine::script::IScriptContext&)
{
}

void MoveX::OnUpdate(engine::script::IScriptContext& ctx)
{
    auto& world = Krom::GetWorld();

    engine::TransformComponent* transform = world.Get<engine::TransformComponent>(GetEntity());
    if (!transform)
        return;

    const float speed = -3.0f;
    transform->localPosition.x += speed * ctx.DeltaTime();

    transform->dirty = true;
    ++transform->localVersion;
}
