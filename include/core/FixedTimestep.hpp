#pragma once
// =============================================================================
// KROM Engine - core/FixedTimestep.hpp
// Akkumulator fuer feste Zeitschritte (Physik, OnFixedUpdate).
//
//   Advance(dt) sammelt Frame-Zeit und liefert die Anzahl faelliger Schritte.
//   Ueber maxSteps hinaus wird der Rueckstand verworfen (keine Todesspirale:
//   das Spiel laeuft kurz langsamer, statt immer weiter zurueckzufallen).
//   Alpha() = Anteil des naechsten Schritts, der schon vergangen ist (0..1) —
//   Mischfaktor fuer die Interpolation zwischen den letzten zwei Schritten.
// =============================================================================
#include <algorithm>
#include <cstdint>

namespace engine {

class FixedTimestep
{
public:
    explicit FixedTimestep(float step = 1.f / 60.f, uint32_t maxSteps = 4u) noexcept
    {
        Configure(step, maxSteps);
    }

    void Configure(float step, uint32_t maxSteps) noexcept
    {
        m_step     = std::max(step, 1.f / 1000.f);
        m_maxSteps = std::max(maxSteps, 1u);
    }

    [[nodiscard]] uint32_t Advance(float dt) noexcept
    {
        if (!(dt > 0.f))
            return 0u;
        m_accumulator += dt;
        uint32_t steps = static_cast<uint32_t>(m_accumulator / m_step);
        if (steps > m_maxSteps)
        {
            steps = m_maxSteps;
            m_accumulator = 0.f;
        }
        else
        {
            m_accumulator -= static_cast<float>(steps) * m_step;
        }
        return steps;
    }

    void Reset() noexcept { m_accumulator = 0.f; }

    [[nodiscard]] float    Step() const noexcept     { return m_step; }
    [[nodiscard]] uint32_t MaxSteps() const noexcept { return m_maxSteps; }
    [[nodiscard]] float    Alpha() const noexcept
    {
        return std::clamp(m_accumulator / m_step, 0.f, 1.f);
    }

private:
    float    m_step        = 1.f / 60.f;
    uint32_t m_maxSteps    = 4u;
    float    m_accumulator = 0.f;
};

} // namespace engine
