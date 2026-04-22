#pragma once
// =============================================================================
// KROM Engine - addons/lighting/ShadowTypes.hpp
// Gemeinsame Shadow-Konfiguration pro Licht.
// Gilt engine-weit, unabhängig von Render-Backend und konkreter Technik.
// =============================================================================
#include <cstdint>

namespace engine {

enum class ShadowType : uint8_t
{
    None = 0,
    Hard = 1,
    PCF = 2,
};

enum class ShadowFilter : uint8_t
{
    None = 0,
    PCF2x2 = 1,
    PCF3x3 = 2,
};

enum class ShadowQuality : uint8_t
{
    Low = 0,
    Medium = 1,
    High = 2,
};

struct ShadowSettings
{
    bool enabled = false;
    ShadowType type = ShadowType::PCF;
    ShadowFilter filter = ShadowFilter::PCF3x3;
    ShadowQuality quality = ShadowQuality::Medium;
    uint32_t resolution = 2048u;
    float bias = 0.001f;
    float normalBias = 0.002f;
    float maxDistance = 100.0f;
    float strength = 1.0f;
    bool staticOnly = false;
    bool updateEveryFrame = true;
};

} // namespace engine
