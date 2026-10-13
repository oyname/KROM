#pragma once
// =============================================================================
// KROM Engine - src/renderer/FloatConvert.hpp   (internal, not public API)
// IEEE 754 half-precision <-> single-precision conversion helpers.
// Used by IBLBaker and EnvironmentSystem for RGBA16F packing.
// =============================================================================
#include <cstdint>
#include <cstring>
#include <cmath>

namespace engine::renderer::internal {

[[nodiscard]] inline float HalfToFloat(uint16_t h) noexcept
{
    const uint32_t sign = static_cast<uint32_t>(h & 0x8000u) << 16u;
    const uint32_t exp  = (h >> 10u) & 0x1Fu;
    const uint32_t mant = h & 0x03FFu;
    uint32_t out = 0u;
    if (exp == 0u)
    {
        if (mant != 0u)
        {
            uint32_t m = mant;
            int32_t  e = -14;
            while ((m & 0x0400u) == 0u) { m <<= 1u; --e; }
            m   &= 0x03FFu;
            out  = sign | static_cast<uint32_t>((e + 127) << 23) | (m << 13u);
        }
        else { out = sign; }
    }
    else if (exp == 31u) { out = sign | 0x7F800000u | (mant << 13u); }
    else                 { out = sign | ((exp + 112u) << 23u) | (mant << 13u); }
    float result = 0.0f;
    std::memcpy(&result, &out, sizeof(float));
    return result;
}

[[nodiscard]] inline uint16_t FloatToHalf(float f) noexcept
{
    if (std::isnan(f))
        return static_cast<uint16_t>(0x7E00u);

    if (std::isinf(f))
        return std::signbit(f) ? static_cast<uint16_t>(0xFC00u) : static_cast<uint16_t>(0x7C00u);

    constexpr float kHalfMax = 65504.0f;
    const float clamped = (f > kHalfMax) ? kHalfMax : ((f < -kHalfMax) ? -kHalfMax : f);

    uint32_t bits = 0u;
    std::memcpy(&bits, &clamped, sizeof(bits));
    const uint32_t sign   = bits & 0x80000000u;
    const uint32_t exp32  = (bits >> 23u) & 0xFFu;
    const uint32_t mant32 = bits & 0x7FFFFFu;

    if (exp32 == 0u)
        return static_cast<uint16_t>(sign >> 16u);

    int32_t exp16 = static_cast<int32_t>(exp32) - 127 + 15;
    if (exp16 <= 0)
        return static_cast<uint16_t>(sign >> 16u);
    if (exp16 >= 31)
        return static_cast<uint16_t>((sign >> 16u) | 0x7BFFu);

    const uint32_t mantRounded = mant32 + 0x00001000u;
    uint32_t halfExp = static_cast<uint32_t>(exp16);
    uint32_t halfMant = mantRounded >> 13u;

    if (halfMant == 0x0400u)
    {
        halfMant = 0u;
        ++halfExp;
        if (halfExp >= 31u)
            return static_cast<uint16_t>((sign >> 16u) | 0x7BFFu);
    }

    return static_cast<uint16_t>(
        (sign >> 16u)
        | (halfExp << 10u)
        | (halfMant & 0x03FFu));
}

} // namespace engine::renderer::internal
