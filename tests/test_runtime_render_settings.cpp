// Opt-in integration test of the krom.h render options: first the module behind
// them (KromRenderSettings) without a device, then the krom.h functions against
// the real runtime. Start from the repository directory; the DX11 window stays hidden.
#include "krom.h"
#include "KromRenderSettings.hpp"

#include <cstdio>
#include <cstdlib>
#include <filesystem>

int main()
{
    namespace fwd = engine::renderer::addons::forward;
    using engine::RenderQualityLevel;
    using Krom::internal::RenderSettingsState;

    int failures = 0;
    const auto check = [&](bool ok, const char* message) {
        std::printf("%s: %s\n", ok ? "PASS" : "FAIL", message);
        if (!ok) ++failures;
    };

    // The player's level goes to a scratch folder, not to %LOCALAPPDATA%.
    const std::filesystem::path settingsDir =
        std::filesystem::temp_directory_path() / "krom_render_settings_test";
    std::error_code ec;
    std::filesystem::remove_all(settingsDir, ec);
    std::filesystem::create_directories(settingsDir, ec);
    _putenv_s("KROM_USER_SETTINGS_DIR", settingsDir.string().c_str());
    const std::filesystem::path settingsFile = settingsDir / "user-settings.json";

    // --- Module without a device: the add-on resources stay null ----------------
    {
        RenderSettingsState render;
        check(Krom::internal::GetMsaa(render) == 1u, "MSAA starts off");
        Krom::internal::SetMsaa(render, 3u);
        check(Krom::internal::GetMsaa(render) == 2u, "MSAA 3 is rounded down to 2");

        Krom::internal::SetGtaoEnabled(render, true);
        Krom::internal::SetSsrEnabled(render, true);
        Krom::internal::SetContactShadowsEnabled(render, true);
        Krom::internal::SetSpecularOcclusion(render, true);
        check(!Krom::internal::GetGtaoEnabled(render) && !Krom::internal::GetSsrEnabled(render) &&
              !Krom::internal::GetContactShadowsEnabled(render) && !Krom::internal::GetSpecularOcclusion(render),
              "add-on switches stay off without their add-ons");

        uint32_t sun = 0u, local = 0u;
        uint8_t filter = 0u;
        Krom::internal::SetShadowQuality(render, 3000u, 10u, 99u);
        Krom::internal::GetShadowQuality(render, sun, local, filter);
        check(sun == 2048u && local == 256u && filter == engine::kMaxShadowQualityFilter,
              "shadow quality is clamped");

        Krom::internal::SetTonemapping(render, fwd::TonemapOperator::AcesFitted, 100.0f);
        const fwd::TonemapSettings tonemap = Krom::internal::GetTonemapping(render);
        check(tonemap.op == fwd::TonemapOperator::AcesFitted && tonemap.exposureEv == fwd::kMaxExposureEv,
              "tonemapping keeps the operator and clamps the exposure");

        Krom::internal::SetPostAA(render, engine::RenderPostAA::Smaa);
        check(Krom::internal::GetPostAA(render) == engine::RenderPostAA::Smaa, "post-AA round trip");

        // Script block: what Get returns can be set again without a change.
        engine::script::RenderSettings script = Krom::internal::GetScriptRenderSettings(render);
        check(script.msaaSamples == 2u && script.sunShadowResolution == 2048u &&
              script.postAA == engine::script::PostAAMode::Smaa, "script block reads the same values");
        script.msaaSamples = 4u;
        script.exposureEv = -1.5f;
        Krom::internal::SetScriptRenderSettings(render, script);
        check(Krom::internal::GetMsaa(render) == 4u &&
              Krom::internal::GetTonemapping(render).exposureEv == -1.5f, "script block writes the values");

        // Scene values.
        fwd::ColorGradingSettings grading{};
        Krom::internal::ApplySceneRenderSettings(render, 2.0f, fwd::ExposureMode::Auto, fwd::AutoExposureSettings{},
                                                 grading, 2u);
        check(Krom::internal::GetTonemapping(render).exposureEv == 2.0f &&
              Krom::internal::GetTonemapping(render).exposureMode == fwd::ExposureMode::Auto,
              "scene load sets exposure and exposure mode");

        // Player's level: saved outside the editor, not from the editor.
        Krom::internal::SetQualityLevel(render, RenderQualityLevel::Low, true, "Test");
        check(!render.userQualityLevel && !std::filesystem::exists(settingsFile),
              "a run started from the editor remembers no level");
        Krom::internal::SetQualityLevel(render, RenderQualityLevel::Low, false, "Test");
        check(render.userQualityLevel == RenderQualityLevel::Low && std::filesystem::exists(settingsFile),
              "the player's level is saved");
        Krom::internal::SetQualityLevel(render, RenderQualityLevel::Custom, false, "Test");
        check(render.userQualityLevel == RenderQualityLevel::Low, "Custom changes nothing");

        RenderSettingsState loaded;
        Krom::internal::LoadUserGameSettings(loaded, "Test");
        check(loaded.userQualityLevel == RenderQualityLevel::Low, "the player's level is read back");

        // The player's level replaces the project's level options, except from the editor.
        engine::ProjectRenderSettings project{};
        project.msaaSamples = 4u;
        project.gtaoIntensity = 0.3f;
        const uint32_t lowMsaa = engine::RenderQualityPreset(RenderQualityLevel::Low).msaaSamples;
        Krom::internal::ApplyProjectRenderSettings(loaded, project, false);
        check(Krom::internal::GetMsaa(loaded) == lowMsaa && loaded.baseGtaoIntensity == 0.3f,
              "project options with the player's level on top");
        Krom::internal::ApplyProjectRenderSettings(loaded, project, true);
        check(Krom::internal::GetMsaa(loaded) == 4u, "project options alone when started from the editor");
    }

    // --- krom.h against the real runtime ---------------------------------------
    const auto sourceRoot = std::filesystem::path(__FILE__).parent_path().parent_path();
    Krom::SetAssetRoot(sourceRoot / "projects/DemoProject/Assets");
    Krom::GraphicsConfig config{};
    config.backend = Krom::Renderer::DX11;
    config.visible = false;
    config.audioDevice = false;
    config.width = 64;
    config.height = 64;
    if (!Krom::Graphics(config))
        return 2;

    for (const RenderQualityLevel level :
         {RenderQualityLevel::Low, RenderQualityLevel::Medium, RenderQualityLevel::High})
    {
        Krom::SetQualityLevel(level);
        const engine::RenderQualityOptions preset = engine::RenderQualityPreset(level);
        uint32_t sun = 0u, local = 0u;
        uint8_t filter = 0u;
        Krom::GetShadowQuality(sun, local, filter);
        const bool matches = Krom::GetQualityLevel() == level && Krom::GetMsaa() == preset.msaaSamples &&
            Krom::GetPostAA() == preset.postAA && Krom::GetGtaoEnabled() == preset.gtaoEnabled &&
            Krom::GetSsrEnabled() == preset.ssrEnabled &&
            Krom::GetContactShadowsEnabled() == preset.contactShadowsEnabled &&
            sun == preset.sunShadowResolution && local == preset.localShadowResolution &&
            filter == preset.shadowFilter;
        check(matches, engine::RenderQualityLevelName(level));
    }

    Krom::SetGtaoEnabled(false);
    Krom::SetSsrEnabled(false);
    Krom::SetContactShadowsEnabled(false);
    check(!Krom::GetGtaoEnabled() && !Krom::GetSsrEnabled() && !Krom::GetContactShadowsEnabled(),
          "add-on switches off");
    Krom::SetGtaoEnabled(true);
    Krom::SetSsrEnabled(true);
    Krom::SetContactShadowsEnabled(true);
    check(Krom::GetGtaoEnabled() && Krom::GetSsrEnabled() && Krom::GetContactShadowsEnabled(),
          "add-on switches on");
    check(Krom::GetQualityLevel() != RenderQualityLevel::Low, "level follows the switches");

    Krom::SetSpecularOcclusion(true);
    check(Krom::GetSpecularOcclusion(), "specular occlusion on");
    Krom::SetSpecularOcclusion(false);
    check(!Krom::GetSpecularOcclusion(), "specular occlusion off");

    fwd::ShadingSettings shading = Krom::GetShading();
    shading.burleyDiffuse = !shading.burleyDiffuse;
    Krom::SetShading(shading);
    check(Krom::GetShading().burleyDiffuse == shading.burleyDiffuse, "shading round trip");

    fwd::ColorGradingSettings grading = Krom::GetColorGrading();
    grading.saturation = 1.25f;
    Krom::SetColorGrading(grading);
    check(Krom::GetColorGrading().saturation == 1.25f, "colour grading round trip");

    Krom::Shutdown();
    std::filesystem::remove_all(settingsDir, ec);
    std::printf("%d failure(s)\n", failures);
    return failures == 0 ? 0 : 1;
}
