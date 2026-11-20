// Optionaler GPU-Test: --headless automatisiert, ohne Argumente sichtbare
// ImGui-Anzeige fuer 40 s. Kein plattformspezifischer UI-Code.
#include "app/KromEditorApp.hpp"
#include <map>
#include <numeric>
#include "app/editor/EnginePaths.hpp"
#include "app/editor/BuildTools.hpp"
#include "addons/animation/AnimationComponents.hpp"
#include "addons/gtao/GtaoPass.hpp"
#include "addons/editor/EditorAnimationWindow.hpp"
#include "addons/editor/EditorAssetBrowser.hpp"
#include "addons/editor/EditorCommands.hpp"
#include "addons/editor/EditorLocalization.hpp"
#include "addons/editor/EditorMaterialLibrary.hpp"
#include "addons/editor/EditorModelClips.hpp"
#include "addons/editor/EditorUIInternal.hpp"
#include "addons/editor/EditorAnimationControllers.hpp"
#include "addons/editor/EditorControllerGraph.hpp"
#include "assets/AssetRegistry.hpp"
#include "assets/KMeshSerializer.hpp"
#include "addons/editor/EditorPreferences.hpp"
#include "core/PathUtf8.hpp"
#include "renderer/ShaderCompiler.hpp"
#include "addons/script/ScriptList.hpp"
#include "addons/editor/EditorScriptAssets.hpp"
#include "addons/particles/ParticleComponents.hpp"
#include "addons/prefab/PrefabInstanceComponent.hpp"
#include "addons/probes/ProbeFeature.hpp"
#include "addons/postprocess/PostProcessVolumeComponent.hpp"
#include "imgui.h"
#include "imgui_internal.h"
#include <chrono>
#include <cmath>
#include <thread>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <algorithm>
#include <set>
#include <vector>

namespace engine::app {
using renderer::addons::editor::Translate;

struct EditorLoadingSmokeTest final : IAppScene
{
    bool sdkFlow = false;
    int sdkStep = 0;
    int sdkPlayFrames = 0;
    bool sdkBuildOk = false;
    std::chrono::steady_clock::time_point sdkStarted;
    std::filesystem::path sdkProject;

    int TestSdkProjectFlow()
    {
        const auto& paths = GetEnginePaths();
        if (!paths.sdk || !paths.BuildsAvailable()) { std::printf("SDK unavailable: %s\n", paths.error.c_str()); return 1; }
        KromEditorAppConfig config;
        config.backend = SelectAppBackend();
        config.visible = false;
        config.enableDebugLayer = false;
        if (!app.Initialize(config)) return 1;
        sdkFlow = true;
        sdkStarted = std::chrono::steady_clock::now();
        Check(app.CreateEditorProject("SDKProbe", PathToUtf8(paths.userDataDir.parent_path()), config.backend), "create project with installed SDK");
        sdkProject = app.m_projectRoot;
        std::filesystem::create_directories(sdkProject / "Assets/Scripts");
        std::ofstream(sdkProject / "Assets/Scripts/SdkProbe.hpp") <<
            "#pragma once\n#include \"krom.h\"\nclass SdkProbe final : public engine::script::ComponentScript {\n"
            "public: void OnUpdate(engine::script::IScriptContext&) override; };\n// KROM_SCRIPT(SdkProbe)\n";
        std::ofstream(sdkProject / "Assets/Scripts/SdkProbe.cpp") <<
            "#include \"SdkProbe.hpp\"\nvoid SdkProbe::OnUpdate(engine::script::IScriptContext& ctx) { ctx.SetPosition({4.f, 0.f, 0.f}); }\n";
        const auto entity = renderer::addons::editor::CreateEmptyEntity(*app.m_editorFrameCtx, "SDK Probe");
        app.m_world->Add<script::ScriptList>(entity).AddNameOnly("SdkProbe");
        app.m_projectSettings.testBuild = 1;
        Check(!app.m_editorFrameCtx->playGame() && app.m_editorFrameCtx->lastFileMessage == Translate("build.sdk_release_only"), "SDK rejects Debug before testbuild preparation");
        app.m_projectSettings.testBuild = 3;
        app.m_projectSettings.exportBuild = 1;
        Check(!app.ExportProjectBuild() && app.m_editorFrameCtx->lastFileMessage == Translate("build.sdk_release_only"), "SDK rejects RelWithDebInfo export");
        app.m_projectSettings.exportBuild = 0;
        if (failures == 0) Check(app.Run(*this) == 0, "installed SDK editor loop completed");
        app.Shutdown();
        return failures ? 1 : 0;
    }

    bool UpdateSdkFlow(AppSceneContext& context)
    {
        if (std::chrono::steady_clock::now() - sdkStarted > std::chrono::minutes(10)) Check(false, "SDK flow timeout");
        if (failures) {
            if (app.m_editorFrameCtx) {
                std::printf("SDK status: %s\n", app.m_editorFrameCtx->lastFileMessage.c_str());
                for (const auto& line : app.m_editorState.consoleLogs) std::printf("%s\n", line.message.c_str());
            }
            context.renderLoop.GetWindow()->RequestClose(); return true;
        }
        if (app.m_runtimeBuildJob) return true;
        if (sdkStep == 0) {
            sdkStep = 1;
            Check(app.StartEditorPlay(), "SDK starts project-local script build");
        } else if (sdkStep == 1) {
            Check(app.m_playActive && app.m_scriptModule.IsLoaded(), "SDK script DLL loads and Play starts");
            if (++sdkPlayFrames < 5) return true;
            const auto probe = FindPlayUiEntity("SDK Probe");
            Check(probe.IsValid() && app.m_world->Get<TransformComponent>(probe)->localPosition.x == 4.f, "SDK script executes through editor ABI");
            app.StopEditorPlay();
            Check(!app.m_lastRuntimeBuildDir.empty() && app.m_lastRuntimeBuildDir.lexically_relative(sdkProject).begin()->string() == ".krom", "SDK build remains inside project/.krom");
            sdkStep = 2;
            Check(app.BuildAllScenes(), "SDK builds project scenes");
            Check(app.StartRuntimeBuild({}, [this](bool ok) { sdkBuildOk = ok; }), "SDK starts runtime testbuild");
        } else if (sdkStep == 2) {
            Check(sdkBuildOk, "SDK runtime links imported Release libraries");
            if (failures) return true;
            sdkStep = 3; sdkBuildOk = false;
            const auto exe = build::TargetPath(app.m_lastRuntimeBuildDir, "krom_runtime_" + app.m_projectRuntimeTarget + "_" +
                (app.m_projectBackend == renderer::DeviceFactory::BackendType::DirectX11 ? "dx11" : "vulkan"));
            Check(!exe.empty() && app.StartBuildJob("SDK runtime smoke", {{exe, {PathToUtf8(sdkProject / "krom-project.json"), "--warmup-shaders"}, sdkProject}},
                [this](bool ok) { sdkBuildOk = ok; }, "SDK runtime smoke"), "SDK testbuild starts and renders project");
        } else if (sdkStep == 3) {
            Check(sdkBuildOk, "SDK testbuild completed");
            sdkStep = 4;
            Check(app.ExportProjectBuild(), "SDK starts Release export");
        } else {
            Check(app.m_editorFrameCtx->lastFileMessage.starts_with(Translate("export.success")), "SDK exported game starts and completes shader warmup");
            context.renderLoop.GetWindow()->RequestClose();
        }
        return true;
    }

    // Run a copy of this executable beside a test manifest. No GPU, compiler
    // discovery or real project is needed: the guards must run before all of it.
    int TestSdkBuildGuards()
    {
        const auto& paths = GetEnginePaths();
        if (!paths.sdk || !paths.error.empty()) return 1;
        int failed = 0;
        auto check = [&](bool ok, const char* message) {
            std::printf("%s: %s\n", ok ? "PASS" : "FAIL", message);
            if (!ok) ++failed;
        };
        app.m_world = std::make_unique<ecs::World>(app.m_componentRegistry);
        app.m_editorFrameCtx = std::make_unique<renderer::addons::editor::EditorFrameContext>(
            *app.m_world, app.m_materialSystem, app.m_assetRegistry, app.m_editorState);
        const auto expected = std::string(Translate("build.sdk_unavailable"));
        check(!app.StartEditorPlay() && app.m_editorFrameCtx->lastFileMessage == expected,
              "SDK Play fails clearly before project or script preparation");
        check(!app.ExportProjectBuild() && app.m_editorFrameCtx->lastFileMessage == expected,
              "SDK export fails clearly before scene saves or staging");
        bool callback = false;
        check(!app.StartRuntimeBuild({}, [&](bool) { callback = true; }) && !app.m_runtimeBuildJob && !callback,
              "SDK test build starts no worker or compiler discovery");
        check(!app.StartRuntimeBuild({}, {}, "krom_scripts_test") && !app.m_runtimeBuildJob,
              "SDK script build starts no worker");
        std::string error;
        check(!app.CheckScriptEngineLibraries(error) && error == expected && !app.ReloadProjectScriptModule(true),
              "SDK script reload never inspects development libraries or headers");
        check(app.GetCurrentAssetRoot() == paths.assetsDir && app.GetCurrentShaderCacheDir() == paths.shaderCacheDir &&
              app.GetCurrentEditorScenePath() == paths.standaloneScenesDir / "editor_scene.json",
              "editor uses SDK resources and separate writable scene/cache locations");
        check(!std::filesystem::exists(paths.userDataDir / "scenes") && !std::filesystem::exists(paths.userDataDir / "cache"),
              "rejected builds create no scene or cache directories");
        app.m_editorFrameCtx.reset();
        app.m_world.reset();
        return failed ? 1 : 0;
    }

    int TestInvalidSdkManifest()
    {
        const auto& paths = GetEnginePaths();
        return paths.sdk && !paths.error.empty() && !app.Initialize({}) && !app.m_initialized &&
            !app.m_renderFeaturesRegistered ? 0 : 1;
    }

    int TestIniPath()
    {
        KromEditorAppConfig config;
        config.backend = SelectAppBackend();
        config.visible = false;
        config.enableDebugLayer = false;
        if (!app.Initialize(config)) return 1;
        const auto expected = GetEnginePaths().userDataDir / "imgui.ini";
        const auto* filename = ImGui::GetIO().IniFilename;
        const bool configured = filename && PathFromUtf8(filename) == expected;
        const bool cacheConfigured = !GetEnginePaths().sdk ||
            renderer::ShaderCompiler::GetCacheDirectory() == GetEnginePaths().shaderCacheDir;
        ImGui::LoadIniSettingsFromMemory("[Window][EnginePathsProbe]\nPos=17,29\nSize=300,200\nCollapsed=0\n");
        if (configured) ImGui::SaveIniSettingsToDisk(filename);
        std::ifstream in(expected, std::ios::binary);
        const std::string saved((std::istreambuf_iterator<char>(in)), {});
        const bool persisted = saved.find("EnginePathsProbe") != std::string::npos;
        std::printf("%s: editor ImGui settings use user directory and save Unicode path\n",
                    configured && persisted ? "PASS" : "FAIL");
        app.Shutdown();
        return configured && persisted && cacheConfigured ? 0 : 1;
    }

    KromEditorApp app;
    int frames = 0;
    int failures = 0;
    bool directSwitch = false;
    bool visualProgress = false;
    bool pausedOnce = false;
    bool progressDrawn = false;
    std::shared_ptr<SceneLoadJob> pausedJob;
    std::chrono::steady_clock::time_point resumeAt;
    std::set<std::string> phases;
    bool playUi = false;
    std::filesystem::path particlePreviewPrefab;
    bool settingsUi = false;
    bool consoleUi = false;
    bool startupProject = false;
    bool sceneClose = false;
    bool toolbarLogo = false;
    int launcherFrameBeforeRequest = 0;
    std::string startupProjectFile;
    int pausedCounter = 0;
    ParticleID previousParticle{};
    EntityID prewarmEmitter{};
    struct PlayCounter final : script::ComponentScript {
        int32_t spawnCount = 0;
        ParticleID particle{};
        bool clipPlayed = false;
        void OnUpdate(script::IScriptContext& ctx) override
        {
            if (++spawnCount == 1)
            {
                particle = ctx.SpawnParticle(ctx.GetEntity(), {0.f, 1.f, 0.f});
                ctx.SetParticleMotionEnabled(particle, false);
                ctx.SetParticleSize(particle, {1.f, 1.f});
                const auto clip = ctx.FindEmitterClip(ctx.GetEntity(), "Reverse");
                clipPlayed = clip && clip->first == 3u && clip->last == 0u && ctx.PlayParticleClip(particle, "Reverse");
            }
        }
    };
    EntityID FindPlayUiEntity(std::string_view entityName) const
    {
        EntityID result{};
        app.m_world->ForEachAlive([&](EntityID e) {
            // Scene objects only: the animation window's hidden copy carries the same names.
            if (const auto* name = app.m_world->Get<NameComponent>(e); name && name->name == entityName &&
                !app.m_world->Has<renderer::addons::editor::EditorRuntimeGizmoTag>(e))
                result = e;
        });
        return result;
    }
    EntityID FindCounter() const { return FindPlayUiEntity("mask2"); }
    int InspectorCounter()
    {
        auto& ctx=*app.m_editorFrameCtx;
        const auto* list=app.m_world->Get<script::ScriptList>(FindCounter());
        const auto& instance=list->Instances()[0];
        const auto& field=app.m_scriptRegistry.GetFields("PlayCounter")->front();
        auto value=instance.fieldValues.at("spawnCount");
        renderer::addons::editor::ReadLiveEditorScriptField(ctx,instance,field,value);
        return value.intValue;
    }
    bool materialUi = false;
    // --auto-exposure <project.json> <capture-dir>: a uniform HDR surface changes
    // from bright to dark, first alone and then with the material preview open.
    std::filesystem::path autoExposureCaptures;
    bool gizmoColorProbe = false;
    std::array<std::array<float, 3>, 3> gizmoReferenceColors{};
    MaterialHandle exposureProbeMaterial{};
    size_t exposureProbeStage = 0u;
    std::chrono::steady_clock::time_point exposureProbeStarted{};
    std::array<float, 5> exposureProbeValues{};
    std::array<float, 5> exposureProbeBrightness{};
    bool UpdateAutoExposure(AppSceneContext& context)
    {
        namespace ed = renderer::addons::editor;
        using namespace renderer::addons::forward;
        auto& ui = *app.m_editorFrameCtx;
        if (!exposureProbeMaterial.IsValid())
        {
            assets::SubMeshData quad;
            quad.positions = {-20.f,-20.f,0.f, 20.f,-20.f,0.f, 20.f,20.f,0.f, -20.f,20.f,0.f};
            quad.normals = {0.f,0.f,1.f, 0.f,0.f,1.f, 0.f,0.f,1.f, 0.f,0.f,1.f};
            quad.uvs = {0.f,0.f, 1.f,0.f, 1.f,1.f, 0.f,1.f};
            quad.indices = {0u,1u,2u, 0u,2u,3u};
            const auto root = app.GetCurrentAssetRoot();
            Check(assets::KMeshSave(root / "ExposureProbe.kmesh", {quad}), "save exposure probe mesh");
            std::ofstream(root / "Materials/ExposureProbe.mat") <<
                "template unlit\ndoubleSided true\nbaseColorFactor 1 1 1 1\n";
            const auto mesh = context.assetPipeline.LoadMesh("ExposureProbe.kmesh");
            const auto entity = ed::CreateEmptyEntity(ui, "Exposure metering probe");
            auto* transform = app.m_world->Get<TransformComponent>(entity);
            transform->localPosition = {};
            transform->localRotation = {};
            transform->localScale = {1.f,1.f,1.f};
            transform->dirty = true;
            app.m_world->Add<MeshComponent>(entity, mesh);
            Check(ed::ApplyMaterialAssetToEntity(ui, entity, "Materials/ExposureProbe.mat"),
                  "bind exposure probe material");
            auto* material = app.m_world->Get<MaterialComponent>(entity);
            exposureProbeMaterial = material ? material->material : MaterialHandle{};
            if (material) material->materialAssetPath.clear(); // the test drives the runtime colour
            Check(exposureProbeMaterial.IsValid(), "exposure probe material ready");
            if (!exposureProbeMaterial.IsValid())
            {
                context.renderLoop.GetWindow()->RequestClose();
                return true;
            }
            ui.state.editorCamera.position = {0.f,0.f,4.f};
            ui.state.editorCamera.pitchDeg = ui.state.editorCamera.yawDeg = 0.f;
            ui.state.tonemap.exposureMode = ExposureMode::Auto;
            ui.state.tonemap.exposureEv = 0.f;
            ui.state.tonemap.freezeAutoExposure = false;
            ui.state.tonemap.autoExposure.toBrightSpeed = ui.state.tonemap.autoExposure.toDarkSpeed = 20.f;
            app.m_autoExposureStatus->RequestReset();
            if (gizmoColorProbe)
            {
                ui.state.editorCamera.position = {4.f, 3.f, 6.f};
                ui.state.editorCamera.yawDeg = 33.690068f;
                ui.state.editorCamera.pitchDeg = -22.588538f;
                ui.state.gizmoMode = ed::GizmoMode::Position;
                ui.state.selectedEntity = entity;
                ui.state.selectionSource = ed::SelectionSource::Outliner;
            }
        }
        if (gizmoColorProbe)
            return UpdateGizmoColors(context);
        const bool bright = exposureProbeStage == 0u || exposureProbeStage == 2u;
        const float value = bright ? 1.8f : 0.018f;
        context.materialSystem.SetVec4(exposureProbeMaterial, "baseColorFactor", {value,value,value,1.f});
        auto& runtime = context.renderLoop.GetRenderSystem().GetShaderRuntime();
        if (runtime.HasPendingShaderCompiles() || frames < 20)
        {
            exposureProbeStarted = {};
            return true;
        }
        const auto now = std::chrono::steady_clock::now();
        if (exposureProbeStarted == std::chrono::steady_clock::time_point{})
            exposureProbeStarted = now;
        if (now - exposureProbeStarted < std::chrono::seconds(2))
            return true;
        const char* names[] = {"bright-closed", "dark-closed", "bright-preview", "dark-preview", "dark-closed-again"};
        Check(app.m_autoExposureStatus->valid.load(), "automatic exposure produced a measurement");
        const float ev = app.m_autoExposureStatus->currentEv.load();
        exposureProbeValues[exposureProbeStage] = ev;
        std::printf("EXPOSURE %s %+.4f EV\n", names[exposureProbeStage], ev);
        auto* device = context.renderLoop.GetRenderSystem().GetDevice();
        const auto texture = device->GetRenderTargetColorTexture(app.m_sceneView.rt);
        std::vector<uint8_t> pixels;
        uint32_t width = 0u, height = 0u;
        const bool read = device->ReadTexturePixels(texture, pixels, width, height) && width >= 40u && height >= 40u &&
            pixels.size() == static_cast<size_t>(width) * height * 4u;
        Check(read, "exposure probe GPU image readback");
        if (read)
        {
            float sum = 0.f;
            for (uint32_t y = height/2u-20u; y < height/2u+20u; ++y)
                for (uint32_t x = width/2u-20u; x < width/2u+20u; ++x)
                    for (uint32_t c = 0u; c < 3u; ++c)
                        sum += pixels[(static_cast<size_t>(y)*width+x)*4u+c];
            exposureProbeBrightness[exposureProbeStage] = sum / (40.f*40.f*3.f);
            std::ofstream out(autoExposureCaptures / (std::string(names[exposureProbeStage]) + ".ppm"), std::ios::binary);
            out << "P6\n" << width << ' ' << height << "\n255\n";
            for (size_t i = 0u; i < pixels.size(); i += 4u)
                out.write(reinterpret_cast<const char*>(pixels.data()+i), 3);
        }
        exposureProbeStarted = now;
        ++exposureProbeStage;
        if (exposureProbeStage == 2u)
            ed::OpenMaterialAsset(ui, app.GetCurrentAssetRoot() / "Materials/Weiss.mat");
        if (exposureProbeStage == 4u)
            ui.state.materialWindowOpen = false;
        if (exposureProbeStage == 5u)
        {
            Check(exposureProbeValues[1] > exposureProbeValues[0] + 4.f, "dark view increases automatic exposure");
            Check(std::fabs(exposureProbeValues[2] - exposureProbeValues[0]) < 0.15f,
                  "material preview preserves bright scene exposure");
            Check(std::fabs(exposureProbeValues[3] - exposureProbeValues[1]) < 0.15f,
                  "material preview preserves dark scene exposure");
            Check(std::fabs(exposureProbeBrightness[2] - exposureProbeBrightness[0]) < 1.f,
                  "material preview preserves rendered bright scene brightness");
            Check(std::fabs(exposureProbeBrightness[3] - exposureProbeBrightness[1]) < 1.f,
                  "material preview preserves rendered dark scene brightness");
            context.renderLoop.GetWindow()->RequestClose();
        }
        return true;
    }

    bool UpdateGizmoColors(AppSceneContext& context)
    {
        using namespace renderer::addons::forward;
        auto& ui = *app.m_editorFrameCtx;
        const bool automatic = exposureProbeStage >= 12u;
        const size_t aa = exposureProbeStage % 3u;
        ui.state.msaaSamples = aa == 0u ? 1u : aa == 1u ? 2u : 4u;
        ui.state.postAA = static_cast<PostAAMode>(aa);
        ui.state.tonemap.op = automatic ? TonemapOperator::AgX : static_cast<TonemapOperator>(exposureProbeStage / 3u);
        ui.state.tonemap.exposureMode = automatic ? ExposureMode::Auto : ExposureMode::Fixed;
        ui.state.tonemap.exposureEv = automatic ? 0.f : (static_cast<float>(exposureProbeStage % 3u) - 1.f) * 8.f;
        const float value = !automatic ? 1.f : exposureProbeStage == 12u ? 100.f : 0.001f;
        context.materialSystem.SetVec4(exposureProbeMaterial, "baseColorFactor", {value, value, value, 1.f});
        const auto now = std::chrono::steady_clock::now();
        auto& runtime = context.renderLoop.GetRenderSystem().GetShaderRuntime();
        if (runtime.HasPendingShaderCompiles() || frames < 30)
        {
            exposureProbeStarted = now;
            return true;
        }
        if (exposureProbeStarted == std::chrono::steady_clock::time_point{}) exposureProbeStarted = now;
        if (now - exposureProbeStarted < std::chrono::milliseconds(500)) return true;
        auto* device = context.renderLoop.GetRenderSystem().GetDevice();
        std::vector<uint8_t> pixels;
        uint32_t width = 0u, height = 0u;
        const bool read = device->ReadTexturePixels(device->GetRenderTargetColorTexture(app.m_sceneView.rt), pixels, width, height);
        Check(read && pixels.size() == size_t(width) * height * 4u, "gizmo display target readback");
        if (read)
        {
            std::array<size_t, 3> counts{};
            std::array<std::array<double, 3>, 3> sums{};
            for (size_t i = 0; i + 3u < pixels.size(); i += 4u)
            {
                const int r = pixels[i], g = pixels[i+1], b = pixels[i+2];
                const int axis = r > 220 && g < 10 && b < 10 ? 0 :
                    r < 10 && g > 220 && b > 50 && b < 180 ? 1 :
                    r < 10 && b > 220 && g > 120 && g < 220 ? 2 : -1;
                if (axis < 0) continue;
                ++counts[axis];
                for (size_t c = 0; c < 3u; ++c) sums[axis][c] += pixels[i+c];
            }
            for (size_t axis = 0; axis < 3u; ++axis)
            {
                Check(counts[axis] > 20u, "saturated gizmo axis remains distinguishable");
                std::printf("GIZMO stage=%zu axis=%zu pixels=%zu", exposureProbeStage, axis, counts[axis]);
                for (size_t c = 0; c < 3u; ++c)
                {
                    const float mean = counts[axis] ? static_cast<float>(sums[axis][c] / counts[axis]) : -100.f;
                    std::printf(" %.3f", mean);
                    if (exposureProbeStage == 0u) gizmoReferenceColors[axis][c] = mean;
                    else Check(std::fabs(mean - gizmoReferenceColors[axis][c]) < 1.f, "axis color independent of exposure and tonemapping");
                }
                std::printf("\n");
            }
            std::ofstream out(autoExposureCaptures / ("gizmo-" + std::to_string(exposureProbeStage) + ".ppm"), std::ios::binary);
            out << "P6\n" << width << ' ' << height << "\n255\n";
            for (size_t i = 0; i < pixels.size(); i += 4u) out.write(reinterpret_cast<const char*>(pixels.data()+i), 3);
        }
        ++exposureProbeStage;
        exposureProbeStarted = now;
        if (exposureProbeStage == 14u) context.renderLoop.GetWindow()->RequestClose();
        return true;
    }
    // --material-ibl <project.json> <capture-dir>: isolate lighting on the real
    // material-editor sphere, without changing the material or production view.
    struct MaterialIblConstants final : renderer::IFrameConstantsContributor
    {
        uint32_t flags = 0u;
        bool disableProbes = false;
        uint32_t probeDebug = 0u;
        std::string_view GetName() const noexcept override { return "test.material_ibl"; }
        void Contribute(const renderer::FrameConstantsContributionContext& context,
                        renderer::FrameConstants& constants) const override
        {
            constants.debugFlags = flags;
            auto& probeSlot = constants.featureParams[engine::addons::probes::kProbeFrameParameterBlock.firstSlot];
            uint32_t probeFlags = 0u;
            std::memcpy(&probeFlags, &probeSlot[1], sizeof(probeFlags));
            probeFlags = (probeFlags & 0xFFu) | (probeDebug << engine::addons::probes::kProbeFlagDebugShift);
            std::memcpy(&probeSlot[1], &probeFlags, sizeof(probeFlags));
            if (disableProbes)
                constants.featureParams[engine::addons::probes::kProbeFrameParameterBlock.firstSlot][0] = 0.f;
        }
    };
    std::filesystem::path materialIblCaptures;
    std::shared_ptr<MaterialIblConstants> materialIblConstants;
    int materialIblReadyFrames = 0;
    size_t materialIblStage = 0u;
    bool environmentSunChecked = false;
    float materialIblSunYaw = 0.f;
    float materialIblSunPitch = 0.f;
    std::chrono::steady_clock::time_point materialIblStart{};

    bool materialSpecularProbe = false;
    bool materialSpecularReady = false;
    assets::MaterialAsset materialSpecularOriginal;
    std::map<std::string, std::vector<uint8_t>> materialSpecularPixels;

    bool UpdateMaterialSpecular(AppSceneContext& context)
    {
        struct Stage { const char* name; uint32_t flags; float specular; float metallic = 0.f;
                       float roughness = .5f; bool graph = false; };
        static constexpr Stage stages[] = {
            {"default-full", 0u, -1.f}, {"half-full", 0u, .5f},
            {"half-ibl", renderer::DBG_VIEW_IBL_SPEC, .5f},
            {"zero-ibl", renderer::DBG_VIEW_IBL_SPEC, 0.f},
            {"one-ibl", renderer::DBG_VIEW_IBL_SPEC, 1.f},
            {"graph-zero-ibl", renderer::DBG_VIEW_IBL_SPEC, 1.f, 0.f, .5f, true},
            {"half-direct", renderer::DBG_VIEW_DIRECT_SPEC, .5f},
            {"zero-direct", renderer::DBG_VIEW_DIRECT_SPEC, 0.f},
            {"zero-diffuse", renderer::DBG_VIEW_IBL_DIFF, 0.f},
            {"metal-zero", renderer::DBG_VIEW_IBL_SPEC, 0.f, 1.f},
            {"metal-one", renderer::DBG_VIEW_IBL_SPEC, 1.f, 1.f},
            {"rough-low", renderer::DBG_VIEW_IBL_SPEC, .5f, 0.f, .05f},
            {"rough-high", renderer::DBG_VIEW_IBL_SPEC, .5f, 0.f, 1.f},
            {"zero-full", 0u, 0.f}, {"one-full", 0u, 1.f},
            {"graph-zero-full", 0u, 1.f, 0.f, .5f, true},
            {"f0-zero", renderer::DBG_VIEW_FRESNEL_F0, 0.f},
            {"f0-half", renderer::DBG_VIEW_FRESNEL_F0, .5f},
            {"f0-one", renderer::DBG_VIEW_FRESNEL_F0, 1.f},
        };
        auto& runtime = context.renderLoop.GetRenderSystem().GetShaderRuntime();
        if (!materialIblConstants)
        {
            materialIblStart = std::chrono::steady_clock::now();
            auto& registry = context.renderLoop.GetRenderSystem().GetFeatureRegistry();
            materialIblConstants = std::make_shared<MaterialIblConstants>();
            Check(registry.RegisterFrameConstantsContributor(*registry.GetFeatures().front(), materialIblConstants),
                  "register specular material diagnostic");
            renderer::addons::editor::OpenMaterialAsset(*app.m_editorFrameCtx,
                app.GetCurrentAssetRoot() / "Materials/Weiss.mat");
        }
        if (std::chrono::steady_clock::now() - materialIblStart > std::chrono::minutes(5))
        {
            Check(false, "specular diagnostic completed within five minutes");
            app.m_editorFrameCtx->state.selectedMaterialAssetDirty = false;
            context.renderLoop.GetWindow()->RequestClose();
            return true;
        }
        if (frames < 20 || !app.m_editorFrameCtx->state.selectedMaterialAssetLoaded || runtime.HasPendingShaderCompiles())
        {
            materialIblReadyFrames = 0;
            return true;
        }
        auto applyStage = [&]() {
            const auto& stage = stages[materialIblStage];
            auto& state = app.m_editorFrameCtx->state;
            auto& asset = state.selectedMaterialAssetData;
            asset = materialSpecularOriginal;
            asset.metallicFactor = stage.metallic;
            asset.roughnessFactor = stage.roughness;
            if (stage.specular >= 0.f) asset.specularFactor = stage.specular;
            asset.clearcoatFactor = 0.f;
            asset.graph.metallic = asset.graph.roughness = asset.graph.specular = 0u;
            if (stage.graph)
            {
                using Node = assets::MaterialNodeType;
                asset.graph.nodes.push_back({100, Node::Value, 0, 0, {0.f, 0.f, 0.f}});
                asset.graph.nodes.push_back({101, Node::Value, 0, 0, {1.f, 1.f, 1.f}});
                asset.graph.nodes.push_back({102, Node::Multiply, 100, 101});
                asset.graph.specular = 102;
            }
            state.selectedMaterialAssetDirty = true;
            state.tonemap.exposureMode = renderer::addons::forward::ExposureMode::Fixed;
            state.tonemap.exposureEv = 0.f;
            materialIblConstants->flags = stage.flags;
            materialIblReadyFrames = 0;
        };
        if (!materialSpecularReady)
        {
            materialSpecularReady = true;
            materialSpecularOriginal = app.m_editorFrameCtx->state.selectedMaterialAssetData;
            Check(materialSpecularOriginal.PbrSpecular() == .5f, "old material defaults to Specular 0.5");
            applyStage();
            return true;
        }
        if (++materialIblReadyFrames < 8) return true;
        auto* device = context.renderLoop.GetRenderSystem().GetDevice();
        uint32_t width = 0u, height = 0u;
        std::vector<uint8_t> pixels;
        const auto texture = device->GetRenderTargetColorTexture(app.m_materialPreviewRT);
        const bool read = device->ReadTexturePixels(texture, pixels, width, height)
            && width > 0u && height > 0u && pixels.size() == static_cast<size_t>(width) * height * 4u;
        Check(read, "specular GPU readback");
        if (read)
        {
            std::ofstream output(materialIblCaptures / (std::string(stages[materialIblStage].name) + ".ppm"), std::ios::binary);
            output << "P6\n" << width << ' ' << height << "\n255\n";
            auto& sphere = materialSpecularPixels[stages[materialIblStage].name];
            const float radius = width * .34f;
            for (uint32_t y = 0u; y < height; ++y)
                for (uint32_t x = 0u; x < width; ++x)
                {
                    const size_t index = (static_cast<size_t>(y) * width + x) * 4u;
                    output.write(reinterpret_cast<const char*>(pixels.data() + index), 3);
                    const float dx = static_cast<float>(x) - width * .5f;
                    const float dy = static_cast<float>(y) - height * .5f;
                    if (dx * dx + dy * dy < radius * radius)
                        sphere.insert(sphere.end(), pixels.begin() + index, pixels.begin() + index + 3u);
                }
            Check(static_cast<bool>(output), stages[materialIblStage].name);
        }
        if (++materialIblStage == std::size(stages))
        {
            const auto energy = [&](const char* name) {
                const auto& pixels = materialSpecularPixels.at(name);
                return std::accumulate(pixels.begin(), pixels.end(), uint64_t{0});
            };
            const bool complete = materialSpecularPixels.size() == std::size(stages);
            Check(complete, "all specular diagnostic images captured");
            if (complete)
            {
                Check(energy("half-ibl") > 0u, "Specular 0.5 has environment reflection");
                Check(energy("zero-ibl") == 0u, "Specular zero removes the complete environment lobe");
                Check(energy("graph-zero-ibl") == 0u, "per-pixel graph zero removes environment reflection");
                Check(energy("half-direct") > 0u, "direct-light specular reference is lit");
                Check(energy("zero-direct") == 0u, "Specular zero removes direct specular");
                Check(energy("zero-diffuse") > 0u, "diffuse HDR lighting remains enabled at Specular zero");
                Check(energy("one-ibl") > energy("half-ibl"), "Specular one increases dielectric reflection");
                Check(energy("f0-zero") == 0u && energy("f0-one") > energy("f0-half"), "dielectric F0 responds to Specular");
                Check(materialSpecularPixels.at("default-full") == materialSpecularPixels.at("half-full"),
                      "old material and explicit Specular 0.5 render identically");
                Check(materialSpecularPixels.at("zero-full") == materialSpecularPixels.at("graph-zero-full"),
                      "constant and per-pixel zero render identically");
                Check(materialSpecularPixels.at("metal-zero") == materialSpecularPixels.at("metal-one"),
                      "Specular does not change metal reflectance");
                Check(materialSpecularPixels.at("rough-low") != materialSpecularPixels.at("rough-high"),
                      "Roughness still changes reflection blur");
            }
            app.m_editorFrameCtx->state.selectedMaterialAssetDirty = false;
            context.renderLoop.GetWindow()->RequestClose();
        }
        else applyStage();
        return true;
    }

    bool UpdateMaterialIbl(AppSceneContext& context)
    {
        if (materialSpecularProbe) return UpdateMaterialSpecular(context);
        auto& runtime = context.renderLoop.GetRenderSystem().GetShaderRuntime();
        if (!materialIblConstants)
        {
            materialIblStart = std::chrono::steady_clock::now();
            auto& registry = context.renderLoop.GetRenderSystem().GetFeatureRegistry();
            materialIblConstants = std::make_shared<MaterialIblConstants>();
            Check(registry.RegisterFrameConstantsContributor(*registry.GetFeatures().front(), materialIblConstants),
                  "register material preview lighting diagnostic");
            renderer::addons::editor::OpenMaterialAsset(*app.m_editorFrameCtx,
                app.GetCurrentAssetRoot() / "Materials/Weiss.mat");
        }
        if (std::chrono::steady_clock::now() - materialIblStart > std::chrono::minutes(3))
        {
            Check(false, "material preview shaders ready within three minutes");
            context.renderLoop.GetWindow()->RequestClose();
            return true;
        }
        if (frames < 20 || !app.m_editorFrameCtx->state.selectedMaterialAssetLoaded ||
            runtime.HasPendingShaderCompiles())
        {
            materialIblReadyFrames = 0;
            return true;
        }
        if (materialIblStage == 0u && materialIblReadyFrames == 0)
        {
            // Only opt in on a disposable project: this checks the real scene writer,
            // editor reader, runtime export and old-scene default without changing fixtures.
            if (!environmentSunChecked && std::getenv("KROM_IBL_CHECK_SCENE_ROUNDTRIP"))
            {
                environmentSunChecked = true;
                auto& state = app.m_editorFrameCtx->state;
                state.environmentRemoveSunFromLighting = false;
                Check(app.SaveEditorScene(), "save scene with sun removal disabled");
                std::ifstream file(app.GetCurrentEditorScenePath(), std::ios::binary);
                const std::string text((std::istreambuf_iterator<char>(file)), {});
                file.close();
                std::string error;
                auto root = serialization::JsonParser::Parse(text, error);
                const auto* environment = root.Get("environment");
                Check(error.empty() && environment && environment->Get("removeSunFromLighting")
                      && !environment->Get("removeSunFromLighting")->AsBool(), "scene stores sun-removal switch");
                const auto exported = app.MakeKsceneJson(text, app.m_currentSceneName);
                Check(exported.has_value(), "export scene with sun-removal switch");
                if (exported)
                {
                    const auto runtimeScene = serialization::JsonParser::Parse(*exported, error);
                    const auto* env = runtimeScene.Get("environment");
                    Check(env && env->Get("removeSunFromLighting") && !env->Get("removeSunFromLighting")->AsBool(),
                          "runtime export preserves disabled sun removal");
                }
                Check(app.LoadEditorScene(true, "", &root), "reload scene with sun-removal switch");
                Check(!state.environmentRemoveSunFromLighting, "editor restores disabled sun removal");
                for (auto& [key, value] : root.objectVal)
                    if (key == "environment")
                        std::erase_if(value.objectVal, [](const auto& field) { return field.first == "removeSunFromLighting"; });
                Check(app.LoadEditorScene(true, "", &root), "load old scene without sun-removal switch");
                Check(state.environmentRemoveSunFromLighting, "old scenes default to sun removal enabled");
                Check(app.SaveEditorScene(), "restore disposable scene to sun removal enabled");
                materialIblReadyFrames = 0;
                return true;
            }
            auto& asset = app.m_editorFrameCtx->state.selectedMaterialAssetData;
            asset.metallicFactor = 0.f;
            asset.roughnessFactor = std::getenv("KROM_IBL_ROUGHNESS_HALF") ? .5f : 1.f;
            asset.graph.metallic = asset.graph.roughness = 0u;
            app.m_editorFrameCtx->state.selectedMaterialAssetDirty = true;
            app.m_editorFrameCtx->state.tonemap.exposureMode = renderer::addons::forward::ExposureMode::Fixed;
            app.m_editorFrameCtx->state.tonemap.exposureEv = 0.f;
            Check(app.m_editorFrameCtx->state.environmentRemoveSunFromLighting,
                  "capture uses sun removal enabled");
            const auto texture = context.assetPipeline.LoadTexture(app.m_editorFrameCtx->state.environmentTexturePath);
            const auto* sky = app.m_assetRegistry.textures.Get(texture);
            if (sky && sky->format == assets::TextureFormat::RGBA16F && !sky->pixelData.empty())
            {
                const auto half = [](uint16_t h) {
                    const uint32_t exponent = (h >> 10u) & 31u, mantissa = h & 1023u;
                    const float value = exponent == 0u ? std::ldexp(static_cast<float>(mantissa), -24)
                        : std::ldexp(static_cast<float>(mantissa + 1024u), static_cast<int>(exponent) - 25);
                    return (h & 0x8000u) ? -value : value;
                };
                size_t brightest = 0u;
                float maximum = 0.f;
                for (size_t i = 0u; i < static_cast<size_t>(sky->width) * sky->height; ++i)
                {
                    uint16_t rgba[4];
                    std::memcpy(rgba, sky->pixelData.data() + i * 8u, 8u);
                    const float luminance = half(rgba[0]) * .2126f + half(rgba[1]) * .7152f + half(rgba[2]) * .0722f;
                    if (luminance > maximum) { maximum = luminance; brightest = i; }
                }
                const float phi = ((static_cast<float>(brightest % sky->width) + .5f) / sky->width - .5f) * math::PI * 2.f;
                materialIblSunYaw = std::atan2(-std::cos(phi), -std::sin(phi)) * math::RAD_TO_DEG;
                materialIblSunPitch = 90.f - (static_cast<float>(brightest / sky->width) + .5f) / sky->height * 180.f;
            }
        }
        if (++materialIblReadyFrames < 8)
            return true;

        struct Stage { const char* name; uint32_t flags; bool disableProbes; uint32_t probeDebug = 0u; };
        const Stage stages[] = {
            {"full", 0u, false},
            {"without-specular", renderer::DBG_DISABLE_IBL_SPEC, false},
            {"diffuse", renderer::DBG_VIEW_IBL_DIFF, false},
            {"specular", renderer::DBG_VIEW_IBL_SPEC, false},
            {"without-ibl", renderer::DBG_DISABLE_IBL, false},
            {"normals", renderer::DBG_VIEW_NORMALS, false},
            {"roughness", renderer::DBG_VIEW_ROUGHNESS, false},
            {"without-probes", 0u, true},
            {"global-specular", renderer::DBG_VIEW_IBL_SPEC, true},
            {"probe-index", 0u, false, 1u},
            {"probe-weight", 0u, false, 2u},
            {"sky-sun", 0u, false},
        };
        auto* device = context.renderLoop.GetRenderSystem().GetDevice();
        const auto texture = device->GetRenderTargetColorTexture(app.m_materialPreviewRT);
        uint32_t width = 0u, height = 0u;
        std::vector<uint8_t> pixels;
        const bool read = device->ReadTexturePixels(texture, pixels, width, height);
        const bool valid = read && width > 0u && height > 0u &&
            pixels.size() == static_cast<size_t>(width) * height * 4u;
        Check(valid,
              "material preview GPU readback");
        if (valid)
        {
            const auto file = materialIblCaptures / (std::string(stages[materialIblStage].name) + ".ppm");
            std::ofstream output(file, std::ios::binary);
            output << "P6\n" << width << ' ' << height << "\n255\n";
            for (size_t i = 0u; i < pixels.size(); i += 4u)
                output.write(reinterpret_cast<const char*>(pixels.data() + i), 3);
            Check(static_cast<bool>(output), stages[materialIblStage].name);
        }
        if (device->ReadTexturePixels(device->GetRenderTargetColorTexture(app.m_sceneView.rt), pixels, width, height))
        {
            std::ofstream output(materialIblCaptures / (std::string("scene-") + stages[materialIblStage].name + ".ppm"),
                                 std::ios::binary);
            output << "P6\n" << width << ' ' << height << "\n255\n";
            for (size_t i = 0u; i < pixels.size(); i += 4u)
                output.write(reinterpret_cast<const char*>(pixels.data() + i), 3);
            Check(static_cast<bool>(output), "scene lighting diagnostic capture");
        }
        if (++materialIblStage == std::size(stages))
        {
            app.m_editorFrameCtx->state.selectedMaterialAssetDirty = false;
            context.renderLoop.GetWindow()->RequestClose();
        }
        else
        {
            materialIblConstants->flags = stages[materialIblStage].flags;
            materialIblConstants->disableProbes = stages[materialIblStage].disableProbes;
            materialIblConstants->probeDebug = stages[materialIblStage].probeDebug;
            if (std::string_view(stages[materialIblStage].name) == "sky-sun")
            {
                auto& state = app.m_editorFrameCtx->state;
                state.editorCamera.position = {0.f, 100.f, 0.f};
                state.editorCamera.yawDeg = materialIblSunYaw;
                state.editorCamera.pitchDeg = materialIblSunPitch;
                state.showSkyboxBackground = true;
                state.showEditorIcons = false;
            }
            materialIblReadyFrames = 0;
        }
        return true;
    }
    std::filesystem::path materialPreviewCaptures;
    int materialPreviewStep = 0;
    int materialPreviewWait = 0;
    std::chrono::steady_clock::time_point materialPreviewStart{};
    bool UpdateMaterialPreview(AppSceneContext& context)
    {
        namespace ed = renderer::addons::editor;
        auto& ui = *app.m_editorFrameCtx;
        auto& io = ImGui::GetIO();
        if (frames == 1)
        {
            materialPreviewStart = std::chrono::steady_clock::now();
            const auto root = app.GetCurrentAssetRoot();
            std::filesystem::create_directories(root / "Materials");
            std::filesystem::create_directories(root / "Textures");
            std::filesystem::create_directories(root / "Prefabs");
            std::filesystem::copy_file(PathFromUtf8(KROM_SOURCE_DIR) /
                "projects/DemoProject/Assets/Textures/Testraum/door_panel.png", root / "Textures/Preview.png");
            std::ofstream(root / "Materials/Preview.mat") <<
                "template pbr-lit\nbaseColorFactor 1 1 1 1\nmetallicFactor 0\nroughnessFactor 1\n"
                "baseColorTexture Textures/Preview.png\n";
            std::ofstream(root / "Prefabs/Preview.prefab") <<
                R"({"type":"KromPrefab","version":2,"rootIndex":0,"records":[{"name":"Preview","parentIndex":-1}]})";
            ed::OpenMaterialAsset(ui, root / "Materials/Preview.mat");
            ui.assetsPanelVisible = true;
            ui.consolePanelVisible = false;
            ui.state.assetBrowser->highlightPath = root / "Prefabs/Preview.prefab";
        }
        if (std::chrono::steady_clock::now() - materialPreviewStart > std::chrono::minutes(2))
        {
            Check(false, "material preview regression completed within two minutes");
            context.renderLoop.GetWindow()->RequestClose();
            return true;
        }
        if (frames < 20 || !ui.state.selectedMaterialAssetLoaded ||
            context.renderLoop.GetRenderSystem().GetShaderRuntime().HasPendingShaderCompiles())
        {
            materialPreviewWait = 0;
            return true;
        }
        const auto capture = [&](const char* stage) {
            Check(ui.state.selectedMaterialAssetPath == "Materials/Preview.mat",
                  "browser selection preserves the open material path");
            const auto* sphere = ui.world.Get<MeshComponent>(app.m_materialPreviewEntity);
            const auto* ground = ui.world.Get<MeshComponent>(app.m_materialPreviewGround);
            Check(sphere && sphere->visible && ground && ground->visible, "preview sphere and ground remain visible");
            auto* device = context.renderLoop.GetRenderSystem().GetDevice();
            uint32_t width = 0, height = 0;
            std::vector<uint8_t> pixels;
            const bool read = device->ReadTexturePixels(device->GetRenderTargetColorTexture(app.m_materialPreviewRT),
                                                        pixels, width, height);
            const bool valid = read && width > 0 && height > 0 && pixels.size() == size_t(width) * height * 4;
            Check(valid, "read back the rendered material preview");
            if (!valid) return;
            // This fixture has no skybox: visible pixels must come from the preview geometry.
            size_t visiblePixels = 0;
            for (uint32_t y = height / 4; y < height * 3 / 4; ++y)
                for (uint32_t x = width / 4; x < width * 3 / 4; ++x)
                {
                    const size_t p = (size_t(y) * width + x) * 4;
                    if (std::max({pixels[p], pixels[p + 1], pixels[p + 2]}) > 16) ++visiblePixels;
                }
            Check(visiblePixels > size_t(width) * height / 10, "textured sphere produces visible GPU pixels");
            std::ofstream image(materialPreviewCaptures / (std::string(stage) + ".ppm"), std::ios::binary);
            image << "P6\n" << width << ' ' << height << "\n255\n";
            for (size_t p = 0; p < pixels.size(); p += 4)
                image.write(reinterpret_cast<const char*>(pixels.data() + p), 3);
        };
        if (++materialPreviewWait < 12) return true;
        switch (materialPreviewStep)
        {
        case 0:
        {
            capture("before-prefab-click");
            const auto title = ed::TranslateLabel("panel.assets", "Assets##editor");
            const auto* assets = ImGui::FindWindowByName(title.c_str());
            const ImGuiWindow* content = nullptr;
            if (assets)
                for (const auto* child : assets->DC.ChildWindows)
                    if (std::string_view(child->Name).find("##content_panel") != std::string_view::npos)
                        content = child;
            Check(content && content->Active, "prefab browser tile is visible");
            if (!content) { context.renderLoop.GetWindow()->RequestClose(); return true; }
            // The fixture directory contains exactly one prefab tile.
            io.AddMousePosEvent(content->DC.CursorStartPos.x + 40.f, content->DC.CursorStartPos.y + 30.f);
            break;
        }
        case 1: io.AddMouseButtonEvent(ImGuiMouseButton_Left, true); break;
        case 2: io.AddMouseButtonEvent(ImGuiMouseButton_Left, false); break;
        case 3:
            Check(ui.state.selectedPrefabAssetPath == "Prefabs/Preview.prefab", "click selected the prefab tile");
            capture("after-prefab-click");
            Check(app.BeginEditorPlay(), "start Play with the material editor open");
            break;
        case 4: app.StopEditorPlay(); break;
        case 5:
            capture("after-play-stop");
            context.renderLoop.GetWindow()->RequestClose();
            break;
        }
        ++materialPreviewStep;
        materialPreviewWait = 0;
        return true;
    }
    EntityID scriptedMaterialEntity{}, sharedMaterialEntity{};
    MaterialHandle sharedMaterial{}, scriptedMaterial{};
    std::chrono::steady_clock::time_point materialShaderWaitStart{};
    static constexpr const char* kUVParameters[] = {"Tiling", "Offset", "Scroll", "DirectScale", "DirectOffset"};
    static constexpr math::Vec2 kUVValues[] = {{4.f, 2.f}, {0.25f, 0.5f}, {0.125f, 0.f}, {3.f, 2.f}, {0.5f, 0.25f}};

    struct MaterialUVProbe final : script::ComponentScript {
        bool applied = false;
        void OnStart(script::IScriptContext& ctx) override
        {
            applied = true;
            for (size_t i = 0; i < std::size(kUVParameters); ++i)
                applied = ctx.SetMaterialParameter(kUVParameters[i], kUVValues[i], ctx.GetEntity()) && applied;
            applied = ctx.SetMaterialUV(ctx.GetEntity(), {6.f, 7.f}, {0.125f, 0.25f}) && applied;
        }
    };

    bool UpdateMaterialUi(AppSceneContext& context)
    {
        namespace ed = renderer::addons::editor;
        constexpr const char* materialPath = "Materials/UVProbe.mat";
        constexpr const char* texturePath = "Textures/UVProbe.png";
        constexpr const char* meshPath = "UVProbe.kmesh";
        auto& ui = *app.m_editorFrameCtx;
        auto& materials = context.materialSystem;
        const auto handle = [&](EntityID entity) {
            const auto* material = app.m_world->Get<MaterialComponent>(entity);
            return material ? material->material : MaterialHandle::Invalid();
        };
        if (frames == 1)
        {
            const auto assetRoot = app.m_projectRoot / "Assets";
            std::filesystem::create_directories(assetRoot / "Materials");
            std::filesystem::create_directories(assetRoot / "Textures");
            std::filesystem::copy_file(PathFromUtf8(KROM_SOURCE_DIR) / "projects/DemoProject/Assets/Textures/Testraum/door_panel.png",
                                       assetRoot / texturePath);
            assets::SubMeshData quad;
            quad.positions = {-0.6f, -0.8f, 0.f, 0.6f, -0.8f, 0.f, 0.6f, 0.8f, 0.f, -0.6f, 0.8f, 0.f};
            quad.uvs = {0.f, 0.f, 1.f, 0.f, 1.f, 1.f, 0.f, 1.f};
            quad.normals = {0.f, 0.f, 1.f, 0.f, 0.f, 1.f, 0.f, 0.f, 1.f, 0.f, 0.f, 1.f};
            quad.tangents = {1.f, 0.f, 0.f, 1.f, 1.f, 0.f, 0.f, 1.f, 1.f, 0.f, 0.f, 1.f, 1.f, 0.f, 0.f, 1.f};
            quad.indices = {0, 1, 2, 0, 2, 3};
            Check(assets::KMeshSave(assetRoot / meshPath, {quad}), "save UV probe mesh");
            const auto mesh = context.assetPipeline.LoadMesh(meshPath);
            Check(mesh.IsValid(), "load UV probe mesh");

            assets::MaterialGraph graph;
            using Node = assets::MaterialNodeType;
            graph.nodes = {{1, Node::Texture, 0, 0, {}, {20.f, 20.f}, texturePath}};
            for (size_t i = 0; i < std::size(kUVParameters); ++i)
                graph.nodes.push_back({static_cast<uint32_t>(i + 2), Node::Parameter, 0, 0,
                    {1.f, 1.f, 0.f}, {230.f, 20.f + static_cast<float>(i) * 85.f}, kUVParameters[i]});
            graph.nodes[0].uvScaleLink = 2;
            graph.nodes[0].uvOffsetLink = 3;
            graph.nodes[0].uvScrollLink = 4;
            graph.emissive = 1;
            graph.uvScaleLink = 5;
            graph.uvOffsetLink = 6;
            {
                std::ofstream file(assetRoot / materialPath);
                file << "template pbr-lit\nalphaMode mask\ndoubleSided true\n"
                        "emissiveFactor 1 1 1\nemissiveStrength 1\nbaseColorTexture " << texturePath << '\n';
                graph.Write(file);
            }

            const auto camera = ed::CreateCameraEntity(ui);
            auto* cameraTransform = app.m_world->Get<TransformComponent>(camera);
            cameraTransform->localPosition = {0.f, 0.f, 4.f};
            cameraTransform->localRotation = {};
            cameraTransform->dirty = true;
            ed::CreateLightEntity(ui, LightType::Directional);
            const auto createProbe = [&](const char* name, float x) {
                const auto entity = ed::CreateEmptyEntity(ui, name);
                auto* transform = app.m_world->Get<TransformComponent>(entity);
                transform->localPosition = {x, 0.f, 0.f};
                transform->localRotation = {};
                transform->dirty = true;
                auto& component = app.m_world->Add<MeshComponent>(entity, mesh);
                component.meshAssetPath = meshPath;
                Check(ed::ApplyMaterialAssetToEntity(ui, entity, materialPath), "bind shared UV probe material");
                return entity;
            };
            scriptedMaterialEntity = createProbe("Scripted UV", -0.9f);
            sharedMaterialEntity = createProbe("Shared UV", 0.9f);
            app.m_scriptRegistry.Register<MaterialUVProbe>("MaterialUVProbe");
            app.m_world->Add<script::ScriptList>(scriptedMaterialEntity).Add(
                "MaterialUVProbe", scriptedMaterialEntity, app.m_scriptRegistry);
            ed::OpenMaterialAsset(ui, assetRoot / materialPath);
        }
        if (frames == 6)
        {
            const auto title = ed::TranslateLabel("material.title", "Materialeditor##editor");
            const auto* window = ImGui::FindWindowByName(title.c_str());
            Check(window && window->Active && ui.state.selectedMaterialAssetLoaded, "linked material opens in the editor");
            sharedMaterial = handle(sharedMaterialEntity);
            Check(sharedMaterial.IsValid() && handle(scriptedMaterialEntity) == sharedMaterial,
                  "both objects begin with the same material");
            ui.state.materialWindowOpen = false;
            Check(app.BeginEditorPlay(), "start Play with the UV material");
        }
        if (frames == 15)
        {
            // The material instance's shader variant compiles in the background
            // (DX11/FXC takes about a second): hold this frame until it is ready.
            const auto* pendingState = context.renderLoop.GetRenderSystem().GetShaderRuntime()
                .GetMaterialState(handle(scriptedMaterialEntity));
            const auto waitNow = std::chrono::steady_clock::now();
            if (materialShaderWaitStart == std::chrono::steady_clock::time_point{})
                materialShaderWaitStart = waitNow;
            if (pendingState && pendingState->waitingForShaders &&
                waitNow - materialShaderWaitStart < std::chrono::minutes(2))
            {
                --frames;
                return true;
            }
            std::printf("material instance shaders ready after %lld ms\n",
                static_cast<long long>(std::chrono::duration_cast<std::chrono::milliseconds>(
                    waitNow - materialShaderWaitStart).count()));
            scriptedMaterial = handle(scriptedMaterialEntity);
            const auto* list = app.m_world->Get<script::ScriptList>(scriptedMaterialEntity);
            const auto* probe = list && !list->Instances().empty()
                ? static_cast<const MaterialUVProbe*>(list->Instances().front().script.get()) : nullptr;
            Check(probe && probe->applied, "Play script set all five UV graph parameters and the material UV");
            Check(scriptedMaterial.IsValid() && scriptedMaterial != sharedMaterial &&
                  handle(sharedMaterialEntity) == sharedMaterial, "only the scripted object gets a material instance");
            const auto read = [&](MaterialHandle material, const std::string& name, size_t components) {
                math::Vec4 value{};
                const auto& bytes = materials.GetCBData(material);
                const uint32_t offset = materials.GetCBLayout(material).GetOffset(name);
                if (offset != UINT32_MAX && offset + components * sizeof(float) <= bytes.size())
                    std::memcpy(&value.x, bytes.data() + offset, components * sizeof(float));
                return value;
            };
            for (size_t i = 0; i < std::size(kUVParameters); ++i)
            {
                const auto* target = materials.FindParameterAlias(scriptedMaterial, kUVParameters[i]);
                const auto own = target ? read(scriptedMaterial, *target, 4) : math::Vec4{};
                const auto shared = target ? read(sharedMaterial, *target, 4) : math::Vec4{};
                Check(target && own.x == kUVValues[i].x && own.y == kUVValues[i].y && own.z == 0.f &&
                      shared.x == 1.f && shared.y == 1.f, kUVParameters[i]);
            }
            const auto directLinks = read(scriptedMaterial, "graphUV1", 4);
            Check(directLinks.z == 5.f && directLinks.w == 6.f, "direct UV links reach the shader buffer");
            const auto ownUV = read(scriptedMaterial, "uvScale", 2);
            const auto sharedUV = read(sharedMaterial, "uvScale", 2);
            Check(ownUV.x == 6.f && ownUV.y == 7.f && sharedUV.x == 1.f && sharedUV.y == 1.f,
                  "direct UV overrides use the existing fields on one instance");
            const auto& renderer = context.renderLoop.GetRenderSystem();
            const auto* gpu = renderer.GetShaderRuntime().GetMaterialState(scriptedMaterial);
            if (gpu && !gpu->valid)
                for (const auto& issue : gpu->issues)
                    std::fprintf(stderr, "UV probe material: %s\n", issue.message.c_str());
            Check(gpu && gpu->valid && gpu->perMaterialCB.IsValid() &&
                  gpu->materialRevision == materials.GetRevision(scriptedMaterial),
                  "renderer uploaded the scripted material instance");
            Check(renderer.GetStats().opaqueDraws >= 2, "the Play frame draws the UV probes");
            app.StopEditorPlay();
        }
        if (frames == 18)
        {
            Check(!materials.GetInstance(scriptedMaterial), "stopping Play frees the UV material instance");
            context.renderLoop.GetWindow()->RequestClose();
        }
        return true;
    }

    bool animationUi = false;
    EntityID animationProbe{};
    std::string AnimationClipText() const
    {
        std::ifstream in(app.m_projectRoot / "Assets/Animations/AnimProbe.anim", std::ios::binary);
        return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    }
    float AnimationProbeX() const
    {
        const auto* transform = app.m_world->Get<TransformComponent>(animationProbe);
        return transform ? transform->localPosition.x : -999.f;
    }
    // Node-animated glTF: node "ClipBox" moves x 0 -> 2 in animation "Move" (0..1 s).
    void WriteModelClipProbe() const
    {
        const auto models = app.m_projectRoot / "Assets/Models";
        std::filesystem::create_directories(models);
        const float data[8] = {0.f, 1.f, 0.f, 0.f, 0.f, 2.f, 0.f, 0.f};
        {
            std::ofstream bin(models / "clip_probe.bin", std::ios::binary);
            bin.write(reinterpret_cast<const char*>(data), sizeof(data));
        }
        std::ofstream(models / "clip_probe.gltf") << R"({
  "asset": { "version": "2.0" },
  "buffers": [ { "uri": "clip_probe.bin", "byteLength": 32 } ],
  "bufferViews": [ { "buffer": 0, "byteOffset": 0, "byteLength": 8 }, { "buffer": 0, "byteOffset": 8, "byteLength": 24 } ],
  "accessors": [
    { "bufferView": 0, "componentType": 5126, "count": 2, "type": "SCALAR", "min": [0], "max": [1] },
    { "bufferView": 1, "componentType": 5126, "count": 2, "type": "VEC3" } ],
  "nodes": [ { "name": "ClipBox" } ],
  "scenes": [ { "nodes": [0] } ], "scene": 0,
  "animations": [ { "name": "Move",
    "samplers": [ { "input": 0, "output": 1, "interpolation": "LINEAR" } ],
    "channels": [ { "sampler": 0, "target": { "node": 0, "path": "translation" } } ] } ]
})";
    }
    std::string modelClipCutId;
    std::string graphPath;
    std::string graphStateA, graphStateB, graphTransition;
    std::string graphTrigger, graphFade;
    float previewCopyX = 0.f;
    int32_t PreviewState(renderer::addons::editor::EditorFrameContext& ui) const
    {
        const auto* player = renderer::addons::editor::ControllerLivePlayer(ui, graphPath);
        return player ? player->controllerState.instance.current : -99;
    }
    float arrowX = 0.f, arrowY = 0.f;
    float AnimationCopyX() const
    {
        const auto* transform = app.m_world->Get<TransformComponent>(renderer::addons::editor::GetAnimationPreviewRoot());
        return transform ? transform->localPosition.x : -999.f;
    }
    bool UpdateAnimationUi(AppSceneContext& context)
    {
        namespace ed = renderer::addons::editor;
        auto& ui = *app.m_editorFrameCtx;
        if (frames == 1)
        {
            // A child of a moved parent: the copy must keep the parent's space.
            const EntityID probeParent = ed::CreateEmptyEntity(ui, "AnimProbeParent");
            app.m_world->Get<TransformComponent>(probeParent)->localPosition = {100.f, 40.f, -30.f};
            app.m_world->Get<TransformComponent>(probeParent)->dirty = true;
            animationProbe = ed::CreateEmptyEntity(ui, "AnimProbe");
            ed::ReparentEntity(ui, animationProbe, probeParent);
            app.m_world->Get<TransformComponent>(animationProbe)->localPosition = {};
            app.m_world->Get<TransformComponent>(animationProbe)->dirty = true;
            app.m_world->Add<AnimationPlayerComponent>(animationProbe);
            ui.state.selectedEntity = animationProbe;
            ui.state.animationWindowOpen = true;
            // The steps below save by hand; auto-save is checked at the end.
            ed::SetAutoSaveEditorsForSession(false);
            Check(ed::CreateAnimationClip(ui, animationProbe), "new animation started");
        }
        if (frames == 3)
        {
            Check(app.m_world->IsAlive(ed::GetAnimationPreviewRoot()), "hidden copy exists");
            Check(AnimationClipText().empty(), "nothing is written before saving");
        }
        if (frames == 4)
            ed::SetAnimationPreviewTime(1.f);
        if (frames == 6)
        {
            const auto* copyWorld = app.m_world->Get<WorldTransformComponent>(ed::GetAnimationPreviewRoot());
            Check(copyWorld && std::fabs(copyWorld->position.x - 100.f) < 1e-3f && std::fabs(copyWorld->position.y - 40.f) < 1e-3f,
                  "the copy keeps the world position given by the parent");
            Check(std::fabs(ui.state.animationPreviewTarget.x - 100.f) < 1e-3f, "the preview camera looks at the object");
            app.m_world->Get<TransformComponent>(ed::GetAnimationPreviewRoot())->localPosition.x = 5.f;
        }
        if (frames == 9)
        {
            Check(std::fabs(AnimationProbeX()) < 1e-3f, "the scene object stays where it is");
            Check(ed::SaveOpenAnimation(ui), "animation saved");
            const std::string text = AnimationClipText();
            Check(text.rfind("kromanim 2", 0) == 0 && text.find("clip Clip") != std::string::npos, "file holds a named clip");
            Check(text.find("channel translation linear") != std::string::npos && text.find("\nk 1 5 ") != std::string::npos,
                  "recording writes a key at the current time");
            Check(text.find("\nk 0 0 ") != std::string::npos, "recording holds the old value at time 0");
            const auto* player = app.m_world->Get<AnimationPlayerComponent>(animationProbe);
            Check(player && player->clipAssetPath == "Animations/AnimProbe.anim#Clip" && player->clip.IsValid(),
                  "saving assigns the clip to the object");
            ed::SetAnimationPreviewTime(0.5f);
        }
        if (frames == 12)
        {
            Check(std::fabs(AnimationCopyX() - 2.5f) < 1e-3f, "preview interpolates between keys");
            ed::SetAnimationPreviewTime(4.f);
        }
        if (frames == 15)
        {
            Check(std::fabs(AnimationCopyX() - 5.f) < 1e-3f, "behind the clip end the last pose holds");
            ed::SetAnimationPreviewTime(0.5f);
            Check(ed::UndoEditorHistory(ui), "undo of the assignment made by saving");
            Check(ed::UndoEditorHistory(ui), "undo of the recorded key");
        }
        if (frames == 18)
        {
            Check(ed::SaveOpenAnimation(ui) && AnimationClipText().find("channel translation") == std::string::npos,
                  "undo removes the recorded key");
            ed::KeyAnimationSelection(ui);
            Check(ed::AddAnimationEvent(0.5f) == 0 && ed::AnimationWindowEvents() && ed::AnimationWindowEvents()->size() == 1u &&
                  ed::SelectedAnimationEvent() == 0, "an event is added at the time and selected");
        }
        if (frames == 20)
        {
            Check(ed::SaveOpenAnimation(ui) && AnimationClipText().find("channel rotation") != std::string::npos,
                  "the current pose can be keyed without moving anything");
            Check(AnimationClipText().find("\nevent 0.5 0 Event\n") != std::string::npos, "the event is saved in the .anim");
            float eventX = 0.f, eventY = 0.f, bandX = 0.f, bandY = 0.f;
            Check(ed::AnimationEventScreenPosition(0, eventX, eventY) && ed::AnimationEventScreenPosition(-1, bandX, bandY) &&
                  eventX > bandX, "the timeline draws the event marker in the event band");
            ui.state.animationWindowOpen = false;
        }
        if (frames == 22)
        {
            Check(!ui.state.animationPreviewActive, "closing the window stops the 3D preview");
            Check(!app.m_world->IsAlive(ed::GetAnimationPreviewRoot()), "closing the window removes the copy");
            // Undo rebuilt the scene entities.
            animationProbe = FindPlayUiEntity("AnimProbe");
            ed::OpenAnimationEditor(ui, animationProbe);
        }
        if (frames == 24)
        {
            Check(ui.state.animationWindowOpen && ui.state.animationPreviewActive, "animator opens the saved animation");
            ed::SetAutoSaveEditorsForSession(true);
            ed::SetAnimationPreviewTime(0.25f);
        }
        if (frames == 26)
            app.m_world->Get<TransformComponent>(ed::GetAnimationPreviewRoot())->localPosition.x = 7.f;
        if (frames == 29)
        {
            Check(AnimationClipText().find("channel translation") != std::string::npos,
                  "auto-save writes a recorded change without Save");
            ui.state.animationWindowOpen = false;
        }
        // Model clips: an older "#animation/0" opens the model's clips, the
        // settings file is created, saving moves the animator to "#clip/<id>".
        if (frames == 32)
        {
            ed::SetAutoSaveEditorsForSession(false);
            WriteModelClipProbe();
            const EntityID box = ed::CreateEmptyEntity(ui, "ClipBox");
            auto& player = app.m_world->Add<AnimationPlayerComponent>(box);
            player.clipAssetPath = "Models/clip_probe.gltf#animation/0";
            ui.state.selectedEntity = box;
            ui.state.animationWindowOpen = true;
            ed::OpenAnimationEditor(ui, box);
        }
        if (frames == 35)
        {
            const auto& list = ed::GetModelClips(app.m_projectRoot / "Assets", "Models/clip_probe.gltf");
            Check(list.sources.size() == 1u && list.sources[0] == "Move", "model animation names come from the file");
            Check(list.settings.clips.size() == 1u && list.settings.clips[0].name == "Move" && !list.settings.clips[0].hasRange,
                  "each model animation gets a clip entry");
            Check(std::filesystem::exists(app.m_projectRoot / "Assets/Models/clip_probe.gltf.clips"),
                  "the clip settings are stored beside the model");
            Check(ui.state.animationPreviewActive, "the animator opens the model clips");
            ed::SetAnimationPreviewTime(0.5f);
            Check(ed::AddAnimationEvent(0.25f) == 0, "an event can be added to a model clip");
        }
        if (frames == 38)
        {
            Check(std::fabs(AnimationCopyX() - 1.f) < 1e-3f, "the preview plays the model clip");
            Check(ed::SaveOpenAnimation(ui), "model clip settings saved");
            const auto* player = app.m_world->Get<AnimationPlayerComponent>(FindPlayUiEntity("ClipBox"));
            const auto& list = ed::GetModelClips(app.m_projectRoot / "Assets", "Models/clip_probe.gltf");
            Check(player && !list.settings.clips.empty() &&
                  player->clipAssetPath == "Models/clip_probe.gltf#clip/" + list.settings.clips[0].id && player->clip.IsValid(),
                  "saving moves an older reference to the clip id");
            // A clip cut from frames 12..24 at 24 fps (0.5 s .. 1 s), added like the window does.
            Check(!list.settings.clips.empty() && list.settings.clips[0].events.size() == 1u &&
                  list.settings.clips[0].events[0].frame == 6, "the model clip event is saved in source frames");
            engine::animation::ModelClipSettings settings = list.settings;
            if (!settings.clips.empty())
                settings.clips[0].events.push_back({12, "Mid", 2.f, "half way"});
            engine::animation::ModelClipEntry cut = settings.clips[0];
            cut.id = settings.NewId();
            cut.name = "Second half";
            cut.hasRange = true;
            cut.startFrame = 12;
            cut.endFrame = 24;
            settings.clips.push_back(cut);
            modelClipCutId = cut.id;
            Check(ed::SaveModelClipSettings(app.m_projectRoot / "Assets", "Models/clip_probe.gltf", settings), "cut clip saved");
            ed::RefreshModelClipAssets(*ui.assetPipeline, ui.registry, "Models/clip_probe.gltf", settings);
            const auto* clip = ui.registry.animations.Get(ui.registry.FindAnimationByPath("Models/clip_probe.gltf#clip/" + cut.id));
            Check(clip && clip->name == "Second half" && std::fabs(clip->duration - 0.5f) < 1e-4f &&
                  !clip->channels.empty() && std::fabs(clip->channels[0].vec3Values.front().x - 1.f) < 1e-3f,
                  "the cut clip starts halfway through the source");
            Check(clip && clip->events.size() == 1u && clip->events[0].name == "Mid" && clip->events[0].time < 1e-4f,
                  "the cut clip keeps the events inside its range, relative to its start");
            ui.state.animationWindowOpen = false;
        }
        if (frames == 41)
        {
            ed::OpenModelClip(ui, "Models/clip_probe.gltf#clip/" + modelClipCutId);
        }
        if (frames == 44)
        {
            ed::SetAnimationPreviewTime(0.f);
        }
        if (frames == 47)
        {
            Check(ui.state.animationPreviewActive && std::fabs(AnimationCopyX() - 1.f) < 1e-3f,
                  "opening a cut clip shows it from its first frame");
            ui.state.animationWindowOpen = false;
        }
        // Scene switch asks to save: changes are detected against the last save.
        if (frames == 50)
        {
            Check(ui.isEditorSceneModified && ui.isEditorSceneModified(), "unsaved scene changes are detected");
            Check(app.SaveEditorScene(), "scene saved");
            Check(!ui.isEditorSceneModified(), "saving clears the unsaved state");
            ed::CreateEmptyEntity(ui, "AfterSave");
            Check(ui.isEditorSceneModified(), "a new object counts as a change");
            Check(ed::UndoEditorHistory(ui) && !ui.isEditorSceneModified(), "undo back to the saved state is not a change");
        }
        // Controller graph: states from model clips, a transition, undo/redo, save.
        if (frames == 52)
        {
            graphPath = ed::CreateAnimationControllerAsset(ui, app.m_projectRoot / "Assets");
            Check(!graphPath.empty() && ed::OpenControllerGraph(ui, graphPath) && ed::ControllerGraphPath() == graphPath &&
                  ui.state.controllerWindowOpen, "a new controller opens in the graph window");
            const auto& clips = ed::GetModelClips(app.m_projectRoot / "Assets", "Models/clip_probe.gltf");
            graphStateA = ed::ControllerGraphAddState(ui, "Models/clip_probe.gltf#clip/" + clips.settings.clips[0].id, 0.f, 0.f);
            graphStateB = ed::ControllerGraphAddState(ui, "Models/clip_probe.gltf#clip/" + modelClipCutId, 0.f, 140.f);
            (void)ed::ControllerGraphAddParameter(ui, engine::addons::animation::AnimationParameterType::Bool);
            graphTransition = ed::ControllerGraphAddTransition(ui, graphStateA, graphStateB);
            const auto* asset = ed::ControllerGraphAsset();
            Check(asset && asset->states.size() == 2u && asset->transitions.size() == 1u && asset->parameters.size() == 1u &&
                  asset->entryState == graphStateA, "states, parameter and transition are added; the first state is the start");
            Check(asset && asset->states[0].name == "Move" && asset->states[1].name == "Second half",
                  "a state takes its clip's name");
            ed::ControllerGraphSelect(graphTransition); // the inspector draws the transition card
        }
        if (frames == 54)
        {
            ed::ControllerGraphSelect(graphStateA);    // and the state card
            Check(ed::UndoEditorHistory(ui) && ed::ControllerGraphAsset()->states.empty(), "undo removes the edit");
            Check(ed::RedoEditorHistory(ui) && ed::ControllerGraphAsset()->transitions.size() == 1u, "redo brings it back");
        }
        if (frames == 56)
        {
            Check(ed::ControllerGraphDirty() && ed::SaveOpenControllerIfChanged(ui) && !ed::ControllerGraphDirty(),
                  "the controller is saved");
            const auto& saved = ed::GetEditorAnimationController(ui, graphPath);
            Check(saved.fileFound && saved.error.empty() && saved.asset.states.size() == 2u && saved.compiled.problems.empty(),
                  "the saved controller loads without problems");
            ed::ControllerGraphSelect(ed::ControllerGraphAsset()->parameters.front().id);
            Check(ed::ControllerGraphDeleteSelection(ui) && ed::ControllerGraphAsset()->parameters.empty(), "a parameter can be deleted");
            Check(ed::SaveOpenControllerIfChanged(ui), "saved again");
            // An object stays selected in the hierarchy: Delete in the graph must not remove it.
            ui.state.selectedEntity = FindPlayUiEntity("ClipBox");
            ed::ControllerGraphSelect({});
        }
        // Real mouse and key events on the arrow.
        ImGuiIO& io = ImGui::GetIO();
        if (frames == 58)
        {
            float x0 = 0.f, y0 = 0.f, x1 = 0.f, y1 = 0.f;
            Check(ed::ControllerGraphScreenPosition(graphTransition, arrowX, arrowY) &&
                  ed::ControllerGraphScreenPosition("#canvasMin", x0, y0) && ed::ControllerGraphScreenPosition("#canvasMax", x1, y1) &&
                  arrowX > x0 && arrowX < x1 && arrowY > y0 && arrowY < y1, "the transition arrow is drawn on the canvas");
            io.AddMousePosEvent(arrowX, arrowY);
        }
        if (frames == 59)
        {
            io.AddMouseButtonEvent(ImGuiMouseButton_Right, true);
        }
        if (frames == 60)
            io.AddMouseButtonEvent(ImGuiMouseButton_Right, false);
        if (frames == 62)
        {
            Check(ed::ControllerGraphSelection() == graphTransition && GImGui->OpenPopupStack.Size > 0,
                  "right-click on the arrow selects it and opens its menu");
            io.AddKeyEvent(ImGuiKey_Escape, true);
        }
        if (frames == 63)
            io.AddKeyEvent(ImGuiKey_Escape, false);
        if (frames == 64)
        {
            ed::ControllerGraphSelect({});
            io.AddMouseButtonEvent(ImGuiMouseButton_Left, true);
        }
        if (frames == 65)
            io.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
        if (frames == 66)
        {
            Check(ed::ControllerGraphSelection() == graphTransition, "a left click on the arrow selects it");
            io.AddKeyEvent(ImGuiKey_Delete, true);
        }
        if (frames == 67)
            io.AddKeyEvent(ImGuiKey_Delete, false);
        if (frames == 69)
        {
            const auto* asset = ed::ControllerGraphAsset();
            Check(asset && asset->transitions.empty(), "Delete removes the selected transition");
            Check(!ui.state.deleteConfirmOpen && app.m_world->IsAlive(FindPlayUiEntity("ClipBox")),
                  "Delete in the graph leaves the selected scene object alone");

            // Step 4b: the selected animator uses the open controller -> live preview.
            namespace ac = engine::addons::animation;
            graphTrigger = ed::ControllerGraphAddParameter(ui, ac::AnimationParameterType::Trigger);
            graphFade = ed::ControllerGraphAddTransition(ui, graphStateA, graphStateB);
            Check(ed::ControllerGraphEdit(ui, [&](ac::AnimationControllerAsset& asset) {
                for (auto& p : asset.parameters)
                    if (p.id == graphTrigger)
                        p.name = "Go";
                for (auto& t : asset.transitions)
                    if (t.id == graphFade)
                    {
                        t.blendDuration = 30.f; // long enough for slow debug frames
                        t.conditions.push_back({graphTrigger, ac::AnimationConditionOp::Triggered, 0.f});
                    }
            }), "trigger and transition set up");
            const EntityID box = FindPlayUiEntity("ClipBox");
            app.m_world->Get<AnimationPlayerComponent>(box)->controllerAssetPath = graphPath;
            ui.state.selectedEntity = box;
            Check(ed::ControllerPreviewCandidate(ui, graphPath) == box && !ed::ControllerPreviewRunning(graphPath),
                  "the selected animator can preview the open controller");
            ed::StartControllerPreview(ui); // the graph's Preview button
        }
        if (frames == 72)
        {
            const EntityID box = FindPlayUiEntity("ClipBox");
            Check(ed::ControllerPreviewRunning(graphPath) && ui.state.animationPreviewActive &&
                  ed::GetAnimationPreviewRoot() != box, "the animation window plays the controller on its copy");
            Check(PreviewState(ui) == 0, "the preview starts in the start state");
            Check(ui.sceneViewVisible || ed::AnimationPreviewOwnsGizmos(),
                  "the controller preview shows the gizmo like the clip preview");
            float x = 0.f, y = 0.f;
            Check(ed::ControllerGraphScreenPosition(graphStateA + "#bar", x, y), "the active state shows its time bar");
            Check(!ed::ControllerGraphScreenPosition(graphStateB + "#bar", x, y), "an inactive state has no bar");
            previewCopyX = AnimationCopyX();
        }
        if (frames == 76)
        {
            Check(std::fabs(AnimationCopyX() - previewCopyX) > 1e-5f, "the copy moves with the real frame delta");
            const auto* sceneBox = app.m_world->Get<TransformComponent>(FindPlayUiEntity("ClipBox"));
            Check(sceneBox && std::fabs(sceneBox->localPosition.x) < 1e-5f, "the scene object stays where it is");
            // The parameter panel's "Fire" sets the preview's value, not the default in the file.
            auto* player = ed::ControllerLivePlayer(ui, graphPath);
            const int32_t go = player ? player->controllerState.controller->FindParameter("Go") : -1;
            Check(go >= 0 && player->SetParameter(go, 1.f), "the trigger is set on the preview");
        }
        if (frames == 79)
        {
            const auto* player = ed::ControllerLivePlayer(ui, graphPath);
            Check(PreviewState(ui) == 1 && player && player->controllerState.instance.Blending(),
                  "the trigger fires the transition into the second state");
            Check(player && player->GetParameter(player->controllerState.controller->FindParameter("Go")) == 0.f,
                  "the transition consumes the trigger");
            float x = 0.f, y = 0.f;
            Check(ed::ControllerGraphScreenPosition(graphFade + "#fill", x, y), "the running transition fills its arrow");
            Check(ed::ControllerGraphScreenPosition(graphStateB + "#bar", x, y) &&
                  ed::ControllerGraphScreenPosition(graphStateA + "#bar", x, y), "both states of the fade show their bars");
            const auto* asset = ed::ControllerGraphAsset();
            Check(asset && asset->parameters.back().defaultValue == 0.f, "the default value in the file stays untouched");
            // Moving a node is no logic change: the preview keeps running.
            ed::ControllerGraphEdit(ui, [&](engine::addons::animation::AnimationControllerAsset& a) { a.states[1].posX += 40.f; });
        }
        if (frames == 82)
        {
            Check(PreviewState(ui) == 1, "moving a node does not restart the preview");
            std::string eventName;
            float eventAge = 0.f;
            Check(ed::ControllerPreviewLastEvent(eventName, eventAge) && (eventName == "Mid" || eventName == "Event"),
                  "the preview fires the clip events of its states");
            float ex = 0.f, ey = 0.f;
            if (eventAge < 1.f)
                Check(ed::ControllerGraphScreenPosition("#event", ex, ey), "the graph shows the fired event at the active state");
            // A logic change rebinds: back to the start state.
            ed::ControllerGraphEdit(ui, [&](engine::addons::animation::AnimationControllerAsset& a) { a.states[1].speed = 2.f; });
        }
        if (frames == 85)
        {
            const auto* player = ed::ControllerLivePlayer(ui, graphPath);
            Check(PreviewState(ui) == 0 && player && player->controllerState.controller->states[1].speed == 2.f,
                  "an edit of the working copy rebinds the preview without saving");
            ed::SetControllerPreviewPaused(true);
            previewCopyX = AnimationCopyX();
        }
        if (frames == 88)
        {
            Check(std::fabs(AnimationCopyX() - previewCopyX) < 1e-6f, "a paused preview holds its pose");
            ed::RestartControllerPreview(ui);
            Check(!ed::ControllerPreviewPaused(), "restart resumes the preview");
            ui.state.selectedEntity = NULL_ENTITY; // e.g. a clip clicked in the asset browser
        }
        if (frames == 90)
        {
            Check(ed::ControllerPreviewRunning(graphPath), "losing the selection keeps the preview running");
            ui.state.selectedEntity = FindPlayUiEntity("AnimProbe");
        }
        if (frames == 92)
        {
            Check(!ed::ControllerPreviewRunning(graphPath) && ed::GetAnimationPreviewRoot() != NULL_ENTITY,
                  "selecting another animator ends the controller preview");
            ui.state.selectedEntity = FindPlayUiEntity("ClipBox");
        }
        if (frames == 94)
        {
            Check(ed::ControllerPreviewRunning(graphPath), "selecting the object again restarts the preview");
            ed::OpenAnimationEditor(ui, FindPlayUiEntity("ClipBox")); // the inspector's Animation button
        }
        if (frames == 97)
        {
            Check(!ed::ControllerPreviewRunning(graphPath) && ui.state.animationPreviewActive,
                  "clips opened on purpose stay open while the controller is shown");
            ed::StartControllerPreview(ui); // the graph's Preview button
        }
        if (frames == 99)
        {
            Check(ed::ControllerPreviewRunning(graphPath), "the Preview button brings the controller preview back");
            const ImGuiWindow* focused = GImGui->NavWindow ? GImGui->NavWindow->RootWindow : nullptr;
            Check(focused && std::string_view(focused->Name).find("Controller##editor") != std::string_view::npos,
                  "the graph window keeps the focus when the preview starts");
            ed::StopControllerPreview(ui); // the graph's Stop button
        }
        if (frames == 100)
        {
            Check(!ed::ControllerPreviewRunning(graphPath) && ui.state.animationPreviewActive,
                  "Stop ends the preview and the window shows the object's clips again (with gizmos)");
            ui.state.animationWindowOpen = false;
        }
        if (frames == 102)
        {
            Check(!ed::ControllerPreviewRunning(graphPath) && !app.m_world->IsAlive(ed::GetAnimationPreviewRoot()),
                  "closing the window ends the preview and removes the copy");
            // Play: the same display for the selected object of the running game.
            Check(ed::SaveOpenControllerIfChanged(ui), "controller saved for Play");
            const EntityID box = FindPlayUiEntity("ClipBox");
            app.m_world->Get<AnimationPlayerComponent>(box)->playing = true;
            ui.state.selectedEntity = box;
            Check(app.BeginEditorPlay(), "Play starts");
        }
        if (frames == 104)
        {
            const EntityID box = FindPlayUiEntity("ClipBox");
            ui.state.selectedEntity = box;
            auto* player = ed::ControllerLivePlayer(ui, graphPath);
            Check(ui.playActive && player == app.m_world->Get<AnimationPlayerComponent>(box),
                  "in Play the graph shows the selected object's animator");
            Check(!ed::ControllerPreviewRunning(graphPath), "no preview copy during Play");
            const int32_t go = player ? player->controllerState.controller->FindParameter("Go") : -1;
            Check(go >= 0 && player->SetParameter(go, 1.f), "the graph sets the real parameter in Play");
            // Play brings the Game tab to the front; the graph tab shares its dock.
            ImGui::SetWindowFocus(ed::TranslateLabel("controller.title", "Controller##editor").c_str());
        }
        if (frames == 107)
        {
            float x = 0.f, y = 0.f;
            Check(PreviewState(ui) == 1, "the running game fires the transition");
            Check(ed::ControllerGraphScreenPosition(graphFade + "#fill", x, y), "the arrow shows the transition's progress");
            app.StopEditorPlay();
        }
        if (frames == 110)
        {
            Check(!ui.playActive, "Play stopped");
            ui.state.controllerWindowOpen = false;
        }
        if (frames == 112)
            context.renderLoop.GetWindow()->RequestClose();
        return true;
    }
    bool UpdatePlayUi(AppSceneContext& context)
    {
        auto& ui=*app.m_editorFrameCtx;
        if (frames==1)
        {
            renderer::addons::editor::CreateCameraEntity(ui);
            app.m_scriptRegistry.Register<PlayCounter>("PlayCounter");
            app.m_scriptRegistry.RegisterField<PlayCounter>("PlayCounter","spawnCount",script::ScriptFieldType::Int,&PlayCounter::spawnCount);
            const auto entity=renderer::addons::editor::CreateEmptyEntity(ui,"mask2");
            ParticleEmitterComponent emitter;
            emitter.playing = false;
            emitter.particleLifetime = 100.f;
            emitter.startSize = emitter.endSize = 1.f;
            emitter.frames = {{0u, 0u, 32u, 32u, .5f, .5f}, {32u, 0u, 32u, 32u, .5f, .5f},
                              {0u, 32u, 32u, 32u, .5f, 1.f}, {32u, 32u, 32u, 32u, .5f, 1.f}};
            emitter.clips = {{"Reverse", 3u, 0u, ParticleClipTiming::Rate, 8.f, true}};
            app.m_world->Add<ParticleEmitterComponent>(entity, emitter);
            auto& list=app.m_world->Add<script::ScriptList>(entity);
            list.Add("PlayCounter",entity,app.m_scriptRegistry);
            script::ScriptFieldValue value; value.type=script::ScriptFieldType::Int;
            list.Instances_Mutable()[0].fieldValues["spawnCount"]=value;
            ui.state.selectedEntity=entity;
        }
        if(frames==5) {
            Check(ui.sceneViewVisible && !ui.gameViewVisible,"startup selects Viewport over saved Game tab");
            const auto assets = renderer::addons::editor::TranslateLabel("panel.assets", "Assets##editor");
            const auto console = renderer::addons::editor::TranslateLabel("assets.console", "Console##editor");
            const auto* assetsWindow = ImGui::FindWindowByName(assets.c_str());
            const auto* consoleWindow = ImGui::FindWindowByName(console.c_str());
            Check(assetsWindow && consoleWindow && assetsWindow->DockId && assetsWindow->DockId == consoleWindow->DockId,
                "Assets and Console are separate tabs in the saved bottom dock");
            Check(assetsWindow && assetsWindow->DockTabIsVisible, "Assets is selected before Play");
            prewarmEmitter = renderer::addons::editor::CreateEmptyEntity(ui, "Prewarm dust");
            ParticleEmitterComponent dust;
            dust.prewarm = true;
            dust.emitRate = 0.f;
            dust.burstCount = 56u;
            dust.duration = 0.1f;
            dust.particleLifetime = 2.92f;
            dust.lifetimeRandom = 0.f;
            dust.maxParticles = 2000u;
            app.m_world->Add<ParticleEmitterComponent>(prewarmEmitter, dust);
            ui.state.selectedEntity = FindCounter();
            Check(app.BeginEditorPlay(),"begin editor Play");
        }
        if (frames == 6)
        {
            const auto* dustParticles = app.m_game.Particles().Particles(prewarmEmitter);
            Check(dustParticles && dustParticles->size() >= 28u * 56u &&
                std::all_of(dustParticles->begin(), dustParticles->end(), [](const particles::Particle& particle) {
                    return particle.age >= 0.049f;
                }), "First Play frame already has aged dust from multiple bursts");
        }
        if(frames==9) {
            Check(ui.gameViewVisible && !ui.sceneViewVisible,"Play selects Game tab");
            const auto assets = renderer::addons::editor::TranslateLabel("panel.assets", "Assets##editor");
            const auto* assetsWindow = ImGui::FindWindowByName(assets.c_str());
            Check(assetsWindow && assetsWindow->DockTabIsVisible, "Play keeps the Assets dock tab selected");
            Check(ui.gameViewFocused, "Game receives keyboard focus");
            Check(InspectorCounter()>0,"Inspector displays increasing live spawnCount");
            const auto* list = app.m_world->Get<script::ScriptList>(FindCounter());
            const auto* counter = static_cast<const PlayCounter*>(list->Instances()[0].script.get());
            previousParticle = counter->particle;
            Check(counter->clipPlayed, "Script resolves and plays a named particle clip");
            Check(app.m_game.Particles().IsParticleAlive(previousParticle), "Script spawns an addressable particle during Play");
            Check(app.m_game.Particles().GetParticleFrame(previousParticle).value_or(99u) <= 3u,
                "Script-controlled atlas animation reaches the live renderer");
            app.SetEditorPlayPaused(true); pausedCounter=InspectorCounter();
        }
        if(frames==13) {
            Check(ui.sceneViewVisible && !ui.gameViewVisible,"Pause selects Viewport tab");
            Check(InspectorCounter()==pausedCounter,"Pause keeps last live script value");
            app.SetEditorPlayPaused(false);
        }
        if(frames==17) {
            Check(ui.gameViewVisible && !ui.sceneViewVisible,"Resume selects Game tab");
            Check(InspectorCounter()>pausedCounter,"Resume continues updating live script value");
            app.StopEditorPlay();
        }
        if(frames==21) {
            Check(ui.sceneViewVisible && !ui.gameViewVisible,"Stop selects Viewport tab");
            Check(InspectorCounter()==0,"Stop restores stored edit value");
            Check(!app.m_game.Particles().IsParticleAlive(previousParticle), "Stop invalidates the script particle handle");
            ui.state.consoleFocusRequest = true;
        }
        if (frames == 25 || frames == 29)
        {
            const auto console = renderer::addons::editor::TranslateLabel("assets.console", "Console##editor");
            const auto* consoleWindow = ImGui::FindWindowByName(console.c_str());
            Check(consoleWindow && consoleWindow->DockTabIsVisible, "Console stays selected across Play");
        }
        if (frames == 25)
        {
            using renderer::addons::editor::AssetBrowserState;
            if (auto* dust = ui.world.Get<ParticleEmitterComponent>(FindPlayUiEntity("Prewarm dust")))
            {
                dust->playing = false;
                dust->burstCount = 73u;
            }
            Check(renderer::addons::editor::EnterPrefabMode(ui, particlePreviewPrefab), "open particle prefab in isolation");
            const auto* emitter = app.m_world->Get<ParticleEmitterComponent>(ui.state.selectedEntity);
            Check(emitter && emitter->playing, "isolated prefab keeps its active emitter");
            auto& thumbnail = ui.state.assetBrowser->assetPreviewThumbnails[PathToGenericUtf8(particlePreviewPrefab)];
            thumbnail.kind = AssetBrowserState::PreviewKind::Prefab;
            thumbnail.status = AssetBrowserState::PreviewStatus::Queued;
            Check(app.BeginEditorPlay(), "begin Play with Console selected");
        }
        if (frames == 29)
        {
            using renderer::addons::editor::AssetBrowserState;
            const auto* list = app.m_world->Get<script::ScriptList>(FindCounter());
            const auto* counter = static_cast<const PlayCounter*>(list->Instances()[0].script.get());
            Check(app.m_game.Particles().IsParticleAlive(counter->particle) && counter->particle != previousParticle,
                "A new Play run cannot reuse an old particle handle");
            const auto& thumbnail = ui.state.assetBrowser->assetPreviewThumbnails.at(PathToGenericUtf8(particlePreviewPrefab));
            Check(thumbnail.status == AssetBrowserState::PreviewStatus::Ready, "particle prefab thumbnail was rendered");
            Check(app.m_game.Particles().ParticleCount() == 1u, "only the scene script emits particles at Play start");
            if (auto* dust = ui.world.Get<ParticleEmitterComponent>(FindPlayUiEntity("Prewarm dust")))
                dust->burstCount = 4u;
            app.LoadSceneForPlay({.path = "Two.json"});
        }
        if (frames == 33)
        {
            Check(!FindCounter().IsValid(), "second scene replaced the first scene");
            Check(app.m_game.Particles().ParticleCount() == 0u, "thumbnail does not emit after scene change");
            app.LoadSceneForPlay({.path = "One.json"});
        }
        if (frames == 37)
        {
            const auto* dust = ui.world.Get<ParticleEmitterComponent>(FindPlayUiEntity("Prewarm dust"));
            Check(dust && dust->prewarm && !dust->playing && dust->burstCount == 73u,
                "Restart after a scene change reloads the emitter settings saved at Play start");
            app.StopEditorPlay();
            context.renderLoop.GetWindow()->RequestClose();
        }
        return true;
    }
    void SeedSavedGameLayout()
    {
        const auto dock=ImHashStr("EditorDockSpaceViewport",0,ImHashStr("##EditorDockHost"));
        char ini[1600];
        std::snprintf(ini,sizeof(ini),
            "[Window][Game##editor]\nDockId=0x00000005,0\n"
            "[Window][Viewport##editor]\nDockId=0x00000005,1\n"
            "[Window][Hierarchie##editor]\nDockId=0x00000003,0\n"
            "[Window][Inspector##editor]\nDockId=0x00000006,0\n"
            "[Window][Assets##editor]\nDockId=0x00000002,0\n"
            "[Docking][Data]\nDockSpace ID=0x%08X Pos=0,64 Size=800,536 Split=Y\n"
            " DockNode ID=0x00000001 Parent=0x%08X SizeRef=800,326 Split=X\n"
            "  DockNode ID=0x00000003 Parent=0x00000001 SizeRef=180,326\n"
            "  DockNode ID=0x00000004 Parent=0x00000001 SizeRef=620,326 Split=X\n"
            "   DockNode ID=0x00000005 Parent=0x00000004 SizeRef=380,326 CentralNode=1 Selected=0x%08X\n"
            "   DockNode ID=0x00000006 Parent=0x00000004 SizeRef=240,326\n"
            " DockNode ID=0x00000002 Parent=0x%08X SizeRef=800,210\n",
            dock,dock,ImHashStr("Game##editor"),dock);
        ImGui::LoadIniSettingsFromMemory(ini);
    }
    std::string reloadProject;
    bool externalScriptProject = false;
    bool externalProjectExport = false;
    bool externalExportOnly = false;
    bool externalStepOk = false;
    std::filesystem::path reloadRoot;
    int reloadStage = 0;
    int reloadPlayFrames = 0;
    int reloadCancelFrames = 0;
    bool reloadModalDrawn = false;
    float reloadModalMinWidth = 1.0e9f;
    bool reloadSawBuild = false;
    std::string reloadUndoLabel;
    std::chrono::steady_clock::time_point reloadStarted;

    EntityID ReloadProbe() const
    {
        EntityID result{};
        app.m_world->ForEachAlive([&](EntityID entity) {
            const auto* name = app.m_world->Get<NameComponent>(entity);
            if (name && name->name == "ReloadProbe") result = entity;
        });
        return result;
    }
    void WriteReloadProbe(int value, bool broken = false)
    {
        std::ofstream file(reloadRoot / "Assets/Scripts/CodexReloadProbe.cpp", std::ios::binary);
        if (broken) { file << "this is an intentional C++ compile error\n"; return; }
        file << "#include \"CodexReloadProbe.hpp\"\n"
                "void CodexReloadProbe::OnUpdate(engine::script::IScriptContext& ctx) {\n"
                "ctx.SetPosition({" << value << ".f + amount, 0.f, 0.f});\n}\n";
    }
    void RequestReloadBuild(int stage)
    {
        reloadStage = stage;
        reloadPlayFrames = 0;
        Check(app.m_editorFrameCtx->startPlay(), "Start button accepted script build");
        Check(app.m_runtimeBuildJob != nullptr && !app.m_playActive, "build runs asynchronously before Play");
        reloadSawBuild = true;
    }
    bool UpdateScriptReload(AppSceneContext& context)
    {
        if (failures) { context.renderLoop.GetWindow()->RequestClose(); return true; }
        if (std::chrono::steady_clock::now() - reloadStarted > std::chrono::minutes(externalProjectExport ? 15 : 4))
        {
            Check(false, "script reload timed out");
            context.renderLoop.GetWindow()->RequestClose();
            return true;
        }
        if (reloadStage >= 7)
        {
            if (app.m_runtimeBuildJob) return true;
            const std::string backend = app.m_projectBackend == renderer::DeviceFactory::BackendType::DirectX11
                ? "dx11" : app.m_projectBackend == renderer::DeviceFactory::BackendType::OpenGL ? "opengl" : "vulkan";
            if (reloadStage == 7)
            {
                Check(externalStepOk, "testbuild of external project succeeds");
                if (failures) return true;
                const auto exe = PathFromUtf8(KROM_BUILD_DIR) / "bin" /
                    PathFromUtf8(app.m_projectRuntimeTarget + "_" + backend + ".exe");
                reloadStage = 8;
                externalStepOk = false;
                Check(app.StartBuildJob("External runtime smoke", {{exe,
                    {PathToUtf8(reloadRoot / "krom-project.json"), "--warmup-shaders"}, reloadRoot, {}}},
                    [this](bool ok) { externalStepOk = ok; }, "External runtime smoke"), "start external testbuild game");
            }
            else if (reloadStage == 8)
            {
                Check(externalStepOk, "external testbuild game starts and renders all scenes");
                reloadStage = 9;
                Check(app.ExportProjectBuild(), "start full external project export");
            }
            else if (reloadStage == 9)
            {
                const auto exported = reloadRoot / "Build" / ("runtime-Release-" + backend);
                Check(std::filesystem::exists(exported / "krom-project.json") &&
                    std::filesystem::exists(exported / "licenses/KROM-LICENSE.txt"), "external export contains project and licenses");
                Check(app.m_editorFrameCtx->lastFileMessage.starts_with(Translate("export.success")),
                    "exported game starts and completes shader warmup");
                Check(app.LoadProjectFile(PathToUtf8(PathFromUtf8(KROM_SOURCE_DIR) / "projects/DemoProject/krom-project.json")),
                    "return to DemoProject after external export");
                reloadStage = 10;
                externalStepOk = false;
                Check(app.StartRuntimeBuild(PathFromUtf8(KROM_BUILD_DIR),
                    [this](bool ok) { externalStepOk = ok; }, "krom_scripts_DemoProject"), "build DemoProject after external selection");
            }
            else
            {
                Check(externalStepOk, "DemoProject script build succeeds after external export");
                context.renderLoop.GetWindow()->RequestClose();
            }
            return true;
        }
        if (reloadStage == 0)
        {
            const auto entity = renderer::addons::editor::CreateEmptyEntity(*app.m_editorFrameCtx, "ReloadProbe");
            auto& list = app.m_world->Add<script::ScriptList>(entity);
            list.SetOwnerEntity(entity);
            list.AddNameOnly("CodexReloadProbe", "Named probe");
            script::ScriptFieldValue amount;
            amount.type = script::ScriptFieldType::Float;
            amount.floatValue = 3.f;
            list.Instances_Mutable()[0].fieldValues["amount"] = amount;
            app.m_editorFrameCtx->state.selectedEntity = entity;
            renderer::addons::editor::ExecuteSceneMutation(*app.m_editorFrameCtx, "Probe setup", [&] {
                app.m_world->Get<TransformComponent>(entity)->localPosition.y = 2.f;
            });
            reloadUndoLabel = renderer::addons::editor::GetUndoEditorHistoryLabel(app.m_editorFrameCtx->state);
            Check(!reloadUndoLabel.empty(), "edit history exists before module swap");
            Check(!app.m_scriptModule.IsLoaded() && !app.m_runtimeBuildJob, "missing DLL stays a placeholder on project open");
            if (externalExportOnly)
            {
                Check(renderer::addons::editor::RefreshCppScriptProject(*app.m_editorFrameCtx).ok,
                    "generate external script bindings before the first runtime build");
                app.m_buildExcludedScenes.insert("Broken");
                Check(app.BuildAllScenes(), "build external project scenes");
                reloadStage = 7;
                externalStepOk = false;
                Check(app.StartRuntimeBuild(PathFromUtf8(KROM_BUILD_DIR),
                    [this](bool ok) { externalStepOk = ok; }), "start external runtime testbuild without a prior script build");
                return true;
            }
            RequestReloadBuild(1);
            return true;
        }
        if (reloadStage == 5 && app.m_runtimeBuildJob)
        {
            if (++reloadCancelFrames >= 5 && reloadModalDrawn && app.m_editorFrameCtx->cancelBuild)
                app.m_editorFrameCtx->cancelBuild();
            return true;
        }
        if (app.m_runtimeBuildJob) return true;
        if (reloadStage == 3)
        {
            Check(!app.m_playActive, "compiler error prevents Play");
            Check(app.m_scriptModule.IsLoaded() && app.m_scriptRegistry.Has("CodexReloadProbe"), "compiler error preserves last loaded DLL");
            bool diagnostic = false;
            for (const auto& log : app.m_editorFrameCtx->state.consoleLogs)
                if (log.level >= LogLevel::Error && log.message.find("[Build]") != std::string::npos)
                    diagnostic = true;
            Check(diagnostic, "compiler diagnostic reaches editor console");
            script::ScriptServices services;
            services.world = app.m_world.get();
            app.m_world->Get<script::ScriptList>(ReloadProbe())->Update(0.016f, ReloadProbe(), services);
            Check(app.m_world->Get<TransformComponent>(ReloadProbe())->localPosition.x == 25.f, "old DLL still executes after failed build");
            WriteReloadProbe(33);
            RequestReloadBuild(4);
            return true;
        }
        if (reloadStage == 5)
        {
            Check(!app.m_playActive, "cancelled build prevents Play");
            Check(app.m_scriptModule.IsLoaded(), "cancelled build preserves loaded DLL");
            WriteReloadProbe(55);
            RequestReloadBuild(6);
            return true;
        }
        if (++reloadPlayFrames < 4) return true;
        const auto entity = ReloadProbe();
        Check(app.m_playActive, "successful script build starts Play");
        const auto* transform = app.m_world->Get<TransformComponent>(entity);
        const float expected = reloadStage == 1 ? 14.f : reloadStage == 2 ? 25.f : reloadStage == 4 ? 36.f : 58.f;
        Check(transform && transform->localPosition.x == expected, "new DLL code executes with preserved field value");
        Check(app.m_editorFrameCtx->state.selectedEntity == entity, "module swap preserves selection and EntityID");
        app.StopEditorPlay();
        Check(renderer::addons::editor::GetUndoEditorHistoryLabel(app.m_editorFrameCtx->state) == reloadUndoLabel,
            "reload and Play preserve edit undo history");
        if (reloadStage == 1) { WriteReloadProbe(22); RequestReloadBuild(2); }
        else if (reloadStage == 2) { WriteReloadProbe(0, true); RequestReloadBuild(3); }
        else if (reloadStage == 4)
        {
            Check(app.StartEditorPlay() && app.m_playActive && !app.m_runtimeBuildJob, "unchanged scripts start immediately without a build");
            app.StopEditorPlay();
            WriteReloadProbe(44);
            RequestReloadBuild(5);
        }
        else
        {
            Check(reloadSawBuild && reloadModalDrawn, "modal build progress rendered during worker build");
            Check(reloadModalMinWidth >= 500.f, "modal build progress keeps its width");
            Check(app.SaveEditorScene(), "save edit scene with reloaded scripts");
            const auto other = reloadRoot / "Other";
            std::filesystem::create_directories(other / "Assets/Scenes");
            std::ofstream(other / "krom-project.json") << "{\"name\":\"Other\",\"runtimeTarget\":\"" <<
                (externalScriptProject ? app.m_projectRuntimeTarget : "Other") << "\",\"sceneOrder\":[\"One\"]}";
            std::ofstream(other / "Assets/Scenes/One.json") << R"({"scene":"One","entities":[]})";
            Check(app.LoadProjectFile(PathToUtf8(other / "krom-project.json")), "switch to another project");
            Check(!app.m_scriptModule.IsLoaded() && app.m_scriptRegistry.GetFactoryCount() == 0, "project switch unloads previous module");
            if (externalScriptProject)
                Check(std::any_of(app.m_editorFrameCtx->state.consoleLogs.begin(), app.m_editorFrameCtx->state.consoleLogs.end(),
                    [](const auto& log) { return log.message.find(Translate("script.dll_wrong_project")) != std::string::npos; }),
                    "project open rejects existing same-target DLL owned by the other project");
            Check(app.LoadProjectFile(PathToUtf8(reloadRoot / "krom-project.json")), "reopen script project");
            Check(app.m_scriptModule.IsLoaded() && !app.m_runtimeBuildJob, "project open loads existing DLL without building");
            Check(ReloadProbe().IsValid() && app.m_world->Get<script::ScriptList>(ReloadProbe())->Instances()[0].script != nullptr,
                "project open restores live script objects");
            if (externalProjectExport)
            {
                // Broken.json is the loading-error fixture used by the base test.
                app.m_buildExcludedScenes.insert("Broken");
                Check(app.BuildAllScenes(), "build external project scenes");
                reloadStage = 7;
                externalStepOk = false;
                Check(app.StartRuntimeBuild(PathFromUtf8(KROM_BUILD_DIR),
                    [this](bool ok) { externalStepOk = ok; }), "start external runtime testbuild");
            }
            else context.renderLoop.GetWindow()->RequestClose();
        }
        return true;
    }

    void Check(bool success, const char* message)
    {
        std::printf("%s: %s\n", success ? "PASS" : "FAIL", message);
        std::fflush(stdout);
        if (!success)
        {
            ++failures;
            if (app.m_editorFrameCtx)
                for (const auto& log : app.m_editorFrameCtx->state.consoleLogs)
                    if (log.level >= LogLevel::Error)
                        std::printf("DIAGNOSTIC: %s\n", log.message.c_str());
            std::fflush(stdout);
        }
    }
    bool Switch(const char* name)
    {
        return directSwitch ? app.SwitchEditorScene(name, false)
                            : app.m_editorFrameCtx->switchEditorScene(name, false);
    }
    bool Build(AppSceneContext&) override { return true; }

    // --dock-drag: Hierarchie-Panel im vollstaendigen Editor am Dock-Reiter herausziehen.
    bool dockDrag = false;
    ImVec2 dockGrab{};
    void TraceDock(const char* step)
    {
        const ImGuiContext& g = *ImGui::GetCurrentContext();
        std::printf("DOCK %s frame=%d mouse=%.0f,%.0f hovered=%s activeId=%08x moving=%s\n", step, frames,
            g.IO.MousePos.x, g.IO.MousePos.y, g.HoveredWindow ? g.HoveredWindow->Name : "-",
            g.ActiveId, g.MovingWindow ? g.MovingWindow->Name : "-");
        std::fflush(stdout);
    }
    bool UpdateDockDrag(AppSceneContext& context)
    {
        ImGuiIO& io = ImGui::GetIO();
        ImGuiWindow* panel = ImGui::FindWindowByName("Hierarchie##editor");
        if (frames == 3)
        {
            Check(panel && panel->DockIsActive && panel->DockNode && panel->DockNode->TabBar,
                  "hierarchy panel docked with tab bar");
            if (!panel || !panel->DockNode || !panel->DockNode->TabBar)
            {
                context.renderLoop.GetWindow()->RequestClose();
                return true;
            }
            const ImRect bar = panel->DockNode->TabBar->BarRect;
            dockGrab = ImVec2(bar.Min.x + 20.f, (bar.Min.y + bar.Max.y) * 0.5f);
            io.AddMousePosEvent(dockGrab.x, dockGrab.y);
        }
        else if (frames == 4) { TraceDock("hover"); io.AddMouseButtonEvent(ImGuiMouseButton_Left, true); }
        else if (frames >= 5 && frames <= 14)
        {
            TraceDock("drag");
            // Ueber das Viewport ziehen, abseits der Andock-Ziele (Mitte = als Reiter einfuegen).
            const auto& ctx = *app.m_editorFrameCtx;
            const ImVec2 target(ctx.sceneViewX + ctx.sceneViewWidth * 0.3f, ctx.sceneViewY + ctx.sceneViewHeight * 0.25f);
            const float t = static_cast<float>(frames - 4) / 10.f;
            io.AddMousePosEvent(dockGrab.x + (target.x - dockGrab.x) * t, dockGrab.y + (target.y - dockGrab.y) * t);
        }
        else if (frames == 15) { TraceDock("release"); io.AddMouseButtonEvent(ImGuiMouseButton_Left, false); }
        else if (frames == 17)
        {
            if (panel && panel->DockNode)
            {
                const ImGuiDockNode* node = panel->DockNode;
                const ImGuiDockNode* rootNode = ImGui::DockNodeGetRootNode(const_cast<ImGuiDockNode*>(node));
                std::printf("DOCK result node=%08x root=%08x floating=%d central=%d pos=%.0f,%.0f size=%.0fx%.0f windows=%d\n",
                    node->ID, rootNode->ID, (int)rootNode->IsFloatingNode(), (int)node->IsCentralNode(),
                    node->Pos.x, node->Pos.y, node->Size.x, node->Size.y, node->Windows.Size);
            }
            Check(panel && !panel->DockIsActive, "hierarchy panel undocked by dragging its tab");
            context.renderLoop.GetWindow()->RequestClose();
        }
        return true;
    }

    // --detached-window: drag the material editor out of the editor window. It must
    // become its own OS window (multi-viewport) with a renderer swapchain, while
    // pinned panels (Hierarchy) stay in the editor window.
    bool detachedWindow = false;
    ImVec2 detachGrab{};
    float detachStartX = 0.f;
    bool UpdateDetachedWindow(AppSceneContext& context)
    {
        ImGuiIO& io = ImGui::GetIO();
        auto& editor = *app.m_editorFrameCtx;
        const auto title = renderer::addons::editor::TranslateLabel("material.title", "Materialeditor##editor");
        ImGuiWindow* window = ImGui::FindWindowByName(title.c_str());
        const ImGuiViewport* mainViewport = ImGui::GetMainViewport();
        if (frames == 1)
        {
            Check((io.ConfigFlags & ImGuiConfigFlags_ViewportsEnable) != 0, "multi-viewports enabled");
            Check((io.BackendFlags & ImGuiBackendFlags_PlatformHasViewports) &&
                  (io.BackendFlags & ImGuiBackendFlags_RendererHasViewports),
                  "platform and renderer backends support viewports");
            editor.state.materialWindowOpen = true;
            editor.state.materialWindowFocusRequest = true;
        }
        else if (frames == 3)
        {
            // Placement is relative to the editor window (desktop coordinates).
            if (window)
                std::printf("OPEN window=%.0f,%.0f main=%.0f,%.0f work=%.0f,%.0f\n", window->Pos.x, window->Pos.y,
                    mainViewport->Pos.x, mainViewport->Pos.y, mainViewport->WorkPos.x, mainViewport->WorkPos.y);
            Check(window && window->Active &&
                  std::abs(window->Pos.x - (mainViewport->Pos.x + 280.f)) < 1.f &&
                  std::abs(window->Pos.y - (mainViewport->Pos.y + 10.f)) < 16.f, // ImGui nudges y a little
                  "material editor opens at its place relative to the editor window");
            Check(window && window->Viewport == mainViewport,
                  "material editor opens inside the editor window");
            if (!window)
            {
                context.renderLoop.GetWindow()->RequestClose();
                return true;
            }
            detachStartX = window->Pos.x;
            const ImRect bar = window->TitleBarRect();
            detachGrab = ImVec2(bar.Min.x + 40.f, (bar.Min.y + bar.Max.y) * 0.5f);
            io.AddMousePosEvent(detachGrab.x, detachGrab.y);
        }
        else if (frames == 4) io.AddMouseButtonEvent(ImGuiMouseButton_Left, true);
        else if (frames >= 5 && frames <= 14)
        {
            // Right of the editor window, so the window no longer fits into it.
            const ImVec2 target(mainViewport->Pos.x + mainViewport->Size.x + 400.f, detachGrab.y + 40.f);
            const float t = static_cast<float>(frames - 4) / 10.f;
            io.AddMousePosEvent(detachGrab.x + (target.x - detachGrab.x) * t, detachGrab.y + (target.y - detachGrab.y) * t);
        }
        else if (frames == 15) io.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
        else if (frames == 18)
        {
            const ImGuiViewport* own = window ? window->Viewport : nullptr;
            std::printf("DETACH viewport=%p main=%p owned=%d created=%d pos=%.0f,%.0f size=%.0fx%.0f\n",
                (const void*)own, (const void*)mainViewport, window ? (int)window->ViewportOwned : -1,
                own ? (int)own->PlatformWindowCreated : -1,
                own ? own->Pos.x : 0.f, own ? own->Pos.y : 0.f, own ? own->Size.x : 0.f, own ? own->Size.y : 0.f);
            Check(own && own != mainViewport && window->ViewportOwned,
                  "dragged material editor owns its own viewport");
            Check(window && window->Pos.x > detachStartX + 300.f && own && own->Pos.x == window->Pos.x,
                  "the OS window follows the dragged material editor");
            Check(own && own->PlatformWindowCreated && own->PlatformHandle && own->RendererUserData,
                  "detached viewport has an OS window and a renderer swapchain");
            const auto hierarchy = renderer::addons::editor::TranslateLabel("menu.hierarchy", "Hierarchie##editor");
            const ImGuiWindow* hierarchyWindow = ImGui::FindWindowByName(hierarchy.c_str());
            Check(hierarchyWindow && hierarchyWindow->Viewport == mainViewport,
                  "pinned panels stay in the editor window");
        }
        else if (frames == 30)
        {
            // Rendered several frames into the secondary swapchain without errors.
            Check(window && window->Viewport != mainViewport && ImGui::GetPlatformIO().Viewports.Size >= 2,
                  "detached window survives further frames");
            editor.state.materialWindowOpen = false;
        }
        else if (frames == 33)
        {
            Check(ImGui::GetPlatformIO().Viewports.Size == 1, "closing the material editor removes its OS window");
            context.renderLoop.GetWindow()->RequestClose();
        }
        return true;
    }

    // --shader-compile-status <krom-project.json>: loads the project like --perf and
    // runs until no background shader compile is left. With a cold shader cache
    // (delete <project>/shader-bin first) it checks that the status bar shows the
    // compiles and that the editor keeps rendering frames meanwhile.
    bool compileStatus = false;
    std::chrono::steady_clock::time_point compileLast{};
    std::chrono::steady_clock::time_point compilePrinted{};
    std::chrono::steady_clock::time_point compileStarted{};
    double compileMaxFrameMs = 0.0;
    int compileFrames = 0;
    int compileLongFrames = 0;   // > 250 ms while compiling
    int compileDoneFrame = 0;
    uint32_t compileTotal = 0u;
    bool compileBarSeen = false;

    bool UpdateShaderCompileStatus(AppSceneContext& context)
    {
        using Clock = std::chrono::steady_clock;
        const renderer::ShaderRuntime& runtime = context.renderLoop.GetRenderSystem().GetShaderRuntime();
        const auto& status = app.m_editorFrameCtx->shaderCompileStatus;
        const auto now = Clock::now();
        if (frames == 1)
            compileStarted = now;
        const bool pending = runtime.HasPendingShaderCompiles();
        if (pending)
        {
            if (compileFrames > 0)
            {
                const double frameMs = std::chrono::duration<double, std::milli>(now - compileLast).count();
                compileMaxFrameMs = std::max(compileMaxFrameMs, frameMs);
                if (frameMs > 250.0)
                    ++compileLongFrames;
            }
            ++compileFrames;
            compileTotal = std::max(compileTotal, status.total);
            // The bar of the previous frame: this frame's UI is not drawn yet.
            const ImGuiWindow* bar = ImGui::FindWindowByName("##editorStatusBar");
            if (status.Active() && bar && bar->LastFrameActive >= ImGui::GetFrameCount() - 1 &&
                bar->Size.y > 0.f)
                compileBarSeen = true;
            if (now - compilePrinted > std::chrono::seconds(2))
            {
                std::printf("COMPILE %u/%u running=%u shader=%s [%s] frames=%d\n",
                    status.completed, status.total, status.running, status.shaderName.c_str(),
                    status.defines.c_str(), compileFrames);
                std::fflush(stdout);
                compilePrinted = now;
            }
        }
        compileLast = now;

        const double seconds = std::chrono::duration<double>(now - compileStarted).count();
        if (frames > 60 && !pending && compileDoneFrame == 0)
        {
            // The app copies the status after this update: check it next frame.
            compileDoneFrame = frames;
            std::printf("COMPILE done: compiles=%u frames-while-compiling=%d long-frames(>250ms)=%d "
                        "max-frame=%.0f ms after %.0f s scene=%s\n",
                compileTotal, compileFrames, compileLongFrames, compileMaxFrameMs, seconds,
                app.m_currentSceneName.c_str());
            std::fflush(stdout);
        }
        else if (compileDoneFrame != 0 && frames > compileDoneFrame)
        {
            if (compileTotal > 0u)
            {
                Check(compileBarSeen, "status bar shown while shaders compile");
                Check(compileFrames > 1, "editor renders frames while shaders compile");
            }
            Check(!status.Active(), "status cleared after the last compile");
            Check(context.renderLoop.GetRenderSystem().GetStats().opaqueDraws > 0u,
                  "scene drawn after the compiles");
            context.renderLoop.GetWindow()->RequestClose();
        }
        else if (seconds > 30.0 * 60.0)
        {
            Check(false, "background shader compiles finished within 30 minutes");
            context.renderLoop.GetWindow()->RequestClose();
        }
        return true;
    }

    // --perf <krom-project.json>: echtes Projekt laden und mittlere Bildzeit messen.
    std::string perfProject;
    std::filesystem::path gtaoCaptureDir;
    EntityID gtaoBody{};
    math::Vec3 gtaoOriginalPosition{};
    math::Vec3 gtaoFocus{};
    bool perfComparedGtao = false;
    bool sceneShadersReady = false;
    int readyFrames = 0;
    std::chrono::steady_clock::time_point shaderWaitStart{};
    renderer::addons::editor::EditorCameraState gtaoOrbitCamera{};
    std::chrono::steady_clock::time_point perfStart;
    std::chrono::steady_clock::time_point perfLast;
    // Start-up cost: test start -> first perf frame -> end of warm-up (frame 60).
    std::chrono::steady_clock::time_point perfProcessStart = std::chrono::steady_clock::now();
    double perfFirstFrameMs = -1.0;
    double perfWarmupEndMs = 0.0;
    std::vector<double> perfFrameMs;
    uint64_t perfDescriptorUpdates = 0u;
    uint64_t perfDescriptorBinds = 0u;
    bool WaitForSceneShaders(AppSceneContext& context)
    {
        if (sceneShadersReady)
            return false;

        const auto now = std::chrono::steady_clock::now();
        if (shaderWaitStart == std::chrono::steady_clock::time_point{})
            shaderWaitStart = now;
        auto& renderSystem = context.renderLoop.GetRenderSystem();
        if (renderSystem.GetStats().opaqueCpu.draws > 0u &&
            !renderSystem.GetShaderRuntime().HasPendingShaderCompiles())
        {
            if (++readyFrames >= 5)
            {
                sceneShadersReady = true;
                frames = 0;
            }
        }
        else
        {
            readyFrames = 0;
        }
        if (now - shaderWaitStart > std::chrono::minutes(5))
        {
            Check(false, "scene shaders ready and opaque geometry submitted");
            context.renderLoop.GetWindow()->RequestClose();
        }
        return true;
    }

    bool UpdateGtaoMotion(AppSceneContext& context)
    {
        auto& state = app.m_editorFrameCtx->state;
        if (frames == 10)
        {
            gtaoBody = FindPlayUiEntity("Spieler Koerper");
            const auto* world = app.m_world->Get<WorldTransformComponent>(gtaoBody);
            const auto* local = app.m_world->Get<TransformComponent>(gtaoBody);
            if (!world || !local)
            {
                Check(false, "Room 2 player body available for GTAO readback");
                context.renderLoop.GetWindow()->RequestClose();
                return true;
            }
            gtaoOriginalPosition = local->localPosition;
            gtaoFocus = world->position;
            auto& camera = state.editorCamera;
            camera.position = world->position + math::Vec3{0.75f, 0.7f, 3.0f};
            const math::Vec3 direction = (world->position - camera.position).Normalized();
            camera.yawDeg = std::atan2(-direction.x, -direction.z) * math::RAD_TO_DEG;
            camera.pitchDeg = std::asin(direction.y) * math::RAD_TO_DEG;
            gtaoOrbitCamera = camera;
            // KROM_CAPTURE_LIT=1: capture the lit view (e.g. specular AA) instead of the GTAO view.
            // KROM_CAPTURE_CONTACT=1: capture the contact shadow view (Shift+F3).
            state.editorDebugFlags = std::getenv("KROM_CAPTURE_CONTACT") ? renderer::DBG_VIEW_CONTACT_SHADOW
                                   : std::getenv("KROM_CAPTURE_LIT")     ? 0u : renderer::DBG_VIEW_GTAO;
        }

        const auto capture = [&](const char* file)
        {
            auto* device = context.renderLoop.GetRenderSystem().GetDevice();
            const TextureHandle texture = device->GetRenderTargetColorTexture(app.m_sceneView.rt);
            std::vector<uint8_t> rgba;
            uint32_t width = 0u, height = 0u;
            if (!device->ReadTexturePixels(texture, rgba, width, height) ||
                rgba.size() != static_cast<size_t>(width) * height * 4u)
            {
                Check(false, "GTAO editor viewport GPU readback");
                return;
            }
            std::vector<char> rgb;
            rgb.reserve(static_cast<size_t>(width) * height * 3u);
            for (size_t i = 0; i < rgba.size(); i += 4u)
                rgb.insert(rgb.end(), {static_cast<char>(rgba[i]), static_cast<char>(rgba[i + 1]),
                                       static_cast<char>(rgba[i + 2])});
            std::ofstream out(gtaoCaptureDir / file, std::ios::binary);
            out << "P6\n" << width << ' ' << height << "\n255\n";
            out.write(rgb.data(), static_cast<std::streamsize>(rgb.size()));
            Check(static_cast<bool>(out), file);
        };

        // KROM_CAPTURE_PPVOLUME=1: same view, the first post-process volume of the
        // scene is moved so the camera is outside, in the middle of the blend band
        // and inside (outside.ppm, edge.ppm, inside.ppm). Needs a box volume.
        if (std::getenv("KROM_CAPTURE_PPVOLUME") && frames >= 120)
        {
            EntityID volumeEntity = NULL_ENTITY;
            app.m_world->View<PostProcessVolumeComponent>([&](EntityID id, PostProcessVolumeComponent&) {
                if (!volumeEntity.IsValid()) volumeEntity = id;
            });
            auto* volume = volumeEntity.IsValid() ? app.m_world->Get<PostProcessVolumeComponent>(volumeEntity) : nullptr;
            auto* transform = volumeEntity.IsValid() ? app.m_world->Get<TransformComponent>(volumeEntity) : nullptr;
            if (!volume || !transform || volume->global)
            {
                Check(false, "scene has a box post-process volume");
                context.renderLoop.GetWindow()->RequestClose();
                return true;
            }
            // Camera depth from the +X wall: < 0 outside, blend/2 in the band, far inside.
            const auto place = [&](float depth) {
                const math::Vec3 cam = state.editorCamera.position;
                const math::Vec3 center{ cam.x - volume->extents.x + depth, cam.y, cam.z };
                transform->localPosition = center - transform->localRotation.Rotate(volume->boxOffset);
                transform->dirty = true;
                ++transform->localVersion;
            };
            const auto report = [&](const char* label) {
                const auto& t = *app.m_tonemapSettings;
                std::printf("PPVOLUME %s exposureEv=%+.3f temperature=%.2f saturation=%.3f look=%u bloom=%.3f\n",
                            label, t.exposureEv, t.grading.temperature, t.grading.saturation,
                            static_cast<unsigned>(t.grading.look), t.bloomIntensity);
            };
            switch (frames)
            {
            case 120: place(-1.f); break;
            case 126: report("outside"); capture("outside.ppm"); break;
            case 127: place(volume->blendDistance * 0.5f); break;
            case 133: report("edge"); capture("edge.ppm"); break;
            case 134: place(volume->extents.x); break;
            case 140: report("inside"); capture("inside.ppm"); break;
            case 141: context.renderLoop.GetWindow()->RequestClose(); break;
            default: break;
            }
            return true;
        }

        // Update runs before rendering. The readback on n+1 captures precisely
        // the first submitted frame after the mutation on n.
        switch (frames)
        {
        case 120:
        {
            const auto& stats = context.renderLoop.GetRenderSystem().GetStats();
            std::printf("GTAO probe visible=%u opaque=%u submitted=%u passes=%u\n",
                stats.visibleProxyCount, stats.opaqueDraws, stats.opaqueCpu.draws, stats.graphPassCount);
            if (const auto* exposure = app.m_editorFrameCtx->autoExposureStatus;
                exposure && exposure->valid.load())
                std::printf("Auto-exposure current=%+.3f EV\n", exposure->currentEv.load());
            capture("still.ppm");
            break;
        }
        case 121:
        {
            // KROM_CAPTURE_YAW: rotation step in degrees (default 9).
            const char* yaw = std::getenv("KROM_CAPTURE_YAW");
            state.editorCamera.yawDeg += yaw ? static_cast<float>(std::atof(yaw)) : 9.f;
            break;
        }
        case 122: capture("rotation-motion.ppm"); break;
        case 128:
            capture("rotation-still.ppm");
            state.editorCamera = gtaoOrbitCamera;
            break;
        case 134: state.editorCamera.position.x += 0.35f; break;
        case 135: capture("translation-motion.ppm"); break;
        case 141:
            capture("translation-still.ppm");
            state.editorCamera = gtaoOrbitCamera;
            break;
        case 147:
            if (auto* transform = app.m_world->Get<TransformComponent>(gtaoBody))
            {
                transform->localPosition = gtaoOriginalPosition + math::Vec3{0.35f, 0.f, 0.f};
                transform->dirty = true;
            }
            break;
        case 148: capture("object-motion.ppm"); break;
        case 154:
            capture("object-still.ppm");
            if (!std::getenv("KROM_GTAO_ORBIT"))
            {
                context.renderLoop.GetWindow()->RequestClose();
                break;
            }
            if (auto* transform = app.m_world->Get<TransformComponent>(gtaoBody))
            {
                transform->localPosition = gtaoOriginalPosition;
                transform->dirty = true;
            }
            state.editorDebugFlags = 0u;
            break;
        case 160: capture("orbit-start.ppm"); break;
        case 161:
        {
            const auto rotation = math::Mat4::Rotation(math::Quat::FromAxisAngleDeg({0.f, 1.f, 0.f}, 35.f));
            state.editorCamera.position = gtaoFocus +
                rotation.TransformDirection(gtaoOrbitCamera.position - gtaoFocus);
            state.editorCamera.yawDeg = gtaoOrbitCamera.yawDeg + 35.f;
            break;
        }
        case 162: capture("orbit-motion.ppm"); break;
        case 168:
            capture("orbit-still.ppm");
            context.renderLoop.GetWindow()->RequestClose();
            break;
        default: break;
        }
        return true;
    }

    bool UpdatePerf(AppSceneContext& context)
    {
        constexpr int kWarmup = 60;
        constexpr int kMeasured = 300;
        // KROM_PERF_SELECT=<Name>: Entity auswaehlen (Inspector-Kosten mitmessen).
        // GPU pass timing runs while the profiler shows its GPU section (bit 1 << 2,
        // kSectionGpu in EditorProfilerWindow.cpp); the window costs the same in both states.
        if (std::getenv("KROM_PERF_GPU_PASSES"))
        {
            app.m_editorState.showProfiler = true;
            app.m_editorState.profilerSections |= 1u << 2;
        }
        if (frames == 30)
            if (const char* wanted = std::getenv("KROM_PERF_SELECT"))
                app.m_world->ForEachAlive([&](EntityID e) {
                    if (const auto* name = app.m_world->Get<NameComponent>(e); name && name->name == wanted)
                        app.m_editorFrameCtx->state.selectedEntity = e;
                });
        const auto now = std::chrono::steady_clock::now();
        if (perfFirstFrameMs < 0.0)
            perfFirstFrameMs = std::chrono::duration<double, std::milli>(now - perfProcessStart).count();
        if (frames == kWarmup)
        {
            perfWarmupEndMs = std::chrono::duration<double, std::milli>(now - perfProcessStart).count();
            perfStart = now;
            perfDescriptorUpdates = 0u;
            perfDescriptorBinds = 0u;
        }
        else if (frames > kWarmup)
        {
            perfFrameMs.push_back(std::chrono::duration<double, std::milli>(now - perfLast).count());
            const auto& stats = context.renderLoop.GetRenderSystem().GetStats();
            perfDescriptorUpdates += stats.backendDescriptorSetUpdates;
            perfDescriptorBinds += stats.backendDescriptorSetBinds;
        }
        perfLast = now;
        if (frames == kWarmup + kMeasured)
        {
            const double ms = std::chrono::duration<double, std::milli>(now - perfStart).count() / kMeasured;
            double maxMs = 0.0;
            int spikes = 0;
            for (double frameMs : perfFrameMs)
            {
                maxMs = std::max(maxMs, frameMs);
                if (frameMs > ms * 2.0)
                    ++spikes;
            }
            std::printf("PERF frame=%.2f ms (%.1f FPS) max=%.1f ms spikes(>2x)=%d scene=%s gtao=%s passes=%u"
                        " descriptorUpdates/frame=%.1f descriptorBinds/frame=%.1f"
                        " firstFrame=%.0f ms warmupEnd=%.0f ms\n",
                ms, 1000.0 / ms, maxMs, spikes, app.m_currentSceneName.c_str(),
                app.m_gtaoResources && app.m_gtaoResources->settings.enabled ? "on" : "off",
                context.renderLoop.GetRenderSystem().GetStats().graphPassCount,
                static_cast<double>(perfDescriptorUpdates) / kMeasured,
                static_cast<double>(perfDescriptorBinds) / kMeasured,
                perfFirstFrameMs, perfWarmupEndMs);
            if (std::getenv("KROM_PERF_GPU_PASSES"))
            {
                // Smoothed GPU time per pass (KROM_PERF_GPU_PASSES=1).
                const auto report = context.renderLoop.GetRenderSystem().GetGpuPassTimings();
                std::printf("PERF gpu total=%.3f ms", report.totalMs);
                for (const auto& pass : report.passes)
                    std::printf(" %s=%.3f", pass.name.c_str(), pass.ms);
                std::printf("%s\n", report.supported ? "" : " (no GPU timestamps)");
            }
            std::fflush(stdout);
            if (std::getenv("KROM_PERF_COMPARE_GTAO") && app.m_gtaoResources && !perfComparedGtao)
            {
                // The editor hands its state to the add-on every frame.
                app.m_editorState.gtaoSettings.enabled = false;
                perfComparedGtao = true;
                sceneShadersReady = false;
                readyFrames = 0;
                shaderWaitStart = {};
                perfFrameMs.clear();
            }
            else
            {
                context.renderLoop.GetWindow()->RequestClose();
            }
        }
        return true;
    }

    bool UpdateSceneClose(AppSceneContext& context)
    {
        namespace ed = renderer::addons::editor;
        auto& ui = *app.m_editorFrameCtx;
        constexpr const char* name = "Scene Close Override Probe";
        if (frames == 1)
        {
            const EntityID entity = ed::CreateEmptyEntity(ui, name);
            constexpr const char* guid = "scene-close-override-probe";
            ui.world.Add<GuidComponent>(entity, GuidComponent{guid});
            auto& link = ui.world.Add<PrefabInstanceComponent>(entity);
            link.bindings.push_back({"root", guid, {"/localScale", "/localPosition"}});
            const bool started = app.BeginEditorPlay();
            Check(started, "Play starts with the prefab overrides saved");
            if (!started)
                context.renderLoop.GetWindow()->RequestClose();
        }
        if (frames == 4)
        {
            app.StopEditorPlay();
            Check(!ed::detail::EditorCloseNeedsSaveQuestion(ui), "stopping Play leaves the scene clean");
            auto* link = ui.world.Get<PrefabInstanceComponent>(FindPlayUiEntity(name));
            const bool restored = link && link->bindings.size() == 1u &&
                link->bindings.front().overriddenProperties.size() == 2u;
            Check(restored, "Play restores the prefab overrides");
            if (restored)
            {
                auto& overrides = link->bindings.front().overriddenProperties;
                std::reverse(overrides.begin(), overrides.end());
                Check(!ed::detail::EditorCloseNeedsSaveQuestion(ui), "reordered overrides do not prompt to save");
                overrides.push_back("/localRotation");
                Check(ed::detail::EditorCloseNeedsSaveQuestion(ui), "an added override prompts to save");
            }
            context.renderLoop.GetWindow()->RequestClose();
        }
        return true;
    }

    bool Update(AppSceneContext& context, float) override
    {
        if (sdkFlow) return UpdateSdkFlow(context);
        ++frames; // AppScene-Update ruht waehrend des Lade-Jobs.
        if (!autoExposureCaptures.empty())
            return UpdateAutoExposure(context);
        if (!materialIblCaptures.empty())
            return UpdateMaterialIbl(context);
        if (!materialPreviewCaptures.empty())
            return UpdateMaterialPreview(context);
        if (sceneClose)
            return UpdateSceneClose(context);
        if (startupProject)
        {
            if (frames == 3)
            {
                const auto* launcher = ImGui::FindWindowByName("##KromStartScreen");
                Check(app.m_projectRoot.empty() && launcher && launcher->Active, "startup opens on launcher");
                launcherFrameBeforeRequest = ImGui::GetFrameCount();
                auto& state = app.m_editorFrameCtx->state;
                const auto projectsDir = PathToUtf8(PathFromUtf8(startupProjectFile).parent_path().parent_path());
                std::snprintf(state.projectParentDirBuffer.data(), state.projectParentDirBuffer.size(),
                              "%s", projectsDir.c_str());
                state.pendingStartProjectFile = startupProjectFile;
            }
            if (frames == 4)
            {
                const auto* launcher = ImGui::FindWindowByName("##KromStartScreen");
                Check(launcher && launcher->LastFrameActive == ImGui::GetFrameCount() &&
                      ImGui::GetFrameCount() > launcherFrameBeforeRequest, "launcher frame rendered before project load");
                const auto* drawData = ImGui::GetDrawData();
                Check(launcher && drawData && std::any_of(drawData->CmdLists.begin(), drawData->CmdLists.end(),
                    [](const ImDrawList* list) {
                        return list->_OwnerName &&
                            std::string_view(list->_OwnerName).find("##StartProjectList") != std::string_view::npos &&
                            std::string_view(list->_OwnerName).find("##StartProjectLoadBar") != std::string_view::npos &&
                            list->VtxBuffer.Size > 0;
                    }), "project load bar rendered under the selected project");
                Check(ImGui::GetFrameCount() - launcherFrameBeforeRequest >= 5,
                      "launcher kept drawing during project load");
                Check(!app.m_projectRoot.empty() && app.m_editorFrameCtx->state.pendingStartProjectFile.empty(),
                      "queued project opens after launcher frame");
                context.renderLoop.GetWindow()->RequestClose();
            }
            return true;
        }
        if (consoleUi)
        {
            auto& editor = *app.m_editorFrameCtx;
            auto& io = ImGui::GetIO();
            static std::string clipboard;
            if (frames == 1)
            {
                ImGui::GetPlatformIO().Platform_SetClipboardTextFn = [](ImGuiContext*, const char* text) {
                    clipboard = text;
                };
                editor.assetsPanelVisible = false;
                editor.consolePanelVisible = true;
                editor.state.consoleLogs = {{LogLevel::Info, "first console line"},
                                            {LogLevel::Warning, "second console line"}};
            }
            if (frames <= 3)
                editor.state.consoleFocusRequest = true;
            if (frames == 4)
            {
                const auto title = renderer::addons::editor::TranslateLabel("assets.console", "Console##editor");
                const auto* panel = ImGui::FindWindowByName(title.c_str());
                const auto* textWindow = panel && !panel->DC.ChildWindows.empty() ? panel->DC.ChildWindows.back() : nullptr;
                Check(textWindow && textWindow->Active, "inline console text visible");
                if (!textWindow || !textWindow->Active)
                {
                    context.renderLoop.GetWindow()->RequestClose();
                    return true;
                }
                io.AddMousePosEvent(textWindow->InnerRect.Min.x + 15.f, textWindow->InnerRect.Min.y + 9.f);
            }
            if (frames == 5)
                io.AddMouseButtonEvent(ImGuiMouseButton_Left, true);
            if (frames == 6)
                io.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
            if (frames == 7)
            {
                io.AddKeyEvent(ImGuiMod_Ctrl, true);
                io.AddKeyEvent(ImGuiKey_A, true);
            }
            if (frames == 8)
            {
                io.AddKeyEvent(ImGuiKey_A, false);
                io.AddKeyEvent(ImGuiKey_C, true);
            }
            if (frames == 9)
            {
                io.AddKeyEvent(ImGuiKey_C, false);
                io.AddKeyEvent(ImGuiMod_Ctrl, false);
                Check(clipboard.find("first console line\nsecond console line") != std::string::npos,
                      "console copies a multiline selection with Ctrl+C");
                context.renderLoop.GetWindow()->RequestClose();
            }
            return true;
        }
        if (settingsUi)
        {
            auto& editor = *app.m_editorFrameCtx;
            if (frames == 1)
            {
                editor.state.cameraSettingsOpen = true;
                Debug::Log("console probe %d", 42);
                std::thread worker([] { Debug::LogWarning("worker console probe"); });
                worker.join();
                const auto& preferences = renderer::addons::editor::GetEditorPreferences();
                Check(ImGui::GetStyle().FontScaleMain == preferences.uiScale, "saved UI scale applied");
                if (preferences.backupMinutes > 0)
                    app.m_sceneBackupElapsed = preferences.backupMinutes * 60.f;
            }
            if (frames == 3)
            {
                const auto title = std::string(Translate("menu.editor_settings")) + "###menu.editor_settings";
                const auto* window = ImGui::FindWindowByName(title.c_str());
                Check(window && window->Active && !window->Hidden, "editor settings rendered");
                const auto hasLog = [&](LogLevel level, std::string_view message) {
                    return std::any_of(editor.state.consoleLogs.begin(), editor.state.consoleLogs.end(),
                        [&](const auto& log) { return log.level == level && log.message == message; });
                };
                Check(hasLog(LogLevel::Info, "console probe 42"), "Debug::Log reached editor console");
                Check(hasLog(LogLevel::Warning, "worker console probe"), "worker log reached editor console");
                Debug::Log("queued before Play");
                editor.sceneLoadActive = true;
                const bool started = editor.startPlay();
                editor.sceneLoadActive = false;
                Check(!started && editor.state.consoleLogs.empty(), "Play button clears displayed logs");
                Debug::Log("logged after Play button");
                editor.state.cameraSettingsOpen = false;
                editor.state.projectSettingsScenePage = true;
                editor.state.settingsWindowOpen = true;
            }
            if (frames == 5)
            {
                const auto title = std::string(Translate("menu.project_settings")) + " · LoadingTest###ProjectSettings";
                const auto* window = ImGui::FindWindowByName(title.c_str());
                Check(window && window->Active && !window->Hidden, "project settings rendered");
                const auto hasLog = [&](std::string_view message) {
                    return std::any_of(editor.state.consoleLogs.begin(), editor.state.consoleLogs.end(),
                        [&](const auto& log) { return log.message == message; });
                };
                Check(!hasLog("queued before Play") && hasLog("logged after Play button"),
                    "Play discards queued old logs and keeps new logs");
                auto settings = editor.projectSettings;
                settings.width = 1440;
                settings.vsync = true;
                Check(editor.saveProjectSettings(settings), "project settings saved");
                std::ifstream in(app.GetCurrentProjectFilePath(), std::ios::binary);
                const std::string json((std::istreambuf_iterator<char>(in)), {});
                std::string error;
                const auto persisted = ReadProjectSettings(serialization::JsonParser::Parse(json, error));
                Check(error.empty() && persisted.width == 1440 && persisted.vsync, "saved project settings readable");

                in.close();
                const auto readText = [](const std::filesystem::path& path) {
                    std::ifstream file(path, std::ios::binary);
                    return std::string((std::istreambuf_iterator<char>(file)), {});
                };
                const auto scenePath = app.GetCurrentEditorScenePath();
                const auto projectPath = app.GetCurrentProjectFilePath();
                const auto originalScene = readText(scenePath);
                renderer::addons::editor::CreateEmptyEntity(editor, "separate scene save probe");
                Check(editor.saveProject() && !originalScene.empty() && readText(scenePath) == originalScene,
                      "project save leaves scene file alone");
                const auto originalProject = readText(projectPath);
                Check(editor.saveScene() && readText(scenePath).find("separate scene save probe") != std::string::npos &&
                      !originalProject.empty() && readText(projectPath) == originalProject,
                      "scene save writes scene independently");
                if (renderer::addons::editor::GetEditorPreferences().backupMinutes > 0)
                    Check(std::filesystem::is_regular_file(app.m_projectRoot / "editor/backups/One.json"), "automatic scene backup written");
                context.renderLoop.GetWindow()->RequestClose();
            }
            return true;
        }
        if (materialUi) return UpdateMaterialUi(context);
        if (animationUi) return UpdateAnimationUi(context);
        if (playUi) return UpdatePlayUi(context);
        if (!reloadProject.empty()) return UpdateScriptReload(context);
        if (compileStatus)
            return UpdateShaderCompileStatus(context);
        if (!perfProject.empty())
        {
            const bool verifyScene = !gtaoCaptureDir.empty() || std::getenv("KROM_PERF_COMPARE_GTAO");
            if (verifyScene && WaitForSceneShaders(context))
                return true;
            return gtaoCaptureDir.empty() ? UpdatePerf(context) : UpdateGtaoMotion(context);
        }
        if (dockDrag)
            return UpdateDockDrag(context);
        if (detachedWindow)
            return UpdateDetachedWindow(context);
        if (frames == 1 || frames == 5)
        {
            renderer::addons::editor::CreateCameraEntity(*app.m_editorFrameCtx);
            app.SyncEditorSceneState();
            // Inspector-Geometrie ohne Kamera-/Outline-Vorschau pruefen.
            const EntityID probe = renderer::addons::editor::CreateEmptyEntity(
                *app.m_editorFrameCtx, "InspectorProbe");
            app.m_world->Add<MeshComponent>(probe);
            app.m_world->Add<MaterialComponent>(probe);
            app.m_editorFrameCtx->state.selectedEntity = probe;
            app.m_editorFrameCtx->state.cameraPreviewWindowOpen = false;
        }
        if (frames == 3 || frames == 7) Check(Switch("Two"), "request Two");
        if (frames == 5 || frames == 9) Check(Switch("One"), "request One");
        if (frames == 4 || frames == 8)
            Check(app.m_currentSceneName == "Two", "Two loaded after normal progress frames");
        if (frames == 6 || frames == 10)
            Check(app.m_currentSceneName == "One", "One loaded after normal progress frames");
        if (frames == 11 && !directSwitch)
            Check(app.m_editorFrameCtx->switchEditorScene("Two", true), "request additive scene");
        if (frames == 12 && !directSwitch)
            Check(app.m_currentSceneName == "One" && app.m_loadedSceneNames.size() == 2,
                "additive load preserves primary scene");
        if (frames == 13 && !directSwitch) Check(Switch("Broken"), "request malformed scene");
        if (frames == 14)
        {
            if (!directSwitch)
            {
                Check(app.m_currentSceneName == "One", "parse failure preserves scene");
                Check(app.m_editorFrameCtx->lastFileMessage.starts_with(Translate("scene.switch_failed")),
                    "parse failure is reported");
                Check(phases.count(Translate("scene.phase.meshes")) &&
                      phases.count(Translate("scene.phase.materials")) &&
                      phases.count(Translate("scene.phase.ready")), "phases rendered in normal loop");
                Check(progressDrawn, "ImGui progress has visible geometry inside viewport");

            }
            context.renderLoop.GetWindow()->RequestClose();
        }
        return true;
    }
    int Run()
    {
        std::vector<std::pair<std::filesystem::path, std::string>> previousSelections;
        std::vector<std::filesystem::path> buildDirs = {PathFromUtf8(KROM_BUILD_DIR)};
        if (externalProjectExport) buildDirs.push_back(PathFromUtf8(KROM_BUILD_DIR).parent_path() / "runtime-Release");
        for (const auto& directory : buildDirs)
        {
            std::ifstream previousSelection(directory / "krom-project-root.txt", std::ios::binary);
            previousSelections.emplace_back(directory, std::string((std::istreambuf_iterator<char>(previousSelection)), {}));
        }
        const auto id = std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
        // Test either automatic in-tree discovery or an external Unicode/space path.
        const auto root = externalScriptProject
            ? std::filesystem::temp_directory_path() / PathFromUtf8("krom-editor-Größe " + id)
            : reloadProject.empty() ? std::filesystem::temp_directory_path() / ("krom-editor-loading-" + id)
            : PathFromUtf8(KROM_SOURCE_DIR) / "projects" / ("_reload_" + id);
        std::filesystem::create_directories(root / "Assets/Scenes");
        if (playUi)
        {
            std::filesystem::create_directories(root / "Assets/Prefabs");
            particlePreviewPrefab = root / "Assets/Prefabs/ParticlePreview.prefab";
            std::ofstream(particlePreviewPrefab) << R"({"type":"KromPrefab","version":2,"rootIndex":0,"records":[
                {"name":"Thumbnail Emitter","parentIndex":-1,"particleEmitter":{"playing":true,"burstCount":10}}
            ]})";
        }
        if (!reloadProject.empty())
        {
            reloadRoot = root;
            auto source = PathFromUtf8(reloadProject);
            if (source.filename() == "krom-project.json") source = source.parent_path();
            if (std::filesystem::exists(source / "Assets/Scripts"))
                std::filesystem::copy(source / "Assets/Scripts", root / "Assets/Scripts", std::filesystem::copy_options::recursive);
            std::filesystem::create_directories(root / "Assets/Scripts");
            std::ofstream(root / "Assets/Scripts/CodexReloadProbe.hpp") <<
                "#pragma once\n#include \"krom.h\"\nclass CodexReloadProbe final : public engine::script::ComponentScript {\n"
                "public: float amount = 1.f; void OnUpdate(engine::script::IScriptContext&) override; };\n"
                "// KROM_SCRIPT(CodexReloadProbe)\n// KROM_FIELD(CodexReloadProbe, amount, Float)\n";
            WriteReloadProbe(11);
            reloadStarted = std::chrono::steady_clock::now();
        }
        {
            std::ofstream project(root / "krom-project.json");
            project << (reloadProject.empty()
                ? R"({"name":"LoadingTest","runtimeTarget":"LoadingTest","backend":"vulkan","editorScene":"One","sceneOrder":["One","Two"]})"
                : "{\"name\":\"ScriptReloadTest\",\"runtimeTarget\":\"Reload_" + id +
                  "\",\"backend\":\"vulkan\",\"editorScene\":\"One\",\"sceneOrder\":[\"One\",\"Two\"]}");
            for (const char* name : {"One", "Two"})
            {
                std::ofstream scene(root / "Assets/Scenes" / (std::string(name) + ".json"));
                scene << "{\"scene\":\"" << name << "\",\"entities\":[]}";
            }
            std::ofstream(root / "Assets/Scenes/Broken.json") << "{broken";
        }
        KromEditorAppConfig config{};
        config.backend = SelectAppBackend();
        config.visible = visualProgress || std::getenv("KROM_PERF_VISIBLE") != nullptr;
        config.windowTitle = "KROM ImGui Ladeanzeige - Sichttest";
        // --detached-window: large enough that the material editor first fits inside.
        config.width = perfProject.empty() && !detachedWindow && materialPreviewCaptures.empty() ? 800 : 1920;
        config.height = perfProject.empty() && !detachedWindow && materialPreviewCaptures.empty() ? 600 : 1080;
        config.enableDebugLayer = perfProject.empty() || std::getenv("KROM_VALIDATION") != nullptr;
        config.enableForwardPlus = true;
        config.enableGtao = std::getenv("KROM_PERF_GTAO_OFF") == nullptr;
        config.enableContactShadows = true;
        if (settingsUi) Debug::MinLevel = LogLevel::Error;
        Check(app.Initialize(config), "editor initialized");
        if (failures == 0)
        {
            ImGui::GetIO().IniFilename = nullptr;
            if (playUi) SeedSavedGameLayout();
            if (const char* ini = std::getenv("KROM_TEST_IMGUI_INI"))
                ImGui::LoadIniSettingsFromDisk(ini);
            app.m_renderLoop.SetPreRenderCallback([this](float) {
                auto& ctx = *app.m_editorFrameCtx;
                if (toolbarLogo && !ctx.sceneLoadActive && frames >= 2)
                {
                    const auto* toolbar = ImGui::FindWindowByName("##editorToolbar");
                    const auto* logo = ctx.state.assetBrowser ? &ctx.state.assetBrowser->icons.editorLogo : nullptr;
                    Check(logo && logo->imguiId, "toolbar logo uploaded");
                    Check(toolbar && toolbar->Active && logo && logo->imguiId &&
                        std::any_of(toolbar->DrawList->CmdBuffer.begin(), toolbar->DrawList->CmdBuffer.end(),
                            [&](const ImDrawCmd& cmd) {
                                return cmd.ElemCount == 6 && cmd.GetTexID() == reinterpret_cast<ImTextureID>(logo->imguiId);
                            }), "toolbar draws the logo texture");
                    app.m_renderLoop.GetWindow()->RequestClose();
                    return;
                }
                if (ctx.scriptBuildActive)
                {
                    const auto* modal = ImGui::FindWindowByName("Scripts bauen##scriptBuild");
                    if (modal && modal->Active && !modal->Hidden && modal->DrawList->VtxBuffer.Size > 0 &&
                        ImGui::GetCurrentContext()->OpenPopupStack.Size > 0)
                    {
                        reloadModalDrawn = true;
                        reloadModalMinWidth = (std::min)(reloadModalMinWidth, modal->Size.x);
                    }
                }
                if (ctx.sceneLoadActive) phases.insert(ctx.sceneLoadPhase);
                if (ctx.sceneLoadActive)
                {
                    const auto* window = ImGui::FindWindowByName("##sceneLoadStatus");
                    const auto* viewport = ImGui::GetMainViewport();
                    const auto* drawData = ImGui::GetDrawData();
                    if (window && window->Active && !window->Hidden && window->DrawList->VtxBuffer.Size > 0 &&
                        drawData && drawData->Valid && drawData->CmdListsCount > 0 &&
                        window->Pos.x >= viewport->Pos.x && window->Pos.y >= viewport->Pos.y &&
                        window->Pos.x + window->Size.x <= viewport->Pos.x + viewport->Size.x &&
                        window->Pos.y + window->Size.y <= viewport->Pos.y + viewport->Size.y)
                        progressDrawn = true;

                }
                // Nur der Sichttest pausiert einen Job. Keine Wartezeit im Editor.
                if (visualProgress && !pausedOnce && ctx.sceneLoadPhase == Translate("scene.phase.meshes"))
                {
                    pausedOnce = true;
                    pausedJob = std::move(app.m_sceneLoadJob);
                    resumeAt = std::chrono::steady_clock::now() + std::chrono::seconds(40);
                }
                if (pausedJob && std::chrono::steady_clock::now() >= resumeAt)
                    app.m_sceneLoadJob = std::move(pausedJob);
            });
            const char* projectOverride = std::getenv("KROM_TEST_PROJECT");
            const std::string projectFile = !perfProject.empty() ? perfProject
                : projectOverride ? std::string(projectOverride) : PathToUtf8(root / "krom-project.json");
            if (startupProject)
                startupProjectFile = projectFile;
            else
                Check(app.LoadProjectFile(projectFile), "project loaded");
            if (externalProjectExport)
            {
                app.m_projectBackend = SelectAppBackend();
                Check(app.SaveProjectFile(false), "save external project with test renderer");
            }
            if (failures == 0) Check(app.Run(*this) == 0, "editor loop completed without crash");
        }
        app.Shutdown();
        bool selectionRestored = true;
#ifdef _WIN32
        if (!reloadProject.empty())
        {
            const auto tools = build::GetBuildTools();
            std::string output;
            for (const auto& [directory, previousRoot] : previousSelections)
            {
                if (!std::filesystem::exists(directory / "CMakeCache.txt")) continue;
                const bool restored = tools.Succeeded() && build::CaptureProcessOutput(
                    {tools.tools.cmake, {"-S", KROM_SOURCE_DIR, "-B", PathToUtf8(directory),
                        "-DKROM_PROJECT_ROOT=" + previousRoot}, PathFromUtf8(KROM_SOURCE_DIR), tools.tools.environment}, output).Succeeded();
                selectionRestored = selectionRestored && restored;
                if (!restored) std::printf("%s\n", output.c_str());
            }
            Check(selectionRestored, "restore original CMake project selection before removing test project");
        }
#endif
        std::error_code ec;
        if (selectionRestored)
            std::filesystem::remove_all(root, ec); // nur eigenes, eindeutig benanntes Testprojekt
        return failures == 0 ? 0 : 1;
    }

    int TestSceneRename()
    {
        const auto id = std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
        const auto root = std::filesystem::temp_directory_path() / ("krom-scene-rename-" + id);
        const auto scenes = root / "Assets/Scenes";
        const auto project = root / "krom-project.json";
        std::filesystem::create_directories(scenes);
        app.m_projectRoot = root;
        app.m_currentSceneName = "Testraum";
        app.m_sceneOrder = {"Testraum", "Ziel", "Unbuilt"};
        RegisterCoreComponents(app.m_componentRegistry);
        ecs::RegisterComponent<script::ScriptList>(app.m_componentRegistry, "ScriptList");
        app.m_world = std::make_unique<ecs::World>(app.m_componentRegistry);
        app.m_editorFrameCtx = std::make_unique<renderer::addons::editor::EditorFrameContext>(
            *app.m_world, app.m_materialSystem, app.m_assetRegistry, app.m_editorState);
        const auto entity = app.m_world->CreateEntity();
        app.m_world->Add<NameComponent>(entity, NameComponent{"Play restart"});
        auto& list = app.m_world->Add<script::ScriptList>(entity);
        list.AddNameOnly("GameControls");
        auto& field = list.Instances_Mutable().front().fieldValues["restartScene"];
        field.type = script::ScriptFieldType::String;
        field.stringValue = "Scenes/Testraum.kscene";
        Check(app.SaveEditorScene(), "save current scene before rename");

        const std::string goal = R"({"entities":[{"id":0,"components":[{"type":"ScriptList","scripts":[{"class":"GameControls","fields":{"restartScene":"Scenes\\Testraum.kscene","other":"Scenes/Testraum extra.kscene"}}]},{"type":"FutureComponent","value":"Scenes/Testraum.kscene"}]}]})";
        const auto readText = [](const std::filesystem::path& path) {
            std::ifstream input(path, std::ios::binary);
            return std::string((std::istreambuf_iterator<char>(input)), {});
        };
        std::ofstream(scenes / "Ziel.json") << goal;
        std::ofstream(scenes / "Unbuilt.json") << goal;
        std::ofstream(scenes / "Ziel.kscene") << "old Ziel export";
        std::ofstream(scenes / "Testraum.kscene") << "old Testraum export";
        std::ofstream(scenes / "Room 2.kscene") << "occupied";
        Check(!app.RenameEditorScene("Testraum", "Room 2") && readText(scenes / "Room 2.kscene") == "occupied",
              "reject an occupied runtime destination");
        std::filesystem::remove(scenes / "Room 2.kscene");

        std::filesystem::create_directory(project);
        Check(!app.RenameEditorScene("Testraum", "Room 2"), "reject rename when project file cannot be saved");
        Check(std::filesystem::exists(scenes / "Testraum.json") && !std::filesystem::exists(scenes / "Room 2.json") &&
              readText(scenes / "Testraum.kscene") == "old Testraum export" &&
              readText(scenes / "Ziel.kscene") == "old Ziel export" && readText(scenes / "Ziel.json") == goal &&
              app.m_currentSceneName == "Testraum" && field.stringValue == "Scenes/Testraum.kscene",
              "restore editor files, exports and in-memory references on failure");
        std::filesystem::remove(project);

        Check(app.RenameEditorScene("Testraum", "Room 2"), "rename built scene with references");
        auto expectedGoal = goal;
        const std::string oldReference = R"(Scenes\\Testraum.kscene)";
        expectedGoal.replace(expectedGoal.find(oldReference), oldReference.size(), "Scenes/Room 2.kscene");
        Check(readText(scenes / "Ziel.json") == expectedGoal && readText(scenes / "Unbuilt.json") == expectedGoal,
              "rewrite only exact script path values and preserve the rest of the source");
        Check(readText(scenes / "Room 2.kscene").find("\"scene\": \"Room 2\"") != std::string::npos &&
              readText(scenes / "Ziel.kscene").find("Scenes/Room 2.kscene") != std::string::npos &&
              !std::filesystem::exists(scenes / "Testraum.kscene") && !std::filesystem::exists(scenes / "Testraum.json") &&
              !std::filesystem::exists(scenes / "Unbuilt.kscene"),
              "regenerate affected exports and remove old paths");
        Check(app.m_currentSceneName == "Room 2" && app.m_sceneOrder.front() == "Room 2" &&
              field.stringValue == "Scenes/Room 2.kscene" && app.SaveEditorScene() &&
              readText(scenes / "Room 2.json").find("Scenes/Room 2.kscene") != std::string::npos,
              "later scene saves retain the updated reference");

        using namespace renderer::addons::editor;
        auto& editor = *app.m_editorFrameCtx;
        const std::string renamedGoal = readText(scenes / "Ziel.json");
        Check(GetUndoEditorHistoryLabel(app.m_editorState) == Translate("history.rename_scene") &&
              UndoEditorHistory(editor) &&
              std::filesystem::exists(scenes / "Testraum.json") && !std::filesystem::exists(scenes / "Room 2.json") &&
              app.m_currentSceneName == "Testraum" && app.m_sceneOrder.front() == "Testraum" &&
              field.stringValue == "Scenes/Testraum.kscene" &&
              readText(scenes / "Ziel.json").find("Scenes/Testraum.kscene") != std::string::npos,
              "undo scene rename restores files, project order and live references");
        Check(RedoEditorHistory(editor) && std::filesystem::exists(scenes / "Room 2.json") &&
              app.m_currentSceneName == "Room 2" && field.stringValue == "Scenes/Room 2.kscene" &&
              readText(scenes / "Ziel.json") == renamedGoal,
              "redo scene rename");

        app.m_buildExcludedScenes.insert("Ziel");
        const std::string zielExport = readText(scenes / "Ziel.kscene");
        Check(app.DeleteEditorScene("Ziel") && !std::filesystem::exists(scenes / "Ziel.json") &&
              !std::filesystem::exists(scenes / "Ziel.kscene") &&
              std::find(app.m_sceneOrder.begin(), app.m_sceneOrder.end(), "Ziel") == app.m_sceneOrder.end() &&
              !app.m_buildExcludedScenes.count("Ziel"),
              "delete scene removes its files and project entries");
        Check(UndoEditorHistory(editor) && readText(scenes / "Ziel.json") == renamedGoal &&
              readText(scenes / "Ziel.kscene") == zielExport &&
              app.m_sceneOrder.size() > 1 && app.m_sceneOrder[1] == "Ziel" && app.m_buildExcludedScenes.count("Ziel") &&
              readText(project).find("Ziel") != std::string::npos,
              "undo scene delete restores files, order position and build exclusion");
        Check(RedoEditorHistory(editor) && !std::filesystem::exists(scenes / "Ziel.json") &&
              std::find(app.m_sceneOrder.begin(), app.m_sceneOrder.end(), "Ziel") == app.m_sceneOrder.end(),
              "redo scene delete");
        Check(UndoEditorHistory(editor) && UndoEditorHistory(editor) &&
              std::filesystem::exists(scenes / "Ziel.json") && app.m_currentSceneName == "Testraum",
              "undo delete and rename in sequence");
        Check(RedoEditorHistory(editor) && RedoEditorHistory(editor) &&
              app.m_currentSceneName == "Room 2" && !std::filesystem::exists(scenes / "Ziel.json"),
              "redo rename and delete in sequence");
        ClearEditorHistory(app.m_editorState);
        Check(!std::filesystem::exists(root / "editor" / ".history-trash"),
              "clearing the history removes the deleted scene for good");

        app.LoadSceneForPlay({"Scenes/Testraum.kscene", 0, false});
        Check(app.m_world->IsAlive(entity), "missing restart target preserves the running scene");

        app.m_editorFrameCtx.reset();
        app.m_world.reset();
        app.m_projectRoot.clear();
        std::error_code ec;
        std::filesystem::remove_all(root, ec); // Only this test's temporary project.
        return failures == 0 ? 0 : 1;
    }
};
}

int main(int argc, char** argv)
{
    engine::app::EditorLoadingSmokeTest test;
    if (argc > 1 && std::string_view(argv[1]) == "--sdk-project-flow")
        return test.TestSdkProjectFlow();
    if (argc > 1 && std::string_view(argv[1]) == "--sdk-build-guards")
        return test.TestSdkBuildGuards();
    if (argc > 1 && std::string_view(argv[1]) == "--sdk-invalid-manifest")
        return test.TestInvalidSdkManifest();
    if (argc > 1 && std::string_view(argv[1]) == "--ini-path")
        return test.TestIniPath();
    if (argc > 1 && std::string_view(argv[1]) == "--scene-rename")
        return test.TestSceneRename();
    test.directSwitch = argc > 1 && std::string_view(argv[1]) == "--without-progress";
    test.dockDrag = argc > 1 && std::string_view(argv[1]) == "--dock-drag";
    test.detachedWindow = argc > 1 && std::string_view(argv[1]) == "--detached-window";
    test.materialUi = argc > 1 && std::string_view(argv[1]) == "--material-ui";
    test.playUi = argc > 1 && std::string_view(argv[1]) == "--play-ui";
    test.animationUi = argc > 1 && std::string_view(argv[1]) == "--animation-ui";
    test.settingsUi = argc > 1 && std::string_view(argv[1]) == "--settings-ui";
    test.consoleUi = argc > 1 && std::string_view(argv[1]) == "--console-ui";
    test.startupProject = argc > 1 && std::string_view(argv[1]) == "--startup-project";
    test.sceneClose = argc > 1 && std::string_view(argv[1]) == "--scene-close";
    test.toolbarLogo = argc > 1 && std::string_view(argv[1]) == "--toolbar-logo";
    if (argc > 2 && std::string_view(argv[1]) == "--perf")
        test.perfProject = argv[2];
    if (argc > 3 && (std::string_view(argv[1]) == "--auto-exposure" || std::string_view(argv[1]) == "--gizmo-colors"))
    {
        test.perfProject = argv[2];
        test.gizmoColorProbe = std::string_view(argv[1]) == "--gizmo-colors";
        test.autoExposureCaptures = engine::PathFromUtf8(argv[3]);
        std::filesystem::create_directories(test.autoExposureCaptures);
    }
    if (argc > 3 && (std::string_view(argv[1]) == "--material-ibl" || std::string_view(argv[1]) == "--material-specular"))
    {
        test.perfProject = argv[2];
        test.materialIblCaptures = engine::PathFromUtf8(argv[3]);
        test.materialSpecularProbe = std::string_view(argv[1]) == "--material-specular";
        std::filesystem::create_directories(test.materialIblCaptures);
    }
    if (argc > 2 && std::string_view(argv[1]) == "--material-preview")
    {
        test.materialPreviewCaptures = engine::PathFromUtf8(argv[2]);
        std::filesystem::create_directories(test.materialPreviewCaptures);
    }
    if (argc > 3 && std::string_view(argv[1]) == "--gtao-motion")
    {
        test.perfProject = argv[2];
        test.gtaoCaptureDir = engine::PathFromUtf8(argv[3]);
        std::filesystem::create_directories(test.gtaoCaptureDir);
    }
    if (argc > 2 && std::string_view(argv[1]) == "--shader-compile-status")
    {
        test.perfProject = argv[2];
        test.compileStatus = true;
    }
    if (argc > 2 && (std::string_view(argv[1]) == "--script-reload" ||
                     std::string_view(argv[1]) == "--external-script-reload" ||
                     std::string_view(argv[1]) == "--external-project-export" ||
                     std::string_view(argv[1]) == "--external-export-only"))
    {
        test.reloadProject = argv[2];
        test.externalScriptProject = std::string_view(argv[1]) != "--script-reload";
        test.externalExportOnly = std::string_view(argv[1]) == "--external-export-only";
        test.externalProjectExport = std::string_view(argv[1]) == "--external-project-export" || test.externalExportOnly;
    }
    test.visualProgress = argc == 1 || std::string_view(argv[1]) == "--visual-progress";
    return test.Run();
}
