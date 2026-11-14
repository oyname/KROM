// =============================================================================
// KROM Engine - src/renderer/GpuPassProfiler.cpp
// =============================================================================
#include "renderer/GpuPassProfiler.hpp"
#include "core/Debug.hpp"
#include <algorithm>
#include <cmath>

namespace engine::renderer {

void GpuPassProfiler::SetEnabled(IDevice& device, bool enabled)
{
    // Repeated calls with the same request are free (no repeated warnings).
    if (enabled == m_requested)
        return;
    m_requested = enabled;
    m_supported = device.GetGpuTimestampCapacity() > 0u;
    const bool effective = enabled && m_supported;
    if (enabled && !m_supported)
        Debug::LogWarning("GpuPassProfiler.cpp: backend '%s' has no GPU timestamps, pass timing stays off",
                          device.GetBackendName());
    if (effective == m_enabled)
        return;

    device.SetGpuTimestampsEnabled(effective);
    m_enabled = effective;
    m_pending.clear();
    m_scratch.clear();
    std::lock_guard<std::mutex> lock(m_reportMutex);
    m_smoothed.clear();
    m_lastOrder.clear();
    m_lastApplySeconds = -1.0;
    m_lastAppliedFrameId = 0u;
}

void GpuPassProfiler::RecordPass(uint64_t frameId, const std::string& name, uint32_t beginIndex, uint32_t endIndex)
{
    if (!m_enabled || beginIndex == kInvalidGpuTimestamp || endIndex == kInvalidGpuTimestamp)
        return;

    if (m_pending.empty() || m_pending.back().frameId != frameId)
    {
        m_pending.push_back(PendingFrame{ frameId, {} });
        while (m_pending.size() > kMaxPendingFrames)
            m_pending.pop_front();
    }
    m_pending.back().passes.push_back(PassRecord{ name, beginIndex, endIndex });
}

void GpuPassProfiler::Collect(IDevice& device, double nowSeconds)
{
    if (!m_enabled)
        return;
    m_scratch.clear();
    device.TakeCompletedGpuTimestamps(m_scratch);
    for (const GpuTimestampFrame& frame : m_scratch)
        ApplyFrame(frame, nowSeconds);
}

void GpuPassProfiler::ApplyFrame(const GpuTimestampFrame& frame, double nowSeconds)
{
    // Drop pending frames that are older than this one; their results are lost.
    while (!m_pending.empty() && m_pending.front().frameId < frame.frameId)
        m_pending.pop_front();
    if (m_pending.empty() || m_pending.front().frameId != frame.frameId)
        return;

    const PendingFrame pending = std::move(m_pending.front());
    m_pending.pop_front();

    // Sum per pass name: a frame may render the same pass for several views.
    std::vector<std::pair<std::string, double>> sums;
    for (const PassRecord& pass : pending.passes)
    {
        const size_t count = frame.timestampsMs.size();
        if (pass.begin >= count || pass.end >= count)
            continue;
        if (pass.begin < frame.valid.size() && !frame.valid[pass.begin])
            continue;
        if (pass.end < frame.valid.size() && !frame.valid[pass.end])
            continue;
        const double ms = std::max(0.0, frame.timestampsMs[pass.end] - frame.timestampsMs[pass.begin]);
        auto it = std::find_if(sums.begin(), sums.end(), [&](const auto& e) { return e.first == pass.name; });
        if (it == sums.end())
            sums.emplace_back(pass.name, ms);
        else
            it->second += ms;
    }
    if (sums.empty())
        return;

    std::lock_guard<std::mutex> lock(m_reportMutex);
    const double dt = (m_lastApplySeconds < 0.0) ? 0.0 : std::max(0.0, nowSeconds - m_lastApplySeconds);
    const double alpha = (m_lastApplySeconds < 0.0) ? 1.0 : (1.0 - std::exp(-dt / kSmoothingSeconds));
    m_lastApplySeconds = nowSeconds;
    m_lastAppliedFrameId = frame.frameId;

    m_lastOrder.clear();
    for (const auto& [name, ms] : sums)
    {
        m_lastOrder.push_back(name);
        auto it = std::find_if(m_smoothed.begin(), m_smoothed.end(), [&](const Smoothed& s) { return s.name == name; });
        if (it == m_smoothed.end())
            m_smoothed.push_back(Smoothed{ name, ms, frame.frameId });
        else
        {
            it->ms += (ms - it->ms) * alpha;
            it->lastFrameId = frame.frameId;
        }
    }
    // Passes that stopped running fade out of the list.
    m_smoothed.erase(std::remove_if(m_smoothed.begin(), m_smoothed.end(),
                                    [&](const Smoothed& s) { return s.lastFrameId + kMaxPendingFrames < frame.frameId; }),
                     m_smoothed.end());
}

GpuPassTimingReport GpuPassProfiler::GetReport() const
{
    GpuPassTimingReport report;
    report.enabled = m_enabled;
    report.supported = m_supported;
    std::lock_guard<std::mutex> lock(m_reportMutex);
    for (const std::string& name : m_lastOrder)
    {
        auto it = std::find_if(m_smoothed.begin(), m_smoothed.end(), [&](const Smoothed& s) { return s.name == name; });
        if (it == m_smoothed.end())
            continue;
        report.passes.push_back(GpuPassTiming{ it->name, static_cast<float>(it->ms) });
        report.totalMs += static_cast<float>(it->ms);
    }
    return report;
}

} // namespace engine::renderer
