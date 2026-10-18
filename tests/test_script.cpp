#include "TestFramework.hpp"
#include "addons/script/ScriptList.hpp"
#include "addons/script/ScriptSerialization.hpp"
#include "addons/script/ScriptTriggers.hpp"
#include "ecs/World.hpp"
#include "ecs/ComponentMeta.hpp"
#include "platform/PlatformInput.hpp"
#include "serialization/SceneSerializer.hpp"
#include <cmath>
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
    CHECK(ctx, !sc.AddForce({1.f, 0.f, 0.f}));          // keine Physik → neutral
    CHECK(ctx, !sc.AddImpulse({1.f, 0.f, 0.f}));
    CHECK(ctx, !sc.SetVelocity({1.f, 0.f, 0.f}));
    CHECK(ctx, Near(sc.GetVelocity(), math::Vec3::Zero()));
    CHECK(ctx, !sc.SetMoveVelocity({1.f, 0.f, 0.f}));
    CHECK(ctx, !sc.Jump(5.f));
    CHECK(ctx, !sc.IsGrounded());
    CHECK(ctx, !sc.PlayAudio());                         // kein Audio → neutral
    CHECK(ctx, !sc.IsAudioPlaying());
    CHECK(ctx, !sc.PlayOneShot("Sounds/jump.wav"));
}

// Merkt sich die Aufrufe, statt zu simulieren.
class RecordingPhysics final : public IPhysicsService
{
public:
    EntityID   lastEntity{};
    math::Vec3 force{}, impulse{}, velocity{};
    int        calls = 0;

    bool AddForce(EntityID e, const math::Vec3& f) override { lastEntity = e; force = f; ++calls; return true; }
    bool AddImpulse(EntityID e, const math::Vec3& i) override { lastEntity = e; impulse = i; ++calls; return true; }
    bool SetLinearVelocity(EntityID e, const math::Vec3& v) override { lastEntity = e; velocity = v; ++calls; return true; }
    bool GetLinearVelocity(EntityID e, math::Vec3& out) const override
    {
        out = e.value == 7u ? math::Vec3{0.f, 3.f, 0.f} : math::Vec3{0.f, 0.f, 0.f};
        return true;
    }
};

void TestContextPhysicsForwardsToService(test::TestContext& ctx)
{
    RecordingPhysics physics;
    ScriptServices services{};
    services.physics = &physics;
    const EntityID self = EntityID::Make(1u, 1u);
    ScriptFrameContext fixed{1.f / 60.f, self, services, true};

    CHECK(ctx, fixed.AddForce({1.f, 2.f, 3.f}));
    CHECK_EQ(ctx, physics.lastEntity.value, self.value);   // Default = eigene Entity
    CHECK(ctx, Near(physics.force, {1.f, 2.f, 3.f}));      // OnFixedUpdate: echte Kraft

    ScriptFrameContext sc{0.016f, self, services};
    const EntityID other(7u);
    CHECK(ctx, sc.AddImpulse({0.f, 5.f, 0.f}, other));
    CHECK_EQ(ctx, physics.lastEntity.value, other.value);
    CHECK(ctx, Near(physics.impulse, {0.f, 5.f, 0.f}));

    CHECK(ctx, sc.SetVelocity({4.f, 0.f, 0.f}));
    CHECK(ctx, Near(physics.velocity, {4.f, 0.f, 0.f}));
    CHECK(ctx, Near(sc.GetVelocity(other), {0.f, 3.f, 0.f}));
    CHECK_EQ(ctx, physics.calls, 3);
}

void TestContextFrameForceBecomesImpulse(test::TestContext& ctx)
{
    RecordingPhysics physics;
    ScriptClock clock;
    clock.deltaTime = 0.25f;
    ScriptServices services{};
    services.physics = &physics;
    services.clock   = &clock;
    const EntityID self = EntityID::Make(1u, 1u);

    // Frame-Takt: Kraft * skaliertes Frame-Delta als Impuls (framerate-unabhaengig).
    ScriptFrameContext sc{0.25f, self, services};
    CHECK(ctx, sc.AddForce({4.f, 0.f, 8.f}));
    CHECK(ctx, Near(physics.impulse, {1.f, 0.f, 2.f}));
    CHECK(ctx, Near(physics.force, {0.f, 0.f, 0.f}));

    // Callbacks bekommen DeltaTime 0, nutzen aber das Frame-Delta der Uhr.
    ScriptFrameContext callback{0.f, self, services};
    CHECK(ctx, callback.AddForce({0.f, 4.f, 0.f}));
    CHECK(ctx, Near(physics.impulse, {0.f, 1.f, 0.f}));
}

void TestContextClock(test::TestContext& ctx)
{
    const EntityID self = EntityID::Make(1u, 1u);
    {
        ScriptFrameContext sc{0.02f, self};   // ohne Uhr neutral
        CHECK_EQ(ctx, sc.Time(), 0.f);
        CHECK_EQ(ctx, sc.UnscaledDeltaTime(), 0.02f);
        CHECK_EQ(ctx, sc.FrameCount(), uint64_t{0});
        CHECK_EQ(ctx, sc.GetTimeScale(), 1.f);
        CHECK(ctx, std::fabs(sc.FixedDeltaTime() - 1.f / 60.f) < 1e-7f);
        sc.SetTimeScale(0.5f);                // ohne Uhr wirkungslos
        CHECK_EQ(ctx, sc.GetTimeScale(), 1.f);
        CHECK(ctx, !sc.IsFixedStep());
    }

    ScriptClock clock;
    clock.deltaTime         = 0.01f;
    clock.unscaledDeltaTime = 0.02f;
    clock.time              = 3.5;
    clock.unscaledTime      = 7.0;
    clock.frameCount        = 42u;
    clock.fixedDeltaTime    = 0.01f;
    ScriptServices services{};
    services.clock = &clock;
    ScriptFrameContext sc{0.01f, self, services};
    CHECK_EQ(ctx, sc.Time(), 3.5f);
    CHECK_EQ(ctx, sc.UnscaledTime(), 7.f);
    CHECK_EQ(ctx, sc.UnscaledDeltaTime(), 0.02f);
    CHECK_EQ(ctx, sc.FixedDeltaTime(), 0.01f);
    CHECK_EQ(ctx, sc.FrameCount(), uint64_t{42});

    sc.SetTimeScale(0.5f);
    CHECK_EQ(ctx, clock.timeScale, 0.5f);
    sc.SetTimeScale(-3.f);
    CHECK_EQ(ctx, sc.GetTimeScale(), 0.f);
    sc.SetTimeScale(1000.f);
    CHECK_EQ(ctx, sc.GetTimeScale(), 100.f);

    CHECK(ctx, sc.GetPhysicsInterpolation());
    sc.SetPhysicsInterpolation(false);
    CHECK(ctx, !clock.physicsInterpolation);
    CHECK(ctx, !sc.GetPhysicsInterpolation());
}

class FixedCapture : public ComponentScript
{
public:
    int   fixedCalls = 0;
    float lastDelta  = 0.f;
    bool  wasFixed   = false;
    void OnFixedUpdate(IScriptContext& ctx) override
    {
        ++fixedCalls;
        lastDelta = ctx.DeltaTime();
        wasFixed  = ctx.IsFixedStep();
    }
};

void TestFixedUpdateOnlyForStartedScripts(test::TestContext& ctx)
{
    const EntityID e = EntityID::Make(1u, 1u);
    ScriptList list;
    auto script = std::make_unique<FixedCapture>();
    FixedCapture* raw = script.get();
    list.Add("Fixed", std::move(script), e);

    list.FixedUpdate(1.f / 60.f, e);   // noch kein OnStart → kein OnFixedUpdate
    CHECK_EQ(ctx, raw->fixedCalls, 0);

    list.Update(0.03f, e);
    list.FixedUpdate(1.f / 60.f, e);
    list.FixedUpdate(1.f / 60.f, e);
    CHECK_EQ(ctx, raw->fixedCalls, 2);
    CHECK(ctx, std::fabs(raw->lastDelta - 1.f / 60.f) < 1e-7f);
    CHECK(ctx, raw->wasFixed);
}

// ─── Kollision, Trigger, Szenenwechsel ───────────────────────────────────────

static ecs::ComponentMetaRegistry MakeCollisionRegistry()
{
    ecs::ComponentMetaRegistry reg;
    ecs::RegisterComponent<engine::NameComponent>(reg, "NameComponent");
    ecs::RegisterComponent<engine::TransformComponent>(reg, "TransformComponent");
    ecs::RegisterComponent<engine::WorldTransformComponent>(reg, "WorldTransformComponent");
    ecs::RegisterComponent<engine::ParentComponent>(reg, "ParentComponent");
    ecs::RegisterComponent<engine::BoundsComponent>(reg, "BoundsComponent");
    ecs::RegisterComponent<engine::OBBComponent>(reg, "OBBComponent");
    ecs::RegisterComponent<ScriptList>(reg, "ScriptList");
    return reg;
}

// Box-Kollision (OBB) an Weltposition, ohne Hierarchie.
static EntityID SpawnBox(ecs::World& world, const math::Vec3& position, bool isTrigger = false)
{
    const EntityID e = world.CreateEntity();
    world.Add<engine::WorldTransformComponent>(e).position = position;
    engine::OBBComponent obb{};
    obb.isTrigger = isTrigger;
    world.Add<engine::OBBComponent>(e, obb);
    return e;
}

void TestContextRaycastHitsRootAndIgnoresSelf(test::TestContext& ctx)
{
    auto reg = MakeCollisionRegistry();
    ecs::World world(reg);
    const EntityID self   = SpawnBox(world, {0.f, 0.f, 0.f});
    const EntityID parent = world.CreateEntity();
    const EntityID wall   = SpawnBox(world, {0.f, 0.f, -10.f});
    world.Add<engine::ParentComponent>(wall).parent = parent;
    SpawnBox(world, {0.f, 0.f, -5.f}, /*isTrigger*/ true);   // Trigger blockiert keine Raycasts

    collision::SceneQueries queries;
    queries.BuildColliders(world);
    ScriptServices services{};
    services.world = &world;
    services.sceneQueries = &queries;
    ScriptFrameContext sc{0.016f, self, services};

    collision::RaycastHit hit{};
    CHECK(ctx, sc.Raycast({0.f, 0.f, 0.f}, {0.f, 0.f, -1.f}, 100.f, hit));
    CHECK_EQ(ctx, hit.entity.value, parent.value);              // Wurzel, nicht das Child
    CHECK(ctx, hit.distance > 9.f && hit.distance < 10.f);
    CHECK(ctx, !sc.Raycast({0.f, 0.f, 0.f}, {0.f, 0.f, -1.f}, 5.f, hit));
    // ohne ignoreSelf trifft der Strahl die eigene Box (Start innerhalb)
    CHECK(ctx, sc.Raycast({0.f, 0.f, 0.f}, {0.f, 0.f, -1.f}, 100.f, hit, false));
    CHECK_EQ(ctx, hit.entity.value, self.value);
}

void TestContextOverlapSphere(test::TestContext& ctx)
{
    auto reg = MakeCollisionRegistry();
    ecs::World world(reg);
    const EntityID self = SpawnBox(world, {0.f, 0.f, 0.f});
    const EntityID near = SpawnBox(world, {2.f, 0.f, 0.f});
    SpawnBox(world, {20.f, 0.f, 0.f});

    collision::SceneQueries queries;
    queries.BuildColliders(world);
    ScriptServices services{};
    services.world = &world;
    services.sceneQueries = &queries;
    ScriptFrameContext sc{0.016f, self, services};

    const auto hits = sc.OverlapSphere({0.f, 0.f, 0.f}, 2.f);
    CHECK_EQ(ctx, hits.size(), size_t{1});
    CHECK_EQ(ctx, hits.empty() ? 0u : hits[0].value, near.value);
    CHECK_EQ(ctx, sc.OverlapSphere({0.f, 0.f, 0.f}, 2.f, false).size(), size_t{2});
}

class TriggerCounter : public ComponentScript
{
public:
    int enters = 0;
    int exits  = 0;
    EntityID lastOther{};
    void OnTriggerEnter(IScriptContext&, EntityID other) override { ++enters; lastOther = other; }
    void OnTriggerExit (IScriptContext&, EntityID other) override { ++exits;  lastOther = other; }
};

static TriggerCounter* AttachCounter(ecs::World& world, EntityID e)
{
    auto counter = std::make_unique<TriggerCounter>();
    TriggerCounter* raw = counter.get();
    ScriptList sl;
    sl.SetOwnerEntity(e);
    sl.Add("TriggerCounter", std::move(counter), e);
    sl.Update(0.f, e);   // OnStart — nur gestartete Scripts bekommen Trigger-Events
    world.Add<ScriptList>(e, std::move(sl));
    return raw;
}

void TestTriggerEnterAndExit(test::TestContext& ctx)
{
    auto reg = MakeCollisionRegistry();
    ecs::World world(reg);
    const EntityID zone   = SpawnBox(world, {0.f, 0.f, 0.f}, true);
    const EntityID player = SpawnBox(world, {10.f, 0.f, 0.f});
    TriggerCounter* zoneScript   = AttachCounter(world, zone);
    TriggerCounter* playerScript = AttachCounter(world, player);

    ScriptServices services{};
    services.world = &world;
    ScriptTriggerSystem triggers;

    triggers.Update(world, services);
    CHECK_EQ(ctx, zoneScript->enters, 0);

    world.Get<engine::WorldTransformComponent>(player)->position = {0.5f, 0.f, 0.f};
    triggers.Update(world, services);
    triggers.Update(world, services);   // bleibt drin → kein zweites Enter
    CHECK_EQ(ctx, zoneScript->enters, 1);
    CHECK_EQ(ctx, playerScript->enters, 1);
    CHECK_EQ(ctx, zoneScript->lastOther.value, player.value);
    CHECK_EQ(ctx, playerScript->lastOther.value, zone.value);

    world.Get<engine::WorldTransformComponent>(player)->position = {10.f, 0.f, 0.f};
    triggers.Update(world, services);
    CHECK_EQ(ctx, zoneScript->exits, 1);
    CHECK_EQ(ctx, playerScript->exits, 1);
    CHECK_EQ(ctx, triggers.ActiveContactCount(), size_t{0});
}

class CollisionCounter : public ComponentScript
{
public:
    int enters = 0;
    int exits  = 0;
    EntityID lastOther{};
    void OnCollisionEnter(IScriptContext&, EntityID other) override { ++enters; lastOther = other; }
    void OnCollisionExit (IScriptContext&, EntityID other) override { ++exits;  lastOther = other; }
};

static CollisionCounter* AttachCollisionCounter(ecs::World& world, EntityID e)
{
    auto counter = std::make_unique<CollisionCounter>();
    CollisionCounter* raw = counter.get();
    ScriptList sl;
    sl.SetOwnerEntity(e);
    sl.Add("CollisionCounter", std::move(counter), e);
    sl.Update(0.f, e);   // OnStart
    world.Add<ScriptList>(e, std::move(sl));
    return raw;
}

void TestDispatchCollisionBothSides(test::TestContext& ctx)
{
    auto reg = MakeCollisionRegistry();
    ecs::World world(reg);
    const EntityID crate = world.CreateEntity();
    const EntityID level = world.CreateEntity();
    const EntityID wall  = world.CreateEntity();               // statischer Collider als Kind
    world.Add<engine::ParentComponent>(wall, engine::ParentComponent{level});
    CollisionCounter* crateScript = AttachCollisionCounter(world, crate);
    CollisionCounter* wallScript  = AttachCollisionCounter(world, wall);

    ScriptServices services{};
    services.world = &world;
    DispatchCollision(world, true, crate, wall, services);
    CHECK_EQ(ctx, crateScript->enters, 1);
    CHECK_EQ(ctx, crateScript->lastOther.value, level.value);   // Gegenseite = Wurzel
    CHECK_EQ(ctx, wallScript->enters, 1);
    CHECK_EQ(ctx, wallScript->lastOther.value, crate.value);

    world.DestroyEntity(crate);
    DispatchCollision(world, false, crate, wall, services);     // tote Seite wird uebersprungen
    CHECK_EQ(ctx, wallScript->exits, 1);
}

class EventLog : public ComponentScript
{
public:
    std::vector<ScriptEvent> received;
    std::string              replyWith;   // nicht leer: antwortet per Broadcast
    void OnEvent(IScriptContext& ctx, const ScriptEvent& event) override
    {
        received.push_back(event);
        if (!replyWith.empty())
            ctx.BroadcastEvent(replyWith);
    }
};

static EventLog* AttachEventLog(ecs::World& world, EntityID e)
{
    auto log = std::make_unique<EventLog>();
    EventLog* raw = log.get();
    ScriptList sl;
    sl.SetOwnerEntity(e);
    sl.Add("EventLog", std::move(log), e);
    sl.Update(0.f, e);
    world.Add<ScriptList>(e, std::move(sl));
    return raw;
}

void TestScriptEventsTargetAndBroadcast(test::TestContext& ctx)
{
    auto reg = MakeCollisionRegistry();
    ecs::World world(reg);
    const EntityID button = world.CreateEntity();
    const EntityID door   = world.CreateEntity();
    const EntityID lamp   = world.CreateEntity();
    AttachEventLog(world, button);
    EventLog* doorLog = AttachEventLog(world, door);
    EventLog* lampLog = AttachEventLog(world, lamp);
    doorLog->replyWith = "door_opened";

    std::vector<ScriptEvent> pending;
    ScriptServices services{};
    services.world = &world;
    services.pendingEvents = &pending;

    ScriptFrameContext buttonCtx{0.016f, button, services};
    CHECK(ctx, buttonCtx.SendEvent(door, "open", 1.5f, {1.f, 2.f, 3.f}, "fast"));
    CHECK(ctx, !buttonCtx.SendEvent(NULL_ENTITY, "invalid"));
    CHECK_EQ(ctx, pending.size(), size_t{1});

    DispatchScriptEvents(world, services);
    CHECK_EQ(ctx, doorLog->received.size(), size_t{1});
    CHECK_EQ(ctx, lampLog->received.size(), size_t{0});            // nur das Ziel
    if (!doorLog->received.empty())
    {
        const ScriptEvent& e = doorLog->received[0];
        CHECK_EQ(ctx, e.name, std::string("open"));
        CHECK_EQ(ctx, e.sender.value, button.value);
        CHECK(ctx, e.value == 1.5f && e.vector.z == 3.f && e.text == "fast");
    }
    // Die Antwort der Tuer kommt erst beim naechsten Zustellen an.
    CHECK_EQ(ctx, pending.size(), size_t{1});
    CHECK_EQ(ctx, lampLog->received.size(), size_t{0});

    DispatchScriptEvents(world, services);
    CHECK_EQ(ctx, lampLog->received.size(), size_t{1});            // Broadcast erreicht alle
    CHECK(ctx, !lampLog->received.empty() && lampLog->received[0].name == "door_opened");
    CHECK_EQ(ctx, doorLog->received.size(), size_t{2});            // auch den Absender

    // Ohne Event-Ziel in den Services ist SendEvent neutral.
    ScriptFrameContext bare{0.016f, button};
    CHECK(ctx, !bare.BroadcastEvent("nobody"));
}

class SceneSwitcher : public ComponentScript
{
public:
    void OnUpdate(IScriptContext& ctx) override
    {
        ctx.LoadScene("Scenes/level 2.kscene");
        ctx.LoadScene(3);   // letzter Request gewinnt
    }
};

void TestContextLoadSceneIsDeferred(test::TestContext& ctx)
{
    std::optional<SceneLoadRequest> pending;
    ScriptServices services{};
    services.pendingSceneLoad = &pending;

    const EntityID e = EntityID::Make(1u, 1u);
    ScriptList list;
    list.Add("SceneSwitcher", std::make_unique<SceneSwitcher>(), e);
    list.Update(0.016f, e, services);

    CHECK(ctx, pending.has_value());
    CHECK_EQ(ctx, pending ? pending->index : 0, 3);
    CHECK(ctx, pending && pending->path.empty() && !pending->additive);
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
        .Add("Context: no services is neutral",  TestContextWithoutServicesIsNeutral)
        .Add("Context: physics forwards",        TestContextPhysicsForwardsToService)
        .Add("Context: frame force as impulse",  TestContextFrameForceBecomesImpulse)
        .Add("Context: clock and time scale",    TestContextClock)
        .Add("FixedUpdate only when started",    TestFixedUpdateOnlyForStartedScripts)
        .Add("Context: raycast root/ignoreSelf", TestContextRaycastHitsRootAndIgnoresSelf)
        .Add("Context: overlap sphere",          TestContextOverlapSphere)
        .Add("Trigger: enter and exit",          TestTriggerEnterAndExit)
        .Add("Collision: dispatch both sides",   TestDispatchCollisionBothSides)
        .Add("Events: target and broadcast",     TestScriptEventsTargetAndBroadcast)
        .Add("Context: LoadScene is deferred",   TestContextLoadSceneIsDeferred);
    return suite.Run();
}
