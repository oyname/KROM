// Opt-in Integrationstest der echten Runtime, kein Mock des Szenen-Unloadings.
// Aus dem Repository-Verzeichnis starten; DX11-Fenster bleibt unsichtbar.
#include "krom.h"
#include "core/PathUtf8.hpp"
#include <chrono>
#include <cstdio>
#include <fstream>
#include <vector>

namespace {
std::vector<LPENTITY> spawned;
class SpawnLoop final : public engine::script::ComponentScript
{
    void OnStart(engine::script::IScriptContext& ctx) override
    {
        spawned.push_back(ctx.SpawnEffect("Prefabs/Testraum/Staubwolke.prefab", {}));
    }
};
}

int main()
{
    const auto sourceRoot = std::filesystem::path(__FILE__).parent_path().parent_path();
    Krom::SetAssetRoot(sourceRoot / "projects/zweitesProjekt/Assets");
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
    Krom::RegisterScriptClass<SpawnLoop>("SpawnLoop");

    const auto fixture = std::filesystem::temp_directory_path() /
        ("krom-scene-lifetime-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".kscene");
    {
        std::ofstream out(fixture);
        out << R"({"scene":"Lifetime","version":2,"persistent":false,"entities":[
            {"entityId":1,"components":[{"type":"NameComponent","name":"Owner"},
            {"type":"TransformComponent","localPosition":[0,0,0],"localRotation":[0,0,0,1],"localScale":[1,1,1]}]}]})";
    }
    const std::string path = engine::PathToUtf8(fixture);
    const LPSCENE first = Krom::LoadScene(path.c_str());
    const LPENTITY owner = Krom::FindEntityInScene(first, "Owner");
    // Der Erzeuger liegt unter einer Szenen-Root, nicht selbst in rootEntities.
    const LPENTITY child = Krom::CreateEntity("SpawnerChild");
    Krom::AddComponent<engine::ParentComponent>(child, {owner});
    Krom::AddComponent<engine::ChildrenComponent>(owner).Add(child);
    Krom::AddScript(child, "SpawnLoop");

    int failures = 0;
    const auto check = [&](bool ok, const char* message) {
        std::printf("%s: %s\n", ok ? "PASS" : "FAIL", message);
        if (!ok) ++failures;
    };
    check(first.IsValid() && owner.IsValid(), "fixture loaded");
    LPSCENE second{};
    int frame = 0;
    const int runResult = Krom::Run([&](float) {
        ++frame;
        if (frame == 2)
        {
            check(spawned.size() == 1 && Krom::IsAlive(spawned[0]), "child script spawned effect");
            if (spawned.size() != 1) { Krom::Quit(); return; }
            check(Krom::FindEntityInScene(first, "Staubwolke") == spawned[0], "spawn belongs to caller scene");
            second = Krom::LoadSceneAdditive(path.c_str());
            Krom::AddScript(Krom::FindEntityInScene(second, "Owner"), "SpawnLoop");
        }
        else if (frame == 3)
        {
            check(spawned.size() == 2, "additive scene spawned effect");
            if (spawned.size() != 2) { Krom::Quit(); return; }
            check(Krom::FindEntityInScene(second, "Staubwolke") == spawned[1], "additive scene owns its spawn");
            Krom::UnloadScene(first);
            check(!Krom::IsAlive(spawned[0]), "unload removes looping effect from first scene");
            check(Krom::IsAlive(spawned[1]), "unload preserves other scene effect");
            Krom::SetPersistent(spawned[1]);
            const LPSCENE third = Krom::LoadScene(path.c_str());
            check(Krom::IsAlive(spawned[1]), "explicitly persistent effect survives exclusive switch");
            Krom::AddScript(Krom::FindEntityInScene(third, "Owner"), "SpawnLoop");
        }
        else if (frame == 4)
        {
            check(spawned.size() == 3, "replacement scene spawned effect");
            if (spawned.size() == 3)
            {
                Krom::LoadScene(path.c_str());
                check(!Krom::IsAlive(spawned[2]), "exclusive switch removes nonpersistent effect");
                check(Krom::IsAlive(spawned[1]), "persistent effect still alive");
            }
            Krom::Quit();
        }
    });
    Krom::Shutdown();
    std::error_code ec;
    std::filesystem::remove(fixture, ec);
    return failures == 0 && runResult == 0 ? 0 : 1;
}
