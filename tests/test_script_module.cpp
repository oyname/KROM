#include "TestFramework.hpp"
#include "addons/script/ScriptModuleApi.hpp"
#include "addons/script/ScriptList.hpp"
#include "addons/script/ScriptSerialization.hpp"
#include "serialization/SceneSerializer.hpp"
#include "ecs/World.hpp"
#include "ecs/Components.hpp"
#include "app/editor/ScriptModuleHost.hpp"
#include "core/PathUtf8.hpp"
#include <filesystem>
#include <fstream>
#include <cmath>
#include <chrono>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

namespace {
bool moduleLogged = false;
void CaptureModuleLog(engine::LogLevel, const char*) { moduleLogged = true; }

void TestDllComponentAccess(test::TestContext& ctx)
{
    using namespace engine;
    using namespace engine::script;
    ecs::ComponentMetaRegistry meta;
    RegisterCoreComponents(meta);
    ecs::RegisterComponent<ScriptList>(meta, "ScriptList");
    ecs::World world(meta);
    const auto entity = world.CreateEntity();
    world.Add<TransformComponent>(entity);
    auto& list = world.Add<ScriptList>(entity);
    list.AddNameOnly("ModuleFixture", "Custom instance");
    ScriptFieldValue speed;
    speed.type = ScriptFieldType::Float;
    speed.floatValue = 3.f;
    list.Instances_Mutable()[0].fieldValues["speed"] = speed;

    for (int iteration = 0; iteration < 2; ++iteration)
    {
        HMODULE dll = LoadLibraryW(L"script_module_fixture.dll");
        CHECK(ctx, dll != nullptr);
        if (!dll) return;
        auto abi = reinterpret_cast<ScriptModuleAbiFn>(GetProcAddress(dll, "KromScriptModuleAbi"));
        auto init = reinterpret_cast<ScriptModuleInitFn>(GetProcAddress(dll, "KromScriptModuleInit"));
        auto registerScripts = reinterpret_cast<RegisterGameScriptsFn>(GetProcAddress(dll, "KromRegisterGameScripts"));
        auto version = reinterpret_cast<ScriptModuleVersionFn>(GetProcAddress(dll, "KromScriptModuleApiVersion"));
        CHECK(ctx, version && version() == ScriptModuleAbiVersion && abi && init && registerScripts);
        if (!version || version() != ScriptModuleAbiVersion || !abi || !init || !registerScripts) { FreeLibrary(dll); return; }
        CHECK(ctx, CompatibleScriptModuleAbi(abi()));
        auto wrongAbi = abi();
        ++wrongAbi.iteratorDebug;
        CHECK(ctx, !CompatibleScriptModuleAbi(wrongAbi));
        wrongAbi = abi();
        wrongAbi.runtime ^= 1u;
        CHECK(ctx, !CompatibleScriptModuleAbi(wrongAbi));
        CHECK(ctx, !init(nullptr));
        ScriptHostApi api;
        api.resolveComponent = &ResolveComponentTypeId;
        api.log = &CaptureModuleLog;
        CHECK(ctx, init(&api));
        {
            ScriptRegistry registry;
            registerScripts(registry);
            ReattachScriptObjects(world, registry);
            ScriptServices services;
            services.world = &world;
            moduleLogged = false;
            world.Get<ScriptList>(entity)->Update(0.016f, entity, services);
            CHECK(ctx, moduleLogged);
            const auto* transform = world.Get<TransformComponent>(entity);
            CHECK(ctx, transform && std::fabs(transform->localPosition.x - 21.f) < 0.001f);
            CHECK(ctx, transform && std::fabs(transform->localRotation.y) > 0.01f);
            CHECK(ctx, world.Has<NameComponent>(entity));
            CHECK_EQ(ctx, world.Get<NameComponent>(entity)->name, std::string("module updated"));
            CHECK_EQ(ctx, world.Get<ScriptList>(entity)->Instances()[0].instanceName, std::string("Custom instance"));
            DetachScriptObjects(world, registry);
            registry.Clear();
        }
        CHECK(ctx, FreeLibrary(dll) != 0);
    }
}

void TestModuleHostSwap(test::TestContext& ctx)
{
    using namespace engine;
    using namespace engine::script;
    ecs::ComponentMetaRegistry meta;
    RegisterCoreComponents(meta);
    ecs::RegisterComponent<ScriptList>(meta, "ScriptList");
    ecs::World world(meta);
    ScriptRegistry registry;
    app::ScriptModuleHost host;
    const auto entity = world.CreateEntity();
    world.Add<TransformComponent>(entity);
    world.Add<NameComponent>(entity);
    world.Add<ScriptList>(entity).AddNameOnly("ModuleFixture");
    const auto library = PathFromUtf8(KROM_BUILD_DIR) / "bin/script_module_fixture.dll";
    std::string error;
    CHECK(ctx, host.Swap(library, world, registry, error));
    CHECK(ctx, host.IsLoaded());
    CHECK(ctx, world.Get<ScriptList>(entity)->Instances()[0].script != nullptr);
    CHECK(ctx, !host.Swap(library.parent_path() / "missing.dll", world, registry, error));
    CHECK(ctx, host.IsLoaded() && registry.Has("ModuleFixture"));
    CHECK(ctx, !host.Swap(library.parent_path() / "script_module_bad_abi.dll", world, registry, error));
    CHECK(ctx, error.find("inkompatibel") != std::string::npos);
    CHECK(ctx, !host.Swap(library.parent_path() / "script_module_preinit.dll", world, registry, error));
    CHECK(ctx, error.find("Init fehlgeschlagen") != std::string::npos);
    CHECK(ctx, host.IsLoaded() && registry.Has("ModuleFixture"));
    ScriptServices services;
    services.world = &world;
    world.Get<ScriptList>(entity)->Update(0.016f, entity, services);
    CHECK_EQ(ctx, world.Get<TransformComponent>(entity)->localPosition.x, 14.f);
    ScriptFieldValue speed;
    speed.type = ScriptFieldType::Float;
    speed.floatValue = 5.f;
    CHECK(ctx, registry.WriteField(*world.Get<ScriptList>(entity)->Instances()[0].script, "ModuleFixture", "speed", speed));
    CHECK(ctx, host.Swap(library, world, registry, error));
    CHECK(ctx, world.IsAlive(entity));
    CHECK(ctx, !world.Get<ScriptList>(entity)->Instances()[0].started);
    world.Get<ScriptList>(entity)->Update(0.016f, entity, services);
    CHECK_EQ(ctx, world.Get<TransformComponent>(entity)->localPosition.x, 35.f);
    host.Unload(&world, registry);
    CHECK(ctx, !host.IsLoaded() && !registry.Has("ModuleFixture"));
    CHECK(ctx, !world.Get<ScriptList>(entity)->Instances()[0].script);
    CHECK_EQ(ctx, world.Get<ScriptList>(entity)->Instances()[0].fieldValues.at("speed").floatValue, 5.f);
}

void TestCameraTargetAcrossModuleSwaps(test::TestContext& ctx)
{
    using namespace engine;
    using namespace engine::script;
    ecs::ComponentMetaRegistry meta;
    RegisterCoreComponents(meta);
    ecs::RegisterComponent<ScriptList>(meta, "ScriptList");
    ecs::World world(meta);
    ScriptRegistry registry;
    app::ScriptModuleHost host;
    const auto target = world.CreateEntity();
    world.Add<TransformComponent>(target);
    world.Add<GuidComponent>(target, GuidComponent{"follow-target-guid"});
    const auto camera = world.CreateEntity();
    world.Add<TransformComponent>(camera);
    world.Add<GuidComponent>(camera, GuidComponent{"follow-camera-guid"});
    auto& list = world.Add<ScriptList>(camera);
    list.AddNameOnly("ModuleFollowCamera");
    ScriptFieldValue reference;
    reference.type = ScriptFieldType::Entity;
    reference.stringValue = "follow-target-guid";
    reference.entityValue = target;
    list.Instances_Mutable()[0].fieldValues["target"] = reference;
    const auto library = PathFromUtf8(KROM_BUILD_DIR) / "bin/script_module_fixture.dll";
    std::string error;
    ScriptServices services;
    services.world = &world;
    for (int iteration = 0; iteration < 3; ++iteration)
    {
        world.Get<TransformComponent>(target)->localPosition.x = 1.f + 10.f * iteration;
        const bool loaded = host.Swap(library, world, registry, error);
        CHECK(ctx, loaded);
        if (!loaded) { host.Unload(&world, registry); return; }
        ResolveScriptEntityReferences(world, registry); // gleicher Ablauf wie im Editor
        const auto& value = list.Instances()[0].fieldValues.at("target");
        CHECK_EQ(ctx, value.stringValue, std::string("follow-target-guid"));
        CHECK_EQ(ctx, value.entityValue.value, target.value);
        list.Update(0.016f, camera, services);
        CHECK_EQ(ctx, world.Get<TransformComponent>(camera)->localPosition.x, 1.f + 10.f * iteration);
    }
    serialization::SceneSerializer serializer(world);
    serializer.RegisterDefaultHandlers();
    RegisterScriptSerializationHandlers(serializer);
    const auto json = serializer.SerializeToJson("CameraReload");
    {
        ecs::World restored(meta);
        (void)restored.CreateEntity(); // andere EntityIDs als in der Ursprungswelt
        serialization::SceneDeserializer deserializer(restored);
        deserializer.RegisterDefaultHandlers();
        RegisterScriptDeserializationHandlers(deserializer, &registry);
        CHECK(ctx, deserializer.DeserializeFromJson(json).success);
        ResolveScriptEntityReferences(restored, registry);
        EntityID restoredCamera{}, restoredTarget{};
        restored.View<GuidComponent>([&](EntityID id, const GuidComponent& guid) {
            if (guid.guid == "follow-camera-guid") restoredCamera = id;
            if (guid.guid == "follow-target-guid") restoredTarget = id;
        });
        CHECK(ctx, restoredCamera.IsValid() && restoredTarget.IsValid());
        if (restoredCamera.IsValid() && restoredTarget.IsValid())
        {
            auto* restoredList = restored.Get<ScriptList>(restoredCamera);
            CHECK_EQ(ctx, restoredList->Instances()[0].fieldValues.at("target").entityValue.value, restoredTarget.value);
            restored.Get<TransformComponent>(restoredTarget)->localPosition.x = 42.f;
            services.world = &restored;
            restoredList->Update(0.016f, restoredCamera, services);
            CHECK_EQ(ctx, restored.Get<TransformComponent>(restoredCamera)->localPosition.x, 42.f);
        }
    }
    host.Unload(&world, registry);
}

void TestModuleFileTimestamps(test::TestContext& ctx)
{
    using namespace engine;
    namespace fs = std::filesystem;
    const auto root = fs::temp_directory_path() / ("krom-script-files-" +
        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::create_directories(root / "bin");
    fs::create_directories(root / "lib");
    fs::create_directories(root / "Assets/Scripts");
    const auto exe = root / "bin/editor.exe";
    const auto library = root / "bin/scripts.dll";
    const auto header = root / "Assets/Scripts/Probe.hpp";
    const auto core = root / "lib/engine_core.lib";
    const auto script = root / "lib/engine_script.lib";
    for (const auto& file : {exe, library, header, core, script}) std::ofstream(file) << "fixture";
    const auto now = fs::file_time_type::clock::now();
    for (const auto& file : {header, core, script}) fs::last_write_time(file, now - std::chrono::seconds(10));
    fs::last_write_time(root / "Assets/Scripts", now - std::chrono::seconds(10));
    fs::last_write_time(library, now - std::chrono::seconds(5));
    fs::last_write_time(exe, now);
    app::ScriptModuleHost host;
    std::string error;
    CHECK(ctx, app::ScriptModuleHost::CheckEngineLibraries(root, exe, error));
    CHECK(ctx, !host.NeedsBuild(root, library));
    fs::last_write_time(header, now);
    CHECK(ctx, host.NeedsBuild(root, library));
    fs::last_write_time(header, now - std::chrono::seconds(10));
    fs::last_write_time(core, now - std::chrono::seconds(2));
    CHECK(ctx, host.NeedsBuild(root, library));
    fs::last_write_time(core, now + std::chrono::seconds(2));
    CHECK(ctx, !app::ScriptModuleHost::CheckEngineLibraries(root, exe, error));
    CHECK(ctx, error.find("Editor neu bauen") != std::string::npos);
    fs::last_write_time(core, now - std::chrono::seconds(10));
    fs::remove(header);
    CHECK(ctx, host.NeedsBuild(root, library));
    fs::remove_all(root);
}
}
#endif

int RunScriptModuleTests()
{
    test::TestSuite suite("Script modules");
#ifdef _WIN32
    suite.Add("DLL ECS access, logging and unload/reload", TestDllComponentAccess);
    suite.Add("Host swap preserves state and survives a bad candidate", TestModuleHostSwap);
    suite.Add("Camera follows target after DLL swaps and scene roundtrip", TestCameraTargetAcrossModuleSwaps);
    suite.Add("Script and engine timestamps gate builds and loading", TestModuleFileTimestamps);
#endif
    return suite.Run();
}
