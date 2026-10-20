#include "GameControls.hpp"

#include "core/Debug.hpp"

void GameControls::OnUpdate(engine::script::IScriptContext& ctx)
{
    using Krom::Key;

    if (ctx.KeyHit(Key::P))
    {
        const bool pause = ctx.GetTimeScale() > 0.0f;
        ctx.SetTimeScale(pause ? 0.0f : 1.0f);
        engine::Debug::Log("Spiel: %s", pause ? "Pause" : "weiter");
    }

    if (ctx.KeyHit(Key::R) && !restartScene.empty())
    {
        // Die Zeitskala gilt ueber den Szenenwechsel hinweg: Pause aufheben.
        ctx.SetTimeScale(1.0f);
        ctx.LoadScene(restartScene);
        engine::Debug::Log("Spiel: Neustart (%s)", restartScene.c_str());
    }
}
