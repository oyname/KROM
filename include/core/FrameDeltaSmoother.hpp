#pragma once
// =============================================================================
// KROM Engine - core/FrameDeltaSmoother.hpp
// Frame delta for game logic, matched to when frames reach the screen.
//
// With vsync the frames reach the screen at a steady rate, but the time the
// CPU measures per frame can alternate strongly (measured in the editor:
// 2.8 ms / 14.5 ms at an average of 6.9 ms on a 144 Hz monitor). Game logic
// that moves by "speed * delta" then steps unevenly and fast-moving parts
// visibly shake. See A. Ladavac, "The Elusive Frame Timing", GDC 2018.
//
// FrameDeltaSmoother: moving average of the last few deltas. Once the window
// is full every raw delta counts exactly once in total, so game time does not
// drift from real time; a long frame (hitch) is spread over the next frames.
//
// FrameDeltaClock: with vsync and a known refresh rate, every frame advances
// by a whole number of refresh periods - one normally, two or more after a
// missed vblank. The number follows the measured real time, so game time
// stays in step with the clock. The period itself is measured while locked
// (the reported rate only seeds it): some platforms report whole hertz only,
// e.g. 59 or 60 for 59.94 Hz. Without vsync, with an unknown rate or when
// frames do not sit on refresh multiples (variable refresh rate, irregular
// load) it falls back to the smoothed delta.
// =============================================================================
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>

namespace engine {

class FrameDeltaSmoother
{
public:
    static constexpr size_t kWindow = 8u;

    // Returns the smoothed delta for this frame. Negative input counts as 0.
    [[nodiscard]] float Smooth(float rawDeltaSeconds) noexcept
    {
        const double raw = rawDeltaSeconds > 0.f ? static_cast<double>(rawDeltaSeconds) : 0.0;
        m_sum -= m_samples[m_next];
        m_samples[m_next] = raw;
        m_sum += raw;
        m_next = (m_next + 1u) % kWindow;
        if (m_count < kWindow)
            ++m_count;
        const double average = m_sum / static_cast<double>(m_count);
        return average > 0.0 ? static_cast<float>(average) : 0.f;
    }

    void Reset() noexcept { *this = FrameDeltaSmoother{}; }
    [[nodiscard]] bool IsFull() const noexcept { return m_count == kWindow; }

private:
    std::array<double, kWindow> m_samples{};
    double m_sum = 0.0;
    size_t m_next = 0u;
    size_t m_count = 0u;
};

class FrameDeltaClock
{
public:
    // refreshHz <= 0: unknown. vsync false: frames are not tied to the display.
    // May change every frame (window moved to another monitor).
    void SetDisplay(double refreshHz, bool vsync) noexcept
    {
        const double period = (vsync && refreshHz > 1.0) ? 1.0 / refreshHz : 0.0;
        if (std::abs(period - m_reportedPeriod) > 1e-9)
        {
            m_reportedPeriod = period;
            m_period = period;
            Unlock();
        }
    }

    [[nodiscard]] float Next(float rawDeltaSeconds) noexcept
    {
        const double raw = rawDeltaSeconds > 0.f ? static_cast<double>(rawDeltaSeconds) : 0.0;
        const double smoothed = static_cast<double>(m_smoother.Smooth(rawDeltaSeconds));
        if (m_period <= 0.0 || raw >= kMaxDeltaSeconds)
        {
            Unlock();   // no display rate, or a hitch: start over afterwards
            return static_cast<float>(smoothed);
        }

        // Lock after the average frame time sat on a refresh multiple for a
        // while; unlock only after it stayed clearly off for a while, so a
        // single missed vblank does not drop the lock right when it matters.
        const double ratio = smoothed / m_period;
        const double multiple = std::round(ratio);
        const double offMultiple = multiple >= 1.0 ? std::abs(ratio - multiple) : 1.0;
        m_framesOn  = offMultiple < 0.1  ? m_framesOn + 1u  : 0u;
        m_framesOff = offMultiple > 0.25 ? m_framesOff + 1u : 0u;
        if (!m_locked && m_framesOn >= kLockFrames)
        {
            m_locked = true;
            m_realTime = m_gameTime = m_drift = 0.0;
        }
        else if (m_locked && m_framesOff >= kLockFrames)
        {
            Unlock();
        }
        if (!m_locked)
            return static_cast<float>(smoothed);

        // Measure the period while every frame takes one: the smoothed delta
        // has the alternating CPU measurement averaged out already.
        if (multiple == 1.0 && offMultiple < 0.1 && m_smoother.IsFull())
        {
            m_period += kPeriodAverage * (smoothed - m_period);
            m_period = std::clamp(m_period, m_reportedPeriod * (1.0 - kMaxRateError),
                                  m_reportedPeriod * (1.0 + kMaxRateError));
        }

        // Whole refresh periods since the last frame, at least one: a frame
        // shown with vsync stays on screen for whole periods. Two or more only
        // when clearly more time passed (a missed vblank takes almost exactly
        // two), so measuring noise does not produce a double step.
        m_realTime += raw;
        const double pending = m_realTime - m_gameTime;   // real time not yet given to the game
        const double exact = pending / m_period;
        const double periods = std::max(1.0, std::floor(exact + 0.5 - kMultipleMargin));
        double delta = periods * m_period;
        // Keep the frame times centred between two refresh boundaries: a small
        // share of the averaged leftover goes back into every step. Frames that
        // got their period only because of the minimum of one do not count.
        if (exact >= 0.5)
            m_drift += kDriftAverage * ((pending - delta) - m_drift);
        delta += std::clamp(kDriftFeedback * m_drift, -kMaxDriftShare * m_period, kMaxDriftShare * m_period);
        m_gameTime += delta;
        // Past a few periods the difference is accepted instead of being made
        // up in one visible step.
        if (std::abs(m_realTime - m_gameTime) > 4.0 * m_period)
            m_gameTime = m_realTime;
        return static_cast<float>(delta);
    }

    void Reset() noexcept
    {
        m_smoother.Reset();
        m_period = m_reportedPeriod;
        Unlock();
    }

    [[nodiscard]] bool IsLockedToDisplay() const noexcept { return m_locked; }
    // Measured refresh period while locked (seeded by the reported rate).
    [[nodiscard]] double RefreshPeriodSeconds() const noexcept { return m_period; }

private:
    void Unlock() noexcept
    {
        m_locked = false;
        m_framesOn = m_framesOff = 0u;
        m_realTime = m_gameTime = m_drift = 0.0;
    }

    static constexpr double   kMaxDeltaSeconds = 0.25;
    static constexpr unsigned kLockFrames      = 8u;
    static constexpr double   kPeriodAverage   = 0.01;   // period measurement per frame
    static constexpr double   kMaxRateError    = 0.03;   // measured rate within 3 % of the reported one
    static constexpr double   kMultipleMargin  = 0.15;   // 2 periods only from 1.65 periods on
    static constexpr double   kDriftAverage    = 0.02;   // averaging of the leftover per frame
    static constexpr double   kDriftFeedback   = 0.05;   // share of the average fed back per frame
    static constexpr double   kMaxDriftShare   = 0.01;   // at most 1 % of a period per frame

    FrameDeltaSmoother m_smoother;
    double   m_reportedPeriod = 0.0;
    double   m_period = 0.0;
    bool     m_locked = false;
    unsigned m_framesOn = 0u;
    unsigned m_framesOff = 0u;
    double   m_realTime = 0.0;
    double   m_gameTime = 0.0;
    double   m_drift = 0.0;
};

} // namespace engine
