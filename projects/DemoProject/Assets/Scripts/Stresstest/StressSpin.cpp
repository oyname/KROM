#include "StressSpin.hpp"

void StressSpin::OnStart(engine::script::IScriptContext& ctx)
{
    spin = stresstest::SpinFor(ctx.GetEntity());
}

void StressSpin::OnUpdate(engine::script::IScriptContext& ctx)
{
    if (stresstest::g_mode != stresstest::PerScript)
        return;
    if (auto* transform = ctx.Get<engine::TransformComponent>())
        stresstest::Turn(*transform, spin, ctx.DeltaTime());
}
