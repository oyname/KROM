// =============================================================================
// KROM Engine - tests/test_postprocess.cpp
// Post-process volumes: blend factor, blending order and rules, serialization
// in scenes and prefabs.
// =============================================================================
#include "TestFramework.hpp"

#include "addons/postprocess/PostProcessBlend.hpp"
#include "addons/postprocess/PostProcessSerialization.hpp"
#include "addons/postprocess/PostProcessVolumeComponent.hpp"
#include "addons/prefab/Prefab.hpp"
#include "addons/prefab/PrefabInstanceComponent.hpp"
#include "assets/AssetRegistry.hpp"
#include "ecs/Components.hpp"
#include "ecs/World.hpp"
#include "serialization/SceneSerializer.hpp"

#include <cmath>
#include <string>

using namespace engine;
using namespace engine::addons::postprocess;
using engine::renderer::addons::forward::AgxLook;

namespace {

bool Near(float a, float b, float eps = 1e-4f) { return std::abs(a - b) <= eps; }

ecs::ComponentMetaRegistry MakeRegistry()
{
    ecs::ComponentMetaRegistry reg;
    RegisterCoreComponents(reg);
    RegisterPostProcessComponents(reg);
    return reg;
}

EntityID AddVolume(ecs::World& world, const PostProcessVolumeComponent& volume,
                   const math::Vec3& position = {0.f, 0.f, 0.f},
                   const math::Quat& rotation = math::Quat::Identity())
{
    const EntityID e = world.CreateEntity();
    WorldTransformComponent xf{};
    xf.position = position;
    xf.rotation = rotation;
    world.Add<WorldTransformComponent>(e, xf);
    world.Add<PostProcessVolumeComponent>(e, volume);
    return e;
}

PostProcessVolumeComponent ExposureVolume(float ev)
{
    PostProcessVolumeComponent v{};
    v.extents = {5.f, 5.f, 5.f};
    v.blendDistance = 2.f;
    v.exposureEv = ev;
    v.SetOverride(PostProcessOverride::ExposureEv, true);
    return v;
}

void TestBlendFactor(test::TestContext& ctx)
{
    PostProcessVolumeComponent v{};
    v.extents = {4.f, 2.f, 4.f};
    v.boxOffset = {1.f, 0.f, 0.f};
    v.blendDistance = 1.f;
    const math::Vec3 origin{0.f, 0.f, 0.f};
    const math::Quat id = math::Quat::Identity();
    // Box spans x -3..5, y -2..2. Deep inside = 1.
    CHECK(ctx, Near(PostProcessVolumeFactor(v, origin, id, {1.f, 0.f, 0.f}), 1.f));
    // At the wall 0, half the blend distance inside 0.5, outside 0.
    CHECK(ctx, Near(PostProcessVolumeFactor(v, origin, id, {5.f, 0.f, 0.f}), 0.f));
    CHECK(ctx, Near(PostProcessVolumeFactor(v, origin, id, {4.5f, 0.f, 0.f}), 0.5f));
    CHECK(ctx, Near(PostProcessVolumeFactor(v, origin, id, {1.f, 1.75f, 0.f}), 0.25f));
    CHECK_EQ(ctx, PostProcessVolumeFactor(v, origin, id, {5.1f, 0.f, 0.f}), 0.f);
    CHECK_EQ(ctx, PostProcessVolumeFactor(v, origin, id, {-3.5f, 0.f, 0.f}), 0.f);
    // The box turns with the entity: 90 degrees about Y moves the offset to -Z.
    const math::Quat yaw = math::Quat::FromEulerDeg(0.f, 90.f, 0.f);
    const math::Vec3 offsetWorld = yaw.Rotate(v.boxOffset);
    CHECK(ctx, Near(PostProcessVolumeFactor(v, origin, yaw, offsetWorld), 1.f));
    CHECK(ctx, Near(PostProcessVolumeFactor(v, origin, yaw, offsetWorld + yaw.Rotate({3.5f, 0.f, 0.f})), 0.5f));    // Hard edge without blend distance; global ignores the box.
    v.blendDistance = 0.f;
    CHECK(ctx, Near(PostProcessVolumeFactor(v, origin, id, {4.99f, 0.f, 0.f}), 1.f));
    v.global = true;
    CHECK(ctx, Near(PostProcessVolumeFactor(v, origin, id, {100.f, 0.f, 0.f}), 1.f));
}

void TestBlendRules(test::TestContext& ctx)
{
    auto reg = MakeRegistry();
    PostProcessValues base;
    base.tonemap.exposureEv = 1.f;
    base.gtaoIntensity = 0.5f;

    // No volume: base unchanged.
    {
        ecs::World world(reg);
        const auto r = BlendPostProcessVolumes(world, {}, base);
        CHECK_EQ(ctx, r.tonemap.exposureEv, 1.f);
        CHECK_EQ(ctx, r.gtaoIntensity, 0.5f);
    }
    // Outside, at the edge band, inside.
    {
        ecs::World world(reg);
        AddVolume(world, ExposureVolume(3.f));
        CHECK(ctx, Near(BlendPostProcessVolumes(world, {6.f, 0.f, 0.f}, base).tonemap.exposureEv, 1.f));
        CHECK(ctx, Near(BlendPostProcessVolumes(world, {4.f, 0.f, 0.f}, base).tonemap.exposureEv, 2.f)); // factor 0.5
        CHECK(ctx, Near(BlendPostProcessVolumes(world, {0.f, 0.f, 0.f}, base).tonemap.exposureEv, 3.f));
    }
    // Weight scales the factor; weight 0 does nothing.
    {
        ecs::World world(reg);
        auto v = ExposureVolume(3.f);
        v.weight = 0.25f;
        const EntityID e = AddVolume(world, v);
        CHECK(ctx, Near(BlendPostProcessVolumes(world, {}, base).tonemap.exposureEv, 1.5f));
        CHECK(ctx, Near(BlendPostProcessVolumes(world, {4.f, 0.f, 0.f}, base).tonemap.exposureEv, 1.25f));
        world.Get<PostProcessVolumeComponent>(e)->weight = 0.f;
        CHECK_EQ(ctx, BlendPostProcessVolumes(world, {}, base).tonemap.exposureEv, 1.f);
    }
    // Unticked fields keep the base; only the ticked one changes.
    {
        ecs::World world(reg);
        auto v = ExposureVolume(3.f);
        v.gtaoIntensity = 2.f;   // value set but not ticked
        v.ssrIntensity = 0.f;
        v.SetOverride(PostProcessOverride::SsrIntensity, true);
        AddVolume(world, v);
        const auto r = BlendPostProcessVolumes(world, {}, base);
        CHECK_EQ(ctx, r.gtaoIntensity, 0.5f);
        CHECK_EQ(ctx, r.ssrIntensity, 0.f);
        CHECK_EQ(ctx, r.bloomIntensity, base.bloomIntensity);
        CHECK(ctx, r.tonemap.grading.IsNeutral());
    }
    // Global volume acts far away.
    {
        ecs::World world(reg);
        auto v = ExposureVolume(-2.f);
        v.global = true;
        AddVolume(world, v, {1000.f, 0.f, 0.f});
        CHECK(ctx, Near(BlendPostProcessVolumes(world, {}, base).tonemap.exposureEv, -2.f));
    }
    // Priority: ascending, the higher one is applied last (wins); the order of
    // creation does not matter.
    {
        ecs::World world(reg);
        auto high = ExposureVolume(5.f);
        high.priority = 2;
        auto low = ExposureVolume(-5.f);
        low.priority = 1;
        AddVolume(world, high);
        AddVolume(world, low);
        CHECK(ctx, Near(BlendPostProcessVolumes(world, {}, base).tonemap.exposureEv, 5.f));
        // Half-weighted winner: lerp over the lower result.
        world.View<PostProcessVolumeComponent>([](EntityID, PostProcessVolumeComponent& v) {
            if (v.priority == 2) v.weight = 0.5f;
        });
        CHECK(ctx, Near(BlendPostProcessVolumes(world, {}, base).tonemap.exposureEv, 0.f));
    }
    // Equal priority: the smaller box wins.
    {
        ecs::World world(reg);
        auto small = ExposureVolume(2.f);
        small.extents = {3.f, 3.f, 3.f};
        small.blendDistance = 0.f;
        auto large = ExposureVolume(-2.f);
        large.blendDistance = 0.f;
        AddVolume(world, small);
        AddVolume(world, large);
        CHECK(ctx, Near(BlendPostProcessVolumes(world, {}, base).tonemap.exposureEv, 2.f));
    }
    // Inactive entity: ignored.
    {
        ecs::World world(reg);
        const EntityID e = AddVolume(world, ExposureVolume(3.f));
        world.Add<ActiveComponent>(e, ActiveComponent{false});
        CHECK_EQ(ctx, BlendPostProcessVolumes(world, {}, base).tonemap.exposureEv, 1.f);
    }
    // Things volumes never touch: operator, mode, speeds.
    {
        ecs::World world(reg);
        auto v = ExposureVolume(3.f);
        v.autoExposureMinEv = 2.f;
        v.autoExposureMaxEv = 6.f;
        v.SetOverride(PostProcessOverride::AutoExposureMinEv, true);
        AddVolume(world, v);
        PostProcessValues b = base;
        b.tonemap.op = engine::renderer::addons::forward::TonemapOperator::AcesFitted;
        b.tonemap.exposureMode = engine::renderer::addons::forward::ExposureMode::Auto;
        b.tonemap.autoExposure.maxEv = 1.f;   // min 2 over max 1 -> max follows
        const auto r = BlendPostProcessVolumes(world, {}, b);
        CHECK(ctx, r.tonemap.op == b.tonemap.op);
        CHECK(ctx, r.tonemap.exposureMode == b.tonemap.exposureMode);
        CHECK_EQ(ctx, r.tonemap.autoExposure.toBrightSpeed, b.tonemap.autoExposure.toBrightSpeed);
        CHECK(ctx, Near(r.tonemap.autoExposure.minEv, 2.f));
        CHECK(ctx, r.tonemap.autoExposure.maxEv >= r.tonemap.autoExposure.minEv);
    }
}

void TestBlendGradingAndLook(test::TestContext& ctx)
{
    PostProcessValues base;
    PostProcessVolumeComponent v{};
    v.grading.look = AgxLook::Golden;
    v.grading.temperature = 40.f;
    v.grading.colorFilter = {0.5f, 1.f, 1.f};
    v.grading.lift = {{0.f, 0.f, 1.f}, 0.4f};
    v.bloomIntensity = 0.32f;
    v.bloomThreshold = 3.f;
    for (PostProcessOverride o : {PostProcessOverride::GradingLook, PostProcessOverride::GradingTemperature,
                                  PostProcessOverride::GradingColorFilter, PostProcessOverride::GradingLift,
                                  PostProcessOverride::BloomIntensity, PostProcessOverride::BloomThreshold})
        v.SetOverride(o, true);

    PostProcessValues r = base;
    ApplyPostProcessVolume(r, v, 0.5f);   // exactly half: the look does not switch yet
    CHECK(ctx, r.tonemap.grading.look == AgxLook::None);
    CHECK(ctx, Near(r.tonemap.grading.temperature, 20.f));
    CHECK(ctx, Near(r.tonemap.grading.colorFilter[0], 0.75f));
    CHECK(ctx, Near(r.tonemap.grading.lift.color[0], 0.5f) && Near(r.tonemap.grading.lift.value, 0.2f));
    CHECK(ctx, Near(r.bloomIntensity, (base.bloomIntensity + 0.32f) * 0.5f));
    CHECK(ctx, Near(r.bloomThreshold, (base.bloomThreshold + 3.f) * 0.5f));

    r = base;
    ApplyPostProcessVolume(r, v, 0.51f);  // above half: switches
    CHECK(ctx, r.tonemap.grading.look == AgxLook::Golden);
    r = base;
    ApplyPostProcessVolume(r, v, 0.f);
    CHECK(ctx, r.tonemap.grading.IsNeutral());
    CHECK_EQ(ctx, r.bloomIntensity, base.bloomIntensity);
}

PostProcessVolumeComponent MakeFullVolume()
{
    PostProcessVolumeComponent v{};
    v.global = true;
    v.weight = 0.6f;
    v.priority = -3;
    v.extents = {2.f, 1.f, 3.f};
    v.boxOffset = {0.5f, -1.f, 2.f};
    v.blendDistance = 0.75f;
    v.exposureEv = -1.5f;
    v.autoExposureMinEv = -2.f;
    v.autoExposureMaxEv = 3.f;
    v.grading.look = AgxLook::Punchy;
    v.grading.temperature = -30.f;
    v.grading.gain = {{1.f, 0.5f, 0.25f}, -0.5f};
    v.gtaoIntensity = 1.25f;
    v.contactShadowIntensity = 0.5f;
    v.SetOverride(PostProcessOverride::ExposureEv, true);
    v.SetOverride(PostProcessOverride::GradingLook, true);
    v.SetOverride(PostProcessOverride::GradingGain, true);
    v.SetOverride(PostProcessOverride::ContactShadowIntensity, true);
    return v;
}

void TestSceneSerialization(test::TestContext& ctx)
{
    auto reg = MakeRegistry();
    ecs::World world(reg);
    const PostProcessVolumeComponent full = MakeFullVolume();
    AddVolume(world, full);
    AddVolume(world, PostProcessVolumeComponent{});   // neutral, no overrides

    serialization::SceneSerializer ser(world);
    ser.RegisterDefaultHandlers();
    RegisterPostProcessSerializationHandlers(ser);
    const std::string json = ser.SerializeToJson("Test");
    CHECK(ctx, json.find("\"contactShadowIntensity\"") != std::string::npos);

    auto reg2 = MakeRegistry();
    ecs::World world2(reg2);
    serialization::SceneDeserializer deser(world2);
    deser.RegisterDefaultHandlers();
    RegisterPostProcessDeserializationHandlers(deser);
    CHECK(ctx, deser.DeserializeFromJson(json).success);
    int found = 0;
    world2.View<PostProcessVolumeComponent>([&](EntityID, const PostProcessVolumeComponent& c) {
        ++found;
        CHECK(ctx, c == (c.global ? full : PostProcessVolumeComponent{}));
    });
    CHECK_EQ(ctx, found, 2);

    // Missing keys: no override, defaults; unknown names ignored; values clamped.
    std::string error;
    const auto v = serialization::JsonParser::Parse(
        R"({"type":"PostProcessVolumeComponent","weight":3,"extents":[0,1,1],
            "overrides":["ssrIntensity","notAField"],"ssrIntensity":7})", error);
    CHECK(ctx, error.empty());
    const PostProcessVolumeComponent c = ReadPostProcessVolumeFields(v);
    CHECK_EQ(ctx, c.overrides, PostProcessOverrideBit(PostProcessOverride::SsrIntensity));
    CHECK_EQ(ctx, c.weight, 1.f);
    CHECK(ctx, c.extents.x > 0.f);
    CHECK_EQ(ctx, c.ssrIntensity, 1.f);
    CHECK(ctx, c.grading.IsNeutral());
    const auto empty = ReadPostProcessVolumeFields(serialization::JsonParser::Parse("{}", error));
    CHECK(ctx, empty == PostProcessVolumeComponent{});

    // Every flag has its own name and survives the round trip.
    PostProcessVolumeComponent all{};
    all.overrides = (1u << kPostProcessOverrideCount) - 1u;
    serialization::JsonWriter w;
    w.BeginObject();
    WritePostProcessVolumeFields(w, all);
    w.EndObject();
    CHECK_EQ(ctx, ReadPostProcessVolumeFields(serialization::JsonParser::Parse(w.GetString(), error)).overrides,
             all.overrides);
}

void TestPrefabRoundTrip(test::TestContext& ctx)
{
    ecs::ComponentMetaRegistry reg;
    RegisterCoreComponents(reg);
    RegisterPostProcessComponents(reg);
    engine::RegisterPrefabInstanceComponent(reg);
    ecs::World world(reg);
    const PostProcessVolumeComponent full = MakeFullVolume();
    const EntityID root = world.CreateEntity();
    world.Add<NameComponent>(root, NameComponent{"Volume"});
    world.Add<TransformComponent>(root);
    world.Add<WorldTransformComponent>(root);
    world.Add<PostProcessVolumeComponent>(root, full);

    const auto prefab = engine::addons::prefab::BuildPrefabFromWorldEntity(world, root, "Volume");
    CHECK(ctx, !prefab.records.empty() && prefab.records.front().postProcessVolume.has_value());
    const std::string json = engine::addons::prefab::SerializePrefabToJson(prefab);
    CHECK(ctx, json.find("\"postProcessVolume\"") != std::string::npos);

    assets::AssetRegistry registry;
    engine::addons::prefab::PrefabAsset loaded;
    std::string error;
    CHECK(ctx, engine::addons::prefab::DeserializePrefabFromJson(json, registry, loaded, &error));
    ecs::World target(reg);
    const auto instance = engine::addons::prefab::InstantiatePrefab(target, loaded);
    const auto* c = target.Get<PostProcessVolumeComponent>(instance.root);
    CHECK(ctx, c != nullptr && *c == full);
}

} // namespace

int RunPostProcessTests()
{
    test::TestSuite suite("Post-process volumes");
    suite
        .Add("Blend factor (box, offset, rotation, edge, global)", TestBlendFactor)
        .Add("Blend rules (weight, priority, overrides, global)",  TestBlendRules)
        .Add("Grading blend and AgX look threshold",               TestBlendGradingAndLook)
        .Add("Scene serialization",                                TestSceneSerialization)
        .Add("Prefab round trip",                                  TestPrefabRoundTrip);
    return suite.Run();
}
