#include "TestFramework.hpp"
#include "addons/script/ScriptList.hpp"
#include "addons/script/ScriptSerialization.hpp"
#include "ecs/World.hpp"
#include "ecs/ComponentMeta.hpp"
#include "platform/PlatformInput.hpp"
#include "serialization/SceneSerializer.hpp"
#include <vector>
#include <string>

using namespace engine;
using namespace engine::script;

namespace {

// ─── Hilftypen ───────────────────────────────────────────────────────────────

struct CallLog
{
    std::vector<std::string> events;
};

class LoggingScript : public ComponentScript
{
public:
    explicit LoggingScript(CallLog& log, std::string name)
        : m_log(log), m_name(std::move(name)) {}

    void OnStart  (IScriptContext&) override { m_log.events.push_back(m_name + ":start");   }
    void OnUpdate (IScriptContext&) override { m_log.events.push_back(m_name + ":update");  }
    void OnDestroy(IScriptContext&) override { m_log.events.push_back(m_name + ":destroy"); }

private:
    CallLog&    m_log;
    std::string m_name;
};

class DeltaCapture : public ComponentScript
{
public:
    float lastDelta = 0.0f;
    void OnUpdate(IScriptContext& ctx) override { lastDelta = ctx.DeltaTime(); }
};

class EntityCapture : public ComponentScript
{
public:
    EntityID captured{};
    void OnStart(IScriptContext& ctx) override { captured = ctx.GetEntity(); }
};

// ─── Tests ───────────────────────────────────────────────────────────────────

void TestOnStartCalledOnce(test::TestContext& ctx)
{
    CallLog log;
    EntityID entity = EntityID::Make(1u, 1u);

    ScriptList list;
    list.Add("A", std::make_unique<LoggingScript>(log, "A"), entity);

    list.Update(0.016f, entity);
    list.Update(0.016f, entity);
    list.Update(0.016f, entity);

    int startCount = 0;
    for (const auto& e : log.events)
        if (e == "A:start") ++startCount;

    CHECK_EQ(ctx, startCount, 1);
}

void TestUpdateCalledEveryFrame(test::TestContext& ctx)
{
    CallLog log;
    EntityID entity = EntityID::Make(1u, 1u);

    ScriptList list;
    list.Add("A", std::make_unique<LoggingScript>(log, "A"), entity);

    list.Update(0.016f, entity);
    list.Update(0.016f, entity);
    list.Update(0.016f, entity);

    int updateCount = 0;
    for (const auto& e : log.events)
        if (e == "A:update") ++updateCount;

    CHECK_EQ(ctx, updateCount, 3);
}

void TestDestroyCalledOnce(test::TestContext& ctx)
{
    CallLog log;
    EntityID entity = EntityID::Make(1u, 1u);

    ScriptList list;
    list.Add("A", std::make_unique<LoggingScript>(log, "A"), entity);

    list.Update(0.016f, entity);
    list.DestroyAll(entity);
    list.DestroyAll(entity); // Doppelaufruf muss sicher sein (Liste ist leer)

    int destroyCount = 0;
    for (const auto& e : log.events)
        if (e == "A:destroy") ++destroyCount;

    CHECK_EQ(ctx, destroyCount, 1);
}

void TestDestroyNotCalledIfNotStarted(test::TestContext& ctx)
{
    CallLog log;
    EntityID entity = EntityID::Make(1u, 1u);

    ScriptList list;
    list.Add("A", std::make_unique<LoggingScript>(log, "A"), entity);

    // Kein Update → OnStart nie aufgerufen → OnDestroy darf nicht folgen
    list.DestroyAll(entity);

    int destroyCount = 0;
    for (const auto& e : log.events)
        if (e == "A:destroy") ++destroyCount;

    CHECK_EQ(ctx, destroyCount, 0);
}

void TestMultipleScriptsOrder(test::TestContext& ctx)
{
    CallLog log;
    EntityID entity = EntityID::Make(1u, 1u);

    ScriptList list;
    list.Add("A", std::make_unique<LoggingScript>(log, "A"), entity);
    list.Add("B", std::make_unique<LoggingScript>(log, "B"), entity);

    list.Update(0.016f, entity);

    CHECK_EQ(ctx, (int)log.events.size(), 4);
    if (log.events.size() == 4u)
    {
        CHECK_EQ(ctx, log.events[0], std::string("A:start"));
        CHECK_EQ(ctx, log.events[1], std::string("A:update"));
        CHECK_EQ(ctx, log.events[2], std::string("B:start"));
        CHECK_EQ(ctx, log.events[3], std::string("B:update"));
    }
}

void TestDestroyReverseOrder(test::TestContext& ctx)
{
    CallLog log;
    EntityID entity = EntityID::Make(1u, 1u);

    ScriptList list;
    list.Add("A", std::make_unique<LoggingScript>(log, "A"), entity);
    list.Add("B", std::make_unique<LoggingScript>(log, "B"), entity);

    list.Update(0.016f, entity);
    log.events.clear();
    list.DestroyAll(entity);

    CHECK_EQ(ctx, (int)log.events.size(), 2);
    if (log.events.size() == 2u)
    {
        CHECK_EQ(ctx, log.events[0], std::string("B:destroy"));
        CHECK_EQ(ctx, log.events[1], std::string("A:destroy"));
    }
}

void TestDeltaTimePassedThrough(test::TestContext& ctx)
{
    EntityID entity = EntityID::Make(1u, 1u);
    auto raw = std::make_unique<DeltaCapture>();
    DeltaCapture* ptr = raw.get();

    ScriptList list;
    list.Add("DeltaCapture", std::move(raw), entity);
    list.Update(0.033f, entity);

    CHECK_EQ(ctx, ptr->lastDelta, 0.033f);
}

void TestEntityIdPassedThrough(test::TestContext& ctx)
{
    EntityID entity = EntityID::Make(42u, 3u);
    auto raw = std::make_unique<EntityCapture>();
    EntityCapture* ptr = raw.get();

    ScriptList list;
    list.Add("EntityCapture", std::move(raw), entity);
    list.Update(0.016f, entity);

    CHECK(ctx, ptr->captured == entity);
}

void TestGetEntityOnScript(test::TestContext& ctx)
{
    EntityID entity = EntityID::Make(7u, 2u);
    ScriptList list;
    list.Add("A", std::make_unique<DeltaCapture>(), entity);
    CHECK(ctx, list.Instances()[0].script->GetEntity() == entity);
}

void TestRegistryHitAndMiss(test::TestContext& ctx)
{
    ScriptRegistry reg;
    reg.Register<DeltaCapture>("DeltaCapture");

    CHECK(ctx,  reg.Has("DeltaCapture"));
    CHECK(ctx, !reg.Has("Unknown"));

    EntityID entity = EntityID::Make(1u, 1u);
    ScriptList list;

    bool addedOk   = list.Add("DeltaCapture", entity, reg);
    bool addedFail = list.Add("Unknown",      entity, reg);

    CHECK(ctx,  addedOk);
    CHECK(ctx, !addedFail);
    CHECK_EQ(ctx, (int)list.Count(), 1);
}

void TestClassNamePreserved(test::TestContext& ctx)
{
    EntityID entity = EntityID::Make(1u, 1u);
    ScriptList list;
    list.Add("MyScript", std::make_unique<DeltaCapture>(), entity);
    CHECK_EQ(ctx, list.Instances()[0].className, std::string("MyScript"));
}

void TestIsEmptyAndCount(test::TestContext& ctx)
{
    EntityID entity = EntityID::Make(1u, 1u);
    ScriptList list;

    CHECK(ctx, list.IsEmpty());
    CHECK_EQ(ctx, (int)list.Count(), 0);

    list.Add("A", std::make_unique<DeltaCapture>(), entity);

    CHECK(ctx, !list.IsEmpty());
    CHECK_EQ(ctx, (int)list.Count(), 1);
}

void TestDestructorCallsOnDestroy(test::TestContext& ctx)
{
    CallLog log;
    EntityID entity = EntityID::Make(1u, 1u);

    {
        ScriptList list;
        list.Add("A", std::make_unique<LoggingScript>(log, "A"), entity);
        list.Update(0.016f, entity); // OnStart + OnUpdate -> started = true
        // kein explizites DestroyAll — Destruktor übernimmt
    }

    int destroyCount = 0;
    for (const auto& e : log.events)
        if (e == "A:destroy") ++destroyCount;

    CHECK_EQ(ctx, destroyCount, 1);
}

// ─── Roundtrip-Hilfsfunktion ─────────────────────────────────────────────────

static ecs::ComponentMetaRegistry MakeScriptRegistry()
{
    ecs::ComponentMetaRegistry reg;
    ecs::RegisterComponent<engine::NameComponent>(reg, "NameComponent");
    ecs::RegisterComponent<engine::script::ScriptList>(reg, "ScriptList");
    return reg;
}

// ─── Roundtrip-Tests ─────────────────────────────────────────────────────────

void TestRoundtripClassNamePreserved(test::TestContext& ctx)
{
    auto compReg = MakeScriptRegistry();
    ecs::World world(compReg);

    ScriptRegistry scriptReg;
    scriptReg.Register<DeltaCapture>("DeltaCapture");

    // Entity mit ScriptList anlegen
    const EntityID entity = world.CreateEntity();
    world.Add<engine::NameComponent>(entity, engine::NameComponent{"RoundtripEntity"});
    {
        ScriptList sl;
        sl.SetOwnerEntity(entity);
        sl.Add("DeltaCapture", entity, scriptReg);
        world.Add<ScriptList>(entity, std::move(sl));
    }

    // Serialisieren
    serialization::SceneSerializer ser(world);
    ser.RegisterDefaultHandlers();
    engine::script::RegisterScriptSerializationHandlers(ser);
    const std::string json = ser.SerializeToJson("Test");

    // Neue World, deserialisieren
    ecs::ComponentMetaRegistry compReg2 = MakeScriptRegistry();
    ecs::World world2(compReg2);
    serialization::SceneDeserializer deser(world2);
    deser.RegisterDefaultHandlers();
    engine::script::RegisterScriptDeserializationHandlers(deser, &scriptReg);
    const auto result = deser.DeserializeFromJson(json);

    CHECK(ctx, result.success);

    // ScriptList in neuer World finden
    bool found = false;
    world2.ForEachAlive([&](EntityID id)
    {
        const auto* sl = world2.Get<ScriptList>(id);
        if (!sl) return;
        found = true;
        CHECK_EQ(ctx, (int)sl->Count(), 1);
        if (sl->Count() == 1u)
            CHECK_EQ(ctx, sl->Instances()[0].className, std::string("DeltaCapture"));
    });
    CHECK(ctx, found);
}

void TestRoundtripMultipleScripts(test::TestContext& ctx)
{
    auto compReg = MakeScriptRegistry();
    ecs::World world(compReg);

    ScriptRegistry scriptReg;
    scriptReg.Register<DeltaCapture>("DeltaCapture");
    scriptReg.Register<EntityCapture>("EntityCapture");

    const EntityID entity = world.CreateEntity();
    world.Add<engine::NameComponent>(entity, engine::NameComponent{"Multi"});
    {
        ScriptList sl;
        sl.SetOwnerEntity(entity);
        sl.Add("DeltaCapture",  entity, scriptReg);
        sl.Add("EntityCapture", entity, scriptReg);
        world.Add<ScriptList>(entity, std::move(sl));
    }

    serialization::SceneSerializer ser(world);
    ser.RegisterDefaultHandlers();
    engine::script::RegisterScriptSerializationHandlers(ser);
    const std::string json = ser.SerializeToJson("Test");

    ecs::ComponentMetaRegistry compReg2 = MakeScriptRegistry();
    ecs::World world2(compReg2);
    serialization::SceneDeserializer deser(world2);
    deser.RegisterDefaultHandlers();
    engine::script::RegisterScriptDeserializationHandlers(deser, &scriptReg);
    const auto result = deser.DeserializeFromJson(json);

    CHECK(ctx, result.success);

    bool found = false;
    world2.ForEachAlive([&](EntityID id)
    {
        const auto* sl = world2.Get<ScriptList>(id);
        if (!sl) return;
        found = true;
        CHECK_EQ(ctx, (int)sl->Count(), 2);
        if (sl->Count() == 2u)
        {
            CHECK_EQ(ctx, sl->Instances()[0].className, std::string("DeltaCapture"));
            CHECK_EQ(ctx, sl->Instances()[1].className, std::string("EntityCapture"));
        }
    });
    CHECK(ctx, found);
}

void TestRoundtripInstanceName(test::TestContext& ctx)
{
    auto compReg = MakeScriptRegistry();
    ecs::World world(compReg);

    ScriptRegistry scriptReg;
    scriptReg.Register<DeltaCapture>("DeltaCapture");

    const EntityID entity = world.CreateEntity();
    world.Add<engine::NameComponent>(entity, engine::NameComponent{"InstName"});
    {
        ScriptList sl;
        sl.SetOwnerEntity(entity);
        sl.Add("DeltaCapture", entity, scriptReg);
        sl.Instances_Mutable()[0].instanceName = "Mein Speed-Script";
        world.Add<ScriptList>(entity, std::move(sl));
    }

    serialization::SceneSerializer ser(world);
    ser.RegisterDefaultHandlers();
    engine::script::RegisterScriptSerializationHandlers(ser);
    const std::string json = ser.SerializeToJson("Test");

    ecs::ComponentMetaRegistry compReg2 = MakeScriptRegistry();
    ecs::World world2(compReg2);
    serialization::SceneDeserializer deser(world2);
    deser.RegisterDefaultHandlers();
    engine::script::RegisterScriptDeserializationHandlers(deser, &scriptReg);
    const auto result = deser.DeserializeFromJson(json);

    CHECK(ctx, result.success);

    bool found = false;
    world2.ForEachAlive([&](EntityID id)
    {
        const auto* sl = world2.Get<ScriptList>(id);
        if (!sl) return;
        found = true;
        if (sl->Count() >= 1u)
        {
            CHECK_EQ(ctx, sl->Instances()[0].className,    std::string("DeltaCapture"));
            CHECK_EQ(ctx, sl->Instances()[0].instanceName, std::string("Mein Speed-Script"));
        }
    });
    CHECK(ctx, found);
}

void TestRoundtripUnknownClassSkipped(test::TestContext& ctx)
{
    // Serialisiertes JSON mit bekannter + unbekannter Klasse
    const std::string json = R"({
        "name": "Test",
        "entities": [{
            "id": 1,
            "components": [{
                "type": "NameComponent", "name": "E"
            },{
                "type": "ScriptList",
                "scripts": [
                    { "class": "DeltaCapture" },
                    { "class": "NoSuchScript" }
                ]
            }]
        }]
    })";

    auto compReg = MakeScriptRegistry();
    ecs::World world(compReg);
    ScriptRegistry scriptReg;
    scriptReg.Register<DeltaCapture>("DeltaCapture");

    serialization::SceneDeserializer deser(world);
    deser.RegisterDefaultHandlers();
    engine::script::RegisterScriptDeserializationHandlers(deser, &scriptReg);
    const auto result = deser.DeserializeFromJson(json);

    CHECK(ctx, result.success);

    bool found = false;
    world.ForEachAlive([&](EntityID id)
    {
        const auto* sl = world.Get<ScriptList>(id);
        if (!sl) return;
        found = true;
        // Nur DeltaCapture hat eine Instanz; NoSuchScript wurde übersprungen
        CHECK_EQ(ctx, (int)sl->Count(), 1);
        if (sl->Count() == 1u)
            CHECK_EQ(ctx, sl->Instances()[0].className, std::string("DeltaCapture"));
    });
    CHECK(ctx, found);
}

void TestRoundtripNoRegistryNameOnly(test::TestContext& ctx)
{
    // Ohne Registry → AddNameOnly; Klassennamen bleiben erhalten
    const std::string json = R"({
        "name": "Test",
        "entities": [{
            "id": 1,
            "components": [{
                "type": "NameComponent", "name": "E"
            },{
                "type": "ScriptList",
                "scripts": [
                    { "class": "DoorController", "name": "Tuer Eingang" }
                ]
            }]
        }]
    })";

    auto compReg = MakeScriptRegistry();
    ecs::World world(compReg);

    serialization::SceneDeserializer deser(world);
    deser.RegisterDefaultHandlers();
    engine::script::RegisterScriptDeserializationHandlers(deser, nullptr); // kein Registry
    const auto result = deser.DeserializeFromJson(json);

    CHECK(ctx, result.success);

    bool found = false;
    world.ForEachAlive([&](EntityID id)
    {
        const auto* sl = world.Get<ScriptList>(id);
        if (!sl) return;
        found = true;
        CHECK_EQ(ctx, (int)sl->Count(), 1);
        if (sl->Count() == 1u)
        {
            CHECK_EQ(ctx, sl->Instances()[0].className,    std::string("DoorController"));
            CHECK_EQ(ctx, sl->Instances()[0].instanceName, std::string("Tuer Eingang"));
            CHECK(ctx, sl->Instances()[0].script == nullptr); // kein Live-Objekt
        }
    });
    CHECK(ctx, found);
}

// ─── Gameplay-API (IScriptContext-Helfer) ────────────────────────────────────

struct HealthComponent
{
    float hp = 100.f;
};

static ecs::ComponentMetaRegistry MakeGameplayRegistry()
{
    ecs::ComponentMetaRegistry reg;
    ecs::RegisterComponent<engine::NameComponent>(reg, "NameComponent");
    ecs::RegisterComponent<engine::TransformComponent>(reg, "TransformComponent");
    ecs::RegisterComponent<HealthComponent>(reg, "HealthComponent");
    return reg;
}

static bool Near(const math::Vec3& a, const math::Vec3& b)
{
    return (a - b).LengthSq() < 1e-6f;
}

void TestContextTransformMarksDirty(test::TestContext& ctx)
{
    auto reg = MakeGameplayRegistry();
    ecs::World world(reg);
    const EntityID e = world.CreateEntity();
    auto& t = world.Add<engine::TransformComponent>(e);
    t.dirty = false;
    const uint32_t versionBefore = t.localVersion;

    ScriptServices services{};
    services.world = &world;
    ScriptFrameContext sc{0.016f, e, services};

    sc.SetPosition({1.f, 2.f, 3.f});
    const auto* after = world.Get<engine::TransformComponent>(e);
    CHECK(ctx, Near(after->localPosition, {1.f, 2.f, 3.f}));
    CHECK(ctx, after->dirty);
    CHECK_GT(ctx, after->localVersion, versionBefore);

    sc.Translate({0.f, 0.f, 1.f});
    CHECK(ctx, Near(sc.GetPosition(), {1.f, 2.f, 4.f}));
}

void TestContextTranslateLocalFollowsRotation(test::TestContext& ctx)
{
    auto reg = MakeGameplayRegistry();
    ecs::World world(reg);
    const EntityID e = world.CreateEntity();
    world.Add<engine::TransformComponent>(e);

    ScriptServices services{};
    services.world = &world;
    ScriptFrameContext sc{0.016f, e, services};

    sc.SetRotationEulerDeg(0.f, 90.f, 0.f);
    sc.Translate({0.f, 0.f, -2.f}, Space::Local);   // 2 Einheiten "vorwaerts"
    CHECK(ctx, Near(sc.GetPosition(), sc.Forward() * 2.f));
    CHECK(ctx, !Near(sc.Forward(), math::Vec3::Forward()));
}

void TestContextComponentAccess(test::TestContext& ctx)
{
    auto reg = MakeGameplayRegistry();
    ecs::World world(reg);
    const EntityID self  = world.CreateEntity();
    const EntityID other = world.CreateEntity();
    world.Add<engine::NameComponent>(other, engine::NameComponent{"Gegner"});

    ScriptServices services{};
    services.world = &world;
    ScriptFrameContext sc{0.016f, self, services};

    CHECK(ctx, !sc.Has<HealthComponent>());
    HealthComponent* hp = sc.Add<HealthComponent>(self, HealthComponent{50.f});
    CHECK(ctx, hp != nullptr);
    CHECK(ctx, sc.Has<HealthComponent>());
    CHECK_EQ(ctx, sc.Get<HealthComponent>()->hp, 50.f);
    // Add auf vorhandene Komponente liefert die bestehende zurueck
    CHECK(ctx, sc.Add<HealthComponent>(self, HealthComponent{1.f}) == hp);

    CHECK_EQ(ctx, sc.FindEntity("Gegner").value, other.value);
    CHECK_INVALID(ctx, sc.FindEntity("Gibtsnicht"));
    CHECK_NULL(ctx, sc.Get<HealthComponent>(other));
}

class SelfDestroyScript : public ComponentScript
{
public:
    void OnUpdate(IScriptContext& ctx) override { ctx.Destroy(); }
};

void TestContextDestroyIsDeferred(test::TestContext& ctx)
{
    auto reg = MakeGameplayRegistry();
    ecs::World world(reg);
    const EntityID e = world.CreateEntity();

    std::vector<EntityID> pending;
    ScriptServices services{};
    services.world = &world;
    services.pendingDestroy = &pending;

    ScriptList list;
    list.Add("SelfDestroy", std::make_unique<SelfDestroyScript>(), e);
    list.Update(0.016f, e, services);

    CHECK_EQ(ctx, pending.size(), size_t{1});
    CHECK_EQ(ctx, pending.empty() ? 0u : pending[0].value, e.value);
    CHECK(ctx, world.IsAlive(e));   // erst die Runtime zerstoert
}

void TestContextInput(test::TestContext& ctx)
{
    platform::StandardInput input;
    input.BeginFrame();
    input.OnKeyEvent({platform::Key::W, true, false});

    ScriptServices services{};
    services.input = &input;
    ScriptFrameContext sc{0.016f, EntityID::Make(1u, 1u), services};

    CHECK(ctx, sc.KeyDown(platform::Key::W));
    CHECK(ctx, sc.KeyHit(platform::Key::W));
    CHECK(ctx, !sc.KeyDown(platform::Key::S));
    CHECK_EQ(ctx, sc.Axis(platform::Key::S, platform::Key::W), 1.f);
}

void TestContextWithoutServicesIsNeutral(test::TestContext& ctx)
{
    ScriptFrameContext sc{0.016f, EntityID::Make(1u, 1u)};
    CHECK(ctx, sc.GetWorld() == nullptr);
    CHECK(ctx, !sc.KeyDown(platform::Key::W));
    CHECK_NULL(ctx, sc.Get<engine::TransformComponent>());
    sc.SetPosition({1.f, 1.f, 1.f});   // darf nicht abstuerzen
    sc.Destroy();
    CHECK(ctx, Near(sc.GetPosition(), math::Vec3::Zero()));
}

} // namespace

int RunScriptTests()
{
    test::TestSuite suite("Script");
    suite
        .Add("OnStart called once",              TestOnStartCalledOnce)
        .Add("OnUpdate called every frame",      TestUpdateCalledEveryFrame)
        .Add("OnDestroy called once",            TestDestroyCalledOnce)
        .Add("OnDestroy not called if unstarted",TestDestroyNotCalledIfNotStarted)
        .Add("Multiple scripts order",           TestMultipleScriptsOrder)
        .Add("Destroy reverse order",            TestDestroyReverseOrder)
        .Add("DeltaTime passed through",         TestDeltaTimePassedThrough)
        .Add("EntityID passed through",          TestEntityIdPassedThrough)
        .Add("GetEntity on script",              TestGetEntityOnScript)
        .Add("Registry hit and miss",            TestRegistryHitAndMiss)
        .Add("ClassName preserved",              TestClassNamePreserved)
        .Add("IsEmpty and Count",                TestIsEmptyAndCount)
        .Add("Destructor calls OnDestroy",       TestDestructorCallsOnDestroy)
        .Add("Roundtrip: ClassName preserved",   TestRoundtripClassNamePreserved)
        .Add("Roundtrip: multiple scripts",      TestRoundtripMultipleScripts)
        .Add("Roundtrip: instanceName",          TestRoundtripInstanceName)
        .Add("Roundtrip: unknown class skipped", TestRoundtripUnknownClassSkipped)
        .Add("Roundtrip: no registry nameOnly",  TestRoundtripNoRegistryNameOnly)
        .Add("Context: transform marks dirty",   TestContextTransformMarksDirty)
        .Add("Context: local translate",         TestContextTranslateLocalFollowsRotation)
        .Add("Context: component access",        TestContextComponentAccess)
        .Add("Context: destroy is deferred",     TestContextDestroyIsDeferred)
        .Add("Context: input",                   TestContextInput)
        .Add("Context: no services is neutral",  TestContextWithoutServicesIsNeutral);
    return suite.Run();
}
