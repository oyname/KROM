#pragma once
// =============================================================================
// KROM Engine - platform/StdTiming.hpp
//
// Plattformneutrale Timing-Implementierung auf Basis von std::chrono.
// Für produktive Loops und Beispiele geeignet.
// =============================================================================
#include "platform/IPlatformTiming.hpp"
#include <chrono>

namespace engine::platform {

class StdTiming final : public IPlatformTiming
{
    using Clock = std::chrono::high_resolution_clock;
    using TP    = std::chrono::time_point<Clock>;

public:
    StdTiming()
        : m_start(Clock::now())
        , m_last(m_start)
    {
    }

    void BeginFrame() override
    {
        const TP now = Clock::now();
        const double raw = std::chrono::duration<double>(now - m_last).count();
        m_last = now;
        m_delta = (raw > m_maxDelta) ? m_maxDelta : raw;
        m_time = std::chrono::duration<double>(now - m_start).count();
        ++m_frame;

        m_fpsAcc += m_delta;
        if (++m_fpsCnt >= 60u)
        {
            m_fps = (m_fpsAcc > 0.0) ? static_cast<float>(60.0 / m_fpsAcc) : 0.0f;
            m_fpsAcc = 0.0;
            m_fpsCnt = 0u;
        }
    }

    void EndFrame() override {}

    [[nodiscard]] double GetTimeSeconds() const override { return m_time; }
    [[nodiscard]] double GetDeltaSeconds() const override { return m_delta; }
    [[nodiscard]] float GetDeltaSecondsF() const override { return static_cast<float>(m_delta); }
    [[nodiscard]] float GetTimeSecondsF() const override { return static_cast<float>(m_time); }
    [[nodiscard]] uint64_t GetFrameCount() const override { return m_frame; }
    [[nodiscard]] float GetSmoothedFPS() const override { return m_fps; }
    [[nodiscard]] double GetRawTimestampSeconds() const override
    {
        return std::chrono::duration<double>(Clock::now() - m_start).count();
    }

    void SetMaxDeltaSeconds(double v) override { m_maxDelta = v; }

private:
    TP m_start;
    TP m_last;
    double m_time = 0.0;
    double m_delta = 0.0;
    double m_maxDelta = 0.25;
    uint64_t m_frame = 0ull;
    float m_fps = 0.0f;
    double m_fpsAcc = 0.0;
    uint32_t m_fpsCnt = 0u;
};

} // namespace engine::platform
