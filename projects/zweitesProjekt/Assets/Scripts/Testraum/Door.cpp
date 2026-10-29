#include "Door.hpp"

#include "core/Debug.hpp"

#include <algorithm>

void Door::OnStart(engine::script::IScriptContext& ctx)
{
    closedPosition = ctx.GetPosition();
}

void Door::OnEvent(engine::script::IScriptContext& ctx, const engine::script::ScriptEvent& event)
{
    if (event.name == "Open")
    {
        closeTimer = -1.0f;
        if (!wantOpen)
        {
            wantOpen = true;
            ctx.PlayAudio();
            engine::Debug::Log("Testraum: Tuer oeffnet");
        }
    }
    else if (event.name == "Close" && wantOpen)
    {
        closeTimer = closeDelay;
    }
}

void Door::OnUpdate(engine::script::IScriptContext& ctx)
{
    const float dt = ctx.DeltaTime();

    if (closeTimer >= 0.0f)
    {
        closeTimer -= dt;
        if (closeTimer < 0.0f)
        {
            wantOpen = false;
            ctx.PlayAudio();
            engine::Debug::Log("Testraum: Tuer schliesst");
        }
    }

    const float goal = wantOpen ? openHeight : 0.0f;
    const float step = speed * dt;
    const float next = lift < goal ? std::min(lift + step, goal) : std::max(lift - step, goal);
    if (next != lift)
    {
        lift = next;
        ctx.SetPosition(closedPosition + engine::math::Vec3{ 0.f, lift, 0.f });
    }
}
