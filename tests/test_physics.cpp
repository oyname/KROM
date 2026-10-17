// =============================================================================
// KROM Engine - tests/test_physics.cpp
// Tests fuer das Physics-Addon (PhysicsWorld, Komponenten-Serialisierung).
// =============================================================================
#include "TestFramework.hpp"

#include "addons/physics/PhysicsComponents.hpp"
#include "addons/physics/PhysicsSerialization.hpp"
#include "addons/physics/PhysicsWorld.hpp"
#include "ecs/Components.hpp"
#include "ecs/World.hpp"
#include "serialization/SceneSerializer.hpp"

#include <cmath>

using namespace engine;
using namespace engine::physics;

namespace {

ecs::ComponentMetaRegistry MakePhysicsRegistry()
{
    ecs::ComponentMetaRegistry reg;
    RegisterCoreComponents(reg);
    RegisterPhysicsComponents(reg);
    return reg;
}

EntityID SpawnBox(ecs::World& world, const math::Vec3& position, const math::Vec3& halfExtents,
                  EntityID parent = NULL_ENTITY)
{
    const EntityID e = world.CreateEntity();
    world.Add<TransformComponent>(e).localPosition = position;
    OBBComponent obb{};
    obb.halfExtents = halfExtents;
    world.Add<OBBComponent>(e, obb);
    if (parent.IsValid())
        world.Add<ParentComponent>(e, ParentComponent{ parent });
    return e;
}

void Simulate(PhysicsWorld& physics, ecs::World& world, float seconds)
{
    const int frames = static_cast<int>(seconds * 60.f);
    for (int i = 0; i < frames; ++i)
        physics.Update(world, 1.f / 60.f);
}

void TestBoxFallsOntoStaticFloor(test::TestContext& ctx)
{
    auto reg = MakePhysicsRegistry();
    ecs::World world(reg);
    SpawnBox(world, { 0.f, 0.f, 0.f }, { 10.f, 0.5f, 10.f });            // Boden, Oberkante y = 0.5
    const EntityID box = SpawnBox(world, { 0.f, 5.f, 0.f }, { 0.5f, 0.5f, 0.5f });
    world.Add<RigidBodyComponent>(box);

    PhysicsWorld physics;
    CHECK(ctx, physics.Initialize());
    Simulate(physics, world, 3.f);

    CHECK_EQ(ctx, physics.BodyCount(), size_t{2});
    const auto* tc = world.Get<TransformComponent>(box);
    CHECK(ctx, std::fabs(tc->localPosition.y - 1.0f) < 0.05f);   // liegt auf dem Boden
    CHECK(ctx, tc->dirty);
}

void TestDisabledAndDestroyedBodies(test::TestContext& ctx)
{
    auto reg = MakePhysicsRegistry();
    ecs::World world(reg);
    const EntityID box = SpawnBox(world, { 0.f, 5.f, 0.f }, { 0.5f, 0.5f, 0.5f });
    world.Add<RigidBodyComponent>(box).enabled = false;

    PhysicsWorld physics;
    physics.Initialize();
    Simulate(physics, world, 0.5f);
    CHECK_EQ(ctx, physics.BodyCount(), size_t{0});
    CHECK(ctx, std::fabs(world.Get<TransformComponent>(box)->localPosition.y - 5.f) < 1e-6f);

    world.Get<RigidBodyComponent>(box)->enabled = true;
    physics.Sync(world);
    CHECK_EQ(ctx, physics.BodyCount(), size_t{1});

    world.DestroyEntity(box);
    physics.Sync(world);
    CHECK_EQ(ctx, physics.BodyCount(), size_t{0});
}

void TestChildCollidersFormOneBody(test::TestContext& ctx)
{
    auto reg = MakePhysicsRegistry();
    ecs::World world(reg);
    SpawnBox(world, { 0.f, 0.f, 0.f }, { 10.f, 0.5f, 10.f });
    const EntityID root = world.CreateEntity();
    world.Add<TransformComponent>(root).localPosition = { 0.f, 5.f, 0.f };
    world.Add<RigidBodyComponent>(root);
    const EntityID left  = SpawnBox(world, { -1.f, 0.f, 0.f }, { 0.5f, 0.5f, 0.5f }, root);
    const EntityID right = SpawnBox(world, {  1.f, 0.f, 0.f }, { 0.5f, 0.5f, 0.5f }, root);

    PhysicsWorld physics;
    physics.Initialize();
    Simulate(physics, world, 3.f);

    CHECK_EQ(ctx, physics.BodyCount(), size_t{2});   // Boden + ein zusammengesetzter Koerper
    CHECK(ctx, physics.HasBody(left));
    CHECK(ctx, physics.HasBody(right));
    // Nur die Rigidbody-Entity wird bewegt, die Kinder behalten ihre lokale Pose.
    CHECK(ctx, std::fabs(world.Get<TransformComponent>(root)->localPosition.y - 1.0f) < 0.05f);
    CHECK(ctx, std::fabs(world.Get<TransformComponent>(left)->localPosition.x + 1.f) < 1e-6f);
}

void TestImpulseMovesDynamicBody(test::TestContext& ctx)
{
    auto reg = MakePhysicsRegistry();
    ecs::World world(reg);
    const EntityID box = SpawnBox(world, { 0.f, 0.f, 0.f }, { 0.5f, 0.5f, 0.5f });
    world.Add<RigidBodyComponent>(box).gravityScale = 0.f;
    const EntityID wall = SpawnBox(world, { 5.f, 0.f, 0.f }, { 0.5f, 0.5f, 0.5f });

    PhysicsWorld physics;
    physics.Initialize();
    physics.Sync(world);
    CHECK(ctx,  physics.AddImpulse(box, { 2.f, 0.f, 0.f }));   // 1 kg → 2 m/s
    CHECK(ctx, !physics.AddImpulse(wall, { 1.f, 0.f, 0.f }));  // statisch
    Simulate(physics, world, 0.5f);

    math::Vec3 velocity{};
    CHECK(ctx, physics.GetLinearVelocity(box, velocity));
    CHECK(ctx, std::fabs(velocity.x - 2.f) < 0.1f);
    CHECK(ctx, world.Get<TransformComponent>(box)->localPosition.x > 0.8f);
}

void TestCommandsBeforeFirstSyncAreQueued(test::TestContext& ctx)
{
    auto reg = MakePhysicsRegistry();
    ecs::World world(reg);
    // Wie ein im selben Frame gespawntes Prefab: Rigidbody + Collider-Kind,
    // Physik hat noch keinen Sync gesehen.
    const EntityID root = world.CreateEntity();
    world.Add<TransformComponent>(root);
    world.Add<RigidBodyComponent>(root).gravityScale = 0.f;
    const EntityID child = SpawnBox(world, { 0.f, 0.f, 0.f }, { 0.5f, 0.5f, 0.5f }, root);

    PhysicsWorld physics;
    physics.Initialize();
    CHECK(ctx, !physics.HasBody(root));
    CHECK(ctx, physics.AddImpulse(child, { 3.f, 0.f, 0.f }));   // vorgemerkt, ueber das Kind

    physics.Update(world, 1.f / 60.f);
    math::Vec3 velocity{};
    CHECK(ctx, physics.GetLinearVelocity(root, velocity));
    CHECK(ctx, std::fabs(velocity.x - 3.f) < 0.05f);

    // Statischer Koerper lehnt ab, unbekannte Entity ohne Koerper verfaellt still.
    const EntityID wall = SpawnBox(world, { 10.f, 0.f, 0.f }, { 0.5f, 0.5f, 0.5f });
    const EntityID empty = world.CreateEntity();
    physics.Sync(world);
    CHECK(ctx, !physics.AddImpulse(wall, { 1.f, 0.f, 0.f }));
    CHECK(ctx, physics.AddForce(empty, { 1.f, 0.f, 0.f }));
    physics.Sync(world);   // darf nichts tun / nicht abstuerzen
    CHECK(ctx, !physics.HasBody(empty));
}

void TestCollisionEventsEnterOncePerBodyPair(test::TestContext& ctx)
{
    auto reg = MakePhysicsRegistry();
    ecs::World world(reg);
    const EntityID floor = SpawnBox(world, { 0.f, 0.f, 0.f }, { 10.f, 0.5f, 10.f });
    // Zusammengesetzter Koerper: zwei Kind-Boxen landen gleichzeitig.
    const EntityID root = world.CreateEntity();
    world.Add<TransformComponent>(root).localPosition = { 0.f, 3.f, 0.f };
    world.Add<RigidBodyComponent>(root);
    SpawnBox(world, { -1.f, 0.f, 0.f }, { 0.5f, 0.5f, 0.5f }, root);
    SpawnBox(world, {  1.f, 0.f, 0.f }, { 0.5f, 0.5f, 0.5f }, root);

    PhysicsWorld physics;
    physics.Initialize();
    std::vector<CollisionEvent> events;
    for (int i = 0; i < 180; ++i)
    {
        physics.Update(world, 1.f / 60.f);
        const auto frame = physics.TakeCollisionEvents();
        events.insert(events.end(), frame.begin(), frame.end());
    }

    int enters = 0, exits = 0;
    for (const CollisionEvent& e : events)
    {
        const bool pair = (e.a == root && e.b == floor) || (e.a == floor && e.b == root);
        CHECK(ctx, pair);
        e.enter ? ++enters : ++exits;
    }
    CHECK_EQ(ctx, enters, 1);   // nicht pro Kind-Box
    CHECK_EQ(ctx, exits, 0);    // liegt ruhig auf (auch nach dem Einschlafen)

    auto countEvents = [&](float seconds, int& outEnters, int& outExits) {
        outEnters = outExits = 0;
        for (int i = 0; i < static_cast<int>(seconds * 60.f); ++i)
        {
            physics.Update(world, 1.f / 60.f);
            for (const CollisionEvent& e : physics.TakeCollisionEvents())
                e.enter ? ++outEnters : ++outExits;
        }
    };

    // Aufwecken ohne Abheben: kein Exit/Enter.
    CHECK(ctx, physics.AddImpulse(root, { 0.2f, 0.f, 0.f }));
    countEvents(0.25f, enters, exits);
    CHECK_EQ(ctx, enters, 0);
    CHECK_EQ(ctx, exits, 0);

    // Abheben: Exit, nach der Landung wieder Enter.
    CHECK(ctx, physics.AddImpulse(root, { 0.f, 8.f, 0.f }));
    countEvents(0.3f, enters, exits);
    CHECK_EQ(ctx, exits, 1);
    countEvents(3.f, enters, exits);
    CHECK_EQ(ctx, enters, 1);

    // Koerper verschwindet → Beruehrung endet mit einem Exit.
    world.DestroyEntity(root);
    physics.Sync(world);
    const auto afterDestroy = physics.TakeCollisionEvents();
    CHECK_EQ(ctx, afterDestroy.size(), size_t{1});
    CHECK(ctx, !afterDestroy.empty() && !afterDestroy[0].enter);

    // Reset verwirft ohne Events.
    physics.Reset();
    CHECK(ctx, physics.TakeCollisionEvents().empty());
}

void TestCharacterWalksAndJumps(test::TestContext& ctx)
{
    auto reg = MakePhysicsRegistry();
    ecs::World world(reg);
    SpawnBox(world, { 0.f, -0.5f, 0.f }, { 20.f, 0.5f, 20.f });          // Boden, Oberkante y = 0
    SpawnBox(world, { 3.f, 0.15f, 0.f }, { 1.f, 0.15f, 2.f });            // Stufe 0.3 m hoch bei x 2..4
    const EntityID hero = world.CreateEntity();
    world.Add<TransformComponent>(hero).localPosition = { 0.f, 0.5f, 0.f };
    world.Add<CharacterControllerComponent>(hero);
    world.Get<TransformComponent>(hero)->localRotation = math::Quat::FromEulerDeg(0.f, 45.f, 0.f);

    PhysicsWorld physics;
    physics.Initialize();
    Simulate(physics, world, 1.f);
    CHECK_EQ(ctx, physics.CharacterCount(), size_t{1});
    CHECK_EQ(ctx, physics.BodyCount(), size_t{2});                         // Collider der Figur zaehlen nicht
    CHECK(ctx, physics.IsCharacterGrounded(hero));
    CHECK(ctx, std::fabs(world.Get<TransformComponent>(hero)->localPosition.y) < 0.05f);   // steht auf dem Boden

    // Laufen in +X ueber die Stufe.
    CHECK(ctx, physics.SetCharacterMove(hero, { 2.f, 0.f, 0.f }));
    Simulate(physics, world, 1.5f);
    const math::Vec3 onStep = world.Get<TransformComponent>(hero)->localPosition;
    CHECK(ctx, onStep.x > 2.4f);                                            // ist vorangekommen
    CHECK(ctx, std::fabs(onStep.y - 0.3f) < 0.05f);                         // steht auf der Stufe
    // Die Physik schreibt nur die Position; die Blickrichtung bleibt beim Script.
    const math::Quat r = world.Get<TransformComponent>(hero)->localRotation;
    const math::Quat expected = math::Quat::FromEulerDeg(0.f, 45.f, 0.f);
    CHECK(ctx, std::fabs(r.x * expected.x + r.y * expected.y + r.z * expected.z + r.w * expected.w) > 0.9999f);

    // Springen: hebt ab und landet wieder.
    physics.SetCharacterMove(hero, { 0.f, 0.f, 0.f });
    Simulate(physics, world, 0.5f);
    CHECK(ctx, physics.CharacterJump(hero, 5.f));
    Simulate(physics, world, 0.2f);
    CHECK(ctx, !physics.IsCharacterGrounded(hero));
    CHECK(ctx, world.Get<TransformComponent>(hero)->localPosition.y > 0.6f);
    Simulate(physics, world, 2.f);
    CHECK(ctx, physics.IsCharacterGrounded(hero));

    world.DestroyEntity(hero);
    physics.Sync(world);
    CHECK_EQ(ctx, physics.CharacterCount(), size_t{0});
}

void TestKinematicStopsWithTransform(test::TestContext& ctx)
{
    auto reg = MakePhysicsRegistry();
    ecs::World world(reg);
    const EntityID platform = SpawnBox(world, { 0.f, 0.f, 0.f }, { 1.f, 0.1f, 1.f });
    world.Add<RigidBodyComponent>(platform).type = RigidBodyType::Kinematic;

    PhysicsWorld physics;
    physics.Initialize();
    physics.Update(world, 1.f / 60.f);

    // Ein Frame Bewegung um 1 m, danach steht die Transform still.
    auto* tc = world.Get<TransformComponent>(platform);
    tc->localPosition.x = 1.f;
    ++tc->localVersion;
    Simulate(physics, world, 1.f);

    math::Vec3 position{};
    math::Quat rotation{};
    CHECK(ctx, physics.GetBodyPose(platform, position, rotation));
    CHECK(ctx, std::fabs(position.x - 1.f) < 1e-3f);   // laeuft nicht weiter
}

void TestColliderChildRespectsInheritParentScale(test::TestContext& ctx)
{
    auto reg = MakePhysicsRegistry();
    ecs::World world(reg);
    SpawnBox(world, { 0.f, 0.f, 0.f }, { 10.f, 0.5f, 10.f });            // Oberkante y = 0.5
    const EntityID root = world.CreateEntity();
    auto& rootTc = world.Add<TransformComponent>(root);
    rootTc.localPosition = { 0.f, 5.f, 0.f };
    rootTc.localScale    = { 2.f, 2.f, 2.f };
    world.Add<RigidBodyComponent>(root).lockRotationX = true;
    world.Get<RigidBodyComponent>(root)->lockRotationZ = true;
    const EntityID child = SpawnBox(world, { 0.f, -1.f, 0.f }, { 0.5f, 0.5f, 0.5f }, root);
    world.Get<TransformComponent>(child)->inheritParentScale = false;

    PhysicsWorld physics;
    physics.Initialize();
    Simulate(physics, world, 3.f);

    // Ohne Parent-Skalierung: Box-Unterkante bei root - 1.5 → root ruht bei y = 2.0
    // (mit faelschlich geerbter Skalierung waere es y = 3.5).
    CHECK(ctx, std::fabs(world.Get<TransformComponent>(root)->localPosition.y - 2.0f) < 0.05f);
}

void TestDirtyOnlyRotationTeleportsDynamicBody(test::TestContext& ctx)
{
    auto reg = MakePhysicsRegistry();
    ecs::World world(reg);
    const EntityID box = SpawnBox(world, { 0.f, 0.f, 0.f }, { 0.5f, 0.5f, 0.5f });
    world.Add<RigidBodyComponent>(box).gravityScale = 0.f;

    PhysicsWorld physics;
    physics.Initialize();
    physics.Update(world, 1.f / 60.f);

    // Wie TransformSystem: dirty nach dem Frame geloescht.
    auto* tc = world.Get<TransformComponent>(box);
    tc->dirty = false;
    tc->SetEulerDeg(0.f, 90.f, 0.f);   // setzt nur dirty, nicht localVersion
    const math::Quat expected = tc->localRotation;
    physics.Update(world, 1.f / 60.f);

    const math::Quat actual = world.Get<TransformComponent>(box)->localRotation;
    const float dot = expected.x * actual.x + expected.y * actual.y +
                      expected.z * actual.z + expected.w * actual.w;
    CHECK(ctx, std::fabs(std::fabs(dot) - 1.f) < 1e-4f);   // nicht zurueckgeschrieben
}

void TestSerializationRoundtrip(test::TestContext& ctx)
{
    auto reg = MakePhysicsRegistry();
    ecs::World world(reg);
    const EntityID e = world.CreateEntity();
    RigidBodyComponent rb{};
    rb.type = RigidBodyType::Kinematic;
    rb.mass = 7.5f;
    rb.lockRotationY = true;
    world.Add<RigidBodyComponent>(e, rb);
    SphereColliderComponent sphere{};
    sphere.radius = 1.25f;
    world.Add<SphereColliderComponent>(e, sphere);
    CapsuleColliderComponent capsule{};
    capsule.axis = CapsuleAxis::Z;
    capsule.height = 3.f;
    world.Add<CapsuleColliderComponent>(e, capsule);

    serialization::SceneSerializer ser(world);
    ser.RegisterDefaultHandlers();
    RegisterPhysicsSerializationHandlers(ser);
    const std::string json = ser.SerializeToJson("Test");

    auto reg2 = MakePhysicsRegistry();
    ecs::World world2(reg2);
    serialization::SceneDeserializer deser(world2);
    deser.RegisterDefaultHandlers();
    RegisterPhysicsDeserializationHandlers(deser);
    CHECK(ctx, deser.DeserializeFromJson(json).success);

    int found = 0;
    world2.ForEachAlive([&](EntityID id)
    {
        const auto* rb2 = world2.Get<RigidBodyComponent>(id);
        if (!rb2) return;
        ++found;
        CHECK(ctx, rb2->type == RigidBodyType::Kinematic);
        CHECK(ctx, std::fabs(rb2->mass - 7.5f) < 1e-5f);
        CHECK(ctx, rb2->lockRotationY && !rb2->lockRotationX);
        const auto* s = world2.Get<SphereColliderComponent>(id);
        CHECK(ctx, s && std::fabs(s->radius - 1.25f) < 1e-5f);
        const auto* c = world2.Get<CapsuleColliderComponent>(id);
        CHECK(ctx, c && c->axis == CapsuleAxis::Z && std::fabs(c->height - 3.f) < 1e-5f);
    });
    CHECK_EQ(ctx, found, 1);
}

} // namespace

int RunPhysicsTests()
{
    test::TestSuite suite("Physics");
    suite
        .Add("Box falls onto static floor",    TestBoxFallsOntoStaticFloor)
        .Add("Disabled and destroyed bodies",  TestDisabledAndDestroyedBodies)
        .Add("Child colliders form one body",  TestChildCollidersFormOneBody)
        .Add("Impulse moves dynamic body",     TestImpulseMovesDynamicBody)
        .Add("Commands before first sync queued", TestCommandsBeforeFirstSyncAreQueued)
        .Add("Collision events once per pair", TestCollisionEventsEnterOncePerBodyPair)
        .Add("Character walks, steps and jumps", TestCharacterWalksAndJumps)
        .Add("Kinematic stops with transform", TestKinematicStopsWithTransform)
        .Add("Child respects inheritParentScale", TestColliderChildRespectsInheritParentScale)
        .Add("Dirty-only rotation teleports",  TestDirtyOnlyRotationTeleportsDynamicBody)
        .Add("Serialization roundtrip",        TestSerializationRoundtrip);
    return suite.Run();
}
