#pragma once
// =============================================================================
// KROM Engine - collision/SceneBvh.hpp
//
// Two-level scene BVH: a top-level BVH (TLAS) over instances, each instance
// pointing at a bottom-level MeshBvh (BLAS) and carrying its object-to-world
// matrix. Pure CPU, no ECS: the caller hands over the instance list each frame
// (addons/mesh_renderer/MeshSceneBvh collects it from the scene).
//
// Update() compares the new list with the previous one by instance key:
//   nothing moved, came or went   -> TLAS kept, Version() unchanged
//   few instances moved           -> refit (boxes bottom-up)
//   many moved, instances added or removed, or the refitted tree has become
//   too poor                      -> rebuild
// It also reports the per-frame change list (moved / added / removed, with old
// and new world AABB) and marks instances that have not moved for
// kRestingFrames updates as resting.
//
// All arrays are flat and GPU-ready (doc/PLAN_GI.md, package 2):
//   TlasNodes()      BvhNode; leaves hold exactly one instance (a = index into
//                    Instances(), count = 1)
//   Instances()      GpuBvhInstance (144 bytes)
//   BlasNodes()      the nodes of all BLAS used, packed; an instance's BLAS
//                    starts at blasNodeOffset, child indices are local
//   BlasTriangles()  the triangles of all BLAS, packed; leaf indices are local
//                    to blasTriangleOffset
// Version() changes whenever TLAS nodes or instance table change,
// BlasPoolVersion() whenever the packed BLAS arrays change.
// =============================================================================
#include "collision/MeshBvh.hpp"
#include "core/Math.hpp"
#include <cstdint>
#include <memory>
#include <unordered_map>
#include <vector>

namespace engine::collision {

// One instance as the caller describes it.
struct SceneBvhInstanceDesc
{
    uint64_t key = 0u;          // stable and unique, e.g. (entity << 32) | submesh
    uint32_t entity = 0u;       // opaque here (EntityID::value)
    uint32_t submesh = 0u;
    uint32_t materialId = 0u;
    std::shared_ptr<const MeshBvh> blas;
    math::Mat4 objectToWorld = math::Mat4::Identity();
    // Changes whenever objectToWorld may have changed (TransformComponent::worldVersion).
    // Same value as last time: the matrix is taken as unchanged without comparing it.
    uint64_t transformVersion = 0u;
};

// Instance flags (GpuBvhInstance::flags).
inline constexpr uint32_t kSceneBvhInstanceResting = 1u << 0u;

// GPU layout of one instance (16-byte aligned rows).
struct GpuBvhInstance
{
    float    worldToObject[12] = {};   // rows 0..2 of the 3x4 affine inverse
    float    objectToWorld[12] = {};   // rows 0..2 of the 3x4 matrix (hit points, normals)
    uint32_t blasNodeOffset     = 0u;
    uint32_t blasTriangleOffset = 0u;
    uint32_t materialId         = 0u;
    uint32_t flags              = 0u;  // kSceneBvhInstance*
    float    albedo[3]          = { 0.5f, 0.5f, 0.5f };   // filled in package 3
    uint32_t entity             = 0u;
    float    emission[3]        = { 0.f, 0.f, 0.f };      // filled in package 3
    uint32_t submesh            = 0u;
};
static_assert(sizeof(GpuBvhInstance) == 144u, "GpuBvhInstance must stay 144 bytes (GPU layout)");

enum class SceneBvhChangeKind : uint8_t { Moved, Added, Removed };

struct SceneBvhChange
{
    SceneBvhChangeKind kind = SceneBvhChangeKind::Moved;
    uint64_t key = 0u;
    uint32_t entity = 0u;
    uint32_t submesh = 0u;
    AABB     oldBounds{};   // world AABB before (invalid for Added: min > max)
    AABB     newBounds{};   // world AABB after (invalid for Removed)
};

enum class SceneBvhUpdateKind : uint8_t { None, Refit, Rebuild };

struct SceneBvhUpdateStats
{
    SceneBvhUpdateKind kind = SceneBvhUpdateKind::None;
    uint32_t moved = 0u;
    uint32_t added = 0u;
    uint32_t removed = 0u;
    uint32_t restingChanged = 0u;
    uint32_t refitsSinceRebuild = 0u;
    float    sahCost = 0.f;          // after this update
    float    sahCostAtRebuild = 0.f; // right after the last rebuild
    double   ms = 0.0;               // whole Update()
    double   tlasMs = 0.0;           // refit or rebuild only
};

struct SceneRayHit
{
    float      distance = 0.f;   // in units of the ray direction (world units if normalised)
    uint32_t   instance = 0u;    // index into Instances()
    uint64_t   key = 0u;
    uint32_t   entity = 0u;
    uint32_t   submesh = 0u;
    uint32_t   triangle = 0u;    // triangle index in the submesh's index buffer (index / 3)
    float      u = 0.f;          // barycentric weight of the triangle's second vertex
    float      v = 0.f;          // barycentric weight of the third vertex
    math::Vec3 position{};       // world
    math::Vec3 normal{};         // geometric normal, world, unit length, along the winding
};

class SceneBvh
{
public:
    // Thresholds of the update policy.
    static constexpr float    kRefitMaxMovedFraction  = 0.10f; // more moved instances: rebuild
    static constexpr float    kRefitMaxSahGrowth      = 1.30f; // refitted cost / cost at rebuild
    static constexpr uint32_t kRefitMaxCount          = 240u;  // refits in a row before a rebuild
    static constexpr uint32_t kRestingFrames          = 30u;   // unmoved updates until "resting"
    static constexpr uint32_t kTlasSahBins            = 8u;
    static constexpr uint32_t kTlasMedianSplitBelow   = 16u;   // smaller TLAS nodes: median split, no SAH
    // Packed BLAS arrays are compacted when more than this share is unused.
    static constexpr float    kBlasPoolMaxWaste       = 0.5f;

    void Update(const std::vector<SceneBvhInstanceDesc>& instances);
    void Clear();

    [[nodiscard]] uint64_t Version() const noexcept { return m_version; }
    [[nodiscard]] uint64_t BlasPoolVersion() const noexcept { return m_blasPoolVersion; }
    [[nodiscard]] const std::vector<SceneBvhChange>& Changes() const noexcept { return m_changes; }
    [[nodiscard]] const SceneBvhUpdateStats& LastUpdate() const noexcept { return m_lastUpdate; }

    [[nodiscard]] const std::vector<BvhNode>& TlasNodes() const noexcept { return m_tlasNodes; }
    [[nodiscard]] const std::vector<GpuBvhInstance>& Instances() const noexcept { return m_gpuInstances; }
    [[nodiscard]] const std::vector<BvhNode>& BlasNodes() const noexcept { return m_blasNodes; }
    [[nodiscard]] const std::vector<BvhTriangle>& BlasTriangles() const noexcept { return m_blasTriangles; }
    [[nodiscard]] uint32_t InstanceCount() const noexcept { return static_cast<uint32_t>(m_instances.size()); }
    // Triangles over all instances (a shared BLAS counts once per instance).
    [[nodiscard]] uint64_t InstancedTriangleCount() const noexcept;
    [[nodiscard]] AABB InstanceBounds(uint32_t instance) const noexcept;
    // Index into Instances() for a key, or UINT32_MAX.
    [[nodiscard]] uint32_t FindInstance(uint64_t key) const noexcept;

    // Closest hit with t in [tMin, tMax]; direction need not be normalised.
    bool TraceClosest(const math::Vec3& origin, const math::Vec3& direction,
                      float tMin, float tMax, SceneRayHit& outHit,
                      BvhTraversalStats* stats = nullptr) const;
    // Any hit with t in [tMin, tMax] (occlusion).
    bool TraceAny(const math::Vec3& origin, const math::Vec3& direction,
                  float tMin, float tMax, BvhTraversalStats* stats = nullptr) const;

private:
    struct InstanceState
    {
        uint64_t key = 0u;
        uint32_t entity = 0u;
        uint32_t submesh = 0u;
        uint32_t materialId = 0u;
        std::shared_ptr<const MeshBvh> blas;
        math::Mat4 objectToWorld = math::Mat4::Identity();
        uint64_t transformVersion = 0u;
        AABB     worldBounds{};
        uint32_t unmovedUpdates = 0u;
        bool     seen = false;
    };
    struct BlasPoolEntry
    {
        std::shared_ptr<const MeshBvh> blas;
        uint32_t nodeOffset = 0u;
        uint32_t triangleOffset = 0u;
        uint32_t users = 0u;
    };

    void RebuildTlas();
    void RefitTlas();
    void WriteGpuInstance(uint32_t index);
    BlasPoolEntry& AcquireBlas(const std::shared_ptr<const MeshBvh>& blas);
    void ReleaseBlas(const MeshBvh* blas);
    void CompactBlasPoolIfWasteful();

    std::vector<InstanceState>  m_instances;      // same order as m_gpuInstances
    std::unordered_map<uint64_t, uint32_t> m_indexByKey;
    std::vector<GpuBvhInstance> m_gpuInstances;
    std::vector<BvhNode>        m_tlasNodes;
    std::vector<uint32_t>       m_leafOfInstance; // TLAS node index per instance

    std::unordered_map<const MeshBvh*, BlasPoolEntry> m_blasPool;
    std::vector<BvhNode>        m_blasNodes;
    std::vector<BvhTriangle>    m_blasTriangles;
    size_t                      m_blasLiveNodes = 0u;

    std::vector<SceneBvhChange> m_changes;
    SceneBvhUpdateStats         m_lastUpdate{};
    float                       m_sahCostAtRebuild = 0.f;
    uint32_t                    m_refitsSinceRebuild = 0u;
    uint64_t                    m_version = 0u;
    uint64_t                    m_blasPoolVersion = 0u;
};

// World AABB of a box under an affine matrix (8 corners).
[[nodiscard]] AABB TransformAABB(const AABB& box, const math::Mat4& m) noexcept;

} // namespace engine::collision
