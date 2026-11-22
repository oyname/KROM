// Opt-in integration test of the krom.h scene functions against the real runtime:
// async loading (LoadSceneAsync, IsSceneReady, ActivateScene), persistent
// entities and scenes across LoadScene/UnloadAll, UnloadScene of an additive
// scene, FindEntityInScene, project scene indices, and a DemoProject scene
// followed by a few frames. Start from the repository directory; the DX11
// window stays hidden.
#include "krom.h"
#include "addons/mesh_renderer/MeshRendererComponents.hpp"
#include "core/Debug.hpp"
#include "core/PathUtf8.hpp"
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <thread>
#include <utility>
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

// One mesh entity of a fixture scene: name, meshAssetPath, optional component
// materialAssetPath (nullptr = no MaterialComponent) and slot overrides.
struct MeshEntityJson
{
    const char* name;
    const char* meshAssetPath;
    const char* materialAssetPath;
    std::vector<std::pair<uint32_t, const char*>> slots;
};

std::string MeshSceneJson(const char* sceneName, const std::vector<MeshEntityJson>& entities)
{
    std::string json = std::string(R"({"scene":")") + sceneName + R"(","version":2,"persistent":false,"entities":[)";
    for (size_t i = 0; i < entities.size(); ++i)
    {
        const MeshEntityJson& e = entities[i];
        if (i > 0) json += ",";
        json += R"({"entityId":)" + std::to_string(i + 1u) +
            R"(,"components":[{"type":"NameComponent","name":")" + e.name +
            R"("},{"type":"TransformComponent","localPosition":[0,0,0],"localRotation":[0,0,0,1],"localScale":[1,1,1]})" +
            R"(,{"type":"MeshComponent","meshAssetPath":")" + e.meshAssetPath +
            R"(","visible":true,"castShadows":true,"layerMask":1})";
        if (e.materialAssetPath)
        {
            json += R"(,{"type":"MaterialComponent","submeshIndex":0,"materialAssetPath":")" +
                std::string(e.materialAssetPath) + R"(","slots":[)";
            for (size_t s = 0; s < e.slots.size(); ++s)
            {
                if (s > 0) json += ",";
                json += R"({"submeshIndex":)" + std::to_string(e.slots[s].first) +
                    R"(,"materialAssetPath":")" + e.slots[s].second + R"("})";
            }
            json += "]}";
        }
        json += "]}";
    }
    json += "]}";
    return json;
}

// Error lines logged while g_capturing is set (engine::Debug::onLog).
std::vector<std::string> g_capturedErrors;
bool g_capturing = false;

int CapturedErrorWith(const char* text, const char* text2 = nullptr)
{
    int count = 0;
    for (const std::string& line : g_capturedErrors)
        if (line.find(text) != std::string::npos && (!text2 || line.find(text2) != std::string::npos))
            ++count;
    return count;
}

const engine::MaterialComponent* MaterialComponentOf(LPENTITY entity)
{
    return Krom::GetComponent<engine::MaterialComponent>(entity);
}

LPMATERIAL ComponentMaterialOf(LPENTITY entity)
{
    const auto* material = MaterialComponentOf(entity);
    return material ? material->material : LPMATERIAL::Invalid();
}

// The material the renderer uses for a submesh (slot override first, then the
// component material; MeshRendererExtraction::EmitSubmeshProxies).
LPMATERIAL SubmeshMaterialOf(LPENTITY entity, uint32_t submeshIndex)
{
    const auto* material = MaterialComponentOf(entity);
    if (!material)
        return LPMATERIAL::Invalid();
    if (const auto* slot = material->FindSlotOverride(submeshIndex); slot && slot->material.IsValid())
        return slot->material;
    return material->material;
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
    engine::Debug::onLog = [](engine::LogLevel level, const char* message) {
        if (g_capturing && level >= engine::LogLevel::Error && message)
            g_capturedErrors.emplace_back(message);
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

    // --- Scene materials given as sub-asset keys of an imported model ----------
    // Dragon.glb: mesh 0 has three submeshes (materials 0, 1, 2), mesh 1 has one
    // submesh (material 3). Krom::PrintAssetTree lists the model's materials.
    check(Krom::PrintAssetTree("Models/Dragon.glb"), "PrintAssetTree of Dragon.glb");
    const std::string pathSub = engine::PathToUtf8(WriteFixture(tempDir, "sub.kscene", MeshSceneJson("Sub", {
        {"A_NoMaterial",   "Models/Dragon.glb#mesh/1", nullptr, {}},
        {"B_OwnKey",       "Models/Dragon.glb#mesh/1", "Models/Dragon.glb#material/3", {}},
        {"B2_OtherKey",    "Models/Dragon.glb#mesh/1", "Models/Dragon.glb#material/0", {}},
        {"C_SlotOverride", "Models/Dragon.glb#mesh/1", "Models/Dragon.glb#material/3", {{0u, "Materials/Rot.mat"}}},
        {"D_OutOfRange",   "Models/Dragon.glb#mesh/1", "Models/Dragon.glb#material/99", {}},
        {"D2_OutOfRange",  "Models/Dragon.glb#mesh/1", "Models/Dragon.glb#material/99", {}},
        {"E_MissingModel", "Models/Dragon.glb#mesh/1", "Models/does_not_exist.glb#material/0", {}},
        {"E2_MissingModel","Models/Dragon.glb#mesh/1", "Models/does_not_exist.glb#material/0", {}},
        {"F_NoMaterials",  "Models/Dragon.glb#mesh/1", "Models/car.glb#material/0", {}},
        {"G_MultiNoMat",   "Models/Dragon.glb#mesh/0", nullptr, {}},
        {"H_MultiKey",     "Models/Dragon.glb#mesh/0", "Models/Dragon.glb#material/0", {{1u, "Materials/Rot.mat"}}},
    })));
    g_capturedErrors.clear();
    g_capturing = true;
    const LPSCENE sub = Krom::LoadScene(pathSub.c_str());
    g_capturing = false;
    for (const std::string& line : g_capturedErrors)
        std::printf("INFO: sub-asset scene error: %s\n", line.c_str());
    const LPENTITY subA = Krom::FindEntityInScene(sub, "A_NoMaterial");
    const LPENTITY subB = Krom::FindEntityInScene(sub, "B_OwnKey");
    const LPENTITY subB2 = Krom::FindEntityInScene(sub, "B2_OtherKey");
    const LPENTITY subC = Krom::FindEntityInScene(sub, "C_SlotOverride");
    const LPENTITY subD = Krom::FindEntityInScene(sub, "D_OutOfRange");
    const LPENTITY subD2 = Krom::FindEntityInScene(sub, "D2_OutOfRange");
    const LPENTITY subE = Krom::FindEntityInScene(sub, "E_MissingModel");
    const LPENTITY subE2 = Krom::FindEntityInScene(sub, "E2_MissingModel");
    const LPENTITY subF = Krom::FindEntityInScene(sub, "F_NoMaterials");
    const LPENTITY subG = Krom::FindEntityInScene(sub, "G_MultiNoMat");
    const LPENTITY subH = Krom::FindEntityInScene(sub, "H_MultiKey");
    check(sub.IsValid() && subA.IsValid() && subB.IsValid() && subB2.IsValid() && subC.IsValid() && subD.IsValid() &&
          subD2.IsValid() && subE.IsValid() && subE2.IsValid() && subF.IsValid() && subG.IsValid() && subH.IsValid(),
          "sub-asset material scene loads all entities");
    // Unresolvable keys keep the default material of the mesh: D, E and F share
    // it (default lit material cached per mesh) and it is valid.
    const LPMATERIAL defaultMat = ComponentMaterialOf(subD);
    check(defaultMat.IsValid() && ComponentMaterialOf(subD2) == defaultMat &&
          ComponentMaterialOf(subE) == defaultMat && ComponentMaterialOf(subE2) == defaultMat &&
          ComponentMaterialOf(subF) == defaultMat,
          "unresolvable sub-asset material keys keep the (shared) default material");
    check(ComponentMaterialOf(subA).IsValid() && ComponentMaterialOf(subA) != defaultMat,
          "(a) mesh without material component gets the model's own material");
    check(ComponentMaterialOf(subB).IsValid() && ComponentMaterialOf(subB) != defaultMat,
          "(b) '<model>#material/N' resolves to a material that is not the default material");
    check(ComponentMaterialOf(subB) == ComponentMaterialOf(subA),
          "(b) '<model>#material/N' with the mesh's own index is the same runtime material as (a)");
    check(ComponentMaterialOf(subB2).IsValid() && ComponentMaterialOf(subB2) != defaultMat &&
          ComponentMaterialOf(subB2) != ComponentMaterialOf(subB),
          "(b2) another material index of the same model gives another runtime material");
    const auto* subCMaterial = MaterialComponentOf(subC);
    const auto* subCSlot = subCMaterial ? subCMaterial->FindSlotOverride(0u) : nullptr;
    check(subCSlot && subCSlot->materialAssetPath == "Materials/Rot.mat" && subCSlot->material.IsValid() &&
          subCSlot->material != defaultMat && subCSlot->material != ComponentMaterialOf(subB),
          "(c) a .mat slot override on top of a sub-asset key is kept");
    check(ComponentMaterialOf(subC) == ComponentMaterialOf(subB), "(c) the component material is still the model material");
    check(CapturedErrorWith("Models/Dragon.glb#material/99") == 1,
          "(d) a key with an index out of range logs exactly one error for two entities");
    check(CapturedErrorWith("Models/does_not_exist.glb#material/0") == 1 &&
          CapturedErrorWith("does_not_exist.glb") == 1,
          "(e) a key of a missing model file logs exactly one error for two entities");
    check(CapturedErrorWith("Models/car.glb#material/0") == 1, "a key of a model without materials logs exactly one error");
    check(SubmeshMaterialOf(subG, 0u).IsValid() && SubmeshMaterialOf(subG, 1u).IsValid() &&
          SubmeshMaterialOf(subG, 2u).IsValid() && SubmeshMaterialOf(subG, 0u) != SubmeshMaterialOf(subG, 2u),
          "multi-submesh mesh without material component gets one model material per submesh");
    check(SubmeshMaterialOf(subH, 0u) == SubmeshMaterialOf(subG, 0u),
          "multi-submesh key: submesh 0 gets model material 0");
    check(SubmeshMaterialOf(subH, 2u) == SubmeshMaterialOf(subG, 2u),
          "multi-submesh key: a submesh without slot override gets the model material of its materialIndex");
    {
        const auto* hMaterial = MaterialComponentOf(subH);
        const auto* hSlot = hMaterial ? hMaterial->FindSlotOverride(1u) : nullptr;
        check(hSlot && hSlot->materialAssetPath == "Materials/Rot.mat" && hSlot->material.IsValid() &&
              hSlot->material != SubmeshMaterialOf(subG, 1u),
              "multi-submesh key: the .mat slot override of submesh 1 is kept");
    }

    // --- DemoProject scene and frames -----------------------------------------
    g_capturedErrors.clear();
    g_capturing = true;
    const LPSCENE room = Krom::LoadScene("Scenes/Room 0.kscene");
    g_capturing = false;
    for (const std::string& line : g_capturedErrors)
        std::printf("INFO: Room 0 error: %s\n", line.c_str());
    check(CapturedErrorWith("#material/", "could not be loaded") == 0,
          "loading 'Room 0' logs no 'could not be loaded' error for a '#material/' key");
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
