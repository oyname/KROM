#include "TestFramework.hpp"
#include "addons/script/ScriptModuleApi.hpp"
#include "addons/script/ScriptList.hpp"
#include "ecs/World.hpp"
#include "ecs/Components.hpp"
#include <cmath>
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
        CHECK(ctx, abi && init && registerScripts);
        if (!abi || !init || !registerScripts) { FreeLibrary(dll); return; }
        CHECK(ctx, CompatibleScriptModuleAbi(abi()));
        auto wrongAbi = abi();
        ++wrongAbi.iteratorDebug;
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
}
#endif

int RunScriptModuleTests()
{
    test::TestSuite suite("Script modules");
#ifdef _WIN32
    suite.Add("DLL ECS access, logging and unload/reload", TestDllComponentAccess);
#endif
    return suite.Run();
}
