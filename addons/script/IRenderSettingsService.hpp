#pragma once
// =============================================================================
// KROM Engine - addons/script/IRenderSettingsService.hpp
// Render options for scripts, e.g. for a graphics options menu.
// =============================================================================
#include "core/RenderQuality.hpp"

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

// Low / Medium / High / Custom (= the options match no level). See
// engine::RenderQualityPreset for what each level sets.
using QualityLevel = engine::RenderQualityLevel;
// Same ids as renderer::addons::forward::PostAAMode.
using PostAAMode = engine::RenderPostAA;

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
    bool        contactShadowsEnabled = true; // contact shadow add-on; no effect under OpenGL
    PostAAMode  postAA = PostAAMode::Off;    // after tonemapping; no effect under OpenGL
};

[[nodiscard]] inline engine::RenderQualityOptions QualityOptionsOf(const RenderSettings& settings) noexcept
{
    return {settings.msaaSamples >= 4u ? 4u : (settings.msaaSamples >= 2u ? 2u : 1u), settings.postAA,
            settings.gtaoEnabled, settings.ssrEnabled, settings.contactShadowsEnabled};
}

inline void SetQualityOptions(RenderSettings& settings, const engine::RenderQualityOptions& options) noexcept
{
    settings.msaaSamples = options.msaaSamples;
    settings.postAA = options.postAA;
    settings.gtaoEnabled = options.gtaoEnabled;
    settings.ssrEnabled = options.ssrEnabled;
    settings.contactShadowsEnabled = options.contactShadowsEnabled;
}

class IRenderSettingsService
{
public:
    virtual ~IRenderSettingsService() = default;

    [[nodiscard]] virtual RenderSettings Get() const = 0;
    // Takes effect on the next frame. Not written to the project file. In the
    // editor the values of the project return when play stops.
    virtual void Set(const RenderSettings& settings) = 0;

    // The level the current options match, or Custom.
    [[nodiscard]] virtual QualityLevel GetQualityLevel() const
    {
        return engine::DetectRenderQualityLevel(QualityOptionsOf(Get()));
    }
    // Sets MSAA, post-AA, GTAO, SSR and contact shadows of a level (Custom:
    // no change); the other options stay. Meant for a player's options menu: an
    // exported game remembers the level on this computer and starts with it
    // next time (instead of the project's options). The editor does not.
    virtual void SetQualityLevel(QualityLevel level)
    {
        if (level == QualityLevel::Custom)
            return;
        RenderSettings settings = Get();
        SetQualityOptions(settings, engine::RenderQualityPreset(level));
        Set(settings);
    }
};

} // namespace engine::script
