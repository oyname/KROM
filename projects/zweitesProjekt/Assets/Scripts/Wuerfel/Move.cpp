#include "Move.hpp"

void Move::OnStart(engine::script::IScriptContext&)
{
}

void Move::OnUpdate(engine::script::IScriptContext& ctx)
{
    if (!target.IsValid())
        return;

    ctx.Translate({0.0f, 0.0f, speedPerSecond * ctx.DeltaTime()}, engine::script::Space::World, target);
}
