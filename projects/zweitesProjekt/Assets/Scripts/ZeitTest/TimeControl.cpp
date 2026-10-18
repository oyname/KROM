#include "TimeControl.hpp"

#include "core/Debug.hpp"

#include <chrono>
#include <thread>

void TimeControl::OnUpdate(engine::script::IScriptContext& ctx)
{
    if (ctx.KeyHit(Krom::Key::P))
        ctx.SetTimeScale(ctx.GetTimeScale() == 0.f ? 1.f : 0.f);
    if (ctx.KeyHit(Krom::Key::T))
        ctx.SetTimeScale(ctx.GetTimeScale() == 1.f ? 0.25f : 1.f);
    if (ctx.KeyHit(Krom::Key::I))
        ctx.SetPhysicsInterpolation(!ctx.GetPhysicsInterpolation());
    if (ctx.KeyHit(Krom::Key::F))
        lowFps = !lowFps;
    if (lowFps)   // simuliert einen langsamen Rechner
        std::this_thread::sleep_for(std::chrono::milliseconds(30));

    // Echte Zeit, damit das Log auch in Pause und Zeitlupe jede Sekunde kommt.
    logTimer += ctx.UnscaledDeltaTime();
    ++logFrames;
    if (logTimer < 1.f)
        return;

    const engine::EntityID a = ctx.FindEntity("RaketeA");
    const engine::EntityID b = ctx.FindEntity("RaketeB");
    engine::Debug::Log("ZeitTest: t=%.1f frame=%llu fps=%.0f scale=%.2f interp=%d lowFps=%d | A y=%.2f  B y=%.2f",
                       ctx.Time(), static_cast<unsigned long long>(ctx.FrameCount()),
                       static_cast<float>(logFrames) / logTimer, ctx.GetTimeScale(),
                       ctx.GetPhysicsInterpolation() ? 1 : 0, lowFps ? 1 : 0,
                       a.IsValid() ? ctx.GetPosition(a).y : 0.f,
                       b.IsValid() ? ctx.GetPosition(b).y : 0.f);
    logTimer  = 0.f;
    logFrames = 0;
}
