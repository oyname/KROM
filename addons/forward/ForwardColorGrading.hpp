#pragma once
// =============================================================================
// KROM Engine - addons/forward/ForwardColorGrading.hpp
// Colour grading of the tonemap pass (per scene, like the exposure) and the
// constants the shader (shaders/passthrough.ps.hlsl) reads. Header only and
// free of GPU types, so the conversion can be unit tested.
//
// Order in the shader:
//   HDR (after exposure): contrast in log space around mid grey 0.18, then one
//   3x3 matrix = saturation * hue shift * colour filter * white balance.
//   Tonemap operator; AgX applies its look between curve and display transform.
//   Display-referred: lift / gamma / gain on gamma-2.2 encoded values.
// All defaults are neutral; a neutral grading sets no shader flag, so the
// image stays bit-identical to the pass without grading.
// =============================================================================
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>

namespace engine::renderer::addons::forward {

// Looks of the AgX operator (ignored by the other operators). Golden and Punchy
// follow the AgX minimal fit by B. Wrensch; the contrast pair are KROM presets.
// Ids are saved by name in scene files.
enum class AgxLook : uint8_t
{
    None         = 0,
    Punchy       = 1,
    Golden       = 2,
    HighContrast = 3,
    LowContrast  = 4,
};
inline constexpr uint8_t kAgxLookCount = 5u;

// Lift, gamma or gain: a tint (white = none) and a value in [-1, 1].
struct ColorGradingWheel
{
    std::array<float, 3> color{1.f, 1.f, 1.f};
    float value = 0.f;
};

struct ColorGradingSettings
{
    AgxLook look = AgxLook::None;
    float temperature = 0.f;                    // -100 (blue) .. 100 (yellow)
    float tint = 0.f;                           // -100 (green) .. 100 (magenta)
    std::array<float, 3> colorFilter{1.f, 1.f, 1.f};   // linear multiplier
    float hueShift = 0.f;                       // degrees, -180 .. 180
    float contrast = 1.f;                       // 0 .. 2, 1 = unchanged
    float saturation = 1.f;                     // 0 (grey) .. 2
    ColorGradingWheel lift;                     // shadows
    ColorGradingWheel gamma;                    // midtones
    ColorGradingWheel gain;                     // highlights

    void Clamp() noexcept
    {
        if (static_cast<uint8_t>(look) >= kAgxLookCount)
            look = AgxLook::None;
        temperature = std::clamp(temperature, -100.f, 100.f);
        tint = std::clamp(tint, -100.f, 100.f);
        for (float& c : colorFilter)
            c = std::clamp(c, 0.f, 4.f);
        hueShift = std::clamp(hueShift, -180.f, 180.f);
        contrast = std::clamp(contrast, 0.f, 2.f);
        saturation = std::clamp(saturation, 0.f, 2.f);
        for (ColorGradingWheel* w : {&lift, &gamma, &gain})
        {
            for (float& c : w->color)
                c = std::clamp(c, 0.f, 1.f);
            w->value = std::clamp(w->value, -1.f, 1.f);
        }
    }

    [[nodiscard]] bool operator==(const ColorGradingSettings& o) const noexcept
    {
        const auto wheelEq = [](const ColorGradingWheel& a, const ColorGradingWheel& b) {
            return a.color == b.color && a.value == b.value;
        };
        return look == o.look && temperature == o.temperature && tint == o.tint &&
               colorFilter == o.colorFilter && hueShift == o.hueShift && contrast == o.contrast &&
               saturation == o.saturation && wheelEq(lift, o.lift) && wheelEq(gamma, o.gamma) &&
               wheelEq(gain, o.gain);
    }
    [[nodiscard]] bool operator!=(const ColorGradingSettings& o) const noexcept { return !(*this == o); }
    [[nodiscard]] bool IsNeutral() const noexcept { return *this == ColorGradingSettings{}; }
};

[[nodiscard]] inline const char* AgxLookName(AgxLook look) noexcept
{
    switch (look)
    {
    case AgxLook::Punchy:       return "punchy";
    case AgxLook::Golden:       return "golden";
    case AgxLook::HighContrast: return "highContrast";
    case AgxLook::LowContrast:  return "lowContrast";
    case AgxLook::None:
    default:                    return "none";
    }
}

[[nodiscard]] inline AgxLook ParseAgxLook(const char* name) noexcept
{
    if (name)
        for (uint8_t i = 1u; i < kAgxLookCount; ++i)
            if (std::strcmp(name, AgxLookName(static_cast<AgxLook>(i))) == 0)
                return static_cast<AgxLook>(i);
    return AgxLook::None;
}

// ASC CDL of a look, applied by the AgX operator on its display-encoded values.
struct AgxLookCdl
{
    std::array<float, 3> slope{1.f, 1.f, 1.f};
    float power = 1.f;
    float saturation = 1.f;
};

[[nodiscard]] inline AgxLookCdl AgxLookParameters(AgxLook look) noexcept
{
    switch (look)
    {
    case AgxLook::Punchy:       return { {1.f, 1.f, 1.f}, 1.35f, 1.4f };
    case AgxLook::Golden:       return { {1.f, 0.9f, 0.5f}, 0.8f, 0.8f };
    case AgxLook::HighContrast: return { {1.f, 1.f, 1.f}, 1.2f, 1.1f };
    case AgxLook::LowContrast:  return { {1.f, 1.f, 1.f}, 0.85f, 0.95f };
    case AgxLook::None:
    default:                    return {};
    }
}

// Shader flags (ColorGradingConstants::params.y).
inline constexpr uint32_t kGradingFlagMatrix   = 1u << 0;
inline constexpr uint32_t kGradingFlagContrast = 1u << 1;
inline constexpr uint32_t kGradingFlagLiftGammaGain = 1u << 2;
inline constexpr uint32_t kGradingFlagAgxLook  = 1u << 3;

// Layout of the grading part of the tonemap PerPass block (float4 rows).
struct ColorGradingConstants
{
    float row0[4]{1.f, 0.f, 0.f, 0.f};   // colour matrix, row-major (xyz)
    float row1[4]{0.f, 1.f, 0.f, 0.f};
    float row2[4]{0.f, 0.f, 1.f, 0.f};
    float params[4]{1.f, 0.f, 1.f, 0.f}; // x = contrast, y = flags, z = look saturation
    float lift[4]{0.f, 0.f, 0.f, 0.f};   // xyz additive (display-encoded)
    float invGamma[4]{1.f, 1.f, 1.f, 0.f};
    float gain[4]{1.f, 1.f, 1.f, 0.f};
    float lookSlopePower[4]{1.f, 1.f, 1.f, 1.f};   // xyz slope, w power
};
static_assert(sizeof(ColorGradingConstants) == 128u, "ColorGradingConstants must match passthrough.ps.hlsl");

namespace detail {

using Mat3 = std::array<float, 9>;   // row-major

[[nodiscard]] inline Mat3 Mul(const Mat3& a, const Mat3& b) noexcept
{
    Mat3 r{};
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j)
            r[i * 3 + j] = a[i * 3 + 0] * b[0 * 3 + j] + a[i * 3 + 1] * b[1 * 3 + j] + a[i * 3 + 2] * b[2 * 3 + j];
    return r;
}

inline constexpr Mat3 kIdentity{1.f, 0.f, 0.f, 0.f, 1.f, 0.f, 0.f, 0.f, 1.f};
inline constexpr float kLumaR = 0.2126f, kLumaG = 0.7152f, kLumaB = 0.0722f;

// White balance in LMS (same approach as Unity's post stack): the D65 white is
// moved along the Planckian locus (temperature) and across it (tint).
[[nodiscard]] inline Mat3 WhiteBalance(float temperature, float tint) noexcept
{
    if (temperature == 0.f && tint == 0.f)
        return kIdentity;
    const float t1 = temperature / 65.f;
    const float t2 = tint / 65.f;
    const float x = 0.31271f - t1 * (t1 < 0.f ? 0.1f : 0.05f);
    const float y = 2.87f * x - 3.f * x * x - 0.27509507f + t2 * 0.05f;
    const auto xyToLms = [](float cx, float cy) -> std::array<float, 3> {
        const float X = cx / cy;
        const float Y = 1.f;
        const float Z = (1.f - cx - cy) / cy;
        return { 0.7328f * X + 0.4296f * Y - 0.1624f * Z,
                -0.7036f * X + 1.6975f * Y + 0.0061f * Z,
                 0.0030f * X + 0.0136f * Y + 0.9834f * Z };
    };
    const std::array<float, 3> w1 = xyToLms(0.31271f, 0.32902f);   // D65
    const std::array<float, 3> w2 = xyToLms(x, y);
    constexpr Mat3 linToLms{ 3.90405e-1f, 5.49941e-1f, 8.92632e-3f,
                             7.08416e-2f, 9.63172e-1f, 1.35775e-3f,
                             2.31082e-2f, 1.28021e-1f, 9.36245e-1f };
    constexpr Mat3 lmsToLin{ 2.85847e+0f, -1.62879e+0f, -2.48910e-2f,
                            -2.10182e-1f,  1.15820e+0f,  3.24281e-4f,
                            -4.18120e-2f, -1.18169e-1f,  1.06867e+0f };
    const Mat3 balance{ w1[0] / w2[0], 0.f, 0.f, 0.f, w1[1] / w2[1], 0.f, 0.f, 0.f, w1[2] / w2[2] };
    return Mul(lmsToLin, Mul(balance, linToLms));
}

// Rotation around the grey axis (luminance kept, as the SVG hue-rotate filter).
[[nodiscard]] inline Mat3 HueRotation(float degrees) noexcept
{
    if (degrees == 0.f)
        return kIdentity;
    const float a = degrees * 3.14159265f / 180.f;
    const float c = std::cos(a), s = std::sin(a);
    return { 0.213f + c * 0.787f - s * 0.213f, 0.715f - c * 0.715f - s * 0.715f, 0.072f - c * 0.072f + s * 0.928f,
             0.213f - c * 0.213f + s * 0.143f, 0.715f + c * 0.285f + s * 0.140f, 0.072f - c * 0.072f - s * 0.283f,
             0.213f - c * 0.213f - s * 0.787f, 0.715f - c * 0.715f + s * 0.715f, 0.072f + c * 0.928f + s * 0.072f };
}

[[nodiscard]] inline Mat3 Saturation(float s) noexcept
{
    const float i = 1.f - s;
    return { i * kLumaR + s, i * kLumaG,     i * kLumaB,
             i * kLumaR,     i * kLumaG + s, i * kLumaB,
             i * kLumaR,     i * kLumaG,     i * kLumaB + s };
}

// Tint normalised to luminance 1, so white (and any grey) means "no tint".
[[nodiscard]] inline std::array<float, 3> NormalisedTint(const std::array<float, 3>& c) noexcept
{
    const float luma = c[0] * kLumaR + c[1] * kLumaG + c[2] * kLumaB;
    if (luma <= 1e-4f)
        return {1.f, 1.f, 1.f};
    return { c[0] / luma, c[1] / luma, c[2] / luma };
}

} // namespace detail

// Converts the settings into shader constants. The AgX look only counts when the
// operator is AgX (agxActive).
[[nodiscard]] inline ColorGradingConstants BuildColorGradingConstants(ColorGradingSettings s, bool agxActive) noexcept
{
    using namespace detail;
    s.Clamp();
    ColorGradingConstants out{};
    uint32_t flags = 0u;

    const Mat3 filter{ s.colorFilter[0], 0.f, 0.f, 0.f, s.colorFilter[1], 0.f, 0.f, 0.f, s.colorFilter[2] };
    const Mat3 m = Mul(Saturation(s.saturation), Mul(HueRotation(s.hueShift), Mul(filter, WhiteBalance(s.temperature, s.tint))));
    if (m != kIdentity)
    {
        flags |= kGradingFlagMatrix;
        for (int c = 0; c < 3; ++c)
        {
            out.row0[c] = m[0 * 3 + c];
            out.row1[c] = m[1 * 3 + c];
            out.row2[c] = m[2 * 3 + c];
        }
    }

    if (s.contrast != 1.f)
    {
        flags |= kGradingFlagContrast;
        out.params[0] = s.contrast;
    }

    const ColorGradingWheel neutral{};
    const auto isNeutral = [&neutral](const ColorGradingWheel& w) { return w.color == neutral.color && w.value == neutral.value; };
    if (!isNeutral(s.lift) || !isNeutral(s.gamma) || !isNeutral(s.gain))
    {
        flags |= kGradingFlagLiftGammaGain;
        const auto liftTint = NormalisedTint(s.lift.color);
        const auto gammaTint = NormalisedTint(s.gamma.color);
        const auto gainTint = NormalisedTint(s.gain.color);
        for (int c = 0; c < 3; ++c)
        {
            // Lift adds up to 0.25 in the shadows; tints are scaled down so a fully
            // saturated colour does not swamp the image.
            out.lift[c] = s.lift.value * 0.25f + (liftTint[c] - 1.f) * 0.05f;
            const float gamma = std::exp2(s.gamma.value) * std::max(1.f + (gammaTint[c] - 1.f) * 0.25f, 0.05f);
            out.invGamma[c] = 1.f / gamma;
            out.gain[c] = std::exp2(s.gain.value) * std::max(1.f + (gainTint[c] - 1.f) * 0.5f, 0.f);
        }
    }

    if (agxActive && s.look != AgxLook::None)
    {
        flags |= kGradingFlagAgxLook;
        const AgxLookCdl cdl = AgxLookParameters(s.look);
        for (int c = 0; c < 3; ++c)
            out.lookSlopePower[c] = cdl.slope[c];
        out.lookSlopePower[3] = cdl.power;
        out.params[2] = cdl.saturation;
    }

    out.params[1] = static_cast<float>(flags);
    return out;
}

} // namespace engine::renderer::addons::forward
