#include "collision/SceneBvh.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <numeric>
#include <unordered_set>

namespace engine::collision {

namespace {

const AABB kInvalidBox{ math::Vec3{ 1e30f, 1e30f, 1e30f }, math::Vec3{ -1e30f, -1e30f, -1e30f } };

[[nodiscard]] bool SameMatrix(const math::Mat4& a, const math::Mat4& b) noexcept
{
    return std::memcmp(a.m, b.m, sizeof(a.m)) == 0;
}

void WriteRows3x4(float out[12], const math::Mat4& m) noexcept
{
    // Mat4 is column-major (m[col][row]); store rows 0..2.
    for (int row = 0; row < 3; ++row)
        for (int col = 0; col < 4; ++col)
            out[row * 4 + col] = m.m[col][row];
}

[[nodiscard]] math::Vec3 Rows3x4Point(const float r[12], const math::Vec3& p) noexcept
{
    return { r[0] * p.x + r[1] * p.y + r[2]  * p.z + r[3],
             r[4] * p.x + r[5] * p.y + r[6]  * p.z + r[7],
             r[8] * p.x + r[9] * p.y + r[10] * p.z + r[11] };
}

[[nodiscard]] math::Vec3 Rows3x4Direction(const float r[12], const math::Vec3& d) noexcept
{
    return { r[0] * d.x + r[1] * d.y + r[2]  * d.z,
             r[4] * d.x + r[5] * d.y + r[6]  * d.z,
             r[8] * d.x + r[9] * d.y + r[10] * d.z };
}

struct InstanceBox
{
    float    min[3];
    float    max[3];
    float    centroid[3];
    uint32_t instance;
};

void SetNodeBox(BvhNode& node, const AABB& box) noexcept
{
    node.min[0] = box.min.x; node.min[1] = box.min.y; node.min[2] = box.min.z;
    node.max[0] = box.max.x; node.max[1] = box.max.y; node.max[2] = box.max.z;
}

} // namespace

AABB TransformAABB(const AABB& box, const math::Mat4& m) noexcept
{
    // Arvo: centre through the matrix, extents through |M|.
    const math::Vec3 c{ (box.min.x + box.max.x) * 0.5f, (box.min.y + box.max.y) * 0.5f, (box.min.z + box.max.z) * 0.5f };
    const math::Vec3 e{ (box.max.x - box.min.x) * 0.5f, (box.max.y - box.min.y) * 0.5f, (box.max.z - box.min.z) * 0.5f };
    const math::Vec3 wc = m.TransformPoint(c);
    math::Vec3 we{};
    float* out = &we.x;
    for (int row = 0; row < 3; ++row)
        out[row] = std::fabs(m.m[0][row]) * e.x + std::fabs(m.m[1][row]) * e.y + std::fabs(m.m[2][row]) * e.z;
    return AABB{ wc - we, wc + we };
}

void SceneBvh::Clear()
{
    const bool hadContent = !m_instances.empty() || !m_tlasNodes.empty();
    m_instances.clear();
    m_indexByKey.clear();
    m_gpuInstances.clear();
    m_tlasNodes.clear();
    m_leafOfInstance.clear();
    m_blasPool.clear();
    m_blasNodes.clear();
    m_blasTriangles.clear();
    m_blasLiveNodes = 0u;
    m_changes.clear();
    m_lastUpdate = {};
    m_sahCostAtRebuild = 0.f;
    m_refitsSinceRebuild = 0u;
    if (hadContent)
    {
        ++m_version;
        ++m_geometryVersion;
        ++m_blasPoolVersion;
    }
}

uint64_t SceneBvh::InstancedTriangleCount() const noexcept
{
    uint64_t total = 0u;
    for (const InstanceState& s : m_instances)
        total += s.blas ? s.blas->Triangles().size() : 0u;
    return total;
}

AABB SceneBvh::InstanceBounds(uint32_t instance) const noexcept
{
    return instance < m_instances.size() ? m_instances[instance].worldBounds : kInvalidBox;
}

uint32_t SceneBvh::FindInstance(uint64_t key) const noexcept
{
    const auto it = m_indexByKey.find(key);
    return it != m_indexByKey.end() ? it->second : UINT32_MAX;
}

SceneBvh::BlasPoolEntry& SceneBvh::AcquireBlas(const std::shared_ptr<const MeshBvh>& blas)
{
    auto it = m_blasPool.find(blas.get());
    if (it != m_blasPool.end())
    {
        ++it->second.users;
        return it->second;
    }
    BlasPoolEntry entry{};
    entry.blas = blas;
    entry.nodeOffset = static_cast<uint32_t>(m_blasNodes.size());
    entry.triangleOffset = static_cast<uint32_t>(m_blasTriangles.size());
    entry.users = 1u;
    m_blasNodes.insert(m_blasNodes.end(), blas->Nodes().begin(), blas->Nodes().end());
    m_blasTriangles.insert(m_blasTriangles.end(), blas->Triangles().begin(), blas->Triangles().end());
    m_blasLiveNodes += blas->Nodes().size();
    ++m_blasPoolVersion;
    return m_blasPool.emplace(blas.get(), std::move(entry)).first->second;
}

void SceneBvh::ReleaseBlas(const MeshBvh* blas)
{
    auto it = m_blasPool.find(blas);
    if (it == m_blasPool.end())
        return;
    if (--it->second.users == 0u)
    {
        // The packed data stays as a hole until CompactBlasPoolIfWasteful().
        m_blasLiveNodes -= it->second.blas->Nodes().size();
        m_blasPool.erase(it);
    }
}

void SceneBvh::CompactBlasPoolIfWasteful()
{
    const size_t total = m_blasNodes.size();
    if (total == 0u || static_cast<float>(m_blasLiveNodes) >= (1.f - kBlasPoolMaxWaste) * static_cast<float>(total))
        return;
    std::vector<BvhNode> nodes;
    std::vector<BvhTriangle> triangles;
    nodes.reserve(m_blasLiveNodes);
    for (auto& [ptr, entry] : m_blasPool)
    {
        entry.nodeOffset = static_cast<uint32_t>(nodes.size());
        entry.triangleOffset = static_cast<uint32_t>(triangles.size());
        nodes.insert(nodes.end(), entry.blas->Nodes().begin(), entry.blas->Nodes().end());
        triangles.insert(triangles.end(), entry.blas->Triangles().begin(), entry.blas->Triangles().end());
    }
    m_blasNodes = std::move(nodes);
    m_blasTriangles = std::move(triangles);
    m_blasLiveNodes = m_blasNodes.size();
    ++m_blasPoolVersion;
    for (uint32_t i = 0u; i < m_instances.size(); ++i)
        WriteGpuInstance(i);
    ++m_version;
}

void SceneBvh::WriteGpuInstance(uint32_t index)
{
    const InstanceState& s = m_instances[index];
    GpuBvhInstance& g = m_gpuInstances[index];
    WriteRows3x4(g.objectToWorld, s.objectToWorld);
    WriteRows3x4(g.worldToObject, s.objectToWorld.Inverse());
    const auto it = m_blasPool.find(s.blas.get());
    g.blasNodeOffset = it != m_blasPool.end() ? it->second.nodeOffset : 0u;
    g.blasTriangleOffset = it != m_blasPool.end() ? it->second.triangleOffset : 0u;
    g.materialId = s.materialId;
    g.flags = s.unmovedUpdates >= kRestingFrames ? kSceneBvhInstanceResting : 0u;
    g.entity = s.entity;
    g.submesh = s.submesh;
    // albedo/emission keep their defaults (0.5 grey, 0) until package 3 fills them.
}

void SceneBvh::Update(const std::vector<SceneBvhInstanceDesc>& descs)
{
    const auto start = std::chrono::steady_clock::now();
    m_changes.clear();
    SceneBvhUpdateStats stats{};

    for (InstanceState& s : m_instances)
        s.seen = false;

    bool structural = false;
    bool tableChanged = false;
    std::vector<uint32_t> moved;
    std::vector<InstanceState> added;
    std::unordered_set<uint64_t> addedKeys;

    for (const SceneBvhInstanceDesc& d : descs)
    {
        if (!d.blas || d.blas->Empty())
            continue;
        const auto it = m_indexByKey.find(d.key);
        if (it == m_indexByKey.end())
        {
            if (!addedKeys.insert(d.key).second)
                continue;   // duplicate key in the list: first one wins
            InstanceState s{};
            s.key = d.key;
            s.entity = d.entity;
            s.submesh = d.submesh;
            s.materialId = d.materialId;
            s.blas = d.blas;
            s.objectToWorld = d.objectToWorld;
            s.transformVersion = d.transformVersion;
            s.worldBounds = TransformAABB(d.blas->Bounds(), d.objectToWorld);
            s.seen = true;
            added.push_back(std::move(s));
            continue;
        }

        const uint32_t index = it->second;
        InstanceState& s = m_instances[index];
        if (s.seen)
            continue;   // duplicate key
        s.seen = true;

        bool changed = false;
        if (s.blas != d.blas)
        {
            // Same instance, new geometry (mesh swapped or re-imported).
            AcquireBlas(d.blas);
            ReleaseBlas(s.blas.get());
            s.blas = d.blas;
            changed = true;
            structural = true;   // pool offsets of this instance changed
        }
        if (d.transformVersion != s.transformVersion)
        {
            s.transformVersion = d.transformVersion;
            if (!SameMatrix(d.objectToWorld, s.objectToWorld))
            {
                s.objectToWorld = d.objectToWorld;
                changed = true;
            }
        }
        if (d.materialId != s.materialId)
        {
            s.materialId = d.materialId;
            tableChanged = true;
            WriteGpuInstance(index);
        }

        if (changed)
        {
            SceneBvhChange change{};
            change.kind = SceneBvhChangeKind::Moved;
            change.key = s.key;
            change.entity = s.entity;
            change.submesh = s.submesh;
            change.oldBounds = s.worldBounds;
            s.worldBounds = TransformAABB(s.blas->Bounds(), s.objectToWorld);
            change.newBounds = s.worldBounds;
            m_changes.push_back(change);
            if (s.unmovedUpdates >= kRestingFrames)
                ++stats.restingChanged;
            s.unmovedUpdates = 0u;
            moved.push_back(index);
        }
        else if (s.unmovedUpdates < kRestingFrames)
        {
            if (++s.unmovedUpdates == kRestingFrames)
            {
                ++stats.restingChanged;
                tableChanged = true;
                WriteGpuInstance(index);
            }
        }
    }

    // Removed: in the old set, not in the new list.
    for (InstanceState& s : m_instances)
    {
        if (s.seen)
            continue;
        SceneBvhChange change{};
        change.kind = SceneBvhChangeKind::Removed;
        change.key = s.key;
        change.entity = s.entity;
        change.submesh = s.submesh;
        change.oldBounds = s.worldBounds;
        change.newBounds = kInvalidBox;
        m_changes.push_back(change);
        ReleaseBlas(s.blas.get());
        ++stats.removed;
        structural = true;
    }
    for (InstanceState& s : added)
    {
        AcquireBlas(s.blas);
        SceneBvhChange change{};
        change.kind = SceneBvhChangeKind::Added;
        change.key = s.key;
        change.entity = s.entity;
        change.submesh = s.submesh;
        change.oldBounds = kInvalidBox;
        change.newBounds = s.worldBounds;
        m_changes.push_back(change);
        ++stats.added;
        structural = true;
    }
    stats.moved = static_cast<uint32_t>(moved.size());

    const auto tlasStart = std::chrono::steady_clock::now();
    if (structural)
    {
        // New instance order: survivors in their old order, then the new ones.
        std::vector<InstanceState> next;
        next.reserve(m_instances.size() + added.size());
        for (InstanceState& s : m_instances)
            if (s.seen)
                next.push_back(std::move(s));
        for (InstanceState& s : added)
            next.push_back(std::move(s));
        m_instances = std::move(next);
        m_indexByKey.clear();
        m_indexByKey.reserve(m_instances.size());
        m_gpuInstances.assign(m_instances.size(), GpuBvhInstance{});
        for (uint32_t i = 0u; i < m_instances.size(); ++i)
        {
            m_indexByKey.emplace(m_instances[i].key, i);
            WriteGpuInstance(i);
        }
        RebuildTlas();
        stats.kind = SceneBvhUpdateKind::Rebuild;
    }
    else if (!moved.empty())
    {
        for (const uint32_t index : moved)
            WriteGpuInstance(index);
        const float movedFraction = static_cast<float>(moved.size()) / static_cast<float>(m_instances.size());
        if (movedFraction > kRefitMaxMovedFraction || m_refitsSinceRebuild >= kRefitMaxCount)
        {
            RebuildTlas();
            stats.kind = SceneBvhUpdateKind::Rebuild;
        }
        else
        {
            RefitTlas();
            ++m_refitsSinceRebuild;
            stats.kind = SceneBvhUpdateKind::Refit;
            if (BvhSahCost(m_tlasNodes) > kRefitMaxSahGrowth * m_sahCostAtRebuild)
            {
                RebuildTlas();
                stats.kind = SceneBvhUpdateKind::Rebuild;
            }
        }
    }
    stats.tlasMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - tlasStart).count();

    if (stats.kind != SceneBvhUpdateKind::None || tableChanged)
        ++m_version;
    if (stats.kind != SceneBvhUpdateKind::None)
        ++m_geometryVersion;
    CompactBlasPoolIfWasteful();

    stats.refitsSinceRebuild = m_refitsSinceRebuild;
    stats.sahCostAtRebuild = m_sahCostAtRebuild;
    stats.sahCost = stats.kind == SceneBvhUpdateKind::None ? m_lastUpdate.sahCost : BvhSahCost(m_tlasNodes);
    stats.ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    m_lastUpdate = stats;
}

void SceneBvh::RebuildTlas()
{
    m_refitsSinceRebuild = 0u;
    m_tlasNodes.clear();
    const uint32_t count = static_cast<uint32_t>(m_instances.size());
    m_leafOfInstance.assign(count, 0u);
    if (count == 0u)
    {
        m_sahCostAtRebuild = 0.f;
        return;
    }

    std::vector<InstanceBox> boxes(count);
    for (uint32_t i = 0u; i < count; ++i)
    {
        const AABB& b = m_instances[i].worldBounds;
        boxes[i] = InstanceBox{ { b.min.x, b.min.y, b.min.z }, { b.max.x, b.max.y, b.max.z },
                                { (b.min.x + b.max.x) * 0.5f, (b.min.y + b.max.y) * 0.5f, (b.min.z + b.max.z) * 0.5f }, i };
    }
    // The boxes themselves are partitioned (not an index list): contiguous reads.
    m_tlasNodes.reserve(static_cast<size_t>(count) * 2u);
    m_tlasNodes.emplace_back();

    struct Task { uint32_t node, begin, end; };
    std::vector<Task> tasks;
    tasks.push_back({ 0u, 0u, count });
    struct Bin { float min[3] = { 1e30f, 1e30f, 1e30f }; float max[3] = { -1e30f, -1e30f, -1e30f }; uint32_t n = 0u; };
    const auto halfArea = [](const float mn[3], const float mx[3])
    {
        if (mn[0] > mx[0])
            return 0.f;
        const float dx = mx[0] - mn[0], dy = mx[1] - mn[1], dz = mx[2] - mn[2];
        return dx * dy + dy * dz + dz * dx;
    };

    while (!tasks.empty())
    {
        const Task task = tasks.back();
        tasks.pop_back();
        float nmin[3] = { 1e30f, 1e30f, 1e30f }, nmax[3] = { -1e30f, -1e30f, -1e30f };
        float cmin[3] = { 1e30f, 1e30f, 1e30f }, cmax[3] = { -1e30f, -1e30f, -1e30f };
        for (uint32_t i = task.begin; i < task.end; ++i)
        {
            const InstanceBox& b = boxes[i];
            for (int k = 0; k < 3; ++k)
            {
                nmin[k] = std::min(nmin[k], b.min[k]);
                nmax[k] = std::max(nmax[k], b.max[k]);
                cmin[k] = std::min(cmin[k], b.centroid[k]);
                cmax[k] = std::max(cmax[k], b.centroid[k]);
            }
        }
        BvhNode& node = m_tlasNodes[task.node];
        for (int k = 0; k < 3; ++k)
        {
            node.min[k] = nmin[k];
            node.max[k] = nmax[k];
        }
        const uint32_t n = task.end - task.begin;
        if (n == 1u)
        {
            node.a = boxes[task.begin].instance;
            node.count = 1u;
            m_leafOfInstance[boxes[task.begin].instance] = task.node;
            continue;
        }

        if (n <= kTlasMedianSplitBelow)
        {
            // Small node: binning costs more than it gains. Median of the longest centroid axis.
            int axis = 0;
            for (int k = 1; k < 3; ++k)
                if (cmax[k] - cmin[k] > cmax[axis] - cmin[axis])
                    axis = k;
            const uint32_t mid = task.begin + n / 2u;
            std::nth_element(boxes.begin() + task.begin, boxes.begin() + mid, boxes.begin() + task.end,
                [axis](const InstanceBox& a, const InstanceBox& b) { return a.centroid[axis] < b.centroid[axis]; });
            const uint32_t left = static_cast<uint32_t>(m_tlasNodes.size());
            m_tlasNodes.emplace_back();
            m_tlasNodes.emplace_back();
            m_tlasNodes[task.node].a = left;
            m_tlasNodes[task.node].count = 0u;
            tasks.push_back({ left + 1u, mid, task.end });
            tasks.push_back({ left, task.begin, mid });
            continue;
        }

        int bestAxis = -1;
        uint32_t bestSplit = 0u;
        float bestCost = 1e30f;
        for (int axis = 0; axis < 3; ++axis)
        {
            const float extent = cmax[axis] - cmin[axis];
            if (!(extent > 1e-12f))
                continue;
            const float scale = static_cast<float>(kTlasSahBins) / extent;
            Bin bins[kTlasSahBins];
            for (uint32_t i = task.begin; i < task.end; ++i)
            {
                const InstanceBox& b = boxes[i];
                const uint32_t bin = std::min(static_cast<uint32_t>((b.centroid[axis] - cmin[axis]) * scale), kTlasSahBins - 1u);
                for (int k = 0; k < 3; ++k)
                {
                    bins[bin].min[k] = std::min(bins[bin].min[k], b.min[k]);
                    bins[bin].max[k] = std::max(bins[bin].max[k], b.max[k]);
                }
                ++bins[bin].n;
            }
            // One sweep from the right (areas and counts right of each split), one from the left.
            float    rightArea[kTlasSahBins] = {};
            uint32_t rightCount[kTlasSahBins] = {};
            Bin acc;
            for (uint32_t bin = kTlasSahBins - 1u; bin > 0u; --bin)
            {
                for (int k = 0; k < 3; ++k)
                {
                    acc.min[k] = std::min(acc.min[k], bins[bin].min[k]);
                    acc.max[k] = std::max(acc.max[k], bins[bin].max[k]);
                }
                acc.n += bins[bin].n;
                rightArea[bin] = halfArea(acc.min, acc.max);
                rightCount[bin] = acc.n;
            }
            Bin l;
            for (uint32_t split = 1u; split < kTlasSahBins; ++split)
            {
                const Bin& b = bins[split - 1u];
                for (int k = 0; k < 3; ++k)
                {
                    l.min[k] = std::min(l.min[k], b.min[k]);
                    l.max[k] = std::max(l.max[k], b.max[k]);
                }
                l.n += b.n;
                if (!l.n || !rightCount[split])
                    continue;
                const float cost = halfArea(l.min, l.max) * static_cast<float>(l.n) +
                                   rightArea[split] * static_cast<float>(rightCount[split]);
                if (cost < bestCost)
                {
                    bestCost = cost;
                    bestAxis = axis;
                    bestSplit = split;
                }
            }
        }

        uint32_t mid = task.begin;
        if (bestAxis >= 0)
        {
            const float scale = static_cast<float>(kTlasSahBins) / (cmax[bestAxis] - cmin[bestAxis]);
            const float lo = cmin[bestAxis];
            const auto it = std::partition(boxes.begin() + task.begin, boxes.begin() + task.end,
                [&](const InstanceBox& p)
                {
                    return std::min(static_cast<uint32_t>((p.centroid[bestAxis] - lo) * scale), kTlasSahBins - 1u) < bestSplit;
                });
            mid = static_cast<uint32_t>(it - boxes.begin());
        }
        if (mid == task.begin || mid == task.end)
            mid = task.begin + n / 2u;

        const uint32_t left = static_cast<uint32_t>(m_tlasNodes.size());
        m_tlasNodes.emplace_back();
        m_tlasNodes.emplace_back();
        m_tlasNodes[task.node].a = left;
        m_tlasNodes[task.node].count = 0u;
        tasks.push_back({ left + 1u, mid, task.end });
        tasks.push_back({ left, task.begin, mid });
    }
    m_sahCostAtRebuild = BvhSahCost(m_tlasNodes);
}

void SceneBvh::RefitTlas()
{
    for (uint32_t i = 0u; i < m_instances.size(); ++i)
        SetNodeBox(m_tlasNodes[m_leafOfInstance[i]], m_instances[i].worldBounds);
    // Children always have larger indices than their parent: one backward pass.
    for (size_t n = m_tlasNodes.size(); n-- > 0u;)
    {
        BvhNode& node = m_tlasNodes[n];
        if (node.count > 0u)
            continue;
        const BvhNode& l = m_tlasNodes[node.a];
        const BvhNode& r = m_tlasNodes[node.a + 1u];
        for (int k = 0; k < 3; ++k)
        {
            node.min[k] = std::min(l.min[k], r.min[k]);
            node.max[k] = std::max(l.max[k], r.max[k]);
        }
    }
}

// -----------------------------------------------------------------------------
// Two-level traversal (reference for the HLSL version in package 2). One stack:
// the TLAS loop keeps its entries below sp, the BLAS loop of a leaf instance
// uses the entries above.
// -----------------------------------------------------------------------------
bool SceneBvh::TraceClosest(const math::Vec3& origin, const math::Vec3& direction,
                            float tMin, float tMax, SceneRayHit& outHit,
                            BvhTraversalStats* stats) const
{
    if (m_tlasNodes.empty())
        return false;
    const math::Vec3 invDir = BvhSafeInverse(direction);
    float tRoot = 0.f;
    if (!BvhRayBox(m_tlasNodes[0].min, m_tlasNodes[0].max, origin, invDir, tMin, tMax, tRoot))
        return false;

    uint32_t stack[kBvhTraversalStackSize];
    uint32_t sp = 0u;
    stack[sp++] = 0u;
    float closest = tMax;
    bool found = false;
    BvhTriangleHit best{};
    uint32_t bestInstance = 0u;

    while (sp > 0u)
    {
        const BvhNode node = m_tlasNodes[stack[--sp]];
        if (stats) ++stats->nodesVisited;
        if (node.count > 0u)
        {
            const GpuBvhInstance& inst = m_gpuInstances[node.a];
            const math::Vec3 localOrigin = Rows3x4Point(inst.worldToObject, origin);
            const math::Vec3 localDir = Rows3x4Direction(inst.worldToObject, direction);
            BvhTriangleHit hit{};
            if (BvhTraverseClosest(m_blasNodes.data(), m_blasTriangles.data(),
                                   inst.blasNodeOffset, inst.blasTriangleOffset,
                                   localOrigin, localDir, tMin, closest, hit, stack, sp, stats))
            {
                best = hit;
                bestInstance = node.a;
                found = true;
            }
            continue;
        }
        const uint32_t left = node.a;
        const uint32_t right = node.a + 1u;
        float tLeft = 0.f, tRight = 0.f;
        const bool hitLeft = BvhRayBox(m_tlasNodes[left].min, m_tlasNodes[left].max, origin, invDir, tMin, closest, tLeft);
        const bool hitRight = BvhRayBox(m_tlasNodes[right].min, m_tlasNodes[right].max, origin, invDir, tMin, closest, tRight);
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
    if (!found)
        return false;

    const GpuBvhInstance& inst = m_gpuInstances[bestInstance];
    const BvhTriangle& tri = m_blasTriangles[best.triangle];
    const math::Vec3 w0 = Rows3x4Point(inst.objectToWorld, { tri.v0[0], tri.v0[1], tri.v0[2] });
    const math::Vec3 w1 = Rows3x4Point(inst.objectToWorld, { tri.v1[0], tri.v1[1], tri.v1[2] });
    const math::Vec3 w2 = Rows3x4Point(inst.objectToWorld, { tri.v2[0], tri.v2[1], tri.v2[2] });
    outHit.distance = best.t;
    outHit.instance = bestInstance;
    outHit.key = m_instances[bestInstance].key;
    outHit.entity = inst.entity;
    outHit.submesh = inst.submesh;
    outHit.triangle = tri.primitive;
    outHit.u = best.u;
    outHit.v = best.v;
    outHit.position = origin + direction * best.t;
    outHit.normal = math::Vec3::Cross(w1 - w0, w2 - w0).Normalized();
    return true;
}

bool SceneBvh::TraceAny(const math::Vec3& origin, const math::Vec3& direction,
                        float tMin, float tMax, BvhTraversalStats* stats) const
{
    if (m_tlasNodes.empty())
        return false;
    const math::Vec3 invDir = BvhSafeInverse(direction);
    float tRoot = 0.f;
    if (!BvhRayBox(m_tlasNodes[0].min, m_tlasNodes[0].max, origin, invDir, tMin, tMax, tRoot))
        return false;

    uint32_t stack[kBvhTraversalStackSize];
    uint32_t sp = 0u;
    stack[sp++] = 0u;
    while (sp > 0u)
    {
        const BvhNode node = m_tlasNodes[stack[--sp]];
        if (stats) ++stats->nodesVisited;
        if (node.count > 0u)
        {
            const GpuBvhInstance& inst = m_gpuInstances[node.a];
            const math::Vec3 localOrigin = Rows3x4Point(inst.worldToObject, origin);
            const math::Vec3 localDir = Rows3x4Direction(inst.worldToObject, direction);
            if (BvhTraverseAny(m_blasNodes.data(), m_blasTriangles.data(),
                               inst.blasNodeOffset, inst.blasTriangleOffset,
                               localOrigin, localDir, tMin, tMax, stack, sp, stats))
                return true;
            continue;
        }
        const uint32_t left = node.a;
        const uint32_t right = node.a + 1u;
        float tLeft = 0.f, tRight = 0.f;
        const bool hitLeft = BvhRayBox(m_tlasNodes[left].min, m_tlasNodes[left].max, origin, invDir, tMin, tMax, tLeft);
        const bool hitRight = BvhRayBox(m_tlasNodes[right].min, m_tlasNodes[right].max, origin, invDir, tMin, tMax, tRight);
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

} // namespace engine::collision
