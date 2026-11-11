// =============================================================================
// KROM Engine - tests/test_animation.cpp
// Tests für AnimationEvaluator, AnimationSampler und SkeletonPoseEvaluator.
// =============================================================================
#include "TestFramework.hpp"

#include "addons/animation/AnimationComponents.hpp"
#include "addons/animation/AnimationSystem.hpp"
#include "addons/animation/AnimationScriptService.hpp"
#include "addons/script/IScriptContext.hpp"
#include "animation/AnimationClipEditing.hpp"
#include "animation/AnimationClipFile.hpp"
#include "animation/AnimationEvaluator.hpp"
#include "animation/AnimationSampler.hpp"
#include "animation/ModelClipSettings.hpp"
#include "animation/SkeletonPoseEvaluator.hpp"
#include "assets/AssetRegistry.hpp"
#include "assets/AnimationClip.hpp"
#include "ecs/Components.hpp"
#include "ecs/World.hpp"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <memory>

using namespace engine;
using namespace engine::assets;
using namespace engine::animation;

namespace {

[[nodiscard]] bool Near(float a, float b, float eps = 1e-5f) noexcept
{
    return std::fabs(a - b) <= eps;
}

[[nodiscard]] bool NearVec3(const math::Vec3& a, const math::Vec3& b, float eps = 1e-5f) noexcept
{
    return Near(a.x, b.x, eps) && Near(a.y, b.y, eps) && Near(a.z, b.z, eps);
}

// ─── Clip-Fabrik-Funktionen ──────────────────────────────────────────────────

// Zwei Bones (Indizes 5 und 9), je separate T/R/S-Channels, LINEAR.
[[nodiscard]] AnimationClip MakeLinearClip()
{
    AnimationClip clip;
    clip.name     = "Linear";
    clip.duration = 2.f;
    clip.looping  = true;

    // Bone 5 – Translation
    {
        AnimationChannel ch;
        ch.targetIndex   = 5;
        ch.targetName    = "bone5";
        ch.property      = AnimationTargetProperty::Translation;
        ch.interpolation = AnimationInterpolation::Linear;
        ch.times         = { 0.f, 2.f };
        ch.vec3Values    = { {0.f,0.f,0.f}, {2.f,4.f,6.f} };
        clip.channels.push_back(ch);
    }
    // Bone 5 – Rotation
    {
        AnimationChannel ch;
        ch.targetIndex   = 5;
        ch.targetName    = "bone5";
        ch.property      = AnimationTargetProperty::Rotation;
        ch.interpolation = AnimationInterpolation::Linear;
        ch.times         = { 0.f, 2.f };
        ch.quatValues    = { math::Quat::Identity(),
                             math::Quat::FromAxisAngleDeg(math::Vec3::Up(), 90.f) };
        clip.channels.push_back(ch);
    }
    // Bone 5 – Scale
    {
        AnimationChannel ch;
        ch.targetIndex   = 5;
        ch.targetName    = "bone5";
        ch.property      = AnimationTargetProperty::Scale;
        ch.interpolation = AnimationInterpolation::Linear;
        ch.times         = { 0.f, 2.f };
        ch.vec3Values    = { {1.f,1.f,1.f}, {3.f,3.f,3.f} };
        clip.channels.push_back(ch);
    }
    // Bone 9 – Translation (nur ein Keyframe)
    {
        AnimationChannel ch;
        ch.targetIndex   = 9;
        ch.targetName    = "bone9";
        ch.property      = AnimationTargetProperty::Translation;
        ch.interpolation = AnimationInterpolation::Linear;
        ch.times         = { 1.f };
        ch.vec3Values    = { {7.f,8.f,9.f} };
        clip.channels.push_back(ch);
    }
    return clip;
}

} // namespace

// =============================================================================
// LINEAR-Interpolation
// =============================================================================

static void TestLinearInterpolation(test::TestContext& ctx)
{
    const AnimationClip clip   = MakeLinearClip();
    const auto          poses  = AnimationSampler::SampleForSkeleton(clip, 1.f, 10u);

    CHECK_EQ(ctx, poses.size(), 10u);

    // Bone 5 bei t=1 → Mitte zwischen Keyframe 0 und 2
    CHECK(ctx, Near(poses[5].position.x, 1.f));
    CHECK(ctx, Near(poses[5].position.y, 2.f));
    CHECK(ctx, Near(poses[5].position.z, 3.f));
    CHECK(ctx, Near(poses[5].scale.x,    2.f));
    CHECK(ctx, Near(poses[5].scale.y,    2.f));
    CHECK(ctx, Near(poses[5].scale.z,    2.f));
    CHECK(ctx, Near(poses[5].rotation.LengthSq(), 1.f));  // normalisiert

    // Bone 9 – einziger Keyframe, immer dieser Wert
    CHECK(ctx, Near(poses[9].position.x, 7.f));
    CHECK(ctx, Near(poses[9].position.y, 8.f));
    CHECK(ctx, Near(poses[9].position.z, 9.f));
}

// =============================================================================
// Looping und Clamping
// =============================================================================

static void TestLoopingAndClamping(test::TestContext& ctx)
{
    AnimationClip clip = MakeLinearClip();  // duration=2

    // Looping: t=3 ≡ t=1
    clip.looping = true;
    {
        const auto poses = AnimationSampler::SampleForSkeleton(clip, 3.f, 10u);
        CHECK(ctx, Near(poses[5].position.x, 1.f));
        CHECK(ctx, Near(poses[5].position.y, 2.f));
    }

    // Non-looping: t=9 → geklammert auf 2 (letzter Keyframe)
    clip.looping = false;
    {
        const auto poses = AnimationSampler::SampleForSkeleton(clip, 9.f, 10u);
        CHECK(ctx, Near(poses[5].position.x, 2.f));
        CHECK(ctx, Near(poses[5].position.y, 4.f));
        CHECK(ctx, Near(poses[5].position.z, 6.f));
    }

    // Non-looping: t<0 → erster Keyframe
    {
        const auto poses = AnimationSampler::SampleForSkeleton(clip, -1.f, 10u);
        CHECK(ctx, Near(poses[5].position.x, 0.f));
    }
}

// =============================================================================
// Randfall: leerer Channel → Rest-Pose
// =============================================================================

static void TestEmptyChannelReturnsRestPose(test::TestContext& ctx)
{
    AnimationClip clip;
    clip.duration = 1.f;
    clip.looping  = false;

    AnimationChannel ch;
    ch.targetIndex   = 0;
    ch.property      = AnimationTargetProperty::Translation;
    ch.interpolation = AnimationInterpolation::Linear;
    // times und vec3Values leer → Channel wird ignoriert
    clip.channels.push_back(ch);

    const auto poses = AnimationSampler::SampleForSkeleton(clip, 0.5f, 1u);
    CHECK(ctx, Near(poses[0].position.x, 0.f));
    CHECK(ctx, Near(poses[0].position.y, 0.f));
    CHECK(ctx, Near(poses[0].rotation.w, 1.f));
    CHECK(ctx, Near(poses[0].scale.x,    1.f));
}

// =============================================================================
// STEP-Interpolation
// =============================================================================

static void TestStepInterpolation(test::TestContext& ctx)
{
    AnimationChannel ch;
    ch.targetIndex   = 0;
    ch.property      = AnimationTargetProperty::Translation;
    ch.interpolation = AnimationInterpolation::Step;
    ch.times         = { 0.f, 1.f, 2.f };
    ch.vec3Values    = { {0.f,0.f,0.f}, {10.f,10.f,10.f}, {20.f,20.f,20.f} };

    // Kurz vor 1.0 → noch Keyframe 0
    {
        const math::Vec3 v = SampleVec3(ch.times, ch.vec3Values, ch.interpolation, 0.99f);
        CHECK(ctx, Near(v.x, 0.f));
    }
    // Genau bei 1.0 → Keyframe 1
    {
        const math::Vec3 v = SampleVec3(ch.times, ch.vec3Values, ch.interpolation, 1.0f);
        CHECK(ctx, Near(v.x, 10.f));
    }
    // Zwischen 1 und 2 → immer noch Keyframe 1 (kein Blend)
    {
        const math::Vec3 v = SampleVec3(ch.times, ch.vec3Values, ch.interpolation, 1.5f);
        CHECK(ctx, Near(v.x, 10.f));
    }
}

// =============================================================================
// CUBICSPLINE-Interpolation — Hermite-Spline Mittelwert-Check
// =============================================================================

static void TestCubicSplineInterpolation(test::TestContext& ctx)
{
    // Zwei Keyframes bei t=0 und t=2, Werte 0 und 8.
    // In/Out-Tangenten = 0 → Hermite degeneriert zu sanfter Kurve.
    // Am Mittelpunkt (t=1, alpha=0.5): h00(0.5)=0.5, h01(0.5)=0.5 → Wert ≈ 4
    AnimationChannel ch;
    ch.targetIndex   = 0;
    ch.property      = AnimationTargetProperty::Translation;
    ch.interpolation = AnimationInterpolation::CubicSpline;
    ch.times         = { 0.f, 2.f };
    // Layout: [inTangent_0, value_0, outTangent_0, inTangent_1, value_1, outTangent_1]
    ch.vec3Values = {
        {0.f,0.f,0.f},  // in-tangent  key 0
        {0.f,0.f,0.f},  // value       key 0
        {0.f,0.f,0.f},  // out-tangent key 0
        {0.f,0.f,0.f},  // in-tangent  key 1
        {8.f,0.f,0.f},  // value       key 1
        {0.f,0.f,0.f},  // out-tangent key 1
    };

    const math::Vec3 v = SampleVec3(ch.times, ch.vec3Values, ch.interpolation, 1.f);
    // Hermite mit Null-Tangenten: 2t³-3t²+1 + (-2t³+3t²) = 0.5 + 0.5 = 1.0 am Mittelpunkt?
    // h00(0.5) = 2(0.125)-3(0.25)+1 = 0.25-0.75+1 = 0.5
    // h01(0.5) = -2(0.125)+3(0.25) = -0.25+0.75 = 0.5
    // Ergebnis = 0*0.5 + 8*0.5 = 4.0
    CHECK(ctx, Near(v.x, 4.f, 1e-4f));
}

// =============================================================================
// Quaternion SLERP — normalisierte Ausgabe
// =============================================================================

static void TestQuatSlerpNormalized(test::TestContext& ctx)
{
    AnimationChannel ch;
    ch.targetIndex   = 0;
    ch.property      = AnimationTargetProperty::Rotation;
    ch.interpolation = AnimationInterpolation::Linear;
    ch.times         = { 0.f, 1.f };
    ch.quatValues    = {
        math::Quat::Identity(),
        math::Quat::FromAxisAngleDeg(math::Vec3::Up(), 90.f),
    };

    for (float t : { 0.f, 0.25f, 0.5f, 0.75f, 1.f })
    {
        const math::Quat q = SampleQuat(ch.times, ch.quatValues, ch.interpolation, t);
        CHECK(ctx, Near(q.LengthSq(), 1.f, 1e-5f));
    }
}

// =============================================================================
// FindKeyframeIndex — Binärsuche Randwerte
// =============================================================================

static void TestFindKeyframeIndex(test::TestContext& ctx)
{
    const std::vector<float> times = { 0.f, 1.f, 2.f, 3.f };

    CHECK_EQ(ctx, FindKeyframeIndex(times, -1.f), size_t{0});
    CHECK_EQ(ctx, FindKeyframeIndex(times,  0.f), size_t{0});
    CHECK_EQ(ctx, FindKeyframeIndex(times,  0.5f), size_t{0});
    CHECK_EQ(ctx, FindKeyframeIndex(times,  1.0f), size_t{1});
    CHECK_EQ(ctx, FindKeyframeIndex(times,  2.5f), size_t{2});
    CHECK_EQ(ctx, FindKeyframeIndex(times,  3.f),  size_t{2});  // letzter gültiger Span
    CHECK_EQ(ctx, FindKeyframeIndex(times, 99.f),  size_t{2});
}

// =============================================================================
// NormalizeTime
// =============================================================================

static void TestNormalizeTime(test::TestContext& ctx)
{
    CHECK(ctx, Near(NormalizeTime( 0.5f, 2.f, false), 0.5f));
    CHECK(ctx, Near(NormalizeTime(-1.f,  2.f, false), 0.f));
    CHECK(ctx, Near(NormalizeTime( 3.f,  2.f, false), 2.f));

    CHECK(ctx, Near(NormalizeTime(2.5f, 2.f, true),  0.5f));
    CHECK(ctx, Near(NormalizeTime(4.f,  2.f, true),  0.f));
    CHECK(ctx, Near(NormalizeTime(0.f,  2.f, true),  0.f));
}

// =============================================================================
// SkeletonPoseEvaluator — Skelett-Hierarchie + Inverse-Bind-Matrizen
// =============================================================================

static void TestSkeletonPoseEvaluatorBuildsBonePalette(test::TestContext& ctx)
{
    SkeletonAsset skeleton;
    skeleton.bones.push_back({ "root",  -1, math::Mat4::Translation({ -1.f, 0.f, 0.f }) });
    skeleton.bones.push_back({ "child",  0, math::Mat4::Translation({  0.f,-2.f, 0.f }) });

    std::vector<BonePose> localPoses;
    localPoses.push_back({ {1.f,0.f,0.f}, math::Quat::Identity(), {1.f,1.f,1.f} });
    localPoses.push_back({ {0.f,2.f,0.f}, math::Quat::Identity(), {1.f,1.f,1.f} });

    const auto palette = SkeletonPoseEvaluator::Evaluate(skeleton, localPoses);

    CHECK_EQ(ctx, palette.size(), skeleton.bones.size());

    // Root: T=(1,0,0) * inverseBindMatrix T=(-1,0,0) → Ursprung bei (0,0,0)
    const math::Vec3 rootOrigin = palette[0].TransformPoint({0.f,0.f,0.f});
    CHECK(ctx, Near(rootOrigin.x, 0.f));
    CHECK(ctx, Near(rootOrigin.y, 0.f));
    CHECK(ctx, Near(rootOrigin.z, 0.f));

    // Child: World = Root_World * Child_Local = T(1,0,0) * T(0,2,0) = T(1,2,0)
    // Skin = World * inverseBindMatrix T(0,-2,0) = T(1,0,0)
    const math::Vec3 childOrigin = palette[1].TransformPoint({0.f,0.f,0.f});
    CHECK(ctx, Near(childOrigin.x, 1.f));
    CHECK(ctx, Near(childOrigin.y, 0.f));
    CHECK(ctx, Near(childOrigin.z, 0.f));
}

// =============================================================================
// AnimationPlayerComponent — Steuerung
// =============================================================================

static void TestAnimationPlayerComponentControl(test::TestContext& ctx)
{
    AnimationPlayerComponent player;
    player.currentTime = 1.f;

    player.Play();
    CHECK(ctx, player.playing);

    player.Pause();
    CHECK(ctx, !player.playing);
    CHECK(ctx, Near(player.currentTime, 1.f));  // Zeit bleibt stehen

    player.Stop();
    CHECK(ctx, !player.playing);
    CHECK(ctx, Near(player.currentTime, 0.f));  // auf Null zurück
    CHECK(ctx, player.bindingsDirty);            // Binding-Cache invalidiert

    player.Play();
    player.Rewind();
    CHECK(ctx, Near(player.currentTime, 0.f));
    CHECK(ctx, player.playing);
}

// =============================================================================
// AnimationSystem — non-looping rueckwaerts stoppt bei Zeit 0
// =============================================================================

static void TestAnimationSystemStopsReverseNonLoopingAtStart(test::TestContext& ctx)
{
    assets::AssetRegistry assets;
    auto clip = std::make_unique<AnimationClip>();
    clip->name = "ReverseNonLooping";
    clip->duration = 2.f;
    clip->looping = false;
    const AnimationHandle clipHandle =
        assets.animations.Add(std::move(clip));

    ecs::ComponentMetaRegistry componentRegistry;
    RegisterCoreComponents(componentRegistry);
    RegisterAnimationComponents(componentRegistry);
    ecs::World world(componentRegistry);

    const EntityID entity = world.CreateEntity();
    AnimationPlayerComponent player;
    player.SetClip(clipHandle);
    player.currentTime = 0.25f;
    player.speed = -1.f;
    player.looping = false;
    player.Play();
    world.Add<AnimationPlayerComponent>(entity, player);

    engine::addons::animation::AnimationSystem system(assets);
    system.Update(world, 0.5f);

    const auto* updated = world.Get<AnimationPlayerComponent>(entity);
    CHECK(ctx, updated != nullptr);
    CHECK(ctx, updated && Near(updated->currentTime, 0.f));
    CHECK(ctx, updated && !updated->playing);
}

// =============================================================================
// Test-Suite
// =============================================================================

namespace {

void TestAnimFileRoundTrip(test::TestContext& ctx)
{
    AnimationClip clip;
    clip.name = "Wave Hand";
    clip.duration = 1.5f;
    clip.looping = true;
    AnimationChannel translation;
    translation.targetName = "Arm left";
    translation.targetIndex = 3;
    translation.property = AnimationTargetProperty::Translation;
    translation.interpolation = AnimationInterpolation::Step;
    SetKey(translation, 0.25f, math::Vec3{1.f, 2.f, 3.f}, {});
    AnimationChannel rotation;
    rotation.targetName = "Arm left";
    rotation.targetIndex = 3;
    rotation.property = AnimationTargetProperty::Rotation;
    SetKey(rotation, 0.f, {}, math::Quat{0.f, 0.7071068f, 0.f, 0.7071068f});
    clip.channels = {translation, rotation};

    AnimationClip second;
    second.name = "Idle";
    second.duration = 3.f;
    std::vector<AnimationClip> clips;
    CHECK(ctx, ParseAnimationSet(SerializeAnimationSet({clip, second}), clips));
    CHECK_EQ(ctx, clips.size(), size_t{2});
    CHECK(ctx, clips[1].name == "Idle");
    CHECK(ctx, Near(clips[1].duration, 3.f));
    const AnimationClip& parsed = clips[0];
    CHECK(ctx, parsed.name == "Wave Hand");
    CHECK(ctx, parsed.looping);
    CHECK(ctx, Near(parsed.duration, 1.5f));
    CHECK_EQ(ctx, parsed.channels.size(), size_t{2});
    CHECK(ctx, parsed.channels[0].targetName == "Arm left");
    CHECK_EQ(ctx, parsed.channels[0].targetIndex, 3);
    CHECK(ctx, parsed.channels[0].interpolation == AnimationInterpolation::Step);
    CHECK(ctx, NearVec3(parsed.channels[0].vec3Values[0], math::Vec3{1.f, 2.f, 3.f}));
    CHECK(ctx, Near(parsed.channels[1].quatValues[0].y, 0.7071068f));

    // Files from before clips existed are one animation with one clip.
    std::vector<AnimationClip> legacy;
    CHECK(ctx, ParseAnimationSet("kromanim 1\nname Old\nduration 2\nlooping 0\n", legacy));
    CHECK_EQ(ctx, legacy.size(), size_t{1});
    CHECK(ctx, legacy[0].name == "Old");

    std::vector<AnimationClip> untouched;
    CHECK(ctx, !ParseAnimationSet("something else\n", untouched));
    CHECK(ctx, !ParseAnimationSet("kromanim 2\nduration 1\n", untouched));
    CHECK(ctx, !ParseAnimationSet("kromanim 1\nk 0 1 2 3\n", untouched));
}

void TestAnimKeyEditing(test::TestContext& ctx)
{
    AnimationClip clip;
    AnimationChannel channel;
    channel.targetName = "a";
    SetKey(channel, 1.f, math::Vec3{1.f, 0.f, 0.f}, {});
    SetKey(channel, 0.f, math::Vec3{0.f, 0.f, 0.f}, {});
    SetKey(channel, 2.f, math::Vec3{2.f, 0.f, 0.f}, {});
    CHECK(ctx, Near(channel.times[1], 1.f));
    SetKey(channel, 1.f, math::Vec3{5.f, 0.f, 0.f}, {});
    CHECK_EQ(ctx, channel.times.size(), size_t{3});
    CHECK(ctx, Near(channel.vec3Values[1].x, 5.f));
    clip.channels.push_back(channel);

    // Moving onto another key replaces it; the reference follows the key.
    auto moved = MoveKeys(clip, {{0, 1}}, 1.f);
    CHECK_EQ(ctx, clip.channels[0].times.size(), size_t{2});
    CHECK_EQ(ctx, moved[0].key, 1);
    CHECK(ctx, Near(clip.channels[0].vec3Values[1].x, 5.f));

    // Keys never move below zero.
    moved = MoveKeys(clip, {{0, 1}}, -10.f);
    CHECK_EQ(ctx, clip.channels[0].times.size(), size_t{1});
    CHECK(ctx, Near(clip.channels[0].times[0], 0.f));

    DeleteKeys(clip, {{0, 0}});
    CHECK(ctx, clip.channels.empty());

    // Behind the last key the value holds instead of extrapolating.
    AnimationChannel held;
    SetKey(held, 0.f, math::Vec3{0.f, 0.f, 0.f}, {});
    SetKey(held, 1.f, math::Vec3{2.f, 0.f, 0.f}, {});
    CHECK(ctx, NearVec3(SampleVec3(held.times, held.vec3Values, held.interpolation, 3.f), math::Vec3{2.f, 0.f, 0.f}));
}

void TestModelClipSettingsRoundTrip(test::TestContext& ctx)
{
    ModelClipSettings settings;
    settings.fps = 30.f;
    SyncModelClipSettings(settings, {"Walk", "Gallop"});
    CHECK_EQ(ctx, settings.clips.size(), size_t{2});
    CHECK(ctx, settings.clips[0].name == "Walk" && settings.clips[0].source == "Walk");
    CHECK(ctx, !settings.clips[0].hasRange && settings.clips[0].looping);
    CHECK(ctx, settings.clips[0].id.size() == 8u && settings.clips[0].id != settings.clips[1].id);

    // A second sync adds nothing; a cut clip of the same source does not count as the whole clip.
    CHECK(ctx, !SyncModelClipSettings(settings, {"Walk", "Gallop"}));
    ModelClipEntry cut;
    cut.id = settings.NewId();
    cut.name = settings.UniqueName("Walk");
    cut.source = "Walk";
    cut.hasRange = true;
    cut.startFrame = 3;
    cut.endFrame = 12;
    cut.looping = false;
    cut.rootMotion = true;
    settings.clips.push_back(cut);
    CHECK(ctx, cut.name == "Walk 2");

    ModelClipSettings parsed;
    std::string error;
    CHECK(ctx, ParseModelClipSettings(SerializeModelClipSettings(settings), parsed, &error));
    CHECK(ctx, parsed == settings);
    CHECK(ctx, parsed.Find(cut.id) && parsed.Find(cut.id)->rootMotion);

    // Missing fields get defaults; entries without id/source are dropped; duplicate ids are renewed.
    CHECK(ctx, ParseModelClipSettings(
        R"({"kromclips":1,"clips":[{"id":"a","source":"Run"},{"source":"NoId"},{"id":"a","source":"Jump","start":5,"end":2}]})",
        parsed));
    CHECK(ctx, Near(parsed.fps, kDefaultModelClipFps));
    CHECK_EQ(ctx, parsed.clips.size(), size_t{2});
    CHECK(ctx, parsed.clips[0].name == "Run" && parsed.clips[0].looping && !parsed.clips[0].rootMotion);
    CHECK(ctx, parsed.clips[1].id != "a" && parsed.clips[1].hasRange && parsed.clips[1].endFrame == 5);

    CHECK(ctx, !ParseModelClipSettings(R"({"kromclips":2,"clips":[]})", parsed, &error));
    CHECK(ctx, !ParseModelClipSettings("not json", parsed));

    std::string model, id;
    CHECK(ctx, SplitModelClipAssetPath(ModelClipAssetPath("Models/Horse.glb", "3f9a01c2"), model, id));
    CHECK(ctx, model == "Models/Horse.glb" && id == "3f9a01c2");
    CHECK(ctx, !SplitModelClipAssetPath("Models/Horse.glb#animation/0", model, id));
    CHECK(ctx, IsModelFilePath("Models/Horse.GLB") && IsModelFilePath("a.gltf") && !IsModelFilePath("a.anim"));
}

void TestSliceAnimationClip(test::TestContext& ctx)
{
    AnimationClip source = MakeLinearClip(); // bone5: 0..2 s, translation (0,0,0)->(2,4,6)
    source.name = "Source";
    const AnimationClip slice = SliceAnimationClip(source, 0.5f, 1.5f);
    CHECK(ctx, Near(slice.duration, 1.f));
    CHECK_EQ(ctx, slice.channels.size(), source.channels.size());
    const AnimationChannel& translation = slice.channels[0];
    CHECK(ctx, Near(translation.times.front(), 0.f) && Near(translation.times.back(), 1.f));
    CHECK(ctx, NearVec3(translation.vec3Values.front(), math::Vec3{0.5f, 1.f, 1.5f}));
    CHECK(ctx, NearVec3(translation.vec3Values.back(), math::Vec3{1.5f, 3.f, 4.5f}));
    // A key inside the range stays at its shifted time (bone9 has one key at 1 s).
    const AnimationChannel& single = slice.channels[3];
    CHECK(ctx, std::any_of(single.times.begin(), single.times.end(), [](float t) { return Near(t, 0.5f); }));

    // Cubic spline becomes linear through the sampled values.
    AnimationClip cubic;
    cubic.duration = 1.f;
    AnimationChannel channel;
    channel.targetName = "a";
    channel.property = AnimationTargetProperty::Translation;
    channel.interpolation = AnimationInterpolation::CubicSpline;
    channel.times = {0.f, 1.f};
    channel.vec3Values = {{}, {0.f, 0.f, 0.f}, {}, {}, {4.f, 0.f, 0.f}, {}};
    cubic.channels.push_back(channel);
    const AnimationClip cubicSlice = SliceAnimationClip(cubic, 0.f, 0.5f);
    CHECK(ctx, cubicSlice.channels[0].interpolation == AnimationInterpolation::Linear);
    CHECK_EQ(ctx, cubicSlice.channels[0].vec3Values.size(), cubicSlice.channels[0].times.size());

    // Building an entry: range in frames, settings override name/loop/root motion.
    ModelClipEntry entry;
    entry.name = "Middle";
    entry.source = "Source";
    entry.hasRange = true;
    entry.startFrame = 12;
    entry.endFrame = 36;
    entry.looping = false;
    entry.rootMotion = true;
    std::string error;
    const auto built = BuildModelClip(entry, 24.f, {&source}, &error);
    CHECK(ctx, built.has_value());
    CHECK(ctx, built && built->name == "Middle" && !built->looping && built->rootMotion && Near(built->duration, 1.f));
    entry.source = "Gone";
    CHECK(ctx, !BuildModelClip(entry, 24.f, {&source}, &error).has_value());
    CHECK(ctx, error.find("Gone") != std::string::npos);
}

void TestSkeletonMismatch(test::TestContext& ctx)
{
    assets::SkeletonAsset skeleton;
    skeleton.bones.resize(10u);
    skeleton.bones[5].name = "bone5";
    skeleton.bones[9].name = "bone9";
    const AnimationClip clip = MakeLinearClip();
    CHECK(ctx, FindSkeletonMismatch(clip, skeleton).empty());
    skeleton.bones[9].name = "other";
    CHECK(ctx, FindSkeletonMismatch(clip, skeleton) == "bone9");
    skeleton.bones.resize(6u);
    CHECK(ctx, FindSkeletonMismatch(clip, skeleton) == "bone9");
}

} // namespace

static void TestClipIsReusableOnOtherObjects(test::TestContext& ctx)
{
    assets::AssetRegistry assets;
    auto clip = std::make_unique<AnimationClip>();
    clip->duration = 2.f;
    AnimationChannel channel;
    channel.targetName = "FirstCube";
    channel.targetIndex = 0;
    channel.property = AnimationTargetProperty::Translation;
    SetKey(channel, 0.f, math::Vec3{0.f, 0.f, 0.f}, {});
    SetKey(channel, 2.f, math::Vec3{2.f, 0.f, 0.f}, {});
    clip->channels.push_back(channel);
    const AnimationHandle clipHandle = assets.animations.Add(std::move(clip));

    ecs::ComponentMetaRegistry componentRegistry;
    RegisterCoreComponents(componentRegistry);
    RegisterAnimationComponents(componentRegistry);
    ecs::World world(componentRegistry);

    const EntityID first = world.CreateEntity();
    world.Add<NameComponent>(first, NameComponent{"FirstCube"});
    world.Add<TransformComponent>(first);
    const EntityID second = world.CreateEntity();
    world.Add<NameComponent>(second, NameComponent{"SecondCube"});
    world.Add<TransformComponent>(second);
    AnimationPlayerComponent player;
    player.SetClip(clipHandle);
    player.Play();
    world.Add<AnimationPlayerComponent>(second, player);

    engine::addons::animation::AnimationSystem system(assets);
    system.Update(world, 1.f);

    CHECK(ctx, Near(world.Get<TransformComponent>(second)->localPosition.x, 1.f));
    CHECK(ctx, Near(world.Get<TransformComponent>(first)->localPosition.x, 0.f));
}

// =============================================================================
// Animation controller (*.animctrl)
// =============================================================================

namespace {

namespace ac = engine::addons::animation;

// Three states (Idle, Walk, Run), parameters speed (float), attack (trigger),
// grounded (bool), phase (int). Clips are fake paths resolved by the tests.
ac::AnimationControllerAsset MakeControllerAsset()
{
    ac::AnimationControllerAsset a;
    a.parameters = {{"p1", "speed", ac::AnimationParameterType::Float, 0.f},
                    {"p2", "attack", ac::AnimationParameterType::Trigger, 0.f},
                    {"p3", "grounded", ac::AnimationParameterType::Bool, 1.f},
                    {"p4", "phase", ac::AnimationParameterType::Int, 0.f}};
    a.states = {{"s1", "Idle", ac::AnimationStateKind::Clip, "idle", 1.f, 0.f, 0.f},
                {"s2", "Walk", ac::AnimationStateKind::Clip, "walk", 1.f, 10.f, 0.f},
                {"s3", "Attack", ac::AnimationStateKind::Clip, "attack", 1.f, 20.f, 0.f}};
    a.entryState = "s1";
    ac::AnimationTransitionDesc toWalk;
    toWalk.id = "t1";
    toWalk.from = "s1";
    toWalk.to = "s2";
    toWalk.priority = 0;
    toWalk.conditions = {{"p1", ac::AnimationConditionOp::Greater, 1.f}};
    toWalk.blendDuration = 0.5f;
    ac::AnimationTransitionDesc attack;
    attack.id = "t2";
    attack.from = ac::kAnyStateId;
    attack.to = "s3";
    attack.priority = 10;
    attack.conditions = {{"p2", ac::AnimationConditionOp::Triggered, 0.f}};
    attack.blendDuration = 0.f;
    ac::AnimationTransitionDesc back;
    back.id = "t3";
    back.from = "s3";
    back.to = "s1";
    back.hasExitTime = true;
    back.exitTime = 1.f;
    back.blendDuration = 0.2f;
    a.transitions = {toWalk, attack, back};
    return a;
}

ac::AnimationClipResolver FakeClips()
{
    return [](const std::string& path) {
        ac::AnimationClipInfo info;
        info.found = path == "idle" || path == "walk" || path == "attack";
        info.duration = path == "attack" ? 0.5f : 1.f;
        info.looping = path != "attack";
        return info;
    };
}

} // namespace

static void TestControllerFileRoundTrip(test::TestContext& ctx)
{
    const ac::AnimationControllerAsset asset = MakeControllerAsset();
    ac::AnimationControllerAsset parsed;
    std::string error;
    CHECK(ctx, ac::ParseAnimationController(ac::SerializeAnimationController(asset), parsed, &error));
    CHECK(ctx, parsed == asset);

    // Renaming a state keeps every reference: they use ids.
    ac::AnimationControllerAsset renamed = asset;
    renamed.states[1].name = "Trot";
    const auto compiled = ac::CompileAnimationController(renamed, FakeClips());
    CHECK(ctx, compiled.FindState("Trot") == 1 && compiled.transitions[0].to == 1);

    CHECK(ctx, !ac::ParseAnimationController(R"({"kromanimctrl":2})", parsed, &error));
    CHECK(ctx, ac::ParseAnimationController(R"({"kromanimctrl":1,"states":[{"id":"a","clip":"x"},{"id":"a"}],)"
                                            R"("transitions":[{"id":"t","from":"any","to":"a","conditions":[{"param":"p","op":"bogus"}]}]})",
                                            parsed));
    CHECK_EQ(ctx, parsed.states.size(), size_t{1});
    CHECK(ctx, parsed.transitions.size() == 1u && parsed.transitions[0].conditions.empty());
    const std::string id = asset.NewId('s');
    CHECK(ctx, id.size() == 5u && id[0] == 's' && !asset.FindState(id));
}

static void TestControllerCompileProblems(test::TestContext& ctx)
{
    ac::AnimationControllerAsset asset = MakeControllerAsset();
    asset.parameters.erase(asset.parameters.begin());   // "speed" deleted
    asset.entryState = "gone";
    asset.states[1].clipAssetPath = "missing";
    const auto c = ac::CompileAnimationController(asset, FakeClips());
    CHECK(ctx, c.entry == 0);
    CHECK(ctx, !c.transitions[0].valid);
    const auto has = [&](const char* text) {
        return std::any_of(c.problems.begin(), c.problems.end(), [&](const std::string& p) { return p.find(text) != std::string::npos; });
    };
    CHECK(ctx, has("deleted parameter"));
    CHECK(ctx, has("entry state is missing"));
    CHECK(ctx, has("clip 'missing' is missing"));

    ac::AnimationControllerInstance in;
    ac::ResetAnimationController(c, in);
    CHECK(ctx, !ac::StepAnimationController(c, in, 0.1f)); // the invalid transition never fires
}

static void TestControllerTransitions(test::TestContext& ctx)
{
    const auto c = ac::CompileAnimationController(MakeControllerAsset(), FakeClips());
    const int32_t speed = c.FindParameter("speed"), attack = c.FindParameter("attack");
    ac::AnimationControllerInstance in;
    ac::ResetAnimationController(c, in);
    CHECK(ctx, in.current == 0 && in.parameters.size() == 4u && in.parameters[2] == 1.f);

    // Condition false: stays; true: cross-fades with weights over the blend time.
    CHECK(ctx, !ac::StepAnimationController(c, in, 0.1f));
    in.parameters[static_cast<size_t>(speed)] = 2.f;
    CHECK(ctx, ac::StepAnimationController(c, in, 0.1f) && in.current == 1 && in.previous == 0);
    CHECK(ctx, Near(in.CurrentWeight(), 0.f));
    ac::StepAnimationController(c, in, 0.25f);
    CHECK(ctx, Near(in.CurrentWeight(), 0.5f));
    ac::StepAnimationController(c, in, 0.25f);
    CHECK(ctx, !in.Blending() && Near(in.CurrentWeight(), 1.f));

    // Any State with the higher priority wins; blend 0 switches at once; the trigger is consumed.
    in.parameters[static_cast<size_t>(attack)] = 1.f;
    CHECK(ctx, ac::StepAnimationController(c, in, 0.01f) && in.current == 2 && !in.Blending());
    CHECK(ctx, in.parameters[static_cast<size_t>(attack)] == 0.f);
    // Any State never re-enters the current state.
    in.parameters[static_cast<size_t>(attack)] = 1.f;
    CHECK(ctx, !ac::StepAnimationController(c, in, 0.01f) && in.current == 2);
    CHECK(ctx, in.parameters[static_cast<size_t>(attack)] == 1.f); // unconsumed trigger stays set
    in.parameters[static_cast<size_t>(attack)] = 0.f;

    // Exit time is reached even by one large step (0.5 s clip, step of 3 s).
    CHECK(ctx, ac::StepAnimationController(c, in, 3.f) && in.current == 0);
}

static void TestControllerPriorityAndInterrupt(test::TestContext& ctx)
{
    ac::AnimationControllerAsset asset = MakeControllerAsset();
    // Two transitions Idle -> X with the same condition: the higher priority wins,
    // equal priority goes to the earlier one.
    ac::AnimationTransitionDesc low = asset.transitions[0];
    low.id = "t4";
    low.to = "s3";
    low.priority = -1;
    asset.transitions.insert(asset.transitions.begin(), low);
    auto c = ac::CompileAnimationController(asset, FakeClips());
    ac::AnimationControllerInstance in;
    ac::ResetAnimationController(c, in);
    in.parameters[0] = 2.f;
    ac::StepAnimationController(c, in, 0.f);
    CHECK(ctx, in.current == 1);

    asset.transitions[0].priority = 0;   // same priority, earlier in the file
    c = ac::CompileAnimationController(asset, FakeClips());
    ac::ResetAnimationController(c, in);
    in.parameters[0] = 2.f;
    ac::StepAnimationController(c, in, 0.f);
    CHECK(ctx, in.current == 2);

    // Not interruptible: the trigger waits until the fade is over.
    asset = MakeControllerAsset();
    asset.transitions[0].interruptible = false;
    c = ac::CompileAnimationController(asset, FakeClips());
    ac::ResetAnimationController(c, in);
    in.parameters[0] = 2.f;
    ac::StepAnimationController(c, in, 0.f);
    in.parameters[1] = 1.f;
    CHECK(ctx, !ac::StepAnimationController(c, in, 0.1f) && in.current == 1);
    CHECK(ctx, ac::StepAnimationController(c, in, 0.5f) && in.current == 2);

    // Interruptible fade interrupted by a fading transition: fades out from a frozen pose.
    asset = MakeControllerAsset();
    asset.transitions[1].blendDuration = 0.3f;
    c = ac::CompileAnimationController(asset, FakeClips());
    ac::ResetAnimationController(c, in);
    in.parameters[0] = 2.f;
    ac::StepAnimationController(c, in, 0.f);
    in.parameters[1] = 1.f;
    CHECK(ctx, ac::StepAnimationController(c, in, 0.1f) && in.current == 2 && in.previousFrozen && in.previous < 0);

    // A direct cross-fade (scripts) works from any state.
    ac::CrossFadeToState(c, in, 0, 0.f);
    CHECK(ctx, in.current == 0 && !in.Blending());
}

static void TestControllerDrivesObjects(test::TestContext& ctx)
{
    namespace fs = std::filesystem;
    const fs::path root = fs::temp_directory_path() / "krom_controller_system";
    fs::remove_all(root);
    fs::create_directories(root);

    assets::AssetRegistry assets;
    const auto addClip = [&](const char* path, float x) {
        auto clip = std::make_unique<AnimationClip>();
        clip->duration = 1.f;
        clip->looping = true;
        AnimationChannel channel;
        channel.targetName = "Box";
        channel.targetIndex = 0;
        channel.property = AnimationTargetProperty::Translation;
        SetKey(channel, 0.f, math::Vec3{x, 0.f, 0.f}, {});
        SetKey(channel, 1.f, math::Vec3{x, 0.f, 0.f}, {});
        clip->channels.push_back(channel);
        (void)assets.GetOrAddAnimation(path, std::move(clip));
    };
    addClip("Clips/a.anim#A", 0.f);
    addClip("Clips/b.anim#B", 10.f);

    ac::AnimationControllerAsset asset;
    asset.parameters = {{"p1", "go", ac::AnimationParameterType::Bool, 0.f}};
    asset.states = {{"s1", "A", ac::AnimationStateKind::Clip, "Clips/a.anim#A"},
                    {"s2", "B", ac::AnimationStateKind::Clip, "Clips/b.anim#B"}};
    asset.entryState = "s1";
    ac::AnimationTransitionDesc t;
    t.id = "t1";
    t.from = "s1";
    t.to = "s2";
    t.conditions = {{"p1", ac::AnimationConditionOp::IsTrue, 0.f}};
    t.blendDuration = 1.f;
    asset.transitions = {t};
    std::ofstream(root / "Box.animctrl") << ac::SerializeAnimationController(asset);

    ecs::ComponentMetaRegistry componentRegistry;
    RegisterCoreComponents(componentRegistry);
    RegisterAnimationComponents(componentRegistry);
    ecs::World world(componentRegistry);
    const EntityID box = world.CreateEntity();
    world.Add<NameComponent>(box, NameComponent{"Mover"}); // another name: the clip is for one object
    world.Add<TransformComponent>(box);
    AnimationPlayerComponent player;
    player.controllerAssetPath = "Box.animctrl";
    player.Play();
    world.Add<AnimationPlayerComponent>(box, player);

    ac::AnimationSystem system(assets);
    system.SetAssetSources(root, nullptr);
    system.Update(world, 0.1f);
    CHECK(ctx, Near(world.Get<TransformComponent>(box)->localPosition.x, 0.f));

    auto* live = world.Get<AnimationPlayerComponent>(box);
    const auto& rt = live->controllerState;
    CHECK(ctx, rt.bound && rt.controller && rt.controller->FindParameter("go") == 0);
    const void* outputData = rt.output.data();
    const size_t outputCapacity = rt.output.capacity();
    CHECK(ctx, live->SetParameter(0, 1.f) && !live->SetParameter(5, 1.f));
    system.Update(world, 0.f);   // transition fires: weight 0
    system.Update(world, 0.5f);  // half way through the 1 s fade
    CHECK(ctx, Near(world.Get<TransformComponent>(box)->localPosition.x, 5.f, 1e-3f));
    system.Update(world, 0.6f);
    CHECK(ctx, Near(world.Get<TransformComponent>(box)->localPosition.x, 10.f, 1e-3f));
    CHECK(ctx, rt.output.data() == outputData && rt.output.capacity() == outputCapacity); // no reallocation

    // Without a controller the single clip still plays.
    AnimationPlayerComponent single;
    single.SetClip(assets.FindAnimationByPath("Clips/b.anim#B"));
    single.Play();
    const EntityID other = world.CreateEntity();
    world.Add<NameComponent>(other, NameComponent{"Box"});
    world.Add<TransformComponent>(other);
    world.Add<AnimationPlayerComponent>(other, single);
    system.Update(world, 0.1f);
    CHECK(ctx, Near(world.Get<TransformComponent>(other)->localPosition.x, 10.f));
    fs::remove_all(root);
}

namespace {

// Smallest script context: the entity, the world and the animation service.
class AnimationTestScriptContext final : public engine::script::IScriptContext
{
public:
    AnimationTestScriptContext(ecs::World& world, EntityID entity, engine::script::IAnimationService* animation)
        : m_world(world), m_entity(entity), m_animation(animation) {}
    float DeltaTime() const noexcept override { return 0.f; }
    EntityID GetEntity() const noexcept override { return m_entity; }
    EntityID InstantiatePrefab(std::string_view, const math::Vec3&, const math::Quat&, const math::Vec3&) override { return NULL_ENTITY; }
    ecs::World* GetWorld() noexcept override { return &m_world; }
    engine::script::IAnimationService* GetAnimation() noexcept override { return m_animation; }
private:
    ecs::World& m_world;
    EntityID m_entity;
    engine::script::IAnimationService* m_animation;
};

} // namespace

static void TestControllerScriptApi(test::TestContext& ctx)
{
    namespace fs = std::filesystem;
    const fs::path root = fs::temp_directory_path() / "krom_controller_script";
    fs::remove_all(root);
    fs::create_directories(root);

    assets::AssetRegistry assets;
    for (const char* path : {"Clips/a.anim#A", "Clips/b.anim#B"})
    {
        auto clip = std::make_unique<AnimationClip>();
        clip->duration = 1.f;
        clip->looping = true;
        (void)assets.GetOrAddAnimation(path, std::move(clip));
    }
    ac::AnimationControllerAsset asset;
    asset.parameters = {{"p1", "go", ac::AnimationParameterType::Bool, 0.f},
                        {"p2", "speed", ac::AnimationParameterType::Float, 0.f},
                        {"p3", "hit", ac::AnimationParameterType::Trigger, 0.f}};
    asset.states = {{"s1", "A", ac::AnimationStateKind::Clip, "Clips/a.anim#A"},
                    {"s2", "B", ac::AnimationStateKind::Clip, "Clips/b.anim#B"}};
    asset.entryState = "s1";
    ac::AnimationTransitionDesc t;
    t.id = "t1";
    t.from = "s1";
    t.to = "s2";
    t.conditions = {{"p1", ac::AnimationConditionOp::IsTrue, 0.f}};
    t.blendDuration = 0.5f;
    asset.transitions = {t};
    std::ofstream(root / "Figure.animctrl") << ac::SerializeAnimationController(asset);

    ecs::ComponentMetaRegistry componentRegistry;
    RegisterCoreComponents(componentRegistry);
    RegisterAnimationComponents(componentRegistry);
    ecs::World world(componentRegistry);
    const EntityID figure = world.CreateEntity();
    world.Add<TransformComponent>(figure);
    AnimationPlayerComponent player;
    player.controllerAssetPath = "Figure.animctrl";
    player.Play();
    world.Add<AnimationPlayerComponent>(figure, player);
    const EntityID plain = world.CreateEntity();

    ac::AnimationSystem system(assets);
    system.SetAssetSources(root, nullptr);
    ac::AnimationScriptService service(system, world);
    AnimationTestScriptContext script(world, figure, &service);

    // Usable before the first animation update (OnStart).
    CHECK(ctx, script.FindAnimationParameter("speed") == 1);
    CHECK(ctx, script.GetAnimationState() == "A");
    CHECK(ctx, script.SetAnimationFloat("speed", 3.5f) && Near(script.GetAnimationFloat("speed"), 3.5f));
    CHECK(ctx, script.SetAnimationParameter(1, 2.f) && Near(script.GetAnimationFloat("speed"), 2.f));
    // Wrong kind or unknown name: refused, value unchanged.
    CHECK(ctx, !script.SetAnimationFloat("go", 1.f) && !script.GetAnimationBool("go"));
    CHECK(ctx, !script.SetAnimationTrigger("missing"));
    CHECK(ctx, script.SetAnimationTrigger("hit") && script.GetAnimationBool("hit"));
    CHECK(ctx, script.ResetAnimationTrigger("hit") && !script.GetAnimationBool("hit"));

    CHECK(ctx, script.SetAnimationBool("go", true));
    system.Update(world, 0.f);
    CHECK(ctx, script.IsAnimationState("B") && script.IsAnimationInTransition());
    system.Update(world, 0.6f);
    CHECK(ctx, !script.IsAnimationInTransition() && Near(script.GetAnimationNormalizedTime(), 0.6f));

    CHECK(ctx, script.CrossFadeAnimation("A", 0.f) && script.GetAnimationState() == "A" && !script.IsAnimationInTransition());
    CHECK(ctx, !script.CrossFadeAnimation("Nope"));

    // Another entity without animator, and a context without service.
    CHECK(ctx, !script.SetAnimationBool("go", true, plain) && script.GetAnimationState(plain).empty());
    AnimationTestScriptContext none(world, figure, nullptr);
    CHECK(ctx, !none.SetAnimationBool("go", true) && none.FindAnimationParameter("go") == -1);
    fs::remove_all(root);
}

int RunAnimationTests()
{
    test::TestSuite suite("Animation");

    suite.Add("Clip is reusable on other objects", TestClipIsReusableOnOtherObjects);

    suite.Add("Anim file round trip", TestAnimFileRoundTrip);
    suite.Add("Anim key editing", TestAnimKeyEditing);
    suite.Add("Model clip settings round trip", TestModelClipSettingsRoundTrip);
    suite.Add("Slice animation clip", TestSliceAnimationClip);
    suite.Add("Clip skeleton mismatch", TestSkeletonMismatch);
    suite.Add("Controller file round trip", TestControllerFileRoundTrip);
    suite.Add("Controller compile problems", TestControllerCompileProblems);
    suite.Add("Controller transitions, triggers, blend", TestControllerTransitions);
    suite.Add("Controller priority and interrupt", TestControllerPriorityAndInterrupt);
    suite.Add("Controller drives objects", TestControllerDrivesObjects);
    suite.Add("Controller script API", TestControllerScriptApi);

    suite.Add("Linear interpolation",           TestLinearInterpolation);
    suite.Add("Looping and clamping",           TestLoopingAndClamping);
    suite.Add("Empty channel returns rest pose", TestEmptyChannelReturnsRestPose);
    suite.Add("Step interpolation",             TestStepInterpolation);
    suite.Add("CubicSpline interpolation",      TestCubicSplineInterpolation);
    suite.Add("Quat slerp normalized",          TestQuatSlerpNormalized);
    suite.Add("FindKeyframeIndex edges",        TestFindKeyframeIndex);
    suite.Add("NormalizeTime",                  TestNormalizeTime);
    suite.Add("SkeletonPoseEvaluator builds bone palette",
              TestSkeletonPoseEvaluatorBuildsBonePalette);
    suite.Add("AnimationPlayerComponent control",
              TestAnimationPlayerComponentControl);
    suite.Add("AnimationSystem stops reverse non-looping at start",
              TestAnimationSystemStopsReverseNonLoopingAtStart);

    return suite.Run();
}
