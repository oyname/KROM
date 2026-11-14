#pragma once
// =============================================================================
// KROM Engine - renderer/GpuPassProfiler.hpp
// GPU time per render-graph pass from backend timestamps (IDevice /
// ICommandList::WriteGpuTimestamp). Results arrive a few frames late and are
// smoothed over about half a second. Off by default; while off nothing is
// recorded and no queries exist.
// =============================================================================
#include "renderer/IDevice.hpp"
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <vector>

namespace engine::renderer {

struct GpuPassTiming
{
    std::string name;
    float       ms = 0.0f;   // smoothed GPU time, summed over all views of a frame
};

struct GpuPassTimingReport
{
    bool                       enabled   = false;
    bool                       supported = false;  // backend has timestamps
    float                      totalMs   = 0.0f;   // sum of the passes (smoothed)
    std::vector<GpuPassTiming> passes;              // order of the last measured frame
};

class GpuPassProfiler
{
public:
    // Smoothing time constant in seconds (exponential, ~63 % after this time).
    static constexpr double kSmoothingSeconds = 0.5;
    // Pending frames older than this are dropped (results never arrived).
    static constexpr uint64_t kMaxPendingFrames = 16u;

    void SetEnabled(IDevice& device, bool enabled);
    [[nodiscard]] bool IsEnabled() const noexcept { return m_enabled; }

    // Called around each pass while recording. begin/end come from
    // ICommandList::WriteGpuTimestamp; invalid indices are ignored.
    void RecordPass(uint64_t frameId, const std::string& name, uint32_t beginIndex, uint32_t endIndex);

    // Pulls finished frames from the device and updates the smoothed values.
    // nowSeconds: any monotonic clock, used for the smoothing.
    void Collect(IDevice& device, double nowSeconds);

    [[nodiscard]] GpuPassTimingReport GetReport() const;

    // Applies one finished frame; public for tests.
    void ApplyFrame(const GpuTimestampFrame& frame, double nowSeconds);

private:
    struct PassRecord
    {
        std::string name;
        uint32_t    begin = kInvalidGpuTimestamp;
        uint32_t    end   = kInvalidGpuTimestamp;
    };
    struct PendingFrame
    {
        uint64_t                frameId = 0u;
        std::vector<PassRecord> passes;
    };
    struct Smoothed
    {
        std::string name;
        double      ms = 0.0;
        uint64_t    lastFrameId = 0u;
    };

    bool                     m_requested = false;
    bool                     m_enabled   = false;
    bool                     m_supported = false;
    std::deque<PendingFrame> m_pending;
    std::vector<Smoothed>    m_smoothed;
    std::vector<std::string> m_lastOrder;
    double                   m_lastApplySeconds = -1.0;
    uint64_t                 m_lastAppliedFrameId = 0u;
    std::vector<GpuTimestampFrame> m_scratch;
    mutable std::mutex       m_reportMutex;
};

} // namespace engine::renderer
