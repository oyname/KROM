// Opt-in integration test of the krom.h scene functions against the real runtime:
// async loading (LoadSceneAsync, IsSceneReady, ActivateScene), persistent
// entities and scenes across LoadScene/UnloadAll, UnloadScene of an additive
// scene, FindEntityInScene, project scene indices, and a DemoProject scene
// followed by a few frames. Start from the repository directory; the DX11
// window stays hidden.
#include "krom.h"
#include "core/PathUtf8.hpp"
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

namespace {

std::filesystem::path WriteFixture(const std::filesystem::path& dir, const char* fileName, const std::string& json)
{
    const std::filesystem::path path = dir / fileName;
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << json;
    return path;
}

std::string SceneJson(const char* sceneName, bool persistent, const std::vector<const char*>& entityNames)
{
    std::string json = std::string(R"({"scene":")") + sceneName + R"(","version":2,"persistent":)" +
        (persistent ? "true" : "false") + R"(,"entities":[)";
    for (size_t i = 0; i < entityNames.size(); ++i)
    {
        if (i > 0) json += ",";
        json += R"({"entityId":)" + std::to_string(i + 1u) +
            R"(,"components":[{"type":"NameComponent","name":")" + entityNames[i] +
            R"("},{"type":"TransformComponent","localPosition":[0,0,0],"localRotation":[0,0,0,1],"localScale":[1,1,1]}]})";
    }
    json += "]}";
    return json;
}

} // namespace

int main()
{
    const auto sourceRoot = std::filesystem::path(__FILE__).parent_path().parent_path();
    const auto tempDir = std::filesystem::temp_directory_path() /
        ("krom-scene-api-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(tempDir / "user-settings");
    _putenv_s("KROM_USER_SETTINGS_DIR", engine::PathToUtf8(tempDir / "user-settings").c_str());

    int failures = 0;
    const auto check = [&](bool ok, const char* message) {
        std::printf("%s: %s\n", ok ? "PASS" : "FAIL", message);
        if (!ok) ++failures;
    };

    // The project gives the scene order (LoadScene(int), GetSceneCount) and the asset root.
    check(Krom::GetSceneCount() == 0, "no project scenes before LoadProject");
    check(Krom::LoadProject(engine::PathToUtf8(sourceRoot / "projects/DemoProject").c_str()), "LoadProject DemoProject");
    check(Krom::GetSceneCount() == 7, "GetSceneCount is the project scene order");

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

    const std::string pathA = engine::PathToUtf8(WriteFixture(tempDir, "a.kscene", SceneJson("A", false, {"AOwner", "AOther"})));
    const std::string pathB = engine::PathToUtf8(WriteFixture(tempDir, "b.kscene", SceneJson("B", false, {"BOnly"})));
    const std::string pathP = engine::PathToUtf8(WriteFixture(tempDir, "p.kscene", SceneJson("P", true, {"PEntity"})));
    const std::string pathMissing = engine::PathToUtf8(tempDir / "missing.kscene");

    // --- FindEntityInScene and additive load/unload ---------------------------
    const LPSCENE a = Krom::LoadScene(pathA.c_str());
    const LPENTITY aOwner = Krom::FindEntityInScene(a, "AOwner");
    const LPENTITY aOther = Krom::FindEntityInScene(a, "AOther");
    check(a.IsValid() && aOwner.IsValid() && aOther.IsValid(), "LoadScene finds both entities of the scene");
    check(!Krom::FindEntityInScene(a, "BOnly").IsValid(), "FindEntityInScene: unknown name is not found");
    check(!Krom::FindEntityInScene(a, nullptr).IsValid(), "FindEntityInScene: null name");
    check(!Krom::FindEntityInScene(LPSCENE{999999u}, "AOwner").IsValid(), "FindEntityInScene: unknown scene");
    check(Krom::ActivateScene(a), "ActivateScene of a synchronously loaded scene is a no-op (true)");
    check(Krom::IsSceneReady(a), "IsSceneReady of a synchronously loaded scene");

    const LPSCENE b = Krom::LoadSceneAdditive(pathB.c_str());
    const LPENTITY bOnly = Krom::FindEntityInScene(b, "BOnly");
    check(b.IsValid() && bOnly.IsValid(), "LoadSceneAdditive loads the second scene");
    check(Krom::IsAlive(aOwner) && Krom::IsAlive(aOther), "additive load keeps the first scene");
    check(!Krom::FindEntityInScene(b, "AOwner").IsValid(), "FindEntityInScene only searches its own scene");
    Krom::UnloadScene(b);
    check(!Krom::IsAlive(bOnly), "UnloadScene removes the additive scene");
    check(Krom::IsAlive(aOwner) && Krom::FindEntityInScene(a, "AOwner") == aOwner, "UnloadScene of the additive scene leaves the first scene intact");
    check(!Krom::FindEntityInScene(b, "BOnly").IsValid() && !Krom::IsSceneReady(b), "unloaded scene handle is gone");

    // --- LoadSceneAsync + IsSceneReady + ActivateScene ------------------------
    const LPSCENE asyncScene = Krom::LoadSceneAsync(pathB.c_str());
    check(asyncScene.IsValid(), "LoadSceneAsync returns a handle");
    check(!Krom::FindEntityInScene(asyncScene, "BOnly").IsValid(), "async scene has no entities before ActivateScene");
    bool ready = false;
    for (int i = 0; i < 500 && !ready; ++i)
    {
        ready = Krom::IsSceneReady(asyncScene);
        if (!ready) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    check(ready, "IsSceneReady becomes true");
    check(!Krom::FindEntityInScene(asyncScene, "BOnly").IsValid(), "ready scene still has no entities before ActivateScene");
    check(Krom::ActivateScene(asyncScene), "ActivateScene of the async scene");
    const LPENTITY asyncBOnly = Krom::FindEntityInScene(asyncScene, "BOnly");
    check(asyncBOnly.IsValid() && Krom::IsAlive(asyncBOnly), "activated async scene has its entity");
    check(Krom::IsAlive(aOwner), "ActivateScene is additive");
    check(Krom::ActivateScene(asyncScene) && Krom::IsSceneReady(asyncScene), "second ActivateScene is a no-op");

    // ActivateScene without waiting for IsSceneReady blocks until parsing is done.
    const LPSCENE asyncDirect = Krom::LoadSceneAsync(pathB.c_str());
    check(Krom::ActivateScene(asyncDirect) && Krom::FindEntityInScene(asyncDirect, "BOnly").IsValid(),
          "ActivateScene right after LoadSceneAsync waits for the load");
    Krom::UnloadScene(asyncDirect);

    const LPSCENE asyncMissing = Krom::LoadSceneAsync(pathMissing.c_str());
    bool missingReady = false;
    for (int i = 0; i < 500 && !missingReady; ++i)
    {
        missingReady = Krom::IsSceneReady(asyncMissing);
        if (!missingReady) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    check(missingReady, "async load of a missing file finishes");
    check(!Krom::ActivateScene(asyncMissing), "ActivateScene of a missing file fails");
    check(!Krom::IsSceneReady(asyncMissing), "failed async scene is removed");
    check(!Krom::LoadScene(pathMissing.c_str()).IsValid(), "LoadScene of a missing file fails");

    // --- SetPersistent / SetScenePersistent -----------------------------------
    Krom::SetPersistent(aOther);
    const LPSCENE b2 = Krom::LoadScene(pathB.c_str());   // exclusive switch
    const LPENTITY b2Only = Krom::FindEntityInScene(b2, "BOnly");
    check(!Krom::IsAlive(aOwner), "exclusive LoadScene removes non-persistent entities");
    check(!Krom::IsAlive(asyncBOnly), "exclusive LoadScene removes the activated async scene");
    check(Krom::IsAlive(aOther), "SetPersistent entity survives LoadScene");
    check(b2Only.IsValid(), "exclusive LoadScene loads the new scene");

    Krom::SetScenePersistent(b2);
    const LPSCENE a2 = Krom::LoadScene(pathA.c_str());
    const LPENTITY a2Owner = Krom::FindEntityInScene(a2, "AOwner");
    check(Krom::IsAlive(b2Only), "SetScenePersistent scene survives LoadScene");
    check(Krom::FindEntityInScene(b2, "BOnly") == b2Only, "persistent scene keeps its handle");
    check(Krom::IsAlive(aOther), "persistent entity survives a second LoadScene");

    const LPSCENE p = Krom::LoadSceneAdditive(pathP.c_str());
    const LPENTITY pEntity = Krom::FindEntityInScene(p, "PEntity");
    check(pEntity.IsValid(), "scene file marked persistent loads");

    Krom::UnloadAll();
    check(!Krom::IsAlive(a2Owner), "UnloadAll removes the non-persistent scene");
    check(!Krom::FindEntityInScene(a2, "AOwner").IsValid(), "UnloadAll drops the non-persistent scene handle");
    check(Krom::IsAlive(b2Only), "SetScenePersistent scene survives UnloadAll");
    check(Krom::IsAlive(pEntity), "persistent scene file survives UnloadAll");
    check(Krom::IsAlive(aOther), "SetPersistent entity survives UnloadAll");

    // --- Project scene index --------------------------------------------------
    check(!Krom::LoadScene(0).IsValid() && !Krom::LoadScene(Krom::GetSceneCount() + 1).IsValid(),
          "LoadScene(int) rejects indices out of range");
    check(!Krom::LoadSceneAdditive(0).IsValid(), "LoadSceneAdditive(int) rejects index 0");

    // --- DemoProject scene and frames -----------------------------------------
    const LPSCENE room = Krom::LoadScene("Scenes/Room 0.kscene");
    const LPENTITY cube = Krom::FindEntityInScene(room, "Cube");
    check(room.IsValid() && cube.IsValid(), "DemoProject scene 'Room 0' loads");
    check(Krom::GetMainCamera().IsValid(), "DemoProject scene has a camera");
    check(Krom::IsAlive(b2Only) && Krom::IsAlive(pEntity) && Krom::IsAlive(aOther),
          "persistent entities survive loading the DemoProject scene");

    int frame = 0;
    LPSCENE indexScene{};
    const int runResult = Krom::Run([&](float) {
        ++frame;
        if (frame == 2)
        {
            check(Krom::IsAlive(cube), "DemoProject scene alive after frames");
            indexScene = Krom::LoadScene(4);   // "Room 0" in the project scene order
            check(indexScene.IsValid() && Krom::FindEntityInScene(indexScene, "Cube").IsValid(),
                  "LoadScene(int) loads the project scene during the frame loop");
            check(!Krom::IsAlive(cube), "LoadScene(int) replaced the previous scene");
        }
        else if (frame == 5)
        {
            check(Krom::FindEntityInScene(indexScene, "Cube").IsValid(), "scene loaded by index alive after frames");
            check(Krom::IsAlive(b2Only) && Krom::IsAlive(pEntity) && Krom::IsAlive(aOther),
                  "persistent entities alive after frames");
            Krom::Quit();
        }
    });
    check(runResult == 0 && frame == 5, "frame loop ran 5 frames");

    Krom::Shutdown();
    check(Krom::GetSceneCount() == 0, "Shutdown clears the project scene order");
    std::error_code ec;
    std::filesystem::remove_all(tempDir, ec);
    return failures == 0 ? 0 : 1;
}
