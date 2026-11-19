#pragma once
// =============================================================================
// KROM Engine - addons/script/IRenderSettingsService.hpp
// Render options for scripts, e.g. for a graphics options menu.
// =============================================================================
#include <cstdint>

namespace engine::script {

// Same ids as renderer::addons::forward::TonemapOperator.
enum class TonemapMode : uint8_t
{
    Reinhard          = 0,
    AcesFitted        = 1,
    AgX               = 2,
    KhronosPbrNeutral = 3,
};

// The project settings give the start values, except exposureEv and
// autoExposure, which come from the scene and are set again by every scene load.
struct RenderSettings
{
    TonemapMode tonemap = TonemapMode::AgX;
    float       exposureEv = 0.f;        // EV stops; HDR colour *= 2^EV. With autoExposure: correction
    // Automatic exposure (limits and speeds are set per scene in the Inspector);
    // no effect under OpenGL, which keeps the fixed exposure.
    bool        autoExposure = false;
    uint32_t    msaaSamples = 1u;        // 1 = off, 2, 4; no effect under OpenGL
    bool        heightCorrelatedVisibility = true;
    bool        perLightDiffuseFresnel = true;
    bool        multiScatterCompensation = true;
    bool        specularAntiAliasing = true;  // geometric specular AA; OpenGL keeps its own variant
    bool        specularOcclusion = true;
    bool        gtaoEnabled = true;          // GTAO add-on; no effect under OpenGL
    bool        ssrEnabled = true;           // SSR add-on; no effect under OpenGL
};

class IRenderSettingsService
{
public:
    virtual ~IRenderSettingsService() = default;

    [[nodiscard]] virtual RenderSettings Get() const = 0;
    // Takes effect on the next frame. Not written to the project file. In the
    // editor the values of the project return when play stops.
    virtual void Set(const RenderSettings& settings) = 0;
};

} // namespace engine::script
