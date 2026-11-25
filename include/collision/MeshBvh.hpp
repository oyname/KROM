#pragma once
// =============================================================================
// KROM Engine - collision/MeshBvh.hpp
//
// Bottom-level BVH (BLAS) over the triangles of one submesh, in object space.
// Built on the CPU with binned SAH; stored flat so the GPU can take the arrays
// unchanged (surface cache GI, doc/PLAN_GI.md, package 2):
//
//   BvhNode      32 bytes: AABB min/max (float3) and two uint.
//                count == 0: inner node, a = index of the left child; the
//                            right child is a + 1.
//                count >  0: leaf, a = first triangle, count triangles.
//   BvhTriangle  48 bytes: the three positions (float3 + 4 bytes each). The
//                spare word after v0 holds the triangle's index in the source
//                index buffer (index / 3), so hits can name it.
//
// Child and triangle indices are local to this BVH. A scene that packs many
// BLAS into one array adds its node/triangle offsets during traversal.
// Degenerate triangles (zero area, NaN/Inf) are dropped; an empty mesh gives
// an empty BVH (no nodes).
// =============================================================================
#include "collision/SceneQueries.hpp"
#include "core/Math.hpp"
#include <cstddef>
#include <cstdint>
#include <vector>

namespace engine::collision {

struct BvhNode
{
    float    min[3] = {0.f, 0.f, 0.f};
    uint32_t a      = 0u;   // inner: left child; leaf: first triangle (or instance)
    float    max[3] = {0.f, 0.f, 0.f};
    uint32_t count  = 0u;   // 0 = inner node, otherwise number of primitives
};
static_assert(sizeof(BvhNode) == 32u, "BvhNode must stay 32 bytes (GPU layout)");

struct BvhTriangle
{
    float    v0[3] = {0.f, 0.f, 0.f};
    uint32_t primitive = 0u;   // triangle index in the source index buffer
    float    v1[3] = {0.f, 0.f, 0.f};
    uint32_t _pad1 = 0u;
    float    v2[3] = {0.f, 0.f, 0.f};
    uint32_t _pad2 = 0u;
};
static_assert(sizeof(BvhTriangle) == 48u, "BvhTriangle must stay 48 bytes (GPU layout)");

// Fixed traversal stack depth, shared with the HLSL version (package 2).
inline constexpr uint32_t kBvhTraversalStackSize = 32u;

// Hit in the space of the ray that was traced.
struct BvhTriangleHit
{
    float    t = 0.f;
    float    u = 0.f;           // barycentric weight of v1
    float    v = 0.f;           // barycentric weight of v2
    uint32_t triangle = 0u;     // index into Triangles()
};

// Traversal counters (filled when a pointer is given).
struct BvhTraversalStats
{
    uint32_t nodesVisited   = 0u;
    uint32_t triangleTests  = 0u;
    uint32_t maxStackDepth  = 0u;
    uint32_t stackOverflows = 0u;   // subtrees skipped because the stack was full
};

struct MeshBvhBuildStats
{
    uint32_t inputTriangles      = 0u;
    uint32_t degenerateTriangles = 0u;   // dropped
    uint32_t nodes               = 0u;
    uint32_t leaves              = 0u;
    uint32_t maxDepth            = 0u;
    double   buildMs             = 0.0;
};

class MeshBvh
{
public:
    static constexpr uint32_t kMaxLeafTriangles = 4u;
    static constexpr uint32_t kSahBins          = 12u;

    // positions: 3 floats per vertex; indices: 3 per triangle. Indices out of
    // range count as degenerate. Returns false when no triangle remains.
    bool Build(const float* positions, size_t vertexCount,
               const uint32_t* indices, size_t indexCount);
    bool Build(const std::vector<float>& positions, const std::vector<uint32_t>& indices)
    {
        return Build(positions.data(), positions.size() / 3u, indices.data(), indices.size());
    }

    void Clear() noexcept;

    [[nodiscard]] bool Empty() const noexcept { return m_nodes.empty(); }
    [[nodiscard]] const std::vector<BvhNode>& Nodes() const noexcept { return m_nodes; }
    [[nodiscard]] const std::vector<BvhTriangle>& Triangles() const noexcept { return m_triangles; }
    [[nodiscard]] AABB Bounds() const noexcept;
    [[nodiscard]] const MeshBvhBuildStats& BuildStats() const noexcept { return m_stats; }

    // Closest hit with t in [tMin, tMax]. The direction need not be normalised;
    // t is in units of the direction's length.
    bool IntersectClosest(const math::Vec3& origin, const math::Vec3& direction,
                          float tMin, float tMax, BvhTriangleHit& outHit,
                          BvhTraversalStats* stats = nullptr) const;
    // Any hit with t in [tMin, tMax].
    bool IntersectAny(const math::Vec3& origin, const math::Vec3& direction,
                      float tMin, float tMax, BvhTraversalStats* stats = nullptr) const;

private:
    std::vector<BvhNode>     m_nodes;
    std::vector<BvhTriangle> m_triangles;
    MeshBvhBuildStats        m_stats{};
};

// ---------------------------------------------------------------------------
// Shared primitives (used by MeshBvh, SceneBvh and the tests). Written so they
// translate to HLSL line by line.
// ---------------------------------------------------------------------------

// Slab test. invDir = 1 / direction (may be +-inf). Returns the entry distance
// in outTNear when the box is hit within [tMin, tMax].
[[nodiscard]] inline bool BvhRayBox(const float bmin[3], const float bmax[3],
                                    const math::Vec3& origin, const math::Vec3& invDir,
                                    float tMin, float tMax, float& outTNear) noexcept
{
    const float o[3] = { origin.x, origin.y, origin.z };
    const float id[3] = { invDir.x, invDir.y, invDir.z };
    float t0 = tMin;
    float t1 = tMax;
    for (int axis = 0; axis < 3; ++axis)
    {
        float tA = (bmin[axis] - o[axis]) * id[axis];
        float tB = (bmax[axis] - o[axis]) * id[axis];
        // 0 * inf = NaN when the origin lies on the slab plane: treat as inside.
        if (tA != tA) tA = -1e30f;
        if (tB != tB) tB = 1e30f;
        const float tNear = tA < tB ? tA : tB;
        const float tFar  = tA < tB ? tB : tA;
        t0 = tNear > t0 ? tNear : t0;
        t1 = tFar  < t1 ? tFar  : t1;
    }
    outTNear = t0;
    return t0 <= t1;
}

// Möller-Trumbore, both faces. Returns t, u (weight of v1), v (weight of v2).
[[nodiscard]] inline bool BvhRayTriangle(const BvhTriangle& tri,
                                         const math::Vec3& origin, const math::Vec3& direction,
                                         float tMin, float tMax,
                                         float& outT, float& outU, float& outV) noexcept
{
    const math::Vec3 v0{ tri.v0[0], tri.v0[1], tri.v0[2] };
    const math::Vec3 e1{ tri.v1[0] - tri.v0[0], tri.v1[1] - tri.v0[1], tri.v1[2] - tri.v0[2] };
    const math::Vec3 e2{ tri.v2[0] - tri.v0[0], tri.v2[1] - tri.v0[1], tri.v2[2] - tri.v0[2] };
    const math::Vec3 p = math::Vec3::Cross(direction, e2);
    const float det = math::Vec3::Dot(e1, p);
    if (det > -1e-12f && det < 1e-12f)
        return false;
    const float invDet = 1.f / det;
    const math::Vec3 s = origin - v0;
    const float u = math::Vec3::Dot(s, p) * invDet;
    if (u < 0.f || u > 1.f)
        return false;
    const math::Vec3 q = math::Vec3::Cross(s, e1);
    const float v = math::Vec3::Dot(direction, q) * invDet;
    if (v < 0.f || u + v > 1.f)
        return false;
    const float t = math::Vec3::Dot(e2, q) * invDet;
    if (t < tMin || t > tMax)
        return false;
    outT = t;
    outU = u;
    outV = v;
    return true;
}

[[nodiscard]] inline math::Vec3 BvhSafeInverse(const math::Vec3& d) noexcept
{
    constexpr float kInf = 1e30f;
    return { d.x != 0.f ? 1.f / d.x : kInf,
             d.y != 0.f ? 1.f / d.y : kInf,
             d.z != 0.f ? 1.f / d.z : kInf };
}

// BLAS traversal over packed arrays: node i of this BLAS is nodes[nodeOffset + i],
// triangle j is triangles[triangleOffset + j]. Uses stack[stackBase ..
// kBvhTraversalStackSize - 1], so a caller (the TLAS loop) can keep its own
// entries below stackBase. ioTMax shrinks to the closest hit; outHit.triangle is
// the packed index (triangleOffset + j). Returns true if a hit closer than the
// incoming ioTMax was found.
bool BvhTraverseClosest(const BvhNode* nodes, const BvhTriangle* triangles,
                        uint32_t nodeOffset, uint32_t triangleOffset,
                        const math::Vec3& origin, const math::Vec3& direction,
                        float tMin, float& ioTMax, BvhTriangleHit& outHit,
                        uint32_t* stack, uint32_t stackBase, BvhTraversalStats* stats);

bool BvhTraverseAny(const BvhNode* nodes, const BvhTriangle* triangles,
                    uint32_t nodeOffset, uint32_t triangleOffset,
                    const math::Vec3& origin, const math::Vec3& direction,
                    float tMin, float tMax,
                    uint32_t* stack, uint32_t stackBase, BvhTraversalStats* stats);

// SAH cost of a tree, relative to its root box (inner node: 1 x area,
// leaf: count x area). Used to judge how much a refit has degraded a tree.
[[nodiscard]] float BvhSahCost(const std::vector<BvhNode>& nodes) noexcept;

} // namespace engine::collision
