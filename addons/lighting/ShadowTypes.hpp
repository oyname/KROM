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
    None   = 0,  // hard shadow: single depth sample
    PCF2x2 = 1,  // 4-tap PCF at ±0.5 texel offsets
    PCF3x3 = 2,  // 9-tap PCF 3×3 kernel
};

struct ShadowSettings
{
    bool         enabled      = false;
    ShadowFilter filter       = ShadowFilter::PCF3x3;
    uint32_t     resolution   = 2048u;
    uint32_t     cascadeCount = 1u;
    float        bias         = 0.001f;
    float        normalBias   = 0.002f;
    float        maxDistance  = 100.0f;
    float        strength     = 1.0f;
    float        cascadeLambda = 0.75f;
};

namespace ShadowPresets {

[[nodiscard]] inline ShadowSettings Low() noexcept
{
    ShadowSettings s;
    s.enabled     = true;
    s.filter      = ShadowFilter::None;
    s.resolution  = 512u;
    s.bias        = 0.002f;
    s.normalBias  = 0.003f;
    s.maxDistance = 30.0f;
    s.strength    = 1.0f;
    return s;
}

[[nodiscard]] inline ShadowSettings Medium() noexcept
{
    ShadowSettings s;
    s.enabled       = true;
    s.filter        = ShadowFilter::PCF2x2;
    s.resolution    = 1024u;
    s.cascadeCount  = 3u;
    s.bias          = 0.001f;
    s.normalBias    = 0.002f;
    s.maxDistance   = 60.0f;
    s.strength      = 1.0f;
    return s;
}

[[nodiscard]] inline ShadowSettings High() noexcept
{
    ShadowSettings s;
    s.enabled       = true;
    s.filter        = ShadowFilter::PCF3x3;
    s.resolution    = 2048u;
    s.cascadeCount  = 4u;
    s.bias          = 0.0005f;
    s.normalBias    = 0.001f;
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
