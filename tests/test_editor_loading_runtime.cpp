// Optionaler GPU-Test: --headless automatisiert, ohne Argumente sichtbare
// ImGui-Anzeige fuer 40 s. Kein plattformspezifischer UI-Code.
#include "app/KromEditorApp.hpp"
#include "addons/editor/EditorCommands.hpp"
#include "addons/editor/EditorLocalization.hpp"
#include "core/PathUtf8.hpp"
#include "addons/script/ScriptList.hpp"
#include "addons/editor/EditorScriptAssets.hpp"
#include "addons/particles/ParticleComponents.hpp"
#include "imgui.h"
#include "imgui_internal.h"
#include <chrono>
#include <thread>
#include <cstdio>
#include <fstream>
#include <algorithm>
#include <set>
#include <vector>

namespace engine::app {
using renderer::addons::editor::Translate;

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
    bool playUi = false;
    bool settingsUi = false;
    bool consoleUi = false;
    bool startupProject = false;
    int launcherFrameBeforeRequest = 0;
    std::string startupProjectFile;
    int pausedCounter = 0;
    ParticleID previousParticle{};
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
    EntityID FindCounter() const
    {
        EntityID result{};
        app.m_world->ForEachAlive([&](EntityID e) {
            if (const auto* name=app.m_world->Get<NameComponent>(e); name && name->name=="mask2") result=e;
        });
        return result;
    }
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
            Check(app.BeginEditorPlay(),"begin editor Play");
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
            Check(app.BeginEditorPlay(), "begin Play with Console selected");
        if (frames == 29)
        {
            const auto* list = app.m_world->Get<script::ScriptList>(FindCounter());
            const auto* counter = static_cast<const PlayCounter*>(list->Instances()[0].script.get());
            Check(app.m_game.Particles().IsParticleAlive(counter->particle) && counter->particle != previousParticle,
                "A new Play run cannot reuse an old particle handle");
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
        if (std::chrono::steady_clock::now() - reloadStarted > std::chrono::minutes(4))
        {
            Check(false, "script reload timed out");
            context.renderLoop.GetWindow()->RequestClose();
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
            std::ofstream(other / "krom-project.json") << R"({"name":"Other","runtimeTarget":"Other","sceneOrder":["One"]})";
            std::ofstream(other / "Assets/Scenes/One.json") << R"({"scene":"One","entities":[]})";
            Check(app.LoadProjectFile(PathToUtf8(other / "krom-project.json")), "switch to project without DLL");
            Check(!app.m_scriptModule.IsLoaded() && app.m_scriptRegistry.GetFactoryCount() == 0, "project switch unloads previous module");
            Check(app.LoadProjectFile(PathToUtf8(reloadRoot / "krom-project.json")), "reopen script project");
            Check(app.m_scriptModule.IsLoaded() && !app.m_runtimeBuildJob, "project open loads existing DLL without building");
            Check(ReloadProbe().IsValid() && app.m_world->Get<script::ScriptList>(ReloadProbe())->Instances()[0].script != nullptr,
                "project open restores live script objects");
            context.renderLoop.GetWindow()->RequestClose();
        }
        return true;
    }

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

    // --perf <krom-project.json>: echtes Projekt laden und mittlere Bildzeit messen.
    std::string perfProject;
    std::chrono::steady_clock::time_point perfStart;
    std::chrono::steady_clock::time_point perfLast;
    std::vector<double> perfFrameMs;
    bool UpdatePerf(AppSceneContext& context)
    {
        constexpr int kWarmup = 60;
        constexpr int kMeasured = 300;
        // KROM_PERF_SELECT=<Name>: Entity auswaehlen (Inspector-Kosten mitmessen).
        if (frames == 30)
            if (const char* wanted = std::getenv("KROM_PERF_SELECT"))
                app.m_world->ForEachAlive([&](EntityID e) {
                    if (const auto* name = app.m_world->Get<NameComponent>(e); name && name->name == wanted)
                        app.m_editorFrameCtx->state.selectedEntity = e;
                });
        const auto now = std::chrono::steady_clock::now();
        if (frames == kWarmup)
            perfStart = now;
        else if (frames > kWarmup)
            perfFrameMs.push_back(std::chrono::duration<double, std::milli>(now - perfLast).count());
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
            std::printf("PERF frame=%.2f ms (%.1f FPS) max=%.1f ms spikes(>2x)=%d scene=%s\n",
                ms, 1000.0 / ms, maxMs, spikes, app.m_currentSceneName.c_str());
            std::fflush(stdout);
            context.renderLoop.GetWindow()->RequestClose();
        }
        return true;
    }

    bool Update(AppSceneContext& context, float) override
    {
        ++frames; // AppScene-Update ruht waehrend des Lade-Jobs.
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
        if (playUi) return UpdatePlayUi(context);
        if (!reloadProject.empty()) return UpdateScriptReload(context);
        if (!perfProject.empty())
            return UpdatePerf(context);
        if (dockDrag)
            return UpdateDockDrag(context);
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
        const auto id = std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
        // Temporaere Projektkopie im projects-Glob: testet auch Ninjas automatische
        // Aufnahme neuer Projekte, ohne die externe Projektoption zu aendern.
        const auto root = reloadProject.empty() ? std::filesystem::temp_directory_path() / ("krom-editor-loading-" + id)
            : PathFromUtf8(KROM_SOURCE_DIR) / "projects" / ("_reload_" + id);
        std::filesystem::create_directories(root / "Assets/Scenes");
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
        config.width = perfProject.empty() ? 800 : 1920;
        config.height = perfProject.empty() ? 600 : 1080;
        config.enableDebugLayer = perfProject.empty();
        config.enableForwardPlus = true;
        config.enableGtao = true;
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
    test.dockDrag = argc > 1 && std::string_view(argv[1]) == "--dock-drag";
    test.playUi = argc > 1 && std::string_view(argv[1]) == "--play-ui";
    test.settingsUi = argc > 1 && std::string_view(argv[1]) == "--settings-ui";
    test.consoleUi = argc > 1 && std::string_view(argv[1]) == "--console-ui";
    test.startupProject = argc > 1 && std::string_view(argv[1]) == "--startup-project";
    if (argc > 2 && std::string_view(argv[1]) == "--perf")
        test.perfProject = argv[2];
    if (argc > 2 && std::string_view(argv[1]) == "--script-reload")
        test.reloadProject = argv[2];
    test.visualProgress = argc == 1 || std::string_view(argv[1]) == "--visual-progress";
    return test.Run();
}
