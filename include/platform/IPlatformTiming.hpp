#pragma once
// =============================================================================
// KROM Engine - platform/IPlatformTiming.hpp
//
// Plattformneutrale Timing-Abstraktion.
// BeginFrame() einmal pro Frame aufrufen - danach ist GetDeltaSeconds() stabil.
//
// Implementierungen: StdTiming (chrono), FixedTiming (Tests), NullTiming
// =============================================================================
#include <cstdint>
#include <memory>

namespace engine::platform {

class IPlatformTiming
{
public:
    virtual ~IPlatformTiming() = default;

    virtual void BeginFrame() = 0;
    virtual void EndFrame()   = 0;

    [[nodiscard]] virtual double   GetTimeSeconds()         const = 0;
    [[nodiscard]] virtual double   GetDeltaSeconds()        const = 0;
    [[nodiscard]] virtual float    GetDeltaSecondsF()       const = 0;
    [[nodiscard]] virtual float    GetTimeSecondsF()        const = 0;
    [[nodiscard]] virtual uint64_t GetFrameCount()          const = 0;
    [[nodiscard]] virtual float    GetSmoothedFPS()         const = 0;
    [[nodiscard]] virtual double   GetRawTimestampSeconds() const = 0;

    // Max-Delta-Clamping (verhindert Physik-Explosion nach Debugger-Pause)
    virtual void SetMaxDeltaSeconds(double maxDelta) = 0;
};

} // namespace engine::platform
