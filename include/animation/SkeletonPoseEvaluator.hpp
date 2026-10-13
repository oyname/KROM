#pragma once
// =============================================================================
// KROM Engine - animation/SkeletonPoseEvaluator.hpp
// Loest die Knochen-Hierarchie auf und berechnet die Bone-Palette fuer den GPU.
//
// Eingabe:  lokale BonePoses (vom AnimationSampler) + SkeletonAsset
// Ausgabe:  eine Mat4 pro Knochen (skinningMatrix = worldMatrix * inverseBindMatrix)
//
// Invariante: skeleton.bones ist topologisch sortiert — Eltern-Knochen haben
//             immer einen kleineren Index als ihre Kinder. Der Algorithmus
//             traversiert das Array einmal von vorne nach hinten.
// =============================================================================
#include "animation/AnimationSampler.hpp"
#include "assets/SkeletonAsset.hpp"
#include "core/Math.hpp"

#include <cassert>
#include <vector>

namespace engine::animation {

using assets::SkeletonAsset;
using math::Mat4;

class SkeletonPoseEvaluator
{
public:
    // Berechnet die Bone-Palette aus lokalen Posen und Skelett-Hierarchie.
    // poses.size() muss gleich skeleton.bones.size() sein.
    // Rueckgabe: skinningMatrix[i] = worldMatrix[i] * bones[i].inverseBindMatrix
    [[nodiscard]] static std::vector<Mat4> Evaluate(
        const SkeletonAsset&         skeleton,
        const std::vector<BonePose>& localPoses)
    {
        const size_t boneCount = skeleton.bones.size();
        assert(localPoses.size() == boneCount && "pose count must match bone count");

        std::vector<Mat4> worldMatrices(boneCount);
        std::vector<Mat4> skinningMatrices(boneCount);

        for (size_t i = 0; i < boneCount; ++i)
        {
            const assets::Bone& bone = skeleton.bones[i];
            const BonePose&     pose = localPoses[i];
            assert(bone.parentIndex < 0 || static_cast<size_t>(bone.parentIndex) < i);

            const Mat4 local = Mat4::TRS(pose.position, pose.rotation, pose.scale);

            // parentIndex == -1 → Root: world = local
            worldMatrices[i] = (bone.parentIndex >= 0)
                ? worldMatrices[static_cast<size_t>(bone.parentIndex)] * local
                : local;

            skinningMatrices[i] = worldMatrices[i] * bone.inverseBindMatrix;
        }

        return skinningMatrices;
    }
};

} // namespace engine::animation
