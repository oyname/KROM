// =============================================================================
// KROM Engine - src/renderer/RenderWorld.cpp
// Extract-Phase, Frustum-Culling, DrawList-Aufbau, Radix-Sort.
// =============================================================================
#include "renderer/RenderWorld.hpp"
#include "renderer/RenderPassRegistry.hpp"
#include "core/Debug.hpp"
#include "jobs/JobSystem.hpp"
#include <cstring>
#include <algorithm>
#include <cmath>

namespace engine::renderer {

RenderFeatureDataSlot RenderFeatureDataRegistry::Register(std::type_index type, std::string name)
{
    for (RenderFeatureDataSlot slot = 0u; slot < m_entries.size(); ++slot)
    {
        if (m_entries[slot].type == type)
        {
            if (m_entries[slot].name.empty() && !name.empty())
                m_entries[slot].name = std::move(name);
            return slot;
        }
    }

    m_entries.push_back(Entry{type, std::move(name)});
    return static_cast<RenderFeatureDataSlot>(m_entries.size() - 1u);
}

const RenderFeatureDataRegistry::Entry* RenderFeatureDataRegistry::Get(RenderFeatureDataSlot slot) const noexcept
{
    return slot < m_entries.size() ? &m_entries[slot] : nullptr;
}

RenderFeatureDataSlot RenderFeatureDataRegistry::Find(std::type_index type) const noexcept
{
    for (RenderFeatureDataSlot slot = 0u; slot < m_entries.size(); ++slot)
    {
        if (m_entries[slot].type == type)
            return slot;
    }
    return static_cast<RenderFeatureDataSlot>(-1);
}

// =============================================================================
// DrawList::Sort - Radix-Sort 64-Bit nach SortKey (2-Pass, 32 Bit je Pass)
// Stabiler als std::sort für große Listen; fällt auf std::sort zurück wenn klein.
// =============================================================================
static void RadixSort64(std::vector<DrawItem>& items)
{
    const size_t n = items.size();
    if (n <= 32u) { std::sort(items.begin(), items.end()); return; }

    std::vector<DrawItem> tmp(n);

    // Pass 1: untere 32 Bit
    {
        uint32_t counts[256] = {};
        for (const auto& it : items)
            ++counts[it.sortKey.value & 0xFFu];
        uint32_t offsets[256] = {};
        for (int i = 1; i < 256; ++i)
            offsets[i] = offsets[i-1] + counts[i-1];
        for (const auto& it : items)
            tmp[offsets[it.sortKey.value & 0xFFu]++] = it;

        items.swap(tmp);
        std::fill(std::begin(counts), std::end(counts), 0u);

        for (const auto& it : items)
            ++counts[(it.sortKey.value >> 8u) & 0xFFu];
        std::fill(std::begin(offsets), std::end(offsets), 0u);
        for (int i = 1; i < 256; ++i)
            offsets[i] = offsets[i-1] + counts[i-1];
        for (const auto& it : items)
            tmp[offsets[(it.sortKey.value >> 8u) & 0xFFu]++] = it;

        items.swap(tmp);
        std::fill(std::begin(counts), std::end(counts), 0u);

        for (const auto& it : items)
            ++counts[(it.sortKey.value >> 16u) & 0xFFu];
        std::fill(std::begin(offsets), std::end(offsets), 0u);
        for (int i = 1; i < 256; ++i)
            offsets[i] = offsets[i-1] + counts[i-1];
        for (const auto& it : items)
            tmp[offsets[(it.sortKey.value >> 16u) & 0xFFu]++] = it;

        items.swap(tmp);
        std::fill(std::begin(counts), std::end(counts), 0u);

        for (const auto& it : items)
            ++counts[(it.sortKey.value >> 24u) & 0xFFu];
        std::fill(std::begin(offsets), std::end(offsets), 0u);
        for (int i = 1; i < 256; ++i)
            offsets[i] = offsets[i-1] + counts[i-1];
        for (const auto& it : items)
            tmp[offsets[(it.sortKey.value >> 24u) & 0xFFu]++] = it;
        items.swap(tmp);
    }

    // Pass 2: obere 32 Bit
    {
        uint32_t counts[256] = {};
        for (const auto& it : items)
            ++counts[(it.sortKey.value >> 32u) & 0xFFu];
        uint32_t offsets[256] = {};
        for (int i = 1; i < 256; ++i)
            offsets[i] = offsets[i-1] + counts[i-1];
        for (const auto& it : items)
            tmp[offsets[(it.sortKey.value >> 32u) & 0xFFu]++] = it;
        items.swap(tmp);
        std::fill(std::begin(counts), std::end(counts), 0u);

        for (const auto& it : items)
            ++counts[(it.sortKey.value >> 40u) & 0xFFu];
        std::fill(std::begin(offsets), std::end(offsets), 0u);
        for (int i = 1; i < 256; ++i)
            offsets[i] = offsets[i-1] + counts[i-1];
        for (const auto& it : items)
            tmp[offsets[(it.sortKey.value >> 40u) & 0xFFu]++] = it;
        items.swap(tmp);
        std::fill(std::begin(counts), std::end(counts), 0u);

        for (const auto& it : items)
            ++counts[(it.sortKey.value >> 48u) & 0xFFu];
        std::fill(std::begin(offsets), std::end(offsets), 0u);
        for (int i = 1; i < 256; ++i)
            offsets[i] = offsets[i-1] + counts[i-1];
        for (const auto& it : items)
            tmp[offsets[(it.sortKey.value >> 48u) & 0xFFu]++] = it;
        items.swap(tmp);
        std::fill(std::begin(counts), std::end(counts), 0u);

        for (const auto& it : items)
            ++counts[(it.sortKey.value >> 56u) & 0xFFu];
        std::fill(std::begin(offsets), std::end(offsets), 0u);
        for (int i = 1; i < 256; ++i)
            offsets[i] = offsets[i-1] + counts[i-1];
        for (const auto& it : items)
            tmp[offsets[(it.sortKey.value >> 56u) & 0xFFu]++] = it;
        items.swap(tmp);
    }
}

void DrawList::Sort()
{
    if (sorted || items.empty()) return;
    RadixSort64(items);
    sorted = true;
}

// =============================================================================
// RenderWorld - AddRenderable
// =============================================================================

void RenderWorld::AddRenderable(EntityID entity, MeshHandle mesh, MaterialHandle material,
                                 const math::Mat4& worldMatrix, const math::Mat4& worldMatrixInvT,
                                 const math::Vec3& boundsCenter, const math::Vec3& boundsExtents,
                                 float boundsRadius, uint32_t layerMask, bool castShadows)
{
    RenderProxy proxy;
    proxy.entity          = entity;
    proxy.mesh            = mesh;
    proxy.material        = material;
    proxy.worldMatrix     = worldMatrix;
    proxy.worldMatrixInvT = worldMatrixInvT;
    proxy.boundsCenter    = boundsCenter;
    proxy.boundsExtents   = boundsExtents;
    proxy.boundsRadius    = boundsRadius;
    proxy.layerMask       = layerMask;
    proxy.castShadows     = castShadows;
    proxy.visible         = true;
    m_proxies.push_back(proxy);
}

// =============================================================================
// Frustum-Culling
// =============================================================================

void RenderWorld::ExtractFrustumPlanes(const math::Mat4& vp, math::Vec4 planes[6]) noexcept
{
    // Gribb-Hartmann: Planes aus ViewProj-Zeilen
    // vp ist column-major: vp.m[col][row]
    // Plane i: kombiniere Zeilen der Matrix
    auto row = [&](int r) -> math::Vec4 {
        return { vp.m[0][r], vp.m[1][r], vp.m[2][r], vp.m[3][r] };
    };
    const math::Vec4 r0 = row(0), r1 = row(1), r2 = row(2), r3 = row(3);

    planes[0] = { r3.x+r0.x, r3.y+r0.y, r3.z+r0.z, r3.w+r0.w }; // Left
    planes[1] = { r3.x-r0.x, r3.y-r0.y, r3.z-r0.z, r3.w-r0.w }; // Right
    planes[2] = { r3.x+r1.x, r3.y+r1.y, r3.z+r1.z, r3.w+r1.w }; // Bottom
    planes[3] = { r3.x-r1.x, r3.y-r1.y, r3.z-r1.z, r3.w-r1.w }; // Top
    planes[4] = { r2.x,      r2.y,      r2.z,      r2.w      }; // Near
    planes[5] = { r3.x-r2.x, r3.y-r2.y, r3.z-r2.z, r3.w-r2.w }; // Far

    // Normalisieren
    for (int i = 0; i < 6; ++i)
    {
        const float len = std::sqrt(planes[i].x*planes[i].x
                                  + planes[i].y*planes[i].y
                                  + planes[i].z*planes[i].z);
        if (len > 1e-6f) planes[i] = planes[i] * (1.f / len);
    }
}

bool RenderWorld::FrustumTest(const math::Vec3& center,
                               const math::Vec3& extents,
                               const math::Vec4  planes[6]) noexcept
{
    for (int i = 0; i < 6; ++i)
    {
        const math::Vec3 n{ planes[i].x, planes[i].y, planes[i].z };
        // Radius der AABB in Richtung der Plane-Normale
        const float r = std::abs(extents.x * n.x)
                       + std::abs(extents.y * n.y)
                       + std::abs(extents.z * n.z);
        const float d = math::Vec3::Dot(n, center) + planes[i].w;
        if (d + r < 0.f) return false; // komplett hinter Plane
    }
    return true;
}

float RenderWorld::ComputeLinearDepth(const math::Vec3& worldPos,
                                       const math::Mat4& view,
                                       float nearZ, float farZ) noexcept
{
    // view ist column-major (m[col][row]).
    // p_view.z = view[0][2]*x + view[1][2]*y + view[2][2]*z + view[3][2]
    // In right-handed space: Kamera schaut in -Z.
    // Tiefe vor Kamera = -p_view.z (positiv für sichtbare Objekte).
    const float viewZ = view.m[0][2] * worldPos.x
                      + view.m[1][2] * worldPos.y
                      + view.m[2][2] * worldPos.z
                      + view.m[3][2];
    const float depth = -viewZ; // positiv vor der Kamera
    const float span  = farZ - nearZ;
    if (span < 1e-6f) return 0.f;
    return std::max(0.f, std::min(1.f, (depth - nearZ) / span));
}

// =============================================================================
// RenderWorld - BuildDrawLists
// =============================================================================

DrawItem RenderWorld::BuildDrawItem(const RenderProxy& proxy,
                                     const MaterialSystem& materials,
                                     const RenderPassRegistry& renderPassRegistry,
                                     float linearDepth,
                                     bool  isShadow,
                                     uint32_t submissionOrder) const noexcept
{
    DrawItem item;
    item.mesh     = proxy.mesh;
    item.material = proxy.material;
    item.entity   = proxy.entity;

    const MaterialInstance* inst = materials.GetInstance(proxy.material);
    const RenderPassID pass = isShadow
        ? StandardRenderPasses::Shadow()
        : (inst ? inst->RenderPass() : StandardRenderPasses::Opaque());

    const uint32_t pipeHash = inst ? inst->pipelineKeyHash : 0u;
    const uint32_t materialKey = proxy.material.IsValid() ? proxy.material.Index() : 0u;
    const uint8_t  layer    = 0u;
    const RenderPassSortMode sortMode = RenderPassSort(renderPassRegistry, pass);

    switch (sortMode)
    {
    case RenderPassSortMode::BackToFront:
        item.sortKey = SortKey::ForBackToFront(pass, layer, linearDepth);
        break;
    case RenderPassSortMode::SubmissionOrder:
        item.sortKey = SortKey::ForSubmissionOrder(pass, layer, submissionOrder);
        break;
    case RenderPassSortMode::FrontToBack:
    default:
        item.sortKey = SortKey::ForFrontToBack(pass, layer, pipeHash, materialKey, linearDepth);
        break;
    }

    // Per-Object CB: Index in objectConstants-Array
    // (wird von BuildDrawLists befüllt, hier nur Slot reservieren)
    item.cbOffset = 0u;
    item.cbSize   = sizeof(PerObjectConstants);
    return item;
}

void RenderWorld::BuildDrawLists(const math::Mat4& view,
                                  const math::Mat4& viewProj,
                                  float              nearZ,
                                  float              farZ,
                                  const MaterialSystem& materials,
                                  const RenderPassRegistry& renderPassRegistry,
                                  uint32_t           layerMask,
                                  jobs::JobSystem*   jobSystem)
{
    m_queue.Clear();
    m_visibleCount = 0u;

    if (m_proxies.empty())
        return;

    math::Vec4 frustumPlanes[6];
    ExtractFrustumPlanes(viewProj, frustumPlanes);

    const size_t n = m_proxies.size();
    constexpr size_t kMinBatch = 64u;
    const bool useParallel = jobSystem && jobSystem->IsParallel() && n >= 256u;

    if (!useParallel)
    {
        m_queue.objectConstants.reserve(n);
        for (size_t i = 0u; i < n; ++i)
        {
            RenderProxy& proxy = m_proxies[i];
            proxy.visible = false;
            if ((proxy.layerMask & layerMask) == 0u) continue;
            if (!FrustumTest(proxy.boundsCenter, proxy.boundsExtents, frustumPlanes)) continue;

            proxy.visible = true;
            ++m_visibleCount;

            const float depth = ComputeLinearDepth(proxy.boundsCenter, view, nearZ, farZ);

            PerObjectConstants objCb;
            std::memcpy(objCb.worldMatrix,     proxy.worldMatrix.Data(),     64u);
            std::memcpy(objCb.worldMatrixInvT, proxy.worldMatrixInvT.Data(), 64u);
            objCb.entityId[0] = static_cast<float>(proxy.entity.Index());
            objCb.entityId[1] = objCb.entityId[2] = objCb.entityId[3] = 0.f;

            const uint32_t cbIdx = static_cast<uint32_t>(m_queue.objectConstants.size());
            m_queue.objectConstants.push_back(objCb);

            const MaterialInstance* inst = materials.GetInstance(proxy.material);
            const RenderPassID pass = inst ? inst->RenderPass() : StandardRenderPasses::Opaque();

            DrawItem item = BuildDrawItem(proxy, materials, renderPassRegistry, depth, false, cbIdx);
            item.cbOffset = cbIdx;
            m_queue.GetOrCreateList(pass).Add(std::move(item));

            const MaterialDesc* matDesc = materials.GetDesc(proxy.material);
            if (proxy.castShadows && (matDesc ? matDesc->castShadows : true))
            {
                DrawItem shadowItem = BuildDrawItem(proxy, materials, renderPassRegistry, depth, true, cbIdx);
                shadowItem.cbOffset = cbIdx;
                m_queue.GetOrCreateList(StandardRenderPasses::Shadow()).Add(std::move(shadowItem));
            }
        }
        m_queue.SortAll();
        return;
    }

    // Parallel path
    // Batch-Layout spiegelt ParallelForImpl exakt: begin = i * batchSize → batchIdx = begin / batchSize.
    const size_t workerCount = jobSystem->WorkerCount();
    const size_t numBatches  = std::min(workerCount, (n + kMinBatch - 1u) / kMinBatch);
    const size_t batchSize   = (n + numBatches - 1u) / numBatches;

    struct BatchEntry {
        uint32_t           proxyIdx;
        PerObjectConstants objCb;
        RenderPassID       opaquePass;
        DrawItem           opaqueItem;
        DrawItem           shadowItem;
        bool               hasShadow;
    };

    // Visible-Flags vorab zurücksetzen (disjunkt im Parallel-Pass gesetzt)
    for (auto& proxy : m_proxies)
        proxy.visible = false;

    std::vector<std::vector<BatchEntry>> batchResults(numBatches);

    jobSystem->ParallelFor(n,
        [&](size_t begin, size_t end)
        {
            const size_t batchIdx = begin / batchSize;
            auto& results = batchResults[batchIdx];
            results.reserve(end - begin);

            for (size_t i = begin; i < end; ++i)
            {
                const RenderProxy& proxy = m_proxies[i];
                if ((proxy.layerMask & layerMask) == 0u) continue;
                if (!FrustumTest(proxy.boundsCenter, proxy.boundsExtents, frustumPlanes)) continue;

                const float depth = ComputeLinearDepth(proxy.boundsCenter, view, nearZ, farZ);

                PerObjectConstants objCb;
                std::memcpy(objCb.worldMatrix,     proxy.worldMatrix.Data(),     64u);
                std::memcpy(objCb.worldMatrixInvT, proxy.worldMatrixInvT.Data(), 64u);
                objCb.entityId[0] = static_cast<float>(proxy.entity.Index());
                objCb.entityId[1] = objCb.entityId[2] = objCb.entityId[3] = 0.f;

                const MaterialInstance* inst = materials.GetInstance(proxy.material);
                const RenderPassID opaquePass = inst ? inst->RenderPass() : StandardRenderPasses::Opaque();

                // Proxy-Index als submissionOrder → stabiler Sort auch ohne cbIdx
                DrawItem opaqueItem = BuildDrawItem(proxy, materials, renderPassRegistry, depth, false,
                                                    static_cast<uint32_t>(i));

                const MaterialDesc* matDesc = materials.GetDesc(proxy.material);
                const bool hasShadow = proxy.castShadows && (matDesc ? matDesc->castShadows : true);

                DrawItem shadowItem{};
                if (hasShadow)
                    shadowItem = BuildDrawItem(proxy, materials, renderPassRegistry, depth, true,
                                              static_cast<uint32_t>(i));

                results.push_back(BatchEntry{
                    static_cast<uint32_t>(i),
                    objCb,
                    opaquePass,
                    std::move(opaqueItem),
                    std::move(shadowItem),
                    hasShadow
                });
            }
        },
        kMinBatch);

    // Serieller Merge: stabile cbOffset-Zuweisung in Batch-Reihenfolge
    size_t totalVisible = 0u;
    for (const auto& batch : batchResults)
        totalVisible += batch.size();

    m_queue.objectConstants.reserve(totalVisible);
    m_visibleCount = static_cast<uint32_t>(totalVisible);

    uint32_t cbIdx = 0u;
    for (auto& batch : batchResults)
    {
        for (auto& entry : batch)
        {
            m_proxies[entry.proxyIdx].visible = true;
            m_queue.objectConstants.push_back(entry.objCb);

            entry.opaqueItem.cbOffset = cbIdx;
            m_queue.GetOrCreateList(entry.opaquePass).Add(std::move(entry.opaqueItem));

            if (entry.hasShadow)
            {
                entry.shadowItem.cbOffset = cbIdx;
                m_queue.GetOrCreateList(StandardRenderPasses::Shadow()).Add(std::move(entry.shadowItem));
            }
            ++cbIdx;
        }
    }

    m_queue.SortAll();
}

} // namespace engine::renderer
