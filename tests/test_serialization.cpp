// =============================================================================
// KROM Engine - tests/test_serialization.cpp
// Tests: JsonWriter, JsonParser, SceneSerializer (Bugfix), SceneDeserializer
// =============================================================================
#include "TestFramework.hpp"
#include "addons/camera/CameraComponents.hpp"
#include "addons/camera/CameraSerialization.hpp"
#include "addons/lighting/LightingComponents.hpp"
#include "addons/lighting/LightingSerialization.hpp"
#include "addons/mesh_renderer/MeshRendererComponents.hpp"
#include "addons/mesh_renderer/MeshRendererSerialization.hpp"
#include "core/Debug.hpp"
#include "ecs/World.hpp"
#include "ecs/Components.hpp"
#include "serialization/SceneSerializer.hpp"
#include <cmath>

using namespace engine;
using namespace engine::serialization;

namespace {

void RegisterSerializationTestComponents(ecs::ComponentMetaRegistry& registry)
{
    RegisterCoreComponents(registry);
    RegisterMeshRendererComponents(registry);
    RegisterCameraComponents(registry);
    RegisterLightingComponents(registry);
}

[[nodiscard]] ecs::ComponentMetaRegistry CreateSerializationRegistry()
{
    ecs::ComponentMetaRegistry registry;
    RegisterSerializationTestComponents(registry);
    return registry;
}

void RegisterSerializationHandlers(SceneSerializer& serializer, SceneDeserializer& deserializer)
{
    serializer.RegisterDefaultHandlers();
    deserializer.RegisterDefaultHandlers();
    engine::addons::camera::RegisterCameraSerializationHandlers(serializer);
    engine::addons::camera::RegisterCameraDeserializationHandlers(deserializer);
    engine::addons::lighting::RegisterLightingSerializationHandlers(serializer);
    engine::addons::lighting::RegisterLightingDeserializationHandlers(deserializer);
    engine::addons::mesh_renderer::RegisterMeshRendererSerializationHandlers(serializer);
    engine::addons::mesh_renderer::RegisterMeshRendererDeserializationHandlers(deserializer);
}

void RegisterSerializationHandlers(SceneSerializer& serializer)
{
    serializer.RegisterDefaultHandlers();
    engine::addons::camera::RegisterCameraSerializationHandlers(serializer);
    engine::addons::lighting::RegisterLightingSerializationHandlers(serializer);
    engine::addons::mesh_renderer::RegisterMeshRendererSerializationHandlers(serializer);
}

void RegisterSerializationHandlers(SceneDeserializer& deserializer)
{
    deserializer.RegisterDefaultHandlers();
    engine::addons::camera::RegisterCameraDeserializationHandlers(deserializer);
    engine::addons::lighting::RegisterLightingDeserializationHandlers(deserializer);
    engine::addons::mesh_renderer::RegisterMeshRendererDeserializationHandlers(deserializer);
}

} // namespace

// ==========================================================================
// JsonParser - Basis-Typen
// ==========================================================================
static void TestJsonParserPrimitives(test::TestContext& ctx)
{
    std::string err;

    // Zahl
    JsonValue n = JsonParser::Parse("42.5", err);
    CHECK(ctx, err.empty());
    CHECK(ctx, n.IsNumber());
    CHECK_EQ(ctx, n.AsFloat(), 42.5f);

    // Bool
    JsonValue b = JsonParser::Parse("true", err);
    CHECK(ctx, b.IsBool());
    CHECK(ctx, b.AsBool());

    // String
    JsonValue s = JsonParser::Parse("\"hello world\"", err);
    CHECK(ctx, s.IsString());
    CHECK_EQ(ctx, s.AsString(), std::string("hello world"));

    // Null
    JsonValue null = JsonParser::Parse("null", err);
    CHECK(ctx, null.IsNull());
}

static void TestJsonParserObject(test::TestContext& ctx)
{
    std::string err;
    const std::string json = R"({"name":"cube","x":1.5,"active":true})";
    JsonValue v = JsonParser::Parse(json, err);
    CHECK(ctx, err.empty());
    CHECK(ctx, v.IsObject());

    const JsonValue* name = v.Get("name");
    CHECK(ctx, name != nullptr);
    CHECK_EQ(ctx, name->AsString(), std::string("cube"));

    const JsonValue* x = v.Get("x");
    CHECK(ctx, x != nullptr);
    CHECK_EQ(ctx, x->AsFloat(), 1.5f);

    CHECK(ctx, v.Get("missing") == nullptr);
}

static void TestJsonParserArray(test::TestContext& ctx)
{
    std::string err;
    JsonValue v = JsonParser::Parse("[1, 2, 3]", err);
    CHECK(ctx, err.empty());
    CHECK(ctx, v.IsArray());
    CHECK_EQ(ctx, v.arrayVal.size(), 3u);
    CHECK_EQ(ctx, v.At(0).AsFloat(), 1.f);
    CHECK_EQ(ctx, v.At(2).AsFloat(), 3.f);
    CHECK(ctx, v.At(99).IsNull());
}

static void TestJsonParserNested(test::TestContext& ctx)
{
    std::string err;
    const std::string json = R"({"pos":[1,2,3],"child":{"val":7}})";
    JsonValue v = JsonParser::Parse(json, err);
    CHECK(ctx, err.empty());

    const JsonValue* pos = v.Get("pos");
    CHECK(ctx, pos && pos->IsArray());
    CHECK_EQ(ctx, pos->AsVec3().x, 1.f);
    CHECK_EQ(ctx, pos->AsVec3().y, 2.f);
    CHECK_EQ(ctx, pos->AsVec3().z, 3.f);

    const JsonValue* child = v.Get("child");
    CHECK(ctx, child && child->IsObject());
    const JsonValue* val = child->Get("val");
    CHECK(ctx, val && val->AsFloat() == 7.f);
}

// ==========================================================================
// BUGFIX: Entities ohne NameComponent werden serialisiert
// ==========================================================================
static void TestSerializerCatchesAllEntities(test::TestContext& ctx)
{
    ecs::ComponentMetaRegistry componentRegistry = CreateSerializationRegistry();
    ecs::World world(componentRegistry);

    // Entity MIT NameComponent
    const EntityID named = world.CreateEntity();
    world.Add<NameComponent>(named, NameComponent("Cube"));
    world.Add<TransformComponent>(named, TransformComponent{});

    // Entity OHNE NameComponent - wurde vorher ignoriert!
    const EntityID unnamed = world.CreateEntity();
    world.Add<TransformComponent>(unnamed, TransformComponent{});
    world.Add<MeshComponent>(unnamed, MeshComponent{});

    // Entity nur mit ActiveComponent
    const EntityID activeOnly = world.CreateEntity();
    world.Add<ActiveComponent>(activeOnly, ActiveComponent{true});

    SceneSerializer ser(world);
    RegisterSerializationHandlers(ser);
    const std::string json = ser.SerializeToJson("TestScene");

    // JSON parsen und entityCount prüfen
    std::string err;
    JsonValue root = JsonParser::Parse(json, err);
    CHECK(ctx, err.empty());

    const JsonValue* count = root.Get("entityCount");
    CHECK(ctx, count != nullptr);
    CHECK_EQ(ctx, count->AsUint(), 3u);  // alle 3 entities

    const JsonValue* entities = root.Get("entities");
    CHECK(ctx, entities != nullptr && entities->IsArray());
    CHECK_EQ(ctx, entities->arrayVal.size(), 3u);  // BUGFIX: war vorher 1
}

// ==========================================================================
// Serializer → Deserializer Round-Trip
// ==========================================================================
static void TestSerializeDeserializeRoundTrip(test::TestContext& ctx)
{
    ecs::ComponentMetaRegistry srcRegistry = CreateSerializationRegistry();
    ecs::World src(srcRegistry);

    const EntityID e1 = src.CreateEntity();
    src.Add<NameComponent>(e1, NameComponent("Player"));
    TransformComponent tc{};
    tc.localPosition = math::Vec3(1.f, 2.f, 3.f);
    tc.localScale    = math::Vec3(1.f, 1.f, 1.f);
    src.Add<TransformComponent>(e1, tc);
    src.Add<ActiveComponent>(e1, ActiveComponent{true});

    const EntityID e2 = src.CreateEntity();
    src.Add<NameComponent>(e2, NameComponent("Light"));
    LightComponent lc{};
    lc.type      = LightType::Directional;
    lc.intensity = 2.5f;
    lc.color     = math::Vec3(1.f, 0.9f, 0.8f);
    src.Add<LightComponent>(e2, lc);

    // Entity ohne NameComponent - wichtig für Bugfix-Test
    const EntityID e3 = src.CreateEntity();
    src.Add<MeshComponent>(e3, MeshComponent{MeshHandle::Make(5u, 1u)});

    // Serialisieren
    SceneSerializer ser(src);
    // Deserialisieren in neue World
    ecs::ComponentMetaRegistry dstRegistry = CreateSerializationRegistry();
    ecs::World dst(dstRegistry);
    SceneDeserializer deser(dst);
    RegisterSerializationHandlers(ser, deser);
    const std::string json = ser.SerializeToJson("RoundTripScene");
    const DeserializeResult result = deser.DeserializeFromJson(json);

    CHECK(ctx, result.success);
    CHECK(ctx, result.error.empty());
    CHECK_EQ(ctx, result.entitiesRead, 3u);
    CHECK_EQ(ctx, dst.EntityCount(), 3u);

    // Prüfen ob Daten korrekt rekonstruiert wurden
    uint32_t foundPlayers = 0u;
    uint32_t foundLights  = 0u;
    uint32_t foundMeshes  = 0u;

    dst.View<NameComponent>([&](EntityID, const NameComponent& n) {
        if (n.name == "Player") ++foundPlayers;
        if (n.name == "Light")  ++foundLights;
    });
    dst.View<MeshComponent>([&](EntityID, const MeshComponent&) { ++foundMeshes; });

    CHECK_EQ(ctx, foundPlayers, 1u);
    CHECK_EQ(ctx, foundLights,  1u);
    CHECK_EQ(ctx, foundMeshes,  1u);

    // Transform-Werte prüfen
    bool posCorrect = false;
    dst.View<NameComponent, TransformComponent>([&](EntityID, const NameComponent& n,
                                                     const TransformComponent& t){
        if (n.name == "Player")
            posCorrect = (t.localPosition.x == 1.f &&
                          t.localPosition.y == 2.f &&
                          t.localPosition.z == 3.f);
    });
    CHECK(ctx, posCorrect);

    // LightComponent-Werte prüfen
    bool lightCorrect = false;
    dst.View<LightComponent>([&](EntityID, const LightComponent& l){
        lightCorrect = (l.type == LightType::Directional && l.intensity == 2.5f);
    });
    CHECK(ctx, lightCorrect);
}

static void TestCameraSerializeDeserializeRoundTrip(test::TestContext& ctx)
{
    ecs::ComponentMetaRegistry srcRegistry = CreateSerializationRegistry();
    ecs::World src(srcRegistry);

    const EntityID cameraEntity = src.CreateEntity();
    src.Add<NameComponent>(cameraEntity, NameComponent("MainCamera"));

    CameraComponent camera{};
    camera.projection   = ProjectionType::Orthographic;
    camera.fovYDeg      = 75.f;
    camera.nearPlane    = 0.25f;
    camera.farPlane     = 500.f;
    camera.orthoSize    = 12.f;
    camera.aspectRatio  = 4.f / 3.f;
    camera.renderLayer  = 7u;
    camera.isMainCamera = true;
    src.Add<CameraComponent>(cameraEntity, camera);

    SceneSerializer ser(src);
    ecs::ComponentMetaRegistry dstRegistry = CreateSerializationRegistry();
    ecs::World dst(dstRegistry);
    SceneDeserializer deser(dst);
    RegisterSerializationHandlers(ser, deser);
    const std::string json = ser.SerializeToJson("CameraScene");

    const DeserializeResult result = deser.DeserializeFromJson(json);

    CHECK(ctx, result.success);
    CHECK(ctx, result.error.empty());
    CHECK_EQ(ctx, result.entitiesRead, 1u);

    bool foundCamera = false;
    dst.View<NameComponent, CameraComponent>([&](EntityID, const NameComponent& name,
                                                 const CameraComponent& restored) {
        if (name.name != "MainCamera")
            return;

        foundCamera = true;
        CHECK(ctx, restored.isMainCamera);
        CHECK_EQ(ctx, restored.projection, ProjectionType::Orthographic);
        CHECK_EQ(ctx, restored.fovYDeg, 75.f);
        CHECK_EQ(ctx, restored.nearPlane, 0.25f);
        CHECK_EQ(ctx, restored.farPlane, 500.f);
        CHECK_EQ(ctx, restored.orthoSize, 12.f);
        CHECK(ctx, std::fabs(restored.aspectRatio - (4.f / 3.f)) < 1e-5f);
        CHECK_EQ(ctx, restored.renderLayer, 7u);
    });

    CHECK(ctx, foundCamera);
}

// ==========================================================================
// Deserializer - Parent-Remap
// ==========================================================================
static void TestDeserializerParentRemap(test::TestContext& ctx)
{
    ecs::ComponentMetaRegistry srcRegistry = CreateSerializationRegistry();
    ecs::World src(srcRegistry);

    const EntityID parent = src.CreateEntity();
    src.Add<NameComponent>(parent, NameComponent("Parent"));
    src.Add<TransformComponent>(parent, TransformComponent{});

    const EntityID child = src.CreateEntity();
    src.Add<NameComponent>(child, NameComponent("Child"));
    src.Add<TransformComponent>(child, TransformComponent{});
    src.Add<ParentComponent>(child, ParentComponent{parent});

    SceneSerializer ser(src);
    ecs::ComponentMetaRegistry dstRegistry = CreateSerializationRegistry();
    ecs::World dst(dstRegistry);
    SceneDeserializer deser(dst);
    RegisterSerializationHandlers(ser, deser);
    const std::string json = ser.SerializeToJson("HierarchyScene");

    const DeserializeResult result = deser.DeserializeFromJson(json);

    CHECK(ctx, result.success);
    CHECK_EQ(ctx, result.entitiesRead, 2u);

    // Child muss einen ParentComponent haben der auf eine gültige Entity zeigt
    EntityID foundParent = NULL_ENTITY;
    dst.View<NameComponent, ParentComponent>([&](EntityID, const NameComponent& n,
                                                  const ParentComponent& pc){
        if (n.name == "Child") foundParent = pc.parent;
    });

    CHECK(ctx, foundParent != NULL_ENTITY);
    CHECK(ctx, dst.IsAlive(foundParent));

    // Der Parent muss "Parent" heißen
    bool parentNameCorrect = false;
    const auto* pName = dst.Get<NameComponent>(foundParent);
    if (pName) parentNameCorrect = (pName->name == "Parent");
    CHECK(ctx, parentNameCorrect);
}

// ==========================================================================
// Deserializer - fehlerhafte JSON-Eingabe
// ==========================================================================
static void TestDeserializerInvalidJson(test::TestContext& ctx)
{
    ecs::ComponentMetaRegistry componentRegistry = CreateSerializationRegistry();
    ecs::World world(componentRegistry);
    SceneDeserializer deser(world);
    RegisterSerializationHandlers(deser);

    // Kein JSON
    auto r1 = deser.DeserializeFromJson("not json at all {{{{");
    CHECK(ctx, !r1.success || r1.entitiesRead == 0u);

    // Leeres Objekt ohne entities
    auto r2 = deser.DeserializeFromJson("{}");
    CHECK(ctx, !r2.success);
    CHECK(ctx, !r2.error.empty());

    CHECK_EQ(ctx, world.EntityCount(), 0u);
}

// ==========================================================================
// Run all serialization tests
// ==========================================================================
int RunSerializationTests()
{
    engine::Debug::MinLevel = engine::LogLevel::Fatal;

    test::TestSuite suite("Serialization");
    suite
        .Add("JsonParser primitives",         TestJsonParserPrimitives)
        .Add("JsonParser object",             TestJsonParserObject)
        .Add("JsonParser array",              TestJsonParserArray)
        .Add("JsonParser nested",             TestJsonParserNested)
        .Add("Serializer catches all entities (bugfix)", TestSerializerCatchesAllEntities)
        .Add("Serialize/Deserialize round-trip",         TestSerializeDeserializeRoundTrip)
        .Add("Camera serialize/deserialize round-trip",  TestCameraSerializeDeserializeRoundTrip)
        .Add("Deserializer parent remap",     TestDeserializerParentRemap)
        .Add("Deserializer invalid JSON",     TestDeserializerInvalidJson);

    return suite.Run();
}
