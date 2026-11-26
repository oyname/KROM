#pragma once
#include "krom.h"

#include "addons/contact_shadow/ContactShadowFeature.hpp"
#include "addons/forward/ForwardAntiAliasing.hpp"
#include "addons/forward/ForwardShading.hpp"
#include "addons/forward/ForwardTonemap.hpp"
#include "addons/gtao/GtaoFeature.hpp"
#include "addons/probes/ProbeFeature.hpp"
#include "addons/script/IRenderSettingsService.hpp"
#include "addons/shadow/ShadowFeature.hpp"
#include "addons/ssr/SsrFeature.hpp"
#include "core/ProjectSettings.hpp"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>

// Render options of the krom.h wrapper (SetTonemapping, SetMsaa, SetQualityLevel,
// ...): the values the render add-ons read, the project and scene values applied
// to them, a player's quality level and the post-process volume blend.
// The functions only see RenderSettingsState, not the wrapper State.
namespace Krom::internal {

// Bloom strength of the forward feature (base of the post-process volumes).
inline constexpr float kRuntimeBloomIntensity = 0.12f;

// Owned by the wrapper State. The shared_ptr members are handed to the render
// features when they are registered; the add-on resources (GTAO, SSR, contact
// shadows, probes) are filled in by their features and stay null without them.
struct RenderSettingsState
{
    // Read every frame by the forward tonemap pass (Krom::SetTonemapping, scene files).
    std::shared_ptr<engine::renderer::addons::forward::TonemapSettings> tonemapSettings =
        std::make_shared<engine::renderer::addons::forward::TonemapSettings>();
    // What the tonemap pass reads: tonemapSettings with the post-process volumes
    // at the camera blended over it, rewritten every frame (tonemapSettings stays).
    std::shared_ptr<engine::renderer::addons::forward::TonemapSettings> effectiveTonemapSettings =
        std::make_shared<engine::renderer::addons::forward::TonemapSettings>();
    // Project effect intensities (base of the post-process volumes); the add-on
    // settings receive the blended values every frame.
    float baseGtaoIntensity = 0.65f;
    float baseSsrIntensity = 1.0f;
    float baseContactShadowIntensity = 1.0f;
    bool  bloomEnabled = true;   // project switch; off forces the bloom intensity to 0
    float baseBloomIntensity = kRuntimeBloomIntensity;
    float bloomThreshold = 1.0f;
    // Reset request of the automatic exposure on scene loads.
    std::shared_ptr<engine::renderer::addons::forward::AutoExposureStatus> autoExposureStatus =
        std::make_shared<engine::renderer::addons::forward::AutoExposureStatus>();
    std::shared_ptr<engine::renderer::addons::forward::MsaaSettings> msaaSettings =
        std::make_shared<engine::renderer::addons::forward::MsaaSettings>();
    // Post-process AA after tonemapping (project settings, KROM_POST_AA).
    std::shared_ptr<engine::renderer::addons::forward::PostAASettings> postAASettings =
        std::make_shared<engine::renderer::addons::forward::PostAASettings>();
    // GTAO add-on resources (settings); null without GTAO (enableGtao off).
    std::shared_ptr<engine::renderer::addons::gtao::GtaoGpuResources> gtaoResources;
    std::shared_ptr<engine::renderer::addons::ssr::SsrGpuResources> ssrResources;
    // Contact shadow add-on resources; null without it (enableContactShadows off).
    std::shared_ptr<engine::renderer::addons::contact_shadow::ContactShadowGpuResources> contactShadowResources;
    // Shadow resolution and filter of the quality level (read by the shadow add-on).
    std::shared_ptr<engine::addons::shadow::ShadowQualitySettings> shadowQuality =
        std::make_shared<engine::addons::shadow::ShadowQualitySettings>();
    // Quality level a player picked (Krom::SetQualityLevel, scripts), remembered on
    // this computer (user-settings.json); replaces the project's level options.
    // Not used by runs started from the editor (editor live state).
    std::optional<engine::RenderQualityLevel> userQualityLevel;
    // BRDF options of the PBR shaders (Krom::SetShading, scene files).
    std::shared_ptr<engine::renderer::addons::forward::ShadingSettings> shadingSettings =
        std::make_shared<engine::renderer::addons::forward::ShadingSettings>();
    // Local reflection probes (baked .kprobe files below the asset root).
    std::shared_ptr<engine::addons::probes::ProbeRuntime> probeRuntime;
};

// --- Project, scene and per-frame values ------------------------------------

// Project-wide render options (project settings, or the scene of a project
// written before they moved there). Exposure stays with the scene. A player's
// quality level replaces the level options unless the run was started from the
// editor (editorLiveState).
void ApplyProjectRenderSettings(RenderSettingsState& render,
                                const engine::ProjectRenderSettings& projectRendering,
                                bool editorLiveState);

// Values that belong to the scene; every scene load sets them again and asks
// the automatic exposure to start over.
void ApplySceneRenderSettings(RenderSettingsState& render,
                              float exposureEv,
                              engine::renderer::addons::forward::ExposureMode exposureMode,
                              const engine::renderer::addons::forward::AutoExposureSettings& autoExposure,
                              const engine::renderer::addons::forward::ColorGradingSettings& grading,
                              uint32_t probeBakePasses);

// KROM_MSAA=<n> and KROM_POST_AA=<name> replace the start values (diagnostics).
void ApplyRenderEnvironmentOverrides(RenderSettingsState& render);

// Post-process volumes at the camera over the scene and project values; writes
// effectiveTonemapSettings and the intensities of GTAO, SSR and contact shadows.
void ApplyPostProcessVolumes(RenderSettingsState& render,
                             const engine::ecs::World& world,
                             const engine::math::Vec3& cameraPosition);

// --- Player settings on this computer ---------------------------------------

// user-settings.json of an exported game: %LOCALAPPDATA%\KROM\Games\<window
// title or EXE name>\ (Windows), $XDG_CONFIG_HOME/krom/games/<...>/ or
// ~/.config/krom/games/<...>/ elsewhere. KROM_USER_SETTINGS_DIR replaces the
// folder (tests). Empty if no folder can be found.
[[nodiscard]] std::filesystem::path UserGameSettingsPath(const std::string& windowTitle);
// Reads userQualityLevel (reset when the file is missing or invalid).
void LoadUserGameSettings(RenderSettingsState& render, const std::string& windowTitle);
void SaveUserGameSettings(const RenderSettingsState& render, const std::string& windowTitle);

// --- krom.h functions --------------------------------------------------------

void SetTonemapping(RenderSettingsState& render,
                    engine::renderer::addons::forward::TonemapOperator op, float exposureEv);
[[nodiscard]] engine::renderer::addons::forward::TonemapSettings GetTonemapping(const RenderSettingsState& render);
void SetColorGrading(RenderSettingsState& render,
                     const engine::renderer::addons::forward::ColorGradingSettings& grading);
[[nodiscard]] engine::renderer::addons::forward::ColorGradingSettings GetColorGrading(const RenderSettingsState& render);

void SetMsaa(RenderSettingsState& render, uint32_t sampleCount);
[[nodiscard]] uint32_t GetMsaa(const RenderSettingsState& render);
void SetGtaoEnabled(RenderSettingsState& render, bool enabled);
[[nodiscard]] bool GetGtaoEnabled(const RenderSettingsState& render);
void SetSsrEnabled(RenderSettingsState& render, bool enabled);
[[nodiscard]] bool GetSsrEnabled(const RenderSettingsState& render);
void SetContactShadowsEnabled(RenderSettingsState& render, bool enabled);
[[nodiscard]] bool GetContactShadowsEnabled(const RenderSettingsState& render);
void SetShadowQuality(RenderSettingsState& render, uint32_t sunResolution, uint32_t localResolution, uint8_t filter);
void GetShadowQuality(const RenderSettingsState& render,
                      uint32_t& sunResolution, uint32_t& localResolution, uint8_t& filter);
void SetPostAA(RenderSettingsState& render, engine::RenderPostAA mode);
[[nodiscard]] engine::RenderPostAA GetPostAA(const RenderSettingsState& render);

// Sets the options of the level; Custom changes nothing. Unless the run was
// started from the editor, the level is also saved to user-settings.json.
void SetQualityLevel(RenderSettingsState& render, engine::RenderQualityLevel level,
                     bool editorLiveState, const std::string& windowTitle);
[[nodiscard]] engine::RenderQualityLevel GetQualityLevel(const RenderSettingsState& render);

void SetShading(RenderSettingsState& render, const engine::renderer::addons::forward::ShadingSettings& settings);
[[nodiscard]] engine::renderer::addons::forward::ShadingSettings GetShading(const RenderSettingsState& render);

void SetReflectionProbesEnabled(RenderSettingsState& render, bool enabled);
void SetSpecularOcclusion(RenderSettingsState& render, bool enabled);
[[nodiscard]] bool GetSpecularOcclusion(const RenderSettingsState& render);
void SetProbeDebugView(RenderSettingsState& render, uint32_t view);

// --- Script API (IScriptContext::GetRenderSettings) --------------------------

// The same values as the Set*/Get* functions above, as one block.
[[nodiscard]] engine::script::RenderSettings GetScriptRenderSettings(const RenderSettingsState& render);
void SetScriptRenderSettings(RenderSettingsState& render, const engine::script::RenderSettings& in);

} // namespace Krom::internal
