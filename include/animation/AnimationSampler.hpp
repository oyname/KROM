#pragma once
// =============================================================================
// KROM Engine - animation/AnimationSampler.hpp
// Skelett-orientiertes Sampling: wertet AnimationClip-Channels aus und
// liefert pro Bone eine BonePose (lokale TRS).
//
// Für Node-Animation (ECS TransformComponent) liegt die Auswertung im
// AnimationSystem (addons/animation/AnimationSystem.cpp) das direkt
// AnimationEvaluator.hpp nutzt.
// =============================================================================
#include "assets/AnimationClip.hpp"
#include "core/Math.hpp"

// AnimationEvaluator liefert die Sampling-Primitive (SampleVec3, SampleQuat,
// FindKeyframeIndex, NormalizeTime) — header-only, keine weiteren Deps.
#include "animation/AnimationEvaluator.hpp"

#include <cmath>
#include <vector>

namespace engine::animation {

using assets::AnimationClip;
using assets::AnimationChannel;
using assets::AnimationTargetProperty;
using math::Quat;
using math::Vec3;

// Lokale Pose eines einzelnen Knochens.
struct BonePose
{
    Vec3 position = Vec3::Zero();
    Quat rotation = Quat::Identity();
    Vec3 scale    = Vec3::One();
};

// =============================================================================
// AnimationSampler
//
// Wertet alle Channels eines Clips nach Bone-Index aus.
// Mehrere Channels für denselben targetIndex (z.B. T + R + S) werden korrekt
// akkumuliert — jeder Channel schreibt nur seine eigene Eigenschaft.
// =============================================================================
class AnimationSampler
{
public:
    // Gibt pro Bone (index = channel.targetIndex) eine BonePose zurück.
    // Bones ohne Channel erhalten Identitätsposen.
    // boneCount: Größe der Ausgabe (entspricht SkeletonAsset::bones.size()).
    [[nodiscard]] static std::vector<BonePose> SampleForSkeleton(
        const AnimationClip& clip,
        float                time,
        size_t               boneCount)
    {
        const float t = NormalizeTime(clip.duration, time, clip.looping);

        std::vector<BonePose> poses(boneCount);  // rest-pose (T=0, R=Identity, S=1)

        for (const AnimationChannel& ch : clip.channels)
        {
            if (ch.targetIndex < 0 || static_cast<size_t>(ch.targetIndex) >= boneCount)
                continue;

            BonePose& pose = poses[static_cast<size_t>(ch.targetIndex)];

            switch (ch.property)
            {
            case AnimationTargetProperty::Translation:
                if (!ch.vec3Values.empty())
                    pose.position = SampleVec3(ch.times, ch.vec3Values, ch.interpolation, t);
                break;

            case AnimationTargetProperty::Rotation:
                if (!ch.quatValues.empty())
                    pose.rotation = SampleQuat(ch.times, ch.quatValues, ch.interpolation, t);
                break;

            case AnimationTargetProperty::Scale:
                if (!ch.vec3Values.empty())
                    pose.scale = SampleVec3(ch.times, ch.vec3Values, ch.interpolation, t);
                break;
            }
        }

        return poses;
    }

private:
    [[nodiscard]] static float NormalizeTime(
        float duration, float time, bool looping) noexcept
    {
        return engine::animation::NormalizeTime(time, duration, looping);
    }
};

} // namespace engine::animation
