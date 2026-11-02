// Optionaler GPU-Test: --headless automatisiert, ohne Argumente sichtbare
// ImGui-Anzeige fuer 40 s. Kein plattformspezifischer UI-Code.
#include "app/KromEditorApp.hpp"
#include "addons/editor/EditorCommands.hpp"
#include "core/PathUtf8.hpp"
#include "imgui.h"
#include "imgui_internal.h"
#include <chrono>
#include <cstdio>
#include <fstream>
#include <set>

namespace engine::app {
struct EditorLoadingSmokeTest final : IAppScene
{
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

    void Check(bool success, const char* message)
    {
        std::printf("%s: %s\n", success ? "PASS" : "FAIL", message);
        std::fflush(stdout);
        if (!success) ++failures;
    }
    bool Switch(const char* name)
    {
        return directSwitch ? app.SwitchEditorScene(name, false)
                            : app.m_editorFrameCtx->switchEditorScene(name, false);
    }
    bool Build(AppSceneContext&) override { return true; }
    bool Update(AppSceneContext& context, float) override
    {
        ++frames; // AppScene-Update ruht waehrend des Lade-Jobs.
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
                Check(app.m_editorFrameCtx->lastFileMessage.find("fehlgeschlagen") != std::string::npos,
                    "parse failure is reported");
                Check(phases.count("Meshes laden") && phases.count("Materialien vorbereiten") &&
                      phases.count("Szene bereit"), "phases rendered in normal loop");
                Check(progressDrawn, "ImGui progress has visible geometry inside viewport");

            }
            context.renderLoop.GetWindow()->RequestClose();
        }
        return true;
    }
    int Run()
    {
        const auto root = std::filesystem::temp_directory_path() /
            ("krom-editor-loading-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        std::filesystem::create_directories(root / "Assets/Scenes");
        {
            std::ofstream project(root / "krom-project.json");
            project << R"({"name":"LoadingTest","runtimeTarget":"LoadingTest","backend":"vulkan","editorScene":"One","sceneOrder":["One","Two"]})";
            for (const char* name : {"One", "Two"})
            {
                std::ofstream scene(root / "Assets/Scenes" / (std::string(name) + ".json"));
                scene << "{\"scene\":\"" << name << "\",\"entities\":[]}";
            }
            std::ofstream(root / "Assets/Scenes/Broken.json") << "{broken";
        }
        KromEditorAppConfig config{};
        config.backend = renderer::DeviceFactory::BackendType::Vulkan;
        config.visible = visualProgress;
        config.windowTitle = "KROM ImGui Ladeanzeige - Sichttest";
        config.width = 800;
        config.height = 600;
        config.enableDebugLayer = true;
        config.enableForwardPlus = true;
        config.enableGtao = true;
        Check(app.Initialize(config), "Vulkan editor initialized");
        if (failures == 0)
        {
            ImGui::GetIO().IniFilename = nullptr;
            app.m_renderLoop.SetPreRenderCallback([this](float) {
                auto& ctx = *app.m_editorFrameCtx;
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
                if (visualProgress && !pausedOnce && ctx.sceneLoadPhase == "Meshes laden")
                {
                    pausedOnce = true;
                    pausedJob = std::move(app.m_sceneLoadJob);
                    resumeAt = std::chrono::steady_clock::now() + std::chrono::seconds(40);
                }
                if (pausedJob && std::chrono::steady_clock::now() >= resumeAt)
                    app.m_sceneLoadJob = std::move(pausedJob);
            });
            Check(app.LoadProjectFile(PathToUtf8(root / "krom-project.json")), "temporary project loaded");
            if (failures == 0) Check(app.Run(*this) == 0, "editor loop completed without crash");
        }
        app.Shutdown();
        std::error_code ec;
        std::filesystem::remove_all(root, ec); // nur eigenes, eindeutig benanntes Testprojekt
        return failures == 0 ? 0 : 1;
    }
};
}

int main(int argc, char** argv)
{
    engine::app::EditorLoadingSmokeTest test;
    test.directSwitch = argc > 1 && std::string_view(argv[1]) == "--without-progress";
    test.visualProgress = argc == 1 || std::string_view(argv[1]) == "--visual-progress";
    return test.Run();
}
