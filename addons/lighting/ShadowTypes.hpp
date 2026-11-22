#pragma once
// =============================================================================
// KROM Engine - addons/lighting/ShadowTypes.hpp
// Gemeinsame Shadow-Konfiguration pro Licht.
// Gilt engine-weit, unabhängig von Render-Backend und konkreter Technik.
// =============================================================================
#include <cstdint>

namespace engine {

enum class ShadowFilter : uint8_t
{
    None    = 0, // hard shadow: single bilinear compare
    // Tent PCF ("The Witness" / MJP): bilinear compares weighted to a tent over
    // 3x3, 5x5 or 7x7 texels (4, 9, 16 taps). PBR shaders; the frozen OpenGL and
    // legacy lit shaders keep their old 2x2/3x3 box kernels for 1 and 2 and use
    // 3x3 for Tent7x7.
    Tent3x3 = 1,
    Tent5x5 = 2,
    Poisson = 3, // 12-tap Poisson disc, rotated per pixel (PBR shaders; legacy lit falls back to 3x3)
    PCSS    = 4, // contact-hardening: blocker search + Poisson filter sized by lightSize (PBR shaders)
    Tent7x7 = 5,
};

// Highest valid ShadowFilter value; values read from files are clamped to it.
inline constexpr uint8_t kShadowFilterMax = static_cast<uint8_t>(ShadowFilter::Tent7x7);

// Range of ShadowSettings::lightSize.
inline constexpr float kShadowLightSizeMin = 0.f;
inline constexpr float kShadowLightSizeMax = 2.f;

// Range of ShadowSettings::normalBias (shadow-map texels).
inline constexpr float kShadowNormalBiasMin = 0.f;
inline constexpr float kShadowNormalBiasMax = 8.f;

struct ShadowSettings
{
    bool         enabled      = false;
    ShadowFilter filter       = ShadowFilter::Tent5x5;
    uint32_t     resolution   = 2048u;
    uint32_t     cascadeCount = 1u;
    // Directional: world units, stable across cascade sizes and camera distance.
    // Point/spot: normalized shadow depth.
    float        bias         = 0.001f;
    // Receiver offset along the surface normal, in shadow-map texels of the
    // view that is sampled (as in Unity/Unreal): one value fits every cascade
    // and resolution. Saved as "shadowNormalBiasTexels".
    float        normalBias   = 1.0f;
    float        maxDistance  = 100.0f;
    float        strength     = 1.0f;
    float        cascadeLambda = 0.75f;
    // ShadowFilter::PCSS only: size of the light source. Point and spot lights:
    // world units; directional lights: tangent of the angular size. Larger =
    // softer shadows away from the contact.
    float        lightSize    = 0.1f;
    // Screen-space contact shadows (contact shadow add-on). Only the first
    // directional light with shadows and this flag gets them.
    bool         contactShadows = true;
};

// Scene defaults for a new sun: 4 cascades, an even split between uniform and
// logarithmic, 50 m. These look good without tuning; very long distances with
// few cascades give coarse, detached shadows (texels grow with the distance).
inline void ApplyDirectionalShadowDefaults(ShadowSettings& s) noexcept
{
    s.cascadeCount = 4u;
    s.cascadeLambda = 0.5f;
    s.maxDistance = 50.0f;
}

namespace ShadowPresets {

[[nodiscard]] inline ShadowSettings Low() noexcept
{
    ShadowSettings s;
    s.enabled     = true;
    s.filter      = ShadowFilter::None;
    s.resolution  = 512u;
    s.bias        = 0.002f;
    s.normalBias  = 1.5f;
    s.maxDistance = 30.0f;
    s.strength    = 1.0f;
    return s;
}

[[nodiscard]] inline ShadowSettings Medium() noexcept
{
    ShadowSettings s;
    s.enabled       = true;
    s.filter        = ShadowFilter::Tent3x3;
    s.resolution    = 1024u;
    s.cascadeCount  = 3u;
    s.bias          = 0.001f;
    s.normalBias    = 1.0f;
    s.maxDistance   = 60.0f;
    s.strength      = 1.0f;
    return s;
}

[[nodiscard]] inline ShadowSettings High() noexcept
{
    ShadowSettings s;
    s.enabled       = true;
    s.filter        = ShadowFilter::Tent5x5;
    s.resolution    = 2048u;
    s.cascadeCount  = 4u;
    s.bias          = 0.0005f;
    s.normalBias    = 0.75f;
    s.maxDistance   = 100.0f;
    s.strength      = 1.0f;
    return s;
}

// Wählt ein Preset basierend auf der erkannten GPU-Klasse.
// dedicatedVRAM in Bytes (0 = unbekannt). isDiscrete = false → integrierte GPU.
// Gedacht als einmaliger Aufruf beim Device-Init; das Ergebnis kann danach
// vom User/Designer überschrieben werden.
[[nodiscard]] inline ShadowSettings Recommend(size_t dedicatedVRAM, bool isDiscrete) noexcept
{
    if (!isDiscrete)
        return Low();

    constexpr size_t kGB = 1024ull * 1024ull * 1024ull;
    if (dedicatedVRAM == 0u || dedicatedVRAM >= 4u * kGB)
        return High();
    if (dedicatedVRAM >= 1u * kGB)
        return Medium();
    return Low();
}

} // namespace ShadowPresets

} // namespace engine
