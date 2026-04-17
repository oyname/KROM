// =============================================================================
// ECS-Tests: EntityID, Generation, Archetype-Migration, View, ECB, QueryCache
// =============================================================================
#include "TestFramework.hpp"
#include "addons/camera/CameraComponents.hpp"
#include "addons/lighting/LightingComponents.hpp"
#include "addons/mesh_renderer/MeshRendererComponents.hpp"
#include "addons/particles/ParticleComponents.hpp"
#include "ecs/World.hpp"
#include "ecs/Components.hpp"
#include "ecs/EntityCommandBuffer.hpp"
#include "ecs/QueryCache.hpp"
#include "core/Debug.hpp"
#include <cstdlib>
#include <string>

using namespace engine;
using namespace engine::ecs;

namespace {

static void RegisterECSTestComponents()
{
    RegisterCoreComponents();
}

[[nodiscard]] static int RunDeathCaseProcess(const char* caseName)
{
    const char* exe = std::getenv("KROM_TEST_BINARY");
    if (exe == nullptr || exe[0] == '\0')
        return -1;

#if defined(_WIN32)
    const std::string command = std::string("set KROM_ECS_DEATH_TEST=") + caseName +
                                " && \"" + exe + "\"";
#else
    const std::string command = std::string("KROM_ECS_DEATH_TEST=") + caseName +
                                " \"" + exe + "\" >/dev/null 2>&1";
#endif
    return std::system(command.c_str());
}

static void TestReadPhaseNesting(test::TestContext& ctx)
{
    World world;

    CHECK(ctx, !world.IsReadPhaseActive());
    CHECK_EQ(ctx, world.ReadPhaseDepth(), 0u);

    world.BeginReadPhase();
    CHECK(ctx, world.IsReadPhaseActive());
    CHECK_EQ(ctx, world.ReadPhaseDepth(), 1u);

    world.BeginReadPhase();
    CHECK_EQ(ctx, world.ReadPhaseDepth(), 2u);

    world.EndReadPhase();
    CHECK(ctx, world.IsReadPhaseActive());
    CHECK_EQ(ctx, world.ReadPhaseDepth(), 1u);

    world.EndReadPhase();
    CHECK(ctx, !world.IsReadPhaseActive());
    CHECK_EQ(ctx, world.ReadPhaseDepth(), 0u);
}

static void TestScopedReadPhase(test::TestContext& ctx)
{
    World world;
    const EntityID e = world.CreateEntity();

    {
        auto readScope = world.ReadPhaseScope();
        CHECK(ctx, world.IsReadPhaseActive());
        CHECK_EQ(ctx, world.ReadPhaseDepth(), 1u);

        auto nestedScope = world.ReadPhaseScope();
        CHECK_EQ(ctx, world.ReadPhaseDepth(), 2u);
        (void)nestedScope;
    }

    CHECK(ctx, !world.IsReadPhaseActive());
    CHECK_EQ(ctx, world.ReadPhaseDepth(), 0u);

    world.Add<TransformComponent>(e);
    CHECK(ctx, world.Has<TransformComponent>(e));
}

static void TestReadPhaseMutationFatal(test::TestContext& ctx)
{
    CHECK(ctx, RunDeathCaseProcess("create") != 0);
    CHECK(ctx, RunDeathCaseProcess("destroy") != 0);
    CHECK(ctx, RunDeathCaseProcess("add") != 0);
    CHECK(ctx, RunDeathCaseProcess("remove") != 0);
    CHECK(ctx, RunDeathCaseProcess("end_underflow") != 0);
}

// ==========================================================================
// EntityID
// ==========================================================================
static void TestEntityIDLayout(test::TestContext& ctx)
{
    CHECK_EQ(ctx, NULL_ENTITY.value, 0u);
    CHECK(ctx, !NULL_ENTITY.IsValid());

    EntityID id = EntityID::Make(42u, 3u);
    CHECK_EQ(ctx, id.Index(),      42u);
    CHECK_EQ(ctx, id.Generation(), 3u);
    CHECK(ctx, id.IsValid());

    EntityID older = EntityID::Make(42u, 2u);
    CHECK_NE(ctx, id, older);

    EntityID maxIdx = EntityID::Make(EntityID::INDEX_MASK, 1u);
    CHECK_EQ(ctx, maxIdx.Index(), EntityID::INDEX_MASK);
}

// ==========================================================================
// World: Create / Destroy / Alive
// ==========================================================================
static void TestWorldCreateDestroy(test::TestContext& ctx)
{
    RegisterECSTestComponents();
    World world;

    CHECK_EQ(ctx, world.EntityCount(), 0u);

    EntityID a = world.CreateEntity();
    EntityID b = world.CreateEntity();
    CHECK(ctx, a.IsValid());
    CHECK(ctx, b.IsValid());
    CHECK_NE(ctx, a, b);
    CHECK_EQ(ctx, world.EntityCount(), 2u);

    world.DestroyEntity(a);
    CHECK(ctx, !world.IsAlive(a));
    CHECK(ctx,  world.IsAlive(b));
    CHECK_EQ(ctx, world.EntityCount(), 1u);

    EntityID a2 = world.CreateEntity();
    CHECK(ctx, a2.IsValid());
    CHECK_NE(ctx, a, a2);
    CHECK_EQ(ctx, a.Index(), a2.Index());
    CHECK_GT(ctx, a2.Generation(), a.Generation());

    world.DestroyEntity(a);
    CHECK_EQ(ctx, world.EntityCount(), 2u);
}

// ==========================================================================
// Komponenten Add / Get / Remove / Has
// ==========================================================================
static void TestComponentCRUD(test::TestContext& ctx)
{
    World world;
    EntityID e = world.CreateEntity();

    TransformComponent& tc = world.Add<TransformComponent>(e);
    tc.localPosition = math::Vec3{1, 2, 3};

    TransformComponent* got = world.Get<TransformComponent>(e);
    CHECK(ctx, got != nullptr);
    CHECK_EQ(ctx, got->localPosition.x, 1.f);
    CHECK_EQ(ctx, got->localPosition.y, 2.f);
    CHECK_EQ(ctx, got->localPosition.z, 3.f);

    CHECK(ctx,  world.Has<TransformComponent>(e));
    CHECK(ctx, !world.Has<NameComponent>(e));

    world.Remove<TransformComponent>(e);
    CHECK(ctx, !world.Has<TransformComponent>(e));
    CHECK_NULL(ctx, world.Get<TransformComponent>(e));

    world.DestroyEntity(e);
    CHECK_NULL(ctx, world.Get<TransformComponent>(e));
}

// ==========================================================================
// Archetype-Migration: Add/Remove wechselt Archetype
// ==========================================================================
static void TestArchetypeMigration(test::TestContext& ctx)
{
    World world;
    EntityID e = world.CreateEntity();

    CHECK_EQ(ctx, world.ArchetypeCount(), 0u);

    world.Add<TransformComponent>(e);
    const size_t arch1 = world.ArchetypeCount();
    CHECK_GE(ctx, arch1, 1u);

    world.Add<NameComponent>(e, std::string("test"));
    const size_t arch2 = world.ArchetypeCount();
    CHECK_GT(ctx, arch2, arch1);

    CHECK(ctx, world.Has<TransformComponent>(e));
    CHECK(ctx, world.Has<NameComponent>(e));
    CHECK_EQ(ctx, world.Get<NameComponent>(e)->name, std::string("test"));

    world.Remove<NameComponent>(e);
    CHECK(ctx,  world.Has<TransformComponent>(e));
    CHECK(ctx, !world.Has<NameComponent>(e));
}

// ==========================================================================
// Swap-and-Pop bei Destroy
// ==========================================================================
static void TestSwapOnDestroy(test::TestContext& ctx)
{
    World world;

    EntityID a = world.CreateEntity();
    EntityID b = world.CreateEntity();
    EntityID c = world.CreateEntity();
    world.Add<TransformComponent>(a);
    world.Add<TransformComponent>(b);
    world.Add<TransformComponent>(c);

    world.Get<TransformComponent>(a)->localPosition = {1,0,0};
    world.Get<TransformComponent>(b)->localPosition = {2,0,0};
    world.Get<TransformComponent>(c)->localPosition = {3,0,0};

    world.DestroyEntity(b);

    CHECK(ctx, !world.IsAlive(b));
    CHECK(ctx,  world.IsAlive(a));
    CHECK(ctx,  world.IsAlive(c));

    CHECK_EQ(ctx, world.Get<TransformComponent>(a)->localPosition.x, 1.f);
    CHECK_EQ(ctx, world.Get<TransformComponent>(c)->localPosition.x, 3.f);
}

// ==========================================================================
// View<Ts...>
// ==========================================================================
static void TestView(test::TestContext& ctx)
{
    World world;

    for (int i = 0; i < 5; ++i)
    {
        EntityID e = world.CreateEntity();
        world.Add<TransformComponent>(e);
        if (i < 3)
            world.Add<NameComponent>(e, std::string("Entity") + std::to_string(i));
    }

    int countTransform = 0;
    world.View<TransformComponent>([&](EntityID, TransformComponent&) {
        ++countTransform;
    });
    CHECK_EQ(ctx, countTransform, 5);

    int countBoth = 0;
    world.View<TransformComponent, NameComponent>([&](EntityID, TransformComponent&, NameComponent&) {
        ++countBoth;
    });
    CHECK_EQ(ctx, countBoth, 3);

    world.View<TransformComponent>([](EntityID, TransformComponent& tc) {
        tc.localPosition.x = 42.f;
    });
    int countMutated = 0;
    world.View<TransformComponent>([&](EntityID, TransformComponent& tc) {
        if (tc.localPosition.x == 42.f) ++countMutated;
    });
    CHECK_EQ(ctx, countMutated, 5);
}

// ==========================================================================
// View - const World
// ==========================================================================
static void TestViewConst(test::TestContext& ctx)
{
    World world;
    EntityID e = world.CreateEntity();
    world.Add<TransformComponent>(e);
    world.Get<TransformComponent>(e)->localPosition = {7,8,9};

    const World& cworld = world;
    int found = 0;
    cworld.View<TransformComponent>([&](EntityID, const TransformComponent& tc) {
        if (tc.localPosition.x == 7.f) ++found;
    });
    CHECK_EQ(ctx, found, 1);
}

// ==========================================================================
// EntityCommandBuffer - deferred changes during iteration
// ==========================================================================
static void TestEntityCommandBuffer(test::TestContext& ctx)
{
    World world;

    std::vector<EntityID> ids;
    for (int i = 0; i < 5; ++i)
    {
        EntityID e = world.CreateEntity();
        world.Add<TransformComponent>(e);
        world.Get<TransformComponent>(e)->localPosition.x = static_cast<float>(i);
        ids.push_back(e);
    }

    EntityCommandBuffer ecb;
    world.View<TransformComponent>([&](EntityID id, TransformComponent& tc) {
        if (static_cast<int>(tc.localPosition.x) % 2 == 0)
            ecb.DestroyEntity(id);
    });
    CHECK_EQ(ctx, ecb.PendingCount(), 3u);

    ecb.Commit(world);
    CHECK_EQ(ctx, world.EntityCount(), 2u);

    int foundOdd = 0;
    world.View<TransformComponent>([&](EntityID, TransformComponent& tc) {
        const int x = static_cast<int>(tc.localPosition.x);
        if (x % 2 != 0) ++foundOdd;
    });
    CHECK_EQ(ctx, foundOdd, 2);

    EntityID newEnt = world.CreateEntity();
    world.Add<TransformComponent>(newEnt);
    EntityCommandBuffer ecb2;
    ecb2.AddComponent<NameComponent>(newEnt, std::string("ECB_Added"));
    ecb2.Commit(world);
    CHECK(ctx, world.Has<NameComponent>(newEnt));
    CHECK_EQ(ctx, world.Get<NameComponent>(newEnt)->name, std::string("ECB_Added"));
}

// ==========================================================================
// QueryCache - version invalidation
// ==========================================================================
static void TestQueryCache(test::TestContext& ctx)
{
    World world;

    EntityID e1 = world.CreateEntity();
    world.Add<TransformComponent>(e1);
    world.Add<NameComponent>(e1, std::string("E1"));

    QueryCache cache;
    ComponentSignature sig;
    sig.Set(ComponentTypeID<TransformComponent>::value);
    sig.Set(ComponentTypeID<NameComponent>::value);

    uint64_t version1 = world.StructureVersion();
    int count1 = 0;
    cache.Query(world, sig, [&](Archetype& arch) { count1 += arch.EntityCount(); });
    CHECK_EQ(ctx, count1, 1);
    CHECK_EQ(ctx, cache.EntryCount(), 1u);

    int count2 = 0;
    cache.Query(world, sig, [&](Archetype& arch) { count2 += arch.EntityCount(); });
    CHECK_EQ(ctx, count2, 1);

    EntityID e2 = world.CreateEntity();
    world.Add<TransformComponent>(e2);
    world.Add<NameComponent>(e2, std::string("E2"));
    CHECK(ctx, world.StructureVersion() > version1);

    int count3 = 0;
    cache.Query(world, sig, [&](Archetype& arch) { count3 += arch.EntityCount(); });
    CHECK_EQ(ctx, count3, 2);
}

static void TestComponentRegistrationBundles(test::TestContext& ctx)
{
    auto& registry = ecs::ComponentMetaRegistry::Instance();
    registry.Clear();

    RegisterCoreComponents();
    CHECK(ctx, registry.Get<TransformComponent>() != nullptr);
    CHECK(ctx, registry.Get<BoundsComponent>() != nullptr);
    CHECK(ctx, registry.Get<MeshComponent>() == nullptr);
    CHECK(ctx, registry.Get<CameraComponent>() == nullptr);
    CHECK(ctx, registry.Get<LightComponent>() == nullptr);
    CHECK(ctx, registry.Get<ParticleEmitterComponent>() == nullptr);

    RegisterMeshRendererComponents();
    RegisterCameraComponents();
    RegisterLightingComponents();
    RegisterParticleComponents();

    CHECK(ctx, registry.Get<MeshComponent>() != nullptr);
    CHECK(ctx, registry.Get<MaterialComponent>() != nullptr);
    CHECK(ctx, registry.Get<CameraComponent>() != nullptr);
    CHECK(ctx, registry.Get<LightComponent>() != nullptr);
    CHECK(ctx, registry.Get<ParticleEmitterComponent>() != nullptr);

    registry.Clear();
    RegisterCoreComponents();
    RegisterMeshRendererComponents();
    CHECK(ctx, registry.Get<TransformComponent>() != nullptr);
    CHECK(ctx, registry.Get<MeshComponent>() != nullptr);
    CHECK(ctx, registry.Get<CameraComponent>() == nullptr);
    CHECK(ctx, registry.Get<LightComponent>() == nullptr);
    CHECK(ctx, registry.Get<ParticleEmitterComponent>() == nullptr);
}

} // namespace

int RunECSReadPhaseDeathTest(const char* caseName)
{
    RegisterECSTestComponents();
    engine::Debug::MinLevel = engine::LogLevel::Fatal;

    World world;

    if (std::string(caseName) == "create")
    {
        const auto readScope = world.ReadPhaseScope();
        (void)readScope;
        world.CreateEntity();
        return 0;
    }

    if (std::string(caseName) == "destroy")
    {
        const EntityID e = world.CreateEntity();
        const auto readScope = world.ReadPhaseScope();
        (void)readScope;
        world.DestroyEntity(e);
        return 0;
    }

    if (std::string(caseName) == "add")
    {
        const EntityID e = world.CreateEntity();
        const auto readScope = world.ReadPhaseScope();
        (void)readScope;
        world.Add<TransformComponent>(e);
        return 0;
    }

    if (std::string(caseName) == "remove")
    {
        const EntityID e = world.CreateEntity();
        world.Add<TransformComponent>(e);
        const auto readScope = world.ReadPhaseScope();
        (void)readScope;
        world.Remove<TransformComponent>(e);
        return 0;
    }

    if (std::string(caseName) == "end_underflow")
    {
        world.EndReadPhase();
        return 0;
    }

    return 3;
}

int RunECSTests()
{
    RegisterECSTestComponents();
    engine::Debug::MinLevel = engine::LogLevel::Fatal;

    test::TestSuite suite("ECS");
    suite
        .Add("Component registration bundles", TestComponentRegistrationBundles)
        .Add("EntityID layout",           TestEntityIDLayout)
        .Add("World create/destroy",      TestWorldCreateDestroy)
        .Add("Component CRUD",            TestComponentCRUD)
        .Add("Archetype migration",       TestArchetypeMigration)
        .Add("Swap-on-destroy",           TestSwapOnDestroy)
        .Add("Read phase nesting",        TestReadPhaseNesting)
        .Add("Scoped read phase",         TestScopedReadPhase)
        .Add("Read phase mutation fatal", TestReadPhaseMutationFatal)
        .Add("View<Ts...>",               TestView)
        .Add("View const",                TestViewConst)
        .Add("EntityCommandBuffer",       TestEntityCommandBuffer)
        .Add("QueryCache invalidation",   TestQueryCache);

    return suite.Run();
}
