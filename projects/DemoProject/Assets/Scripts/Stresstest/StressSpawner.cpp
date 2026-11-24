#include "StressSpawner.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <random>

namespace {

float MillisecondsSince(std::chrono::steady_clock::time_point start)
{
    return std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - start).count();
}

} // namespace

void StressSpawner::OnStart(engine::script::IScriptContext& ctx)
{
    stresstest::g_mode = stresstest::PerScript;
    cubes.clear();
    spins.clear();
    if (cubePrefab.empty() || count <= 0)
        return;

    const auto start = std::chrono::steady_clock::now();
    cubes.reserve(static_cast<size_t>(count));
    spins.reserve(static_cast<size_t>(count));

    std::mt19937 rng(static_cast<uint32_t>(seed));
    std::uniform_real_distribution<float> unit(-0.5f, 0.5f);
    std::uniform_real_distribution<float> angle(0.f, 360.f);
    std::uniform_real_distribution<float> size(0.5f, 1.5f);

    const engine::math::Vec3 centre = ctx.GetWorldPosition();
    for (int i = 0; i < count; ++i)
    {
        const engine::math::Vec3 position{
            centre.x + unit(rng) * areaSize.x,
            centre.y + unit(rng) * areaSize.y,
            centre.z + unit(rng) * areaSize.z};
        const engine::math::Quat rotation =
            engine::math::Quat::FromEulerDeg(angle(rng), angle(rng), angle(rng));
        const float scale = size(rng);

        const engine::EntityID cube = ctx.InstantiatePrefab(cubePrefab, position, rotation, {scale, scale, scale});
        if (!cube.IsValid())
            break;   // prefab missing: the engine has logged it, do not repeat 5000 times
        cubes.push_back(cube);
        spins.push_back(stresstest::SpinFor(cube));
    }
    spawnMs = MillisecondsSince(start);
}

void StressSpawner::OnUpdate(engine::script::IScriptContext& ctx)
{
    using Krom::Key;
    if (ctx.KeyHit(Key::Num1)) stresstest::g_mode = stresstest::PerScript;
    if (ctx.KeyHit(Key::Num2)) stresstest::g_mode = stresstest::SingleLoop;
    if (ctx.KeyHit(Key::Num3)) stresstest::g_mode = stresstest::Stopped;

    loopMs = 0.f;
    if (stresstest::g_mode == stresstest::SingleLoop)
    {
        const auto start = std::chrono::steady_clock::now();
        const float dt = ctx.DeltaTime();
        for (size_t i = 0; i < cubes.size(); ++i)
        {
            if (auto* transform = ctx.Get<engine::TransformComponent>(cubes[i]))
                stresstest::Turn(*transform, spins[i], dt);
        }
        loopMs = MillisecondsSince(start);
    }

    // Real time, averaged over half a second, so the numbers stay readable.
    statTimer += ctx.UnscaledDeltaTime();
    ++statFrames;
    if (statTimer >= 0.5f)
    {
        shownFps = static_cast<float>(statFrames) / statTimer;
        shownFrameMs = 1000.f * statTimer / static_cast<float>(statFrames);
        statTimer = 0.f;
        statFrames = 0;
    }

    const char* modeName =
        stresstest::g_mode == stresstest::PerScript  ? "1: one script per cube" :
        stresstest::g_mode == stresstest::SingleLoop ? "2: one loop in the spawner" :
                                                       "3: nothing turns";
    char text[320];
    std::snprintf(text, sizeof(text),
                  "Stress test: %zu cubes (spawned in %.0f ms)\n"
                  "%.1f fps   %.2f ms per frame\n"
                  "Mode %s\n"
                  "Loop time %.3f ms\n"
                  "Keys 1 / 2 / 3 switch the mode",
                  cubes.size(), spawnMs, shownFps, shownFrameMs, modeName, loopMs);
    ctx.DrawScreenText(text, 16.f, 16.f, 22.f);
}
