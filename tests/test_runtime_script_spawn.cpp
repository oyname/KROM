// Opt-in integration test of script prefab spawns against the real runtime:
// scene ownership of spawned roots over thousands of spawn/destroy cycles,
// persistent ids, prefab file reloads, the spawn result being usable right away,
// and spawn/destroy timings for a small and a large world.
// Start from the repository directory; the DX11 window stays hidden.
#include "krom.h"
#include "KromSceneManager.hpp"
#include "core/PathUtf8.hpp"
#include "scene/TransformSystem.hpp"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace {

using engine::math::Vec3;
using Clock = std::chrono::steady_clock;

// What the driver script does in its next OnUpdate (set by the frame lambda,
// which runs before the scripts of the same frame).
struct SpawnCommand
{
    LPENTITY    owner = NULL_LPENTITY;   // only this driver acts
    bool        destroyLive = false;     // ctx.Destroy every root of g_live first
    int         spawnCount = 0;
    std::string prefab;
};

struct SpawnRecord
{
    LPENTITY root = NULL_LPENTITY;
    Vec3     position{};
};

SpawnCommand             g_command;
std::vector<SpawnRecord> g_live;          // roots spawned by the last command
std::vector<LPENTITY>    g_scriptDestroy; // extra entities the script destroys with ctx.Destroy
double                   g_spawnMicros = 0.0;
int                      g_spawnCalls = 0;
int                      g_spawnFailures = 0;
int                      g_immediatePositionFailures = 0;
int                      g_immediateBoundsFailures = 0;
int                      g_immediateChecks = 0;
int                      g_spawnIndex = 0;

bool Near(const Vec3& a, const Vec3& b, float eps = 1e-3f)
{
    return std::fabs(a.x - b.x) <= eps && std::fabs(a.y - b.y) <= eps && std::fabs(a.z - b.z) <= eps;
}

class SpawnDriver final : public engine::script::ComponentScript
{
    void OnUpdate(engine::script::IScriptContext& ctx) override
    {
        if (GetEntity() != g_command.owner)
            return;
        for (const LPENTITY entity : g_scriptDestroy)
            ctx.Destroy(entity);
        g_scriptDestroy.clear();
        if (g_command.destroyLive)
            for (const SpawnRecord& record : g_live)
                ctx.Destroy(record.root);
        g_live.clear();

        for (int i = 0; i < g_command.spawnCount; ++i, ++g_spawnIndex)
        {
            const Vec3 position{static_cast<float>(g_spawnIndex % 10) * 1.5f - 7.0f,
                                1.0f + static_cast<float>((g_spawnIndex / 10) % 5) * 0.5f,
                                -5.0f - static_cast<float>(g_spawnIndex % 3)};
            const auto start = Clock::now();
            const LPENTITY root = ctx.InstantiatePrefab(g_command.prefab, position);
            g_spawnMicros += std::chrono::duration<double, std::micro>(Clock::now() - start).count();
            ++g_spawnCalls;
            if (!root.IsValid())
            {
                ++g_spawnFailures;
                continue;
            }

            // Usable in the same callback: current world position and a fresh box.
            ++g_immediateChecks;
            if (!Near(ctx.GetWorldPosition(root), position))
                ++g_immediatePositionFailures;
            Vec3 center{}, extents{};
            float sphere = 0.0f;
            const bool hasBounds = Krom::GetWorldBounds(root, center, extents, sphere);
            const Vec3 d = center - position;
            if (!hasBounds || d.Length() > 1.0f ||
                std::fabs(d.x) > extents.x + 1e-3f || std::fabs(d.y) > extents.y + 1e-3f ||
                std::fabs(d.z) > extents.z + 1e-3f)
                ++g_immediateBoundsFailures;

            g_live.push_back({root, position});
        }
        g_command = {};
    }
};

// Minimal prefab: root with a mesh child; the child name and scale vary per version.
std::string HotPrefabJson(const char* childName, float scale)
{
    char buffer[2048];
    std::snprintf(buffer, sizeof(buffer), R"({
  "type": "KromPrefab", "version": 2, "name": "HotPrefab", "rootIndex": 0,
  "records": [
    {"id": "r0", "name": "HotRoot", "parentIndex": -1,
     "localPosition": [0, 0, 0], "localRotation": [0, 0, 0, 1], "localScale": [1, 1, 1],
     "active": true, "addBounds": false, "addObb": false},
    {"id": "r1", "name": "%s", "parentIndex": 0,
     "localPosition": [0, 0, 0], "localRotation": [0, 0, 0, 1], "localScale": [%.2f, %.2f, %.2f],
     "meshVisible": true, "castShadows": true, "receiveShadows": true, "layerMask": 1,
     "meshAssetPath": "Models/cubue2.glb#mesh/0",
     "active": true, "addBounds": true, "boundsCenterLocal": [0, 0, 0], "boundsExtentsLocal": [1, 1, 1],
     "addObb": false}
  ]
})", childName, scale, scale, scale);
    return buffer;
}

void WriteFile(const std::filesystem::path& path, const std::string& text)
{
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << text;
}

} // namespace

int main()
{
    const auto sourceRoot = std::filesystem::path(__FILE__).parent_path().parent_path();
    Krom::SetAssetRoot(sourceRoot / "projects/DemoProject/Assets");
    Krom::GraphicsConfig config{};
    config.backend = Krom::Renderer::DX11;
    config.visible = false;
    config.audioDevice = false;
    config.enableForwardPlus = false;
    config.enableGtao = false;
    config.width = 64;
    config.height = 64;
    if (!Krom::Graphics(config))
        return 2;
    Krom::RegisterScriptClass<SpawnDriver>("SpawnDriver");

    const std::string stamp = std::to_string(Clock::now().time_since_epoch().count());
    const auto tempDir = std::filesystem::temp_directory_path();
    const auto sceneFile = tempDir / ("krom-script-spawn-" + stamp + ".kscene");
    const auto hotPrefab = tempDir / ("krom-script-spawn-" + stamp + ".prefab");
    WriteFile(sceneFile, R"({"scene":"SpawnTest","version":2,"persistent":false,"entities":[
        {"entityId":1,"components":[{"type":"NameComponent","name":"Owner"},
        {"type":"TransformComponent","localPosition":[0,0,0],"localRotation":[0,0,0,1],"localScale":[1,1,1]}]},
        {"entityId":2,"components":[{"type":"NameComponent","name":"Floor"},
        {"type":"TransformComponent","localPosition":[0,-1,0],"localRotation":[0,0,0,1],"localScale":[1,1,1]}]}]})");
    const std::string scenePath = engine::PathToUtf8(sceneFile);
    const std::string hotPrefabPath = engine::PathToUtf8(hotPrefab);
    const std::string cubePrefab = "Prefabs/cubue2.prefab";

    int failures = 0;
    const auto check = [&](bool ok, const std::string& message) {
        std::printf("%s: %s\n", ok ? "PASS" : "FAIL", message.c_str());
        if (!ok) ++failures;
    };

    const LPSCENE scene = Krom::LoadScene(scenePath.c_str());
    const LPENTITY owner = Krom::FindEntityInScene(scene, "Owner");
    check(scene.IsValid() && owner.IsValid(), "fixture scene loaded");
    // The driver sits below a scene root (ancestor walk of RegisterScriptSpawn).
    const LPENTITY driver = Krom::CreateEntity("Driver");
    Krom::SetParent(driver, owner);
    Krom::AddScript(driver, "SpawnDriver");
    // A persistent driver outside every scene: its spawns are tracked through persistent ids only.
    const LPENTITY persistentDriver = Krom::CreateEntity("PersistentDriver");
    Krom::SetPersistent(persistentDriver);
    Krom::AddScript(persistentDriver, "SpawnDriver");

    const size_t baselineRoots = Krom::internal::GetWrapperSceneRootCount(scene);
    const size_t baselinePersistent = Krom::internal::GetWrapperPersistentEntityCount();
    std::printf("INFO: baseline scene roots = %zu, persistent ids = %zu\n", baselineRoots, baselinePersistent);

    // Checks a batch spawned in the previous frame (the frame's transform update ran
    // since); the counts of a phase are reported once at its end.
    struct BatchStats { int batches = 0, dead = 0, wrongRoots = 0, wrongChildren = 0, notFound = 0; };
    BatchStats stats;
    const auto checkLiveBatch = [&] {
        ++stats.batches;
        for (const SpawnRecord& record : g_live)
        {
            if (!Krom::IsAlive(record.root)) { ++stats.dead; continue; }
            const auto* world = Krom::GetComponent<engine::WorldTransformComponent>(record.root);
            if (!world || !Near(world->position, record.position))
                ++stats.wrongRoots;
            const LPENTITY child = Krom::GetChild(record.root, 0);
            const auto* childWorld = Krom::GetComponent<engine::WorldTransformComponent>(child);
            const auto* childLocal = Krom::GetComponent<engine::TransformComponent>(child);
            if (!childWorld || !childLocal || !Near(childWorld->position, record.position) ||
                !Near(childWorld->scale, childLocal->localScale) || childLocal->dirty)
                ++stats.wrongChildren;
        }
        bool found = false;
        if (const LPENTITY hit = Krom::FindEntityInScene(scene, "cubue2"); hit.IsValid())
            for (const SpawnRecord& record : g_live)
                found = found || record.root == hit;
        if (!found)
            ++stats.notFound;
    };
    const auto reportBatches = [&](const std::string& label) {
        const std::string batches = " (" + std::to_string(stats.batches) + " batches)";
        check(stats.batches > 0 && stats.dead == 0, label + ": spawned instances alive one frame later" + batches);
        check(stats.batches > 0 && stats.wrongRoots == 0 && stats.wrongChildren == 0,
              label + ": stored WorldTransformComponent of roots and children right after the frame (" +
              std::to_string(stats.wrongRoots) + " roots, " + std::to_string(stats.wrongChildren) + " children wrong)");
        check(stats.batches > 0 && stats.notFound == 0,
              label + ": FindEntityInScene finds a live instance in the caller's scene" + batches);
        stats = {};
    };

    const auto resetTiming = [&] { g_spawnMicros = 0.0; g_spawnCalls = 0; };
    const auto printTiming = [&](const char* label, double destroyMicros, int destroyCalls) {
        if (destroyCalls > 0)
            std::printf("INFO: %s: spawn avg = %.1f us over %d spawns, destroy avg = %.1f us over %d destroys\n",
                label, g_spawnCalls ? g_spawnMicros / g_spawnCalls : 0.0, g_spawnCalls,
                destroyMicros / destroyCalls, destroyCalls);
        else
            std::printf("INFO: %s: spawn avg = %.1f us over %d spawns, destroy = deferred script destroys (not timed)\n",
                label, g_spawnCalls ? g_spawnMicros / g_spawnCalls : 0.0, g_spawnCalls);
    };

    enum class Phase { ScriptDestroy, ApiDestroy, Reparent, Persistent, LargeWorld, HotReload, Unload, Warmup, Done };
    Phase phase = Phase::ScriptDestroy;
    int step = 0;
    double destroyMicros = 0.0;
    int destroyCalls = 0;
    std::vector<LPENTITY> hotRoots;
    std::filesystem::file_time_type hotTime{};
    bool largeWorld = false;
    int frame = 0;

    const auto timedDestroyLive = [&] {
        for (const SpawnRecord& record : g_live)
        {
            const auto start = Clock::now();
            Krom::DestroyEntity(record.root);
            destroyMicros += std::chrono::duration<double, std::micro>(Clock::now() - start).count();
            ++destroyCalls;
        }
        g_live.clear();
    };

    const int runResult = Krom::Run([&](float) {
        ++frame;
        if (frame > 2000) { check(false, "test did not finish in 2000 frames"); Krom::Quit(); return; }

        switch (phase)
        {
        case Phase::ScriptDestroy:
        case Phase::LargeWorld:
        {
            // K spawns per frame, the previous batch is destroyed by the script (ctx.Destroy).
            const char* label = largeWorld ? "large world, script destroy" : "small world, script destroy";
            const int frames = largeWorld ? 10 : 30;
            if (largeWorld && step == 0)
            {
                for (int i = 0; i < 2000; ++i)
                    Krom::SetPosition(Krom::CreateEntity("Filler"), static_cast<float>(i % 50), 0.0f, static_cast<float>(i / 50));
                resetTiming();
            }
            if (step > 0 && step <= frames)
                checkLiveBatch();
            if (step < frames)
                g_command = {driver, true, 100, cubePrefab};
            else if (step == frames)
                g_command = {driver, true, 0, {}};   // last destroy, no spawn
            else
            {
                const size_t roots = Krom::internal::GetWrapperSceneRootCount(scene);
                check(roots == baselineRoots, std::string(label) + ": scene root count back to baseline after " +
                    std::to_string(g_spawnCalls) + " spawns/destroys (now " + std::to_string(roots) +
                    ", baseline " + std::to_string(baselineRoots) + ")");
                check(g_spawnFailures == 0, std::string(label) + ": every spawn succeeded");
                reportBatches(label);
                printTiming(label, 0.0, 0);
                resetTiming();
                destroyMicros = 0.0;
                destroyCalls = 0;
                phase = Phase::ApiDestroy;
                step = 0;
                return;
            }
            ++step;
            break;
        }
        case Phase::ApiDestroy:
        {
            // Same, but the frame lambda destroys the previous batch with Krom::DestroyEntity.
            const char* label = largeWorld ? "large world, Krom::DestroyEntity" : "small world, Krom::DestroyEntity";
            const int frames = largeWorld ? 10 : 30;
            if (step > 0)
            {
                checkLiveBatch();
                timedDestroyLive();
            }
            if (step < frames)
            {
                g_command = {driver, false, 100, cubePrefab};
                ++step;
                break;
            }
            const size_t roots = Krom::internal::GetWrapperSceneRootCount(scene);
            check(roots == baselineRoots, std::string(label) + ": scene root count back to baseline right after the last destroy (now " +
                std::to_string(roots) + ", baseline " + std::to_string(baselineRoots) + ")");
            reportBatches(label);
            printTiming(label, destroyMicros, destroyCalls);
            resetTiming();
            if (!largeWorld)
            {
                phase = Phase::Reparent;
                step = 0;
                return;
            }
            phase = Phase::HotReload;
            step = 0;
            return;
        }
        case Phase::Reparent:
        {
            // Spawned roots reparented under a holder, which is destroyed: the roots die as children.
            constexpr int kFrames = 20;
            if (step > 0 && !g_live.empty())
            {
                const LPENTITY holder = Krom::CreateEntity("Holder");
                bool parented = true;
                for (const SpawnRecord& record : g_live)
                    parented = Krom::SetParent(record.root, holder) && parented;
                if (!parented)
                    check(false, "reparent: SetParent of a spawned root");
                if (step % 2 == 0)
                    Krom::DestroyEntity(holder);   // Krom::DestroyEntity on the parent
                else
                    g_scriptDestroy.push_back(holder);   // script ctx.Destroy on the parent
            }
            if (step < kFrames)
            {
                g_command = {driver, false, 20, cubePrefab};
                g_live.clear();
                ++step;
                break;
            }
            if (step == kFrames)
            {
                g_command = {driver, false, 0, {}};   // runs the last script destroy
                g_live.clear();
                ++step;
                break;
            }
            const size_t roots = Krom::internal::GetWrapperSceneRootCount(scene);
            check(roots == baselineRoots, "reparent: scene root count back to baseline after destroying the parents (now " +
                std::to_string(roots) + ", baseline " + std::to_string(baselineRoots) + ")");
            phase = Phase::Persistent;
            step = 0;
            return;
        }
        case Phase::Persistent:
        {
            // Spawns of a persistent driver outside every scene, destroyed by the script.
            constexpr int kFrames = 10;
            if (step < kFrames)
                g_command = {persistentDriver, true, 20, cubePrefab};
            else if (step == kFrames)
                g_command = {persistentDriver, true, 0, {}};
            else
            {
                const size_t persistent = Krom::internal::GetWrapperPersistentEntityCount();
                check(persistent == baselinePersistent,
                      "persistent: persistent ids back to baseline after destroying the spawns with their children (now " +
                      std::to_string(persistent) + ", baseline " + std::to_string(baselinePersistent) + ")");
                resetTiming();
                largeWorld = true;
                phase = Phase::LargeWorld;
                step = 0;
                return;
            }
            ++step;
            break;
        }
        case Phase::HotReload:
        {
            const auto hasChild = [](LPENTITY root, const char* name) {
                return Krom::FindChild(root, name).IsValid();
            };
            switch (step)
            {
            case 0:
                WriteFile(hotPrefab, HotPrefabJson("HotA", 0.4f));
                g_command = {driver, false, 1, hotPrefabPath};
                break;
            case 1:
                check(g_live.size() == 1 && hasChild(g_live[0].root, "HotA"), "hot reload: first version spawned");
                for (const SpawnRecord& record : g_live) hotRoots.push_back(record.root);
                g_command = {driver, false, 1, hotPrefabPath};   // same file again (cache hit)
                break;
            case 2:
            {
                check(g_live.size() == 1 && hasChild(g_live[0].root, "HotA"), "hot reload: unchanged file spawns the same content");
                for (const SpawnRecord& record : g_live) hotRoots.push_back(record.root);
                // Same size, only the modification time changes.
                std::error_code ec;
                hotTime = std::filesystem::last_write_time(hotPrefab, ec);
                WriteFile(hotPrefab, HotPrefabJson("HotB", 0.4f));
                hotTime += std::chrono::seconds(5);
                std::filesystem::last_write_time(hotPrefab, hotTime, ec);
                g_command = {driver, false, 1, hotPrefabPath};
                break;
            }
            case 3:
            {
                check(g_live.size() == 1 && hasChild(g_live[0].root, "HotB") && !hasChild(g_live[0].root, "HotA"),
                      "hot reload: rewritten file (same size, newer time) spawns the new content");
                for (const SpawnRecord& record : g_live) hotRoots.push_back(record.root);
                // Different size, same modification time as before.
                WriteFile(hotPrefab, HotPrefabJson("HotCCCC", 0.8f));
                std::error_code ec;
                std::filesystem::last_write_time(hotPrefab, hotTime, ec);
                g_command = {driver, false, 1, hotPrefabPath};
                break;
            }
            case 4:
            {
                bool ok = g_live.size() == 1 && hasChild(g_live[0].root, "HotCCCC");
                if (ok)
                {
                    const auto* local = Krom::GetComponent<engine::TransformComponent>(Krom::FindChild(g_live[0].root, "HotCCCC"));
                    ok = local && Near(local->localScale, Vec3{0.8f, 0.8f, 0.8f});
                }
                check(ok, "hot reload: rewritten file (other size, same time) spawns the new content");
                for (const SpawnRecord& record : g_live) hotRoots.push_back(record.root);
                g_live.clear();
                for (const LPENTITY root : hotRoots) Krom::DestroyEntity(root);
                hotRoots.clear();
                // A file that cannot be loaded is not cached: deleting it makes spawns fail,
                // writing it again makes them work again.
                std::error_code ec;
                std::filesystem::remove(hotPrefab, ec);
                g_command = {driver, false, 1, hotPrefabPath};
                break;
            }
            case 5:
                check(g_live.empty(), "hot reload: missing file does not spawn");
                g_spawnFailures = 0;
                WriteFile(hotPrefab, HotPrefabJson("HotD", 0.4f));
                g_command = {driver, false, 1, hotPrefabPath};
                break;
            case 6:
                check(g_live.size() == 1 && hasChild(g_live[0].root, "HotD"), "hot reload: file written again spawns again");
                for (const SpawnRecord& record : g_live) Krom::DestroyEntity(record.root);
                g_live.clear();
                phase = Phase::Unload;
                step = 0;
                return;
            }
            ++step;
            break;
        }
        case Phase::Unload:
        {
            if (step == 0)
            {
                g_command = {driver, false, 10, cubePrefab};
                ++step;
                break;
            }
            checkLiveBatch();
            reportBatches("unload");
            Krom::UnloadScene(scene);
            int alive = 0;
            for (const SpawnRecord& record : g_live)
                alive += Krom::IsAlive(record.root) ? 1 : 0;
            check(!g_live.empty() && alive == 0, "UnloadScene destroys the live spawns of the scene");
            check(Krom::internal::GetWrapperSceneRootCount(scene) == 0u, "UnloadScene forgets the scene roots");
            g_live.clear();
            phase = Phase::Warmup;
            step = 0;
            return;
        }
        case Phase::Warmup:
        {
            if (step == 0)
            {
                const int spawned = Krom::SpawnShaderWarmupAssets();
                check(spawned > 0, "SpawnShaderWarmupAssets spawns objects (" + std::to_string(spawned) + ")");
                ++step;
                break;
            }
            phase = Phase::Done;
            Krom::Quit();
            return;
        }
        case Phase::Done:
            Krom::Quit();
            return;
        }
    });

    check(g_immediateChecks > 0 && g_immediatePositionFailures == 0,
          "ctx.GetWorldPosition(root) equals the spawn position right after the spawn (" +
          std::to_string(g_immediatePositionFailures) + " of " + std::to_string(g_immediateChecks) + " wrong)");
    check(g_immediateChecks > 0 && g_immediateBoundsFailures == 0,
          "Krom::GetWorldBounds(root) is a box around the spawn position right after the spawn (" +
          std::to_string(g_immediateBoundsFailures) + " of " + std::to_string(g_immediateChecks) + " wrong)");
    check(phase == Phase::Done, "all phases ran");

    Krom::Shutdown();
    std::error_code ec;
    std::filesystem::remove(sceneFile, ec);
    std::filesystem::remove(hotPrefab, ec);
    std::printf("RESULT: %d failure(s)\n", failures);
    return failures == 0 && runResult == 0 ? 0 : 1;
}
