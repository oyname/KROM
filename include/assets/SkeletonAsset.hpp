#pragma once
// =============================================================================
// KROM Engine - assets/SkeletonAsset.hpp
// CPU-seitige Skeleton-Daten fuer Animation/Skinning.
// =============================================================================
#include "assets/AssetBase.hpp"
#include "core/Math.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace engine::assets {

using math::Mat4;

struct Bone
{
    std::string name;
    int32_t     parentIndex = -1; // -1 = root
    Mat4        inverseBindMatrix = Mat4::Identity();
};

struct SkeletonAsset : AssetBase
{
    std::vector<Bone> bones; // Index = bone ID, parentIndex points into this array
};

} // namespace engine::assets
