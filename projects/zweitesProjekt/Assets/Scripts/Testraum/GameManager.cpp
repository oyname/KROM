#include "GameManager.hpp"

#include "core/Debug.hpp"

#include <algorithm>
#include <cstdio>
#include <string_view>

namespace {

const engine::math::Vec4 kWhite { 1.0f, 1.0f, 1.0f, 1.0f };
const engine::math::Vec4 kHint  { 0.85f, 0.88f, 0.92f, 0.9f };
const engine::math::Vec4 kGold  { 1.0f, 0.82f, 0.25f, 1.0f };
const engine::math::Vec4 kShadow{ 0.0f, 0.0f, 0.0f, 0.65f };

// Text mit leichtem Schatten, damit er auf hellen Flaechen lesbar bleibt.
void DrawShadowed(engine::script::IScriptContext& ctx, std::string_view text,
                  float x, float y, float size, const engine::math::Vec4& color)
{
    const float offset = size > 30.0f ? 2.0f : 1.0f;
    ctx.DrawScreenText(text, x + offset, y + offset, size, kShadow);
    ctx.DrawScreenText(text, x, y, size, color);
}

void DrawCentered(engine::script::IScriptContext& ctx, std::string_view text,
                  float centerY, float size, const engine::math::Vec4& color)
{
    const engine::math::Vec2 screen = ctx.GetScreenSize();
    const engine::math::Vec2 extent = ctx.MeasureScreenText(text, size);
    DrawShadowed(ctx, text, (screen.x - extent.x) * 0.5f, centerY - extent.y * 0.5f, size, color);
}

} // namespace

void GameManager::OnStart(engine::script::IScriptContext& ctx)
{
    // Startzustand: alle Lampen aus, unabhaengig davon, wie die Szene sie speichert.
    for (int i = 1; i <= required; ++i)
        SetLamp(ctx, i, false);
    engine::Debug::Log("Testraum: Start - sammle %d Objekte (P = Pause, R = Neustart)", required);
}

void GameManager::OnUpdate(engine::script::IScriptContext& ctx)
{
    // Lade-Haenger (der erste Frame dauert im Debug-Build Sekunden) nicht mitzaehlen.
    if (!finished)
        elapsed += std::min(ctx.DeltaTime(), 0.25f);
    DrawHud(ctx);
}

void GameManager::OnEvent(engine::script::IScriptContext& ctx, const engine::script::ScriptEvent& event)
{
    if (event.name != "Collected" || finished)
        return;

    ++collected;
    SetLamp(ctx, collected, true);
    ctx.PlayOneShot(collectSound);
    engine::Debug::Log("Testraum: eingesammelt %d/%d", collected, required);

    if (collected >= required)
    {
        finished = true;
        ctx.SendEvent(exitDoor, "Open");
        ctx.PlayOneShot(unlockSound);
        engine::Debug::Log("Testraum: Ausgang offen nach %.1f s", elapsed);
    }
}

void GameManager::SetLamp(engine::script::IScriptContext& ctx, int number, bool on)
{
    const std::string base = lampPrefix + " " + std::to_string(number);
    if (auto* lampOn = ctx.Get<engine::ActiveComponent>(ctx.FindEntity(base + " An")))
        lampOn->active = on;
    if (auto* lampOff = ctx.Get<engine::ActiveComponent>(ctx.FindEntity(base + " Aus")))
        lampOff->active = !on;
}

void GameManager::DrawHud(engine::script::IScriptContext& ctx)
{
    const engine::math::Vec2 screen = ctx.GetScreenSize();
    if (screen.x <= 0.0f || screen.y <= 0.0f)
        return;

    char line[128];
    std::snprintf(line, sizeof(line), "%d/%d", collected, required);
    DrawShadowed(ctx, line, 24.0f, 18.0f, 44.0f, finished ? kGold : kWhite);
    std::snprintf(line, sizeof(line), "Zeit %.1f s", elapsed);
    DrawShadowed(ctx, line, 26.0f, 68.0f, 22.0f, kHint);

    DrawShadowed(ctx, "WASD laufen   Shift rennen   Leertaste springen   P Pause   R Neustart",
                 24.0f, screen.y - 42.0f, 20.0f, kHint);

    if (finished)
    {
        std::snprintf(line, sizeof(line), "Geschafft in %.1f s!", elapsed);
        DrawCentered(ctx, line, screen.y * 0.28f, 52.0f, kGold);
        DrawCentered(ctx, "Der Ausgang ist offen - geh durch die T\xC3\xBCr!", screen.y * 0.28f + 50.0f, 26.0f, kWhite);
    }

    if (ctx.GetTimeScale() <= 0.0f)
        DrawCentered(ctx, "Pause", screen.y * 0.5f, 64.0f, kWhite);
}
