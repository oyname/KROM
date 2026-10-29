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

    if (!message.empty())
    {
        const engine::math::Vec2 screen = ctx.GetScreenSize();
        const auto drawCentered = [&](std::string_view text, float y, float size,
                                      const engine::math::Vec4& color) {
            const float x = (screen.x - ctx.MeasureScreenText(text, size).x) * 0.5f;
            ctx.DrawScreenText(text, x + 2.0f, y + 2.0f, size, { 0.0f, 0.0f, 0.0f, 0.65f });
            ctx.DrawScreenText(text, x, y, size, color);
        };
        drawCentered(message, screen.y * 0.18f, 56.0f, { 1.0f, 0.82f, 0.25f, 1.0f });
        drawCentered("R Neustart", screen.y * 0.18f + 70.0f, 24.0f, { 0.85f, 0.88f, 0.92f, 0.9f });
    }
}
