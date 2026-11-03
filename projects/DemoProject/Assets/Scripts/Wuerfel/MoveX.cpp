#include "MoveX.hpp"

void MoveX::OnStart(engine::script::IScriptContext&)
{
}

void MoveX::OnUpdate(engine::script::IScriptContext& ctx)
{
    const float speed = -3.0f;
    ctx.Translate({speed * ctx.DeltaTime(), 0.0f, 0.0f});
}
