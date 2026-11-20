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
#include "core/AtomicTextFile.hpp"
#include "core/PathUtf8.hpp"
#include "core/ProjectSettings.hpp"
#include "addons/script/IRenderSettingsService.hpp"
#include "ecs/World.hpp"
#include "ecs/Components.hpp"
#include "serialization/SceneSerializer.hpp"
#include <cmath>
#include <chrono>
#include <fstream>

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
    lc.shadowSettings.contactShadows = false;   // default is on
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
        lightCorrect = (l.type == LightType::Directional && l.intensity == 2.5f &&
                        !l.shadowSettings.contactShadows);
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
static void TestProjectSettingsRoundTrip(test::TestContext& ctx)
{
    std::string error;
    const auto legacy = ReadProjectSettings(JsonParser::Parse(R"({"name":"Legacy"})", error));
    CHECK(ctx, error.empty());
    CHECK_EQ(ctx, legacy.width, 1280);
    CHECK(ctx, !legacy.useSystemResolution);
    CHECK_EQ(ctx, legacy.collisionMask[31], UINT32_MAX);
    CHECK(ctx, legacy.actions.empty());
    CHECK(ctx, !legacy.renderingStored);

    // Scene of a project without the "rendering" block: missing keys = old look.
    const auto oldScene = ReadLegacySceneRenderSettings(JsonParser::Parse(
        R"({"environment":{"tonemapOperator":"aces","msaaSamples":3,"brdfMultiScatter":true,"gtaoReprojection":false}})", error));
    CHECK(ctx, error.empty());
    CHECK_EQ(ctx, oldScene.tonemapOperator, std::string("aces"));
    CHECK_EQ(ctx, oldScene.msaaSamples, 2u);
    CHECK(ctx, oldScene.brdfMultiScatter);
    CHECK(ctx, !oldScene.brdfHeightCorrelated);
    CHECK(ctx, !oldScene.brdfBurleyDiffuse);
    CHECK(ctx, !oldScene.specularAntiAliasing);
    CHECK(ctx, !oldScene.specularOcclusion);
    const auto keylessScene = ReadLegacySceneRenderSettings(JsonParser::Parse(R"({"entities":[]})", error));
    CHECK_EQ(ctx, keylessScene.tonemapOperator, std::string("reinhard"));
    CHECK_EQ(ctx, keylessScene.msaaSamples, 1u);

    ProjectSettings settings;
    settings.windowTitle = "KROM \"Test\"";
    settings.width = 1920;
    settings.height = 1080;
    settings.useSystemResolution = true;
    settings.windowMode = platform::WindowMode::BorderlessWindowed;
    settings.vsync = true;
    settings.testBuild = 2;
    settings.exportBuild = 1;
    settings.gravity.y = -3.5f;
    settings.collisionMask[31] &= ~1u;
    settings.actions.push_back({"Jump", platform::Key::Space, platform::GamepadButton::South});
    settings.rendering.tonemapOperator = "pbrNeutral";
    settings.rendering.msaaSamples = 4u;
    settings.rendering.postAA = "smaa";
    settings.rendering.brdfPerLightFresnel = false;
    settings.rendering.brdfBurleyDiffuse = false;
    settings.rendering.specularOcclusion = false;
    settings.rendering.gtaoEnabled = false;
    settings.rendering.gtaoRadius = 1.5f;   // above the old 1.0 m limit
    settings.rendering.contactShadowsEnabled = false;
    settings.rendering.contactShadowLength = 0.8f;
    settings.rendering.contactShadowThickness = 0.02f;
    settings.rendering.contactShadowIntensity = 0.5f;
    settings.rendering.contactShadowSteps = 20u;
    settings.rendering.specularAntiAliasing = false;
    JsonWriter out;
    out.BeginObject();
    WriteProjectSettings(out, settings);
    out.EndObject();
    CHECK(ctx, out.GetString().find("gtaoReprojection") == std::string::npos);
    const auto oldProject = ReadProjectSettings(JsonParser::Parse(
        R"({"settings":{"rendering":{"msaaSamples":2,"gtaoReprojection":true}}})", error));
    CHECK(ctx, error.empty());
    CHECK(ctx, oldProject.renderingStored);
    CHECK_EQ(ctx, oldProject.rendering.msaaSamples, 2u);
    CHECK_EQ(ctx, oldProject.rendering.postAA, std::string("off"));   // missing key: post-AA off
    CHECK(ctx, oldProject.rendering.gtaoEnabled);   // missing key: GTAO stays on
    CHECK_EQ(ctx, oldProject.rendering.gtaoRadius, 0.5f);   // missing key: default radius
    CHECK(ctx, oldProject.rendering.contactShadowsEnabled);   // missing keys: contact shadow defaults
    CHECK_EQ(ctx, oldProject.rendering.contactShadowLength, 0.30f);
    CHECK_EQ(ctx, oldProject.rendering.contactShadowSteps, 12u);
    CHECK(ctx, oldProject.rendering.specularAntiAliasing);   // missing key: specular AA stays on
    CHECK(ctx, oldProject.rendering.brdfBurleyDiffuse);      // missing key: Burley on
    const auto result = ReadProjectSettings(JsonParser::Parse(out.GetString(), error));
    CHECK(ctx, error.empty());
    CHECK_EQ(ctx, result.windowTitle, settings.windowTitle);
    CHECK_EQ(ctx, result.width, settings.width);
    CHECK_EQ(ctx, result.height, settings.height);
    CHECK(ctx, result.useSystemResolution);
    CHECK_EQ(ctx, result.windowMode, settings.windowMode);
    CHECK(ctx, result.vsync);
    CHECK_EQ(ctx, result.testBuild, settings.testBuild);
    CHECK_EQ(ctx, result.exportBuild, settings.exportBuild);
    CHECK_EQ(ctx, result.gravity.y, settings.gravity.y);
    CHECK_EQ(ctx, result.collisionMask[31], settings.collisionMask[31]);
    CHECK(ctx, result.renderingStored);
    CHECK_EQ(ctx, result.rendering.tonemapOperator, settings.rendering.tonemapOperator);
    CHECK_EQ(ctx, result.rendering.msaaSamples, 4u);
    CHECK_EQ(ctx, result.rendering.postAA, std::string("smaa"));
    CHECK(ctx, result.rendering.brdfHeightCorrelated);
    CHECK(ctx, !result.rendering.brdfPerLightFresnel);
    CHECK(ctx, result.rendering.brdfMultiScatter);
    CHECK(ctx, !result.rendering.brdfBurleyDiffuse);
    CHECK(ctx, !result.rendering.specularOcclusion);
    CHECK(ctx, !result.rendering.gtaoEnabled);
    CHECK_EQ(ctx, result.rendering.gtaoRadius, 1.5f);
    CHECK(ctx, !result.rendering.contactShadowsEnabled);
    CHECK_EQ(ctx, result.rendering.contactShadowLength, 0.8f);
    CHECK_EQ(ctx, result.rendering.contactShadowThickness, 0.02f);
    CHECK_EQ(ctx, result.rendering.contactShadowIntensity, 0.5f);
    CHECK_EQ(ctx, result.rendering.contactShadowSteps, 20u);
    CHECK(ctx, !result.rendering.specularAntiAliasing);
    CHECK_EQ(ctx, result.actions.size(), size_t{1});
    if (!result.actions.empty())
    {
        CHECK_EQ(ctx, result.actions[0].name, settings.actions[0].name);
        CHECK_EQ(ctx, result.actions[0].key, platform::Key::Space);
        CHECK_EQ(ctx, result.actions[0].button, platform::GamepadButton::South);
    }
}

namespace {
// Plain IRenderSettingsService: checks the default SetQualityLevel/GetQualityLevel.
class FakeRenderSettingsService final : public script::IRenderSettingsService
{
public:
    script::RenderSettings values{};
    int setCalls = 0;
    script::RenderSettings Get() const override { return values; }
    void Set(const script::RenderSettings& in) override { values = in; ++setCalls; }
};
} // namespace

static void TestRenderQualityLevels(test::TestContext& ctx)
{
    // Every level detects as itself; the levels differ.
    for (RenderQualityLevel level : {RenderQualityLevel::Low, RenderQualityLevel::Medium, RenderQualityLevel::High})
    {
        CHECK_EQ(ctx, DetectRenderQualityLevel(RenderQualityPreset(level)), level);
        RenderQualityLevel parsed = RenderQualityLevel::Custom;
        CHECK(ctx, ParseRenderQualityLevel(RenderQualityLevelName(level), parsed));
        CHECK_EQ(ctx, parsed, level);
    }
    RenderQualityLevel parsed = RenderQualityLevel::Low;
    CHECK(ctx, !ParseRenderQualityLevel("ultra", parsed));
    CHECK(ctx, !ParseRenderQualityLevel(nullptr, parsed));
    CHECK_EQ(ctx, parsed, RenderQualityLevel::Low);

    // Contents as agreed with the user (2026-11-19).
    const RenderQualityOptions low = RenderQualityPreset(RenderQualityLevel::Low);
    CHECK_EQ(ctx, low.msaaSamples, 1u);
    CHECK_EQ(ctx, low.postAA, RenderPostAA::Fxaa);
    CHECK(ctx, !low.gtaoEnabled && !low.ssrEnabled && !low.contactShadowsEnabled);
    const RenderQualityOptions medium = RenderQualityPreset(RenderQualityLevel::Medium);
    CHECK_EQ(ctx, medium.msaaSamples, 1u);
    CHECK_EQ(ctx, medium.postAA, RenderPostAA::Smaa);
    CHECK(ctx, medium.gtaoEnabled && !medium.ssrEnabled && medium.contactShadowsEnabled);
    const RenderQualityOptions high = RenderQualityPreset(RenderQualityLevel::High);
    CHECK_EQ(ctx, high.msaaSamples, 2u);
    CHECK_EQ(ctx, high.postAA, RenderPostAA::Smaa);
    CHECK(ctx, high.gtaoEnabled && high.ssrEnabled && high.contactShadowsEnabled);

    // Project settings: defaults (no post-AA) are Custom; a level sets only its
    // five options; one changed option makes it Custom; Custom changes nothing.
    ProjectRenderSettings rendering;
    rendering.gtaoRadius = 1.25f;
    rendering.ssrIntensity = 0.4f;
    CHECK_EQ(ctx, GetRenderQualityLevel(rendering), RenderQualityLevel::Custom);
    ApplyRenderQualityLevel(rendering, RenderQualityLevel::Medium);
    CHECK_EQ(ctx, GetRenderQualityLevel(rendering), RenderQualityLevel::Medium);
    CHECK_EQ(ctx, rendering.postAA, std::string("smaa"));
    CHECK(ctx, !rendering.ssrEnabled);
    CHECK_EQ(ctx, rendering.gtaoRadius, 1.25f);
    CHECK_EQ(ctx, rendering.ssrIntensity, 0.4f);
    ApplyRenderQualityLevel(rendering, RenderQualityLevel::Low);
    CHECK_EQ(ctx, rendering.postAA, std::string("fxaa"));
    CHECK_EQ(ctx, GetRenderQualityLevel(rendering), RenderQualityLevel::Low);
    rendering.ssrEnabled = true;
    CHECK_EQ(ctx, GetRenderQualityLevel(rendering), RenderQualityLevel::Custom);
    ApplyRenderQualityLevel(rendering, RenderQualityLevel::Custom);
    CHECK(ctx, rendering.ssrEnabled);
    CHECK_EQ(ctx, rendering.postAA, std::string("fxaa"));
    ApplyRenderQualityLevel(rendering, RenderQualityLevel::High);
    rendering.postAA = "unknown";   // unknown name counts as off
    CHECK_EQ(ctx, GetRenderQualityOptions(rendering).postAA, RenderPostAA::Off);
    rendering.postAA = "smaa";
    rendering.msaaSamples = 3u;     // normalised like the renderer: 3 -> 2
    CHECK_EQ(ctx, GetRenderQualityLevel(rendering), RenderQualityLevel::High);

    // Script service: default level functions go through Get/Set and keep the
    // other options.
    FakeRenderSettingsService service;
    service.values.exposureEv = 1.5f;
    service.values.specularOcclusion = false;
    CHECK_EQ(ctx, service.GetQualityLevel(), RenderQualityLevel::Custom);   // defaults: post-AA off
    service.SetQualityLevel(script::QualityLevel::Low);
    CHECK_EQ(ctx, service.GetQualityLevel(), script::QualityLevel::Low);
    CHECK_EQ(ctx, service.values.postAA, script::PostAAMode::Fxaa);
    CHECK(ctx, !service.values.gtaoEnabled && !service.values.contactShadowsEnabled);
    CHECK_EQ(ctx, service.values.exposureEv, 1.5f);
    CHECK(ctx, !service.values.specularOcclusion);
    service.SetQualityLevel(script::QualityLevel::High);
    CHECK_EQ(ctx, service.values.msaaSamples, 2u);
    CHECK_EQ(ctx, service.GetQualityLevel(), script::QualityLevel::High);
    const int calls = service.setCalls;
    service.SetQualityLevel(script::QualityLevel::Custom);
    CHECK_EQ(ctx, service.setCalls, calls);
    service.values.ssrEnabled = false;
    CHECK_EQ(ctx, service.GetQualityLevel(), script::QualityLevel::Custom);

    // Per-computer user settings: round trip, missing/invalid/custom = none.
    std::string error;
    UserGameSettings user;
    user.qualityLevel = RenderQualityLevel::Medium;
    const UserGameSettings read = ReadUserGameSettings(JsonParser::Parse(WriteUserGameSettings(user), error));
    CHECK(ctx, error.empty());
    CHECK(ctx, read.qualityLevel.has_value());
    if (read.qualityLevel)
        CHECK_EQ(ctx, *read.qualityLevel, RenderQualityLevel::Medium);
    CHECK(ctx, !ReadUserGameSettings(JsonParser::Parse(WriteUserGameSettings({}), error)).qualityLevel);
    CHECK(ctx, !ReadUserGameSettings(JsonParser::Parse(R"({"qualityLevel":"custom"})", error)).qualityLevel);
    CHECK(ctx, !ReadUserGameSettings(JsonParser::Parse(R"({"qualityLevel":"ultra"})", error)).qualityLevel);
    CHECK(ctx, !ReadUserGameSettings(JsonParser::Parse(R"({"qualityLevel":2})", error)).qualityLevel);

    // Folder names: forbidden characters replaced, trailing dots/spaces dropped.
    CHECK_EQ(ctx, UserGameSettingsFolderName("My Game"), std::string("My Game"));
    CHECK_EQ(ctx, UserGameSettingsFolderName("  A:B/C\\D?*. "), std::string("A_B_C_D__"));
    CHECK_EQ(ctx, UserGameSettingsFolderName(" .. "), std::string());
    CHECK_EQ(ctx, UserGameSettingsFolderName(""), std::string());
    for (const std::string name : {"CON", "con.txt", "PRN", "aux", "NUL", "CONIN$", "conout$.txt",
                                  "COM1", "com9.txt", "LPT1", "lpt9", "COM\xC2\xB9", "LPT\xC2\xB2.txt",
                                  "COM\xC2\xB3"})
        CHECK_EQ(ctx, UserGameSettingsFolderName(name), "_" + name);
    for (const std::string name : {"COM0", "COM10", "LPT0", "LPT10", "Console", "_CON"})
        CHECK_EQ(ctx, UserGameSettingsFolderName(name), name);
    CHECK_EQ(ctx, UserGameSettingsFolderName(" con .txt "), std::string("_con .txt"));
}

static void TestUserGameSettingsFiles(test::TestContext& ctx)
{
    namespace fs = std::filesystem;
    const fs::path root = fs::temp_directory_path() / ("krom-user-settings-" +
        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    for (const std::string title : {"CON", "con.txt", "NUL", "PRN", "AUX", "LPT1", "COM9",
                                   "COM\xC2\xB9", "LPT\xC2\xB2.txt", "Normal Game", "Spiel Gr\xC3\xBCn"})
    {
        const fs::path path = root / PathFromUtf8(UserGameSettingsFolderName(title)) / "user-settings.json";
        // Exercise creation, replacement and a fresh read, including device
        // names and UTF-8 paths on the actual filesystem.
        for (const auto level : {RenderQualityLevel::Low, RenderQualityLevel::High})
        {
            UserGameSettings settings;
            settings.qualityLevel = level;
            CHECK(ctx, WriteTextFileAtomically(path, WriteUserGameSettings(settings)));
            std::ifstream file(path, std::ios::binary);
            CHECK(ctx, file.is_open());
            const std::string json(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>{});
            std::string error;
            const auto read = ReadUserGameSettings(JsonParser::Parse(json, error));
            CHECK(ctx, error.empty());
            CHECK(ctx, read.qualityLevel == level);
        }
    }
    fs::remove_all(root);
}

static void TestPortableAssetPathRule(test::TestContext& ctx)
{
    CHECK(ctx, IsPortableAssetPath(""));
    CHECK(ctx, IsPortableAssetPath("Materials/dpad_gelb.mat"));
    CHECK(ctx, IsPortableAssetPath("Materials\\Mask\\Mask.mat"));
    CHECK(ctx, IsPortableAssetPath("Models/mask2.glb#material/0"));
    CHECK(ctx, IsPortableAssetPath("./Materials/a.mat"));
    CHECK(ctx, IsPortableAssetPath("Materials/../Shared/a.mat"));
    CHECK(ctx, IsPortableAssetPath("Materials/\xC3\xBC" "ber.mat"));

    CHECK(ctx, !IsPortableAssetPath("F:/working/krom codex/editor/dpad_gelb.mat"));
    CHECK(ctx, !IsPortableAssetPath("f:\\projects\\a.mat"));
    CHECK(ctx, !IsPortableAssetPath("C:relative.mat"));
    CHECK(ctx, !IsPortableAssetPath("/home/user/a.mat"));
    CHECK(ctx, !IsPortableAssetPath("\\\\server\\share\\a.mat"));
    CHECK(ctx, !IsPortableAssetPath("../editor/dpad_gelb.mat"));
    CHECK(ctx, !IsPortableAssetPath("Materials/../../editor/dpad_gelb.mat"));
}

// Regression: an editor gizmo material (absolute path into the engine's
// editor/ folder) ended up on a scene mesh and was saved verbatim. Scenes must
// only store material paths relative to the project's Assets folder.
static void TestSceneDropsNonPortableMaterialPaths(test::TestContext& ctx)
{
    ecs::ComponentMetaRegistry srcRegistry = CreateSerializationRegistry();
    ecs::World src(srcRegistry);

    const EntityID leaked = src.CreateEntity();
    src.Add<NameComponent>(leaked, NameComponent("Leaked"));
    MaterialComponent leakedMaterial{};
    leakedMaterial.materialAssetPath = "F:/working/krom codex/editor/dpad_gelb.mat";
    leakedMaterial.materialOverrideSource = "{\"leaked\":true}";
    MaterialComponent::SlotOverride keptSlot{};
    keptSlot.submeshIndex = 0u;
    keptSlot.materialAssetPath = "Materials/Mask/Zaehne.mat";
    leakedMaterial.slotOverrides.push_back(keptSlot);
    MaterialComponent::SlotOverride leakedSlot{};
    leakedSlot.submeshIndex = 1u;
    leakedSlot.materialAssetPath = "../editor/dpad_rot.mat";
    leakedMaterial.slotOverrides.push_back(leakedSlot);
    src.Add<MaterialComponent>(leaked, leakedMaterial);

    const EntityID portable = src.CreateEntity();
    src.Add<NameComponent>(portable, NameComponent("Portable"));
    MaterialComponent portableMaterial{};
    portableMaterial.materialAssetPath = "Materials/dpad_gelb.mat";
    portableMaterial.materialOverrideSource = "{\"kept\":true}";
    src.Add<MaterialComponent>(portable, portableMaterial);

    SceneSerializer ser(src);
    ecs::ComponentMetaRegistry dstRegistry = CreateSerializationRegistry();
    ecs::World dst(dstRegistry);
    SceneDeserializer deser(dst);
    RegisterSerializationHandlers(ser, deser);
    const std::string json = ser.SerializeToJson("PortablePathScene");
    CHECK(ctx, json.find("F:/working") == std::string::npos);
    CHECK(ctx, json.find("../editor") == std::string::npos);
    CHECK(ctx, json.find("leaked") == std::string::npos);

    const DeserializeResult result = deser.DeserializeFromJson(json);
    CHECK(ctx, result.success);

    bool checkedLeaked = false;
    bool checkedPortable = false;
    dst.View<NameComponent, MaterialComponent>([&](EntityID, const NameComponent& n,
                                                    const MaterialComponent& m) {
        if (n.name == "Leaked")
        {
            checkedLeaked = true;
            CHECK(ctx, m.materialAssetPath.empty());
            CHECK(ctx, m.materialOverrideSource.empty());
            const MaterialComponent::SlotOverride* slot0 = m.FindSlotOverride(0u);
            CHECK(ctx, slot0 != nullptr);
            if (slot0)
                CHECK_EQ(ctx, slot0->materialAssetPath, std::string("Materials/Mask/Zaehne.mat"));
            const MaterialComponent::SlotOverride* slot1 = m.FindSlotOverride(1u);
            CHECK(ctx, slot1 == nullptr || slot1->materialAssetPath.empty());
        }
        if (n.name == "Portable")
        {
            checkedPortable = true;
            CHECK_EQ(ctx, m.materialAssetPath, std::string("Materials/dpad_gelb.mat"));
            CHECK_EQ(ctx, m.materialOverrideSource, std::string("{\"kept\":true}"));
        }
    });
    CHECK(ctx, checkedLeaked);
    CHECK(ctx, checkedPortable);
}

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
        .Add("Deserializer invalid JSON",     TestDeserializerInvalidJson)
        .Add("Project settings and legacy defaults", TestProjectSettingsRoundTrip)
        .Add("Render quality levels",         TestRenderQualityLevels)
        .Add("User game settings files",      TestUserGameSettingsFiles)
        .Add("Portable asset path rule",      TestPortableAssetPathRule)
        .Add("Scene drops non-portable material paths", TestSceneDropsNonPortableMaterialPaths);

    return suite.Run();
}
