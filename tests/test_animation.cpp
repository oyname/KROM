// =============================================================================
// KROM Engine - tests/test_animation.cpp
// Tests für AnimationEvaluator, AnimationSampler und SkeletonPoseEvaluator.
// =============================================================================
#include "TestFramework.hpp"

#include "addons/animation/AnimationComponents.hpp"
#include "addons/animation/AnimationSystem.hpp"
#include "animation/AnimationEvaluator.hpp"
#include "animation/AnimationSampler.hpp"
#include "animation/SkeletonPoseEvaluator.hpp"
#include "assets/AssetRegistry.hpp"
#include "assets/AnimationClip.hpp"
#include "ecs/Components.hpp"
#include "ecs/World.hpp"

#include <cmath>
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

int RunAnimationTests()
{
    test::TestSuite suite("Animation");

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
