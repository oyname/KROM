#include "KromRenderSettings.hpp"

#include "addons/postprocess/PostProcessBlend.hpp"
#include "addons/script/ScriptColorGrading.hpp"
#include "core/AtomicTextFile.hpp"
#include "core/Debug.hpp"
#include "core/PathUtf8.hpp"

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <iterator>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

using engine::PathFromUtf8;
using engine::PathToUtf8;

namespace Krom::internal {

static_assert(static_cast<uint8_t>(engine::RenderPostAA::Fxaa) ==
              static_cast<uint8_t>(engine::renderer::addons::forward::PostAAMode::Fxaa) &&
              static_cast<uint8_t>(engine::RenderPostAA::Smaa) ==
              static_cast<uint8_t>(engine::renderer::addons::forward::PostAAMode::Smaa),
              "RenderPostAA ids must match PostAAMode");

// =============================================================================
// Project, scene and per-frame values
// =============================================================================

void ApplyProjectRenderSettings(RenderSettingsState& render,
                                const engine::ProjectRenderSettings& projectRendering,
                                bool editorLiveState)
{
    // A player's quality level replaces the level options of the project.
    engine::ProjectRenderSettings rendering = projectRendering;
    render.baseGtaoIntensity = rendering.gtaoIntensity;
    render.baseSsrIntensity = rendering.ssrIntensity;
    render.baseContactShadowIntensity = rendering.contactShadowIntensity;
    render.bloomEnabled = rendering.bloomEnabled;
    render.baseBloomIntensity = rendering.bloomIntensity;
    render.bloomThreshold = rendering.bloomThreshold;
    if (render.userQualityLevel && !editorLiveState)
        engine::ApplyRenderQualityLevel(rendering, *render.userQualityLevel);
    if (render.tonemapSettings &&
        !engine::renderer::addons::forward::ParseTonemapOperator(rendering.tonemapOperator.c_str(),
                                                                 render.tonemapSettings->op))
        engine::Debug::LogWarning("krom.h: unknown tonemap operator '%s'", rendering.tonemapOperator.c_str());
    if (render.msaaSettings)
        render.msaaSettings->sampleCount =
            engine::renderer::addons::forward::NormalizeMsaaSamples(rendering.msaaSamples);
    if (render.postAASettings)
    {
        render.postAASettings->mode = engine::renderer::addons::forward::PostAAMode::Off;
        if (!engine::renderer::addons::forward::ParsePostAAMode(rendering.postAA.c_str(), render.postAASettings->mode))
            engine::Debug::LogWarning("krom.h: unknown post-AA mode '%s'", rendering.postAA.c_str());
    }
    if (render.shadingSettings)
        *render.shadingSettings = { rendering.brdfHeightCorrelated, rendering.brdfPerLightFresnel,
                                    rendering.brdfMultiScatter, rendering.specularAntiAliasing,
                                    rendering.brdfBurleyDiffuse };
    if (render.ssrResources)
    {
        engine::renderer::addons::ssr::SsrSettings& ssr = render.ssrResources->settings;
        ssr.enabled      = rendering.ssrEnabled;
        ssr.intensity    = rendering.ssrIntensity;
        ssr.maxRoughness = rendering.ssrMaxRoughness;
        ssr.maxDistance  = rendering.ssrMaxDistance;
        ssr.thickness    = rendering.ssrThickness;
    }
    if (render.gtaoResources)
    {
        engine::renderer::addons::gtao::GtaoSettings& gtao = render.gtaoResources->settings;
        gtao.enabled   = rendering.gtaoEnabled;
        gtao.radius    = rendering.gtaoRadius;
        gtao.intensity = rendering.gtaoIntensity;
        gtao.bias      = rendering.gtaoBias;
    }
    if (render.contactShadowResources)
    {
        engine::renderer::addons::contact_shadow::ContactShadowSettings& cs = render.contactShadowResources->settings;
        cs.enabled   = rendering.contactShadowsEnabled;
        cs.length    = rendering.contactShadowLength;
        cs.thickness = rendering.contactShadowThickness;
        cs.intensity = rendering.contactShadowIntensity;
        cs.steps     = rendering.contactShadowSteps;
    }
    if (render.probeRuntime)
    {
        engine::addons::probes::ProbeSettings probeSettings = render.probeRuntime->GetSettings();
        probeSettings.specularOcclusion = rendering.specularOcclusion;
        render.probeRuntime->SetSettings(probeSettings);
    }
    if (render.shadowQuality)
    {
        render.shadowQuality->directionalResolution = rendering.shadowSunResolution;
        render.shadowQuality->localResolution = rendering.shadowLocalResolution;
        render.shadowQuality->filter = static_cast<engine::ShadowFilter>(
            std::min<uint8_t>(rendering.shadowFilter, engine::kMaxShadowQualityFilter));
    }
}

void ApplySceneRenderSettings(RenderSettingsState& render,
                              float exposureEv,
                              engine::renderer::addons::forward::ExposureMode exposureMode,
                              const engine::renderer::addons::forward::AutoExposureSettings& autoExposure,
                              const engine::renderer::addons::forward::ColorGradingSettings& grading,
                              uint32_t probeBakePasses)
{
    if (render.tonemapSettings)
    {
        render.tonemapSettings->exposureEv = exposureEv;
        render.tonemapSettings->exposureMode = exposureMode;
        render.tonemapSettings->autoExposure = autoExposure;
        render.tonemapSettings->grading = grading;
    }
    if (render.autoExposureStatus)
        render.autoExposureStatus->RequestReset();   // the new scene starts at its own exposure
    if (render.probeRuntime)
    {
        engine::addons::probes::ProbeSettings probeSettings = render.probeRuntime->GetSettings();
        probeSettings.bakePasses = probeBakePasses;
        render.probeRuntime->SetSettings(probeSettings);
    }
}

void ApplyRenderEnvironmentOverrides(RenderSettingsState& render)
{
    if (const char* msaa = std::getenv("KROM_MSAA"); msaa && render.msaaSettings)
        render.msaaSettings->sampleCount = engine::renderer::addons::forward::NormalizeMsaaSamples(
            static_cast<uint32_t>(std::strtoul(msaa, nullptr, 10)));
    if (const char* postAA = std::getenv("KROM_POST_AA"); postAA && render.postAASettings)
        (void)engine::renderer::addons::forward::ParsePostAAMode(postAA, render.postAASettings->mode);
}

void ApplyPostProcessVolumes(RenderSettingsState& render,
                             const engine::ecs::World& world,
                             const engine::math::Vec3& cameraPosition)
{
    engine::addons::postprocess::PostProcessValues ppBase;
    ppBase.tonemap = *render.tonemapSettings;
    ppBase.bloomIntensity = render.baseBloomIntensity;
    ppBase.bloomThreshold = render.bloomThreshold;
    ppBase.gtaoIntensity = render.baseGtaoIntensity;
    ppBase.ssrIntensity = render.baseSsrIntensity;
    ppBase.contactShadowIntensity = render.baseContactShadowIntensity;
    const engine::addons::postprocess::PostProcessValues pp =
        engine::addons::postprocess::BlendPostProcessVolumes(world, cameraPosition, ppBase);
    *render.effectiveTonemapSettings = pp.tonemap;
    render.effectiveTonemapSettings->bloomIntensity = render.bloomEnabled ? pp.bloomIntensity : 0.0f;
    render.effectiveTonemapSettings->bloomThreshold = pp.bloomThreshold;
    if (render.gtaoResources)
        render.gtaoResources->settings.intensity = pp.gtaoIntensity;
    if (render.ssrResources)
        render.ssrResources->settings.intensity = pp.ssrIntensity;
    if (render.contactShadowResources)
        render.contactShadowResources->settings.intensity = pp.contactShadowIntensity;
}

// =============================================================================
// Player settings on this computer
// =============================================================================

std::filesystem::path UserGameSettingsPath(const std::string& windowTitle)
{
    if (const char* dir = std::getenv("KROM_USER_SETTINGS_DIR"); dir && *dir)
        return PathFromUtf8(dir) / "user-settings.json";
    std::string game = engine::UserGameSettingsFolderName(windowTitle);
#if defined(_WIN32)
    if (game.empty())
    {
        wchar_t exeBuf[260]{};
        if (::GetModuleFileNameW(nullptr, exeBuf, 260) > 0)
            game = engine::UserGameSettingsFolderName(PathToUtf8(std::filesystem::path(exeBuf).stem()));
    }
    if (game.empty())
        return {};
    if (const wchar_t* appData = _wgetenv(L"LOCALAPPDATA"); appData && *appData)
        return std::filesystem::path(appData) / "KROM" / "Games" / PathFromUtf8(game) / "user-settings.json";
#else
    if (game.empty())
        return {};
    if (const char* config = std::getenv("XDG_CONFIG_HOME"); config && *config)
        return PathFromUtf8(config) / "krom" / "games" / PathFromUtf8(game) / "user-settings.json";
    if (const char* home = std::getenv("HOME"); home && *home)
        return PathFromUtf8(home) / ".config" / "krom" / "games" / PathFromUtf8(game) / "user-settings.json";
#endif
    return {};
}

void LoadUserGameSettings(RenderSettingsState& render, const std::string& windowTitle)
{
    render.userQualityLevel.reset();
    const std::filesystem::path path = UserGameSettingsPath(windowTitle);
    std::ifstream file(path);
    if (path.empty() || !file)
        return;
    const std::string json(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>{});
    std::string err;
    const engine::serialization::JsonValue root = engine::serialization::JsonParser::Parse(json, err);
    if (!err.empty() || !root.IsObject())
    {
        engine::Debug::LogWarning("krom.h: ignoring invalid user settings '%s'", PathToUtf8(path).c_str());
        return;
    }
    render.userQualityLevel = engine::ReadUserGameSettings(root).qualityLevel;
    if (render.userQualityLevel)
        engine::Debug::Log("krom.h: quality level '%s' from %s",
                           engine::RenderQualityLevelName(*render.userQualityLevel), PathToUtf8(path).c_str());
}

void SaveUserGameSettings(const RenderSettingsState& render, const std::string& windowTitle)
{
    const std::filesystem::path path = UserGameSettingsPath(windowTitle);
    engine::UserGameSettings settings;
    settings.qualityLevel = render.userQualityLevel;
    if (path.empty() || !engine::WriteTextFileAtomically(path, engine::WriteUserGameSettings(settings)))
        engine::Debug::LogWarning("krom.h: could not save user settings '%s'", PathToUtf8(path).c_str());
}

// =============================================================================
// krom.h functions
// =============================================================================

void SetTonemapping(RenderSettingsState& render,
                    engine::renderer::addons::forward::TonemapOperator op, float exposureEv)
{
    if (!render.tonemapSettings)
        return;
    render.tonemapSettings->op = op;
    render.tonemapSettings->exposureEv = std::clamp(exposureEv, engine::renderer::addons::forward::kMinExposureEv,
                                                    engine::renderer::addons::forward::kMaxExposureEv);
}

engine::renderer::addons::forward::TonemapSettings GetTonemapping(const RenderSettingsState& render)
{
    return render.tonemapSettings ? *render.tonemapSettings : engine::renderer::addons::forward::TonemapSettings{};
}

void SetColorGrading(RenderSettingsState& render,
                     const engine::renderer::addons::forward::ColorGradingSettings& grading)
{
    if (!render.tonemapSettings)
        return;
    render.tonemapSettings->grading = grading;
    render.tonemapSettings->grading.Clamp();
}

engine::renderer::addons::forward::ColorGradingSettings GetColorGrading(const RenderSettingsState& render)
{
    return render.tonemapSettings ? render.tonemapSettings->grading
                                  : engine::renderer::addons::forward::ColorGradingSettings{};
}

void SetMsaa(RenderSettingsState& render, uint32_t sampleCount)
{
    if (render.msaaSettings)
        render.msaaSettings->sampleCount = engine::renderer::addons::forward::NormalizeMsaaSamples(sampleCount);
}

uint32_t GetMsaa(const RenderSettingsState& render)
{
    return render.msaaSettings ? render.msaaSettings->sampleCount : 1u;
}

void SetGtaoEnabled(RenderSettingsState& render, bool enabled)
{
    if (render.gtaoResources)
        render.gtaoResources->settings.enabled = enabled;
}

bool GetGtaoEnabled(const RenderSettingsState& render)
{
    return render.gtaoResources && render.gtaoResources->settings.enabled;
}

void SetSsrEnabled(RenderSettingsState& render, bool enabled)
{
    if (render.ssrResources)
        render.ssrResources->settings.enabled = enabled;
}

bool GetSsrEnabled(const RenderSettingsState& render)
{
    return render.ssrResources && render.ssrResources->settings.enabled;
}

void SetContactShadowsEnabled(RenderSettingsState& render, bool enabled)
{
    if (render.contactShadowResources)
        render.contactShadowResources->settings.enabled = enabled;
}

bool GetContactShadowsEnabled(const RenderSettingsState& render)
{
    return render.contactShadowResources && render.contactShadowResources->settings.enabled;
}

void SetShadowQuality(RenderSettingsState& render, uint32_t sunResolution, uint32_t localResolution, uint8_t filter)
{
    if (!render.shadowQuality)
        return;
    render.shadowQuality->directionalResolution = engine::ClampShadowQualityResolution(sunResolution);
    render.shadowQuality->localResolution = engine::ClampShadowQualityResolution(localResolution);
    render.shadowQuality->filter = static_cast<engine::ShadowFilter>(std::min<uint8_t>(filter, engine::kMaxShadowQualityFilter));
}

void GetShadowQuality(const RenderSettingsState& render,
                      uint32_t& sunResolution, uint32_t& localResolution, uint8_t& filter)
{
    const engine::addons::shadow::ShadowQualitySettings quality =
        render.shadowQuality ? *render.shadowQuality : engine::addons::shadow::ShadowQualitySettings{};
    sunResolution = quality.directionalResolution;
    localResolution = quality.localResolution;
    filter = static_cast<uint8_t>(quality.filter);
}

void SetPostAA(RenderSettingsState& render, engine::RenderPostAA mode)
{
    if (render.postAASettings)
        render.postAASettings->mode = static_cast<engine::renderer::addons::forward::PostAAMode>(
            std::min<uint8_t>(static_cast<uint8_t>(mode), 2u));
}

engine::RenderPostAA GetPostAA(const RenderSettingsState& render)
{
    return render.postAASettings ? static_cast<engine::RenderPostAA>(render.postAASettings->mode)
                                 : engine::RenderPostAA::Off;
}

void SetQualityLevel(RenderSettingsState& render, engine::RenderQualityLevel level,
                     bool editorLiveState, const std::string& windowTitle)
{
    if (level == engine::RenderQualityLevel::Custom)
        return;
    const engine::RenderQualityOptions options = engine::RenderQualityPreset(level);
    SetMsaa(render, options.msaaSamples);
    SetPostAA(render, options.postAA);
    SetGtaoEnabled(render, options.gtaoEnabled);
    SetSsrEnabled(render, options.ssrEnabled);
    SetContactShadowsEnabled(render, options.contactShadowsEnabled);
    SetShadowQuality(render, options.sunShadowResolution, options.localShadowResolution, options.shadowFilter);
    if (editorLiveState)
        return;   // started from the editor: nothing to remember
    if (render.userQualityLevel != level)
    {
        render.userQualityLevel = level;
        SaveUserGameSettings(render, windowTitle);
    }
}

engine::RenderQualityLevel GetQualityLevel(const RenderSettingsState& render)
{
    engine::RenderQualityOptions options{};
    options.msaaSamples = GetMsaa(render);
    options.postAA = GetPostAA(render);
    options.gtaoEnabled = GetGtaoEnabled(render);
    options.ssrEnabled = GetSsrEnabled(render);
    options.contactShadowsEnabled = GetContactShadowsEnabled(render);
    GetShadowQuality(render, options.sunShadowResolution, options.localShadowResolution, options.shadowFilter);
    return engine::DetectRenderQualityLevel(options);
}

void SetShading(RenderSettingsState& render, const engine::renderer::addons::forward::ShadingSettings& settings)
{
    if (render.shadingSettings)
        *render.shadingSettings = settings;
}

engine::renderer::addons::forward::ShadingSettings GetShading(const RenderSettingsState& render)
{
    return render.shadingSettings ? *render.shadingSettings : engine::renderer::addons::forward::ShadingSettings{};
}

void SetReflectionProbesEnabled(RenderSettingsState& render, bool enabled)
{
    if (render.probeRuntime)
        render.probeRuntime->SetEnabled(enabled);
}

void SetSpecularOcclusion(RenderSettingsState& render, bool enabled)
{
    if (!render.probeRuntime)
        return;
    engine::addons::probes::ProbeSettings settings = render.probeRuntime->GetSettings();
    settings.specularOcclusion = enabled;
    render.probeRuntime->SetSettings(settings);
}

bool GetSpecularOcclusion(const RenderSettingsState& render)
{
    return render.probeRuntime && render.probeRuntime->GetSettings().specularOcclusion;
}

void SetProbeDebugView(RenderSettingsState& render, uint32_t view)
{
    if (!render.probeRuntime)
        return;
    engine::addons::probes::ProbeSettings settings = render.probeRuntime->GetSettings();
    settings.debugView = static_cast<engine::addons::probes::ProbeDebugView>(std::min(view, 3u));
    render.probeRuntime->SetSettings(settings);
}

// =============================================================================
// Script API
// =============================================================================

engine::script::RenderSettings GetScriptRenderSettings(const RenderSettingsState& render)
{
    engine::script::RenderSettings out{};
    if (render.tonemapSettings)
    {
        out.tonemap = static_cast<engine::script::TonemapMode>(render.tonemapSettings->op);
        out.exposureEv = render.tonemapSettings->exposureEv;
        out.autoExposure = render.tonemapSettings->exposureMode == engine::renderer::addons::forward::ExposureMode::Auto;
    }
    out.msaaSamples = GetMsaa(render);
    const engine::renderer::addons::forward::ShadingSettings shading = GetShading(render);
    out.heightCorrelatedVisibility = shading.heightCorrelatedVisibility;
    out.perLightDiffuseFresnel = shading.perLightDiffuseFresnel;
    out.multiScatterCompensation = shading.multiScatterCompensation;
    out.specularAntiAliasing = shading.specularAntiAliasing;
    out.burleyDiffuse = shading.burleyDiffuse;
    out.specularOcclusion = GetSpecularOcclusion(render);
    out.gtaoEnabled = GetGtaoEnabled(render);
    out.ssrEnabled = GetSsrEnabled(render);
    out.contactShadowsEnabled = GetContactShadowsEnabled(render);
    out.postAA = render.postAASettings ? static_cast<engine::script::PostAAMode>(render.postAASettings->mode)
                                       : engine::script::PostAAMode::Off;
    if (render.tonemapSettings)
        out.grading = engine::script::ToScriptColorGrading(render.tonemapSettings->grading);
    GetShadowQuality(render, out.sunShadowResolution, out.localShadowResolution, out.shadowFilter);
    return out;
}

void SetScriptRenderSettings(RenderSettingsState& render, const engine::script::RenderSettings& in)
{
    using namespace engine::renderer::addons::forward;
    if (render.tonemapSettings)
    {
        render.tonemapSettings->op = static_cast<TonemapOperator>(
            std::min<uint8_t>(static_cast<uint8_t>(in.tonemap), 3u));
        render.tonemapSettings->exposureEv = std::clamp(in.exposureEv, kMinExposureEv, kMaxExposureEv);
        render.tonemapSettings->exposureMode = in.autoExposure ? ExposureMode::Auto : ExposureMode::Fixed;
        render.tonemapSettings->grading = engine::script::FromScriptColorGrading(in.grading);
    }
    SetMsaa(render, in.msaaSamples);
    if (render.shadingSettings)
        *render.shadingSettings = { in.heightCorrelatedVisibility, in.perLightDiffuseFresnel,
                                    in.multiScatterCompensation, in.specularAntiAliasing,
                                    in.burleyDiffuse };
    SetSpecularOcclusion(render, in.specularOcclusion);
    SetGtaoEnabled(render, in.gtaoEnabled);
    SetSsrEnabled(render, in.ssrEnabled);
    SetContactShadowsEnabled(render, in.contactShadowsEnabled);
    if (render.postAASettings)
        render.postAASettings->mode = static_cast<PostAAMode>(std::min<uint8_t>(static_cast<uint8_t>(in.postAA), 2u));
    SetShadowQuality(render, in.sunShadowResolution, in.localShadowResolution, in.shadowFilter);
}

} // namespace Krom::internal
