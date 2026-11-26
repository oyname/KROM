#include "collision/MeshBvh.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <numeric>

namespace engine::collision {

namespace {

struct PrimBounds
{
    float min[3];
    float max[3];
    float centroid[3];
};

struct Box
{
    float min[3] = {  1e30f,  1e30f,  1e30f };
    float max[3] = { -1e30f, -1e30f, -1e30f };

    void Grow(const float lo[3], const float hi[3]) noexcept
    {
        for (int i = 0; i < 3; ++i)
        {
            min[i] = std::min(min[i], lo[i]);
            max[i] = std::max(max[i], hi[i]);
        }
    }
    void GrowPoint(const float p[3]) noexcept { Grow(p, p); }
    [[nodiscard]] bool Valid() const noexcept { return min[0] <= max[0]; }
    [[nodiscard]] float HalfArea() const noexcept
    {
        if (!Valid())
            return 0.f;
        const float dx = max[0] - min[0], dy = max[1] - min[1], dz = max[2] - min[2];
        return dx * dy + dy * dz + dz * dx;
    }
};

[[nodiscard]] bool Finite(float v) noexcept { return std::isfinite(v); }

[[nodiscard]] float NodeHalfArea(const BvhNode& n) noexcept
{
    const float dx = n.max[0] - n.min[0], dy = n.max[1] - n.min[1], dz = n.max[2] - n.min[2];
    return std::max(dx * dy + dy * dz + dz * dx, 0.f);
}

} // namespace

void MeshBvh::Clear() noexcept
{
    m_nodes.clear();
    m_triangles.clear();
    m_stats = {};
}

AABB MeshBvh::Bounds() const noexcept
{
    if (m_nodes.empty())
        return AABB{};
    const BvhNode& root = m_nodes.front();
    return AABB{ math::Vec3{ root.min[0], root.min[1], root.min[2] },
                 math::Vec3{ root.max[0], root.max[1], root.max[2] } };
}

bool MeshBvh::Build(const float* positions, size_t vertexCount,
                    const uint32_t* indices, size_t indexCount)
{
    const auto start = std::chrono::steady_clock::now();
    Clear();
    const size_t triangleCount = indexCount / 3u;
    m_stats.inputTriangles = static_cast<uint32_t>(triangleCount);
    if (!positions || !indices || triangleCount == 0u)
        return false;

    // Valid triangles with their bounds; degenerate ones are dropped.
    std::vector<BvhTriangle> source;
    std::vector<PrimBounds>  bounds;
    source.reserve(triangleCount);
    bounds.reserve(triangleCount);
    for (size_t t = 0u; t < triangleCount; ++t)
    {
        const uint32_t i0 = indices[t * 3u + 0u];
        const uint32_t i1 = indices[t * 3u + 1u];
        const uint32_t i2 = indices[t * 3u + 2u];
        if (i0 >= vertexCount || i1 >= vertexCount || i2 >= vertexCount)
        {
            ++m_stats.degenerateTriangles;
            continue;
        }
        const float* p0 = positions + static_cast<size_t>(i0) * 3u;
        const float* p1 = positions + static_cast<size_t>(i1) * 3u;
        const float* p2 = positions + static_cast<size_t>(i2) * 3u;
        bool finite = true;
        for (int k = 0; k < 3; ++k)
            finite = finite && Finite(p0[k]) && Finite(p1[k]) && Finite(p2[k]);
        const math::Vec3 e1{ p1[0] - p0[0], p1[1] - p0[1], p1[2] - p0[2] };
        const math::Vec3 e2{ p2[0] - p0[0], p2[1] - p0[1], p2[2] - p0[2] };
        const float areaSq = math::Vec3::Cross(e1, e2).LengthSq();
        if (!finite || !(areaSq > 0.f) || !Finite(areaSq))
        {
            ++m_stats.degenerateTriangles;
            continue;
        }
        BvhTriangle tri{};
        for (int k = 0; k < 3; ++k)
        {
            tri.v0[k] = p0[k];
            tri.v1[k] = p1[k];
            tri.v2[k] = p2[k];
        }
        tri.primitive = static_cast<uint32_t>(t);
        PrimBounds pb{};
        for (int k = 0; k < 3; ++k)
        {
            pb.min[k] = std::min({ p0[k], p1[k], p2[k] });
            pb.max[k] = std::max({ p0[k], p1[k], p2[k] });
            pb.centroid[k] = (pb.min[k] + pb.max[k]) * 0.5f;
        }
        source.push_back(tri);
        bounds.push_back(pb);
    }
    if (source.empty())
        return false;

    const uint32_t primCount = static_cast<uint32_t>(source.size());
    std::vector<uint32_t> order(primCount);
    std::iota(order.begin(), order.end(), 0u);
    m_nodes.reserve(static_cast<size_t>(primCount) * 2u);
    m_nodes.emplace_back();

    struct Task { uint32_t node, begin, end, depth; };
    std::vector<Task> tasks;
    tasks.push_back({ 0u, 0u, primCount, 1u });

    struct Bin { Box box; uint32_t count = 0u; };
    while (!tasks.empty())
    {
        const Task task = tasks.back();
        tasks.pop_back();
        m_stats.maxDepth = std::max(m_stats.maxDepth, task.depth);

        Box nodeBox, centroidBox;
        for (uint32_t i = task.begin; i < task.end; ++i)
        {
            const PrimBounds& pb = bounds[order[i]];
            nodeBox.Grow(pb.min, pb.max);
            centroidBox.GrowPoint(pb.centroid);
        }
        {
            BvhNode& node = m_nodes[task.node];
            for (int k = 0; k < 3; ++k)
            {
                node.min[k] = nodeBox.min[k];
                node.max[k] = nodeBox.max[k];
            }
        }

        const uint32_t count = task.end - task.begin;
        if (count <= kMaxLeafTriangles)
        {
            m_nodes[task.node].a = task.begin;
            m_nodes[task.node].count = count;
            ++m_stats.leaves;
            continue;
        }

        // Binned SAH over the centroid bounds.
        int   bestAxis = -1;
        uint32_t bestSplit = 0u;
        float bestCost = 1e30f;
        for (int axis = 0; axis < 3; ++axis)
        {
            const float lo = centroidBox.min[axis];
            const float extent = centroidBox.max[axis] - lo;
            if (!(extent > 1e-12f))
                continue;
            const float scale = static_cast<float>(kSahBins) / extent;
            Bin bins[kSahBins];
            for (uint32_t i = task.begin; i < task.end; ++i)
            {
                const PrimBounds& pb = bounds[order[i]];
                uint32_t b = static_cast<uint32_t>((pb.centroid[axis] - lo) * scale);
                b = std::min(b, kSahBins - 1u);
                bins[b].box.Grow(pb.min, pb.max);
                ++bins[b].count;
            }
            float    rightArea[kSahBins] = {};
            uint32_t rightCount[kSahBins] = {};
            Box acc;
            uint32_t accCount = 0u;
            for (uint32_t b = kSahBins - 1u; b > 0u; --b)
            {
                if (bins[b].count)
                    acc.Grow(bins[b].box.min, bins[b].box.max);
                accCount += bins[b].count;
                rightArea[b] = acc.HalfArea();
                rightCount[b] = accCount;
            }
            Box left;
            uint32_t leftCount = 0u;
            for (uint32_t b = 0u; b + 1u < kSahBins; ++b)
            {
                if (bins[b].count)
                    left.Grow(bins[b].box.min, bins[b].box.max);
                leftCount += bins[b].count;
                if (leftCount == 0u || rightCount[b + 1u] == 0u)
                    continue;
                const float cost = left.HalfArea() * static_cast<float>(leftCount) +
                                   rightArea[b + 1u] * static_cast<float>(rightCount[b + 1u]);
                if (cost < bestCost)
                {
                    bestCost = cost;
                    bestAxis = axis;
                    bestSplit = b + 1u;
                }
            }
        }

        uint32_t mid = task.begin;
        if (bestAxis >= 0)
        {
            const float lo = centroidBox.min[bestAxis];
            const float scale = static_cast<float>(kSahBins) / (centroidBox.max[bestAxis] - lo);
            const auto it = std::partition(order.begin() + task.begin, order.begin() + task.end,
                [&](uint32_t p)
                {
                    uint32_t b = static_cast<uint32_t>((bounds[p].centroid[bestAxis] - lo) * scale);
                    return std::min(b, kSahBins - 1u) < bestSplit;
                });
            mid = static_cast<uint32_t>(it - order.begin());
        }
        if (mid == task.begin || mid == task.end)
            mid = task.begin + count / 2u;   // all centroids equal: split the list in half

        const uint32_t leftIndex = static_cast<uint32_t>(m_nodes.size());
        m_nodes.emplace_back();
        m_nodes.emplace_back();
        m_nodes[task.node].a = leftIndex;
        m_nodes[task.node].count = 0u;
        tasks.push_back({ leftIndex + 1u, mid, task.end, task.depth + 1u });
        tasks.push_back({ leftIndex, task.begin, mid, task.depth + 1u });
    }

    // Triangles in leaf order: leaf.a already indexes into `order`.
    m_triangles.resize(primCount);
    for (uint32_t i = 0u; i < primCount; ++i)
        m_triangles[i] = source[order[i]];

    m_nodes.shrink_to_fit();
    m_stats.nodes = static_cast<uint32_t>(m_nodes.size());
    m_stats.buildMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    return true;
}

bool MeshBvh::IntersectClosest(const math::Vec3& origin, const math::Vec3& direction,
                               float tMin, float tMax, BvhTriangleHit& outHit,
                               BvhTraversalStats* stats) const
{
    if (m_nodes.empty())
        return false;
    uint32_t stack[kBvhTraversalStackSize];
    float closest = tMax;
    return BvhTraverseClosest(m_nodes.data(), m_triangles.data(), 0u, 0u, origin, direction,
                              tMin, closest, outHit, stack, 0u, stats);
}

bool MeshBvh::IntersectAny(const math::Vec3& origin, const math::Vec3& direction,
                           float tMin, float tMax, BvhTraversalStats* stats) const
{
    if (m_nodes.empty())
        return false;
    uint32_t stack[kBvhTraversalStackSize];
    return BvhTraverseAny(m_nodes.data(), m_triangles.data(), 0u, 0u, origin, direction,
                          tMin, tMax, stack, 0u, stats);
}

// -----------------------------------------------------------------------------
// Traversal. Written to translate to HLSL almost word for word: fixed stack, no
// recursion, nearer child first, overflow counted instead of undefined.
// -----------------------------------------------------------------------------
bool BvhTraverseClosest(const BvhNode* nodes, const BvhTriangle* triangles,
                        uint32_t nodeOffset, uint32_t triangleOffset,
                        const math::Vec3& origin, const math::Vec3& direction,
                        float tMin, float& ioTMax, BvhTriangleHit& outHit,
                        uint32_t* stack, uint32_t stackBase, BvhTraversalStats* stats)
{
    const math::Vec3 invDir = BvhSafeInverse(direction);
    float tRoot = 0.f;
    if (!BvhRayBox(nodes[nodeOffset].min, nodes[nodeOffset].max, origin, invDir, tMin, ioTMax, tRoot))
        return false;
    if (stackBase >= kBvhTraversalStackSize)
    {
        if (stats) ++stats->stackOverflows;
        return false;
    }

    bool found = false;
    uint32_t sp = stackBase;
    stack[sp++] = 0u;
    while (sp > stackBase)
    {
        const BvhNode node = nodes[nodeOffset + stack[--sp]];
        if (stats) ++stats->nodesVisited;
        if (node.count > 0u)
        {
            for (uint32_t i = 0u; i < node.count; ++i)
            {
                const uint32_t triIndex = triangleOffset + node.a + i;
                float t = 0.f, u = 0.f, v = 0.f;
                if (stats) ++stats->triangleTests;
                if (BvhRayTriangle(triangles[triIndex], origin, direction, tMin, ioTMax, t, u, v))
                {
                    ioTMax = t;
                    outHit.t = t;
                    outHit.u = u;
                    outHit.v = v;
                    outHit.triangle = triIndex;
                    found = true;
                }
            }
            continue;
        }

        const uint32_t left = node.a;
        const uint32_t right = node.a + 1u;
        float tLeft = 0.f, tRight = 0.f;
        const bool hitLeft = BvhRayBox(nodes[nodeOffset + left].min, nodes[nodeOffset + left].max,
                                       origin, invDir, tMin, ioTMax, tLeft);
        const bool hitRight = BvhRayBox(nodes[nodeOffset + right].min, nodes[nodeOffset + right].max,
                                        origin, invDir, tMin, ioTMax, tRight);
        if (hitLeft && hitRight)
        {
            const uint32_t nearChild = tLeft <= tRight ? left : right;
            const uint32_t farChild  = tLeft <= tRight ? right : left;
            if (sp + 2u <= kBvhTraversalStackSize)
            {
                stack[sp++] = farChild;
                stack[sp++] = nearChild;
            }
            else
            {
                if (stats) ++stats->stackOverflows;   // far subtree skipped
                stack[sp++] = nearChild;               // sp < size: one entry was just popped
            }
        }
        else if (hitLeft)
            stack[sp++] = left;
        else if (hitRight)
            stack[sp++] = right;
        if (stats) stats->maxStackDepth = std::max(stats->maxStackDepth, sp);
    }
    return found;
}

bool BvhTraverseAny(const BvhNode* nodes, const BvhTriangle* triangles,
                    uint32_t nodeOffset, uint32_t triangleOffset,
                    const math::Vec3& origin, const math::Vec3& direction,
                    float tMin, float tMax,
                    uint32_t* stack, uint32_t stackBase, BvhTraversalStats* stats)
{
    const math::Vec3 invDir = BvhSafeInverse(direction);
    float tRoot = 0.f;
    if (!BvhRayBox(nodes[nodeOffset].min, nodes[nodeOffset].max, origin, invDir, tMin, tMax, tRoot))
        return false;
    if (stackBase >= kBvhTraversalStackSize)
    {
        if (stats) ++stats->stackOverflows;
        return false;
    }

    uint32_t sp = stackBase;
    stack[sp++] = 0u;
    while (sp > stackBase)
    {
        const BvhNode node = nodes[nodeOffset + stack[--sp]];
        if (stats) ++stats->nodesVisited;
        if (node.count > 0u)
        {
            for (uint32_t i = 0u; i < node.count; ++i)
            {
                float t = 0.f, u = 0.f, v = 0.f;
                if (stats) ++stats->triangleTests;
                if (BvhRayTriangle(triangles[triangleOffset + node.a + i], origin, direction, tMin, tMax, t, u, v))
                    return true;
            }
            continue;
        }
        const uint32_t left = node.a;
        const uint32_t right = node.a + 1u;
        float tLeft = 0.f, tRight = 0.f;
        const bool hitLeft = BvhRayBox(nodes[nodeOffset + left].min, nodes[nodeOffset + left].max,
                                       origin, invDir, tMin, tMax, tLeft);
        const bool hitRight = BvhRayBox(nodes[nodeOffset + right].min, nodes[nodeOffset + right].max,
                                        origin, invDir, tMin, tMax, tRight);
        if (hitLeft && hitRight)
        {
            if (sp + 2u <= kBvhTraversalStackSize)
            {
                stack[sp++] = tLeft <= tRight ? right : left;
                stack[sp++] = tLeft <= tRight ? left : right;
            }
            else
            {
                if (stats) ++stats->stackOverflows;
                stack[sp++] = tLeft <= tRight ? left : right;
            }
        }
        else if (hitLeft)
            stack[sp++] = left;
        else if (hitRight)
            stack[sp++] = right;
        if (stats) stats->maxStackDepth = std::max(stats->maxStackDepth, sp);
    }
    return false;
}

float BvhSahCost(const std::vector<BvhNode>& nodes) noexcept
{
    if (nodes.empty())
        return 0.f;
    const float rootArea = NodeHalfArea(nodes.front());
    if (!(rootArea > 0.f))
        return 0.f;
    double cost = 0.0;
    for (const BvhNode& n : nodes)
        cost += static_cast<double>(NodeHalfArea(n)) * (n.count > 0u ? static_cast<double>(n.count) : 1.0);
    return static_cast<float>(cost / rootArea);
}

} // namespace engine::collision
