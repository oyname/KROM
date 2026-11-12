// =============================================================================
// KROM Engine - addons/dx11/DX11GpuTimestamps.cpp
// =============================================================================
#include "DX11GpuTimestamps.hpp"
#include "core/Debug.hpp"
#include <algorithm>
#ifdef _WIN32
#   define WIN32_LEAN_AND_MEAN
#   define NOMINMAX
#   include <windows.h>
#   include <d3d11.h>
#endif

namespace engine::renderer::dx11 {

void DX11GpuTimestamps::Init(ID3D11Device* device, ID3D11DeviceContext* context) noexcept
{
    m_device = device;
    m_context = context;
}

void DX11GpuTimestamps::Release() noexcept
{
    DestroyQueries();
    m_enabled = false;
    m_device = nullptr;
    m_context = nullptr;
}

void DX11GpuTimestamps::SetEnabled(bool enabled)
{
    if (enabled == m_enabled)
        return;
    if (enabled)
    {
        if (!CreateQueries())
        {
            Debug::LogWarning("DX11GpuTimestamps.cpp: creating timestamp queries failed, GPU timing stays off");
            DestroyQueries();
            return;
        }
        m_enabled = true;
        return;
    }
    // Close an open frame so no Begin is left dangling, then drop everything.
#ifdef _WIN32
    for (FrameSet& set : m_sets)
        if (set.open && m_context)
            m_context->End(set.disjoint);
#endif
    DestroyQueries();
    m_enabled = false;
}

bool DX11GpuTimestamps::CreateQueries()
{
#ifdef _WIN32
    if (!m_device)
        return false;
    m_sets.assign(kFrameSets, FrameSet{});
    for (FrameSet& set : m_sets)
    {
        D3D11_QUERY_DESC disjointDesc{ D3D11_QUERY_TIMESTAMP_DISJOINT, 0u };
        if (FAILED(m_device->CreateQuery(&disjointDesc, &set.disjoint)))
            return false;
        set.stamps.assign(kCapacity, nullptr);
        D3D11_QUERY_DESC stampDesc{ D3D11_QUERY_TIMESTAMP, 0u };
        for (ID3D11Query*& query : set.stamps)
            if (FAILED(m_device->CreateQuery(&stampDesc, &query)))
                return false;
    }
    m_current = 0u;
    return true;
#else
    return false;
#endif
}

void DX11GpuTimestamps::DestroyQueries() noexcept
{
#ifdef _WIN32
    for (FrameSet& set : m_sets)
    {
        if (set.disjoint)
            set.disjoint->Release();
        for (ID3D11Query* query : set.stamps)
            if (query)
                query->Release();
    }
#endif
    m_sets.clear();
    m_current = 0u;
}

void DX11GpuTimestamps::BeginFrame(uint64_t frameId)
{
#ifdef _WIN32
    if (!m_enabled || m_sets.empty() || !m_context)
        return;
    // A frame without EndFrame (skipped present) still closes its disjoint query.
    EndFrame();

    m_current = (m_current + 1u) % static_cast<uint32_t>(m_sets.size());
    FrameSet& set = m_sets[m_current];
    // Results never read in time: the set is reused and the frame is lost.
    set.pending = false;
    set.frameId = frameId;
    set.written = 0u;
    m_context->Begin(set.disjoint);
    set.open = true;
#else
    (void)frameId;
#endif
}

void DX11GpuTimestamps::EndFrame()
{
#ifdef _WIN32
    if (!m_enabled || m_sets.empty() || !m_context)
        return;
    FrameSet& set = m_sets[m_current];
    if (!set.open)
        return;
    m_context->End(set.disjoint);
    set.open = false;
    set.pending = set.written > 0u;
#endif
}

uint32_t DX11GpuTimestamps::Write()
{
#ifdef _WIN32
    if (!m_enabled || m_sets.empty() || !m_context)
        return kInvalidGpuTimestamp;
    FrameSet& set = m_sets[m_current];
    if (!set.open || set.written >= kCapacity)
        return kInvalidGpuTimestamp;
    m_context->End(set.stamps[set.written]);
    return set.written++;
#else
    return kInvalidGpuTimestamp;
#endif
}

void DX11GpuTimestamps::TakeCompleted(std::vector<GpuTimestampFrame>& out)
{
#ifdef _WIN32
    if (!m_enabled || !m_context)
        return;

    std::vector<FrameSet*> ready;
    for (FrameSet& set : m_sets)
        if (set.pending)
            ready.push_back(&set);
    std::sort(ready.begin(), ready.end(), [](const FrameSet* a, const FrameSet* b) { return a->frameId < b->frameId; });

    for (FrameSet* set : ready)
    {
        D3D11_QUERY_DATA_TIMESTAMP_DISJOINT disjoint{};
        if (m_context->GetData(set->disjoint, &disjoint, sizeof(disjoint), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK)
            break;  // later frames are not ready either
        set->pending = false;
        if (disjoint.Disjoint || disjoint.Frequency == 0u)
            continue;

        GpuTimestampFrame frame;
        frame.frameId = set->frameId;
        frame.timestampsMs.assign(set->written, 0.0);
        frame.valid.assign(set->written, false);
        const double toMs = 1000.0 / static_cast<double>(disjoint.Frequency);
        uint64_t origin = 0u;
        bool haveOrigin = false;
        for (uint32_t i = 0u; i < set->written; ++i)
        {
            UINT64 ticks = 0u;
            if (m_context->GetData(set->stamps[i], &ticks, sizeof(ticks), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK)
                continue;
            if (!haveOrigin)
            {
                origin = ticks;
                haveOrigin = true;
            }
            frame.timestampsMs[i] = static_cast<double>(static_cast<int64_t>(ticks - origin)) * toMs;
            frame.valid[i] = true;
        }
        out.push_back(std::move(frame));
    }
#else
    (void)out;
#endif
}

} // namespace engine::renderer::dx11
