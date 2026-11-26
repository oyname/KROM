// =============================================================================
// KROM Engine - addons/editor/EditorUIProject.cpp
// Projekt- und Szenen-Verwaltung: Startbildschirm, Dialoge, Menueleiste,
// Projekt-Einstellungen, Szenen-Manager, Prefab speichern/spawnen.
// =============================================================================
#include "addons/editor/EditorUIInternal.hpp"
#include "addons/editor/EditorCard.hpp"
#include "addons/editor/EditorValueClipboard.hpp"
#include "core/PathUtf8.hpp"
#include "addons/editor/EditorProjectHistory.hpp"
#include "addons/editor/EditorLocalization.hpp"
#include "core/AtomicTextFile.hpp"
#include "renderer/RenderFrameConstants.hpp"

#include <algorithm>
#include <cstdio>
#include <functional>
#include <cfloat>
#include <cmath>
#include <iterator>
#include <unordered_set>

#ifdef KROM_EDITOR_HAS_IMGUI

namespace engine::renderer::addons::editor {

using namespace detail;

namespace {

static std::string LocalizedLabel(const char* key, const char* prefix = "", const char* suffix = "")
{
    return std::string(prefix) + Translate(key) + suffix + "###" + key;
}

static void DrawLanguageChoices(EditorFrameContext& ctx)
{
    const EditorLanguage current = GetEditorLanguage();
    const auto choose = [&](const char* name, EditorLanguage language) {
        if (ImGui::Selectable(name, current == language) && current != language)
        {
            if (SetEditorLanguage(language))
                ctx.lastFileMessage.clear();
            else
                ctx.lastFileMessage = Translate("language.save_failed");
        }
    };
    choose("Deutsch", EditorLanguage::German);
    choose("English", EditorLanguage::English);
}

static std::string SanitizeAssetStem(std::string name)
{
    for (char& c : name)
    {
        const unsigned char uc = static_cast<unsigned char>(c);
        if (!std::isalnum(uc) && c != '_' && c != '-')
            c = '_';
    }
    return name.empty() ? std::string("Prefab") : name;
}

static const char* EditorProjectBackendLabel(renderer::DeviceFactory::BackendType backend) noexcept
{
    switch (backend)
    {
    case renderer::DeviceFactory::BackendType::DirectX11: return "DirectX11";
    case renderer::DeviceFactory::BackendType::Vulkan: return "Vulkan";
    default: return "Unknown";
    }
}

static renderer::DeviceFactory::BackendType EditorProjectBackendFromSelection(int selection) noexcept
{
    switch (selection)
    {
    case 1: return renderer::DeviceFactory::BackendType::DirectX11;
    default: return renderer::DeviceFactory::BackendType::Vulkan;
    }
}

struct StartProjectEntry
{
    std::string name;
    std::string path;
    std::string projectFile;
    std::filesystem::file_time_type writeTime{};
    std::string date;
    std::string lastOpened;
};

static std::string ReadProjectDisplayName(const std::filesystem::path& projectFile,
                                          const std::string& fallback)
{
    std::ifstream in(projectFile, std::ios::binary);
    if (!in)
        return fallback;

    const std::string json((std::istreambuf_iterator<char>(in)),
                           std::istreambuf_iterator<char>());
    const std::string key = "\"name\"";
    const size_t keyPos = json.find(key);
    if (keyPos == std::string::npos)
        return fallback;
    const size_t colon = json.find(':', keyPos + key.size());
    if (colon == std::string::npos)
        return fallback;
    const size_t quote0 = json.find('"', colon + 1u);
    if (quote0 == std::string::npos)
        return fallback;
    const size_t quote1 = json.find('"', quote0 + 1u);
    if (quote1 == std::string::npos || quote1 <= quote0 + 1u)
        return fallback;
    return json.substr(quote0 + 1u, quote1 - quote0 - 1u);
}

static std::string FormatProjectTime(std::time_t timestamp, bool relative)
{
    if (timestamp <= 0)
        return relative ? TranslateWithValue("start.last_opened", Translate("common.unknown")) : "";
    std::tm local{};
#ifdef _WIN32
    if (localtime_s(&local, &timestamp) != 0) return "";
#else
    if (!localtime_r(&timestamp, &local)) return "";
#endif
    const bool german = GetEditorLanguage() == EditorLanguage::German;
    char text[80]{};
    if (!relative)
    {
        std::strftime(text, sizeof(text), german ? "%d.%m.%Y" : "%Y-%m-%d", &local);
        return text;
    }

    const std::time_t now = std::time(nullptr);
    std::tm today{};
#ifdef _WIN32
    localtime_s(&today, &now);
#else
    localtime_r(&now, &today);
#endif
    const bool sameDay = today.tm_year == local.tm_year && today.tm_yday == local.tm_yday;
    if (sameDay)
    {
        std::strftime(text, sizeof(text), "%H:%M", &local);
        return TranslateWithValue("start.last_opened", TranslateWithValue("start.today", text));
    }
    std::strftime(text, sizeof(text), german ? "%d.%m.%Y, %H:%M" : "%Y-%m-%d, %H:%M", &local);
    return TranslateWithValue("start.last_opened", text);
}

static std::vector<StartProjectEntry> ScanStartProjects(EditorFrameContext& ctx)
{
    std::vector<StartProjectEntry> projects;
    std::filesystem::path root = PathFromUtf8(ctx.state.projectParentDirBuffer.data());
    if (root.empty() && !ctx.currentProjectRoot.empty())
        root = PathFromUtf8(ctx.currentProjectRoot).parent_path();
    if (root.empty() || !std::filesystem::exists(root))
        return projects;

    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator(root, ec))
    {
        if (ec || !entry.is_directory())
            continue;
        const std::filesystem::path projectFile = entry.path() / "krom-project.json";
        if (!std::filesystem::exists(projectFile))
            continue;

        StartProjectEntry p{};
        p.name = ReadProjectDisplayName(projectFile, PathToUtf8(entry.path().filename()));
        p.path = PathToUtf8(entry.path());
        p.projectFile = PathToUtf8(projectFile);
        p.writeTime = std::filesystem::last_write_time(projectFile, ec);
        if (!ec)
            p.date = FormatProjectTime(std::chrono::system_clock::to_time_t(
                std::chrono::clock_cast<std::chrono::system_clock>(p.writeTime)), false);
        p.lastOpened = FormatProjectTime(ReadProjectLastOpened(entry.path()), true);
        projects.push_back(std::move(p));
    }

    std::sort(projects.begin(), projects.end(), [](const StartProjectEntry& a,
                                                   const StartProjectEntry& b) {
        if (a.writeTime != b.writeTime)
            return a.writeTime > b.writeTime;
        return a.name < b.name;
    });
    return projects;
}

static void* GetStartEditorTextureId(EditorFrameContext& ctx,
                                     const char* filename,
                                     std::string& cachedPath,
                                     TextureHandle& cachedTexture)
{
    if (!ctx.assetPipeline || !ctx.editorTextureId || ctx.engineEditorDir.empty())
        return nullptr;

    const std::string path = PathToUtf8(PathFromUtf8(ctx.engineEditorDir) / PathFromUtf8(filename));
    if (!cachedTexture.IsValid() || cachedPath != path)
    {
        cachedPath = path;
        const TextureHandle assetTexture = ctx.assetPipeline->LoadTexture(path);
        if (!assetTexture.IsValid())
            return nullptr;
        ctx.assetPipeline->UploadPendingGpuAssets();
        cachedTexture = ctx.assetPipeline->GetGpuTexture(assetTexture);
    }

    return cachedTexture.IsValid() ? ctx.editorTextureId(cachedTexture) : nullptr;
}

static void* GetStartPreviewTextureId(EditorFrameContext& ctx)
{
    static std::string s_previewPath;
    static TextureHandle s_previewTexture;
    return GetStartEditorTextureId(ctx, "logo.png", s_previewPath, s_previewTexture);
}

static void DrawStartEnginePreview(EditorFrameContext& ctx, const ImVec2& size)
{
    ImGui::BeginChild("##StartEnginePreview", size, false);
    const ImVec2 min = ImGui::GetWindowPos();
    const ImVec2 max = ImVec2(min.x + ImGui::GetWindowSize().x, min.y + ImGui::GetWindowSize().y);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(min, max, IM_COL32(38, 45, 50, 255));

    // Cover the entire panel, preserving the image aspect ratio by cropping.
    constexpr float kPreviewAspect = 572.f / 852.f;
    const float panelAspect = size.x / std::max(1.f, size.y);
    ImVec2 uvMin(0.f, 0.f), uvMax(1.f, 1.f);
    if (panelAspect < kPreviewAspect)
    {
        const float visible = panelAspect / kPreviewAspect;
        uvMin.x = (1.f - visible) * 0.5f;
        uvMax.x = 1.f - uvMin.x;
    }
    else
    {
        const float visible = kPreviewAspect / panelAspect;
        uvMin.y = (1.f - visible) * 0.5f;
        uvMax.y = 1.f - uvMin.y;
    }
    if (void* texId = GetStartPreviewTextureId(ctx))
        dl->AddImage(reinterpret_cast<ImTextureID>(texId), min, max, uvMin, uvMax);
    ImGui::EndChild();
}

static void CancelEditorInteraction(EditorFrameContext& ctx)
{
    FinishViewportDragEdits(ctx);
    EditorState& st = ctx.state;

    st.selectedEntity = NULL_ENTITY;
    st.hierarchyRevealEntity = NULL_ENTITY;
    st.hierarchyHighlightedEntity = NULL_ENTITY;
    st.multiSelection.clear();
    st.multiSelectionDragStartPositions.clear();
    st.multiSelectionDragStartRotations.clear();
    st.multiSelectionDragStartScales.clear();

    st.gizmoHoveredAxis = -1;
    st.rotGizmoHoveredAxis = -1;
    st.sclGizmoHoveredAxis = -1;
    st.obbHoveredFace = -1;
    st.snapMoveLocked = false;

    st.deleteConfirmOpen = false;
    st.closeConfirmOpen = false;
    st.closeSaveFailed = false;

    // Keine ImGui-Internals hier verwenden: ESC kann aus verschiedenen
    // Fenster-/Popup-Kontexten kommen, und Fokus-Internals assertieren dann.
}

static constexpr ImGuiKey kDefaultEditorKeys[] = {ImGuiKey_S, ImGuiKey_Z, ImGuiKey_Y, ImGuiKey_1, ImGuiKey_2, ImGuiKey_3};

static bool IsEditorShortcutKey(ImGuiKey key)
{
    return (key >= ImGuiKey_A && key <= ImGuiKey_Z) || (key >= ImGuiKey_0 && key <= ImGuiKey_9) ||
           (key >= ImGuiKey_F1 && key <= ImGuiKey_F12);
}

static ImGuiKey EditorShortcutKey(const EditorPreferences& preferences, size_t index)
{
    const auto key = static_cast<ImGuiKey>(preferences.shortcutKeys[index]);
    return IsEditorShortcutKey(key) && (index < 3 || key != ImGuiKey_G) ? key : kDefaultEditorKeys[index];
}

static std::string EditorShortcutLabel(size_t index)
{
    return std::string(index < 3 ? "Ctrl+" : "") + ImGui::GetKeyName(EditorShortcutKey(GetEditorPreferences(), index));
}

static void SaveCurrentSceneFromUI(EditorFrameContext& ctx, bool saveOpenMaterial = false)
{
    if (IsInPrefabMode(ctx.state))
    {
        SavePrefabMode(ctx);
        return;
    }

    ctx.lastFileMessage.clear();
    const bool sceneSaved = ctx.saveScene && ctx.saveScene();
    const bool materialSaved = saveOpenMaterial && ctx.state.materialWindowOpen &&
        ctx.state.selectedMaterialAssetLoaded && SaveSelectedMaterialAsset(ctx);

    if (sceneSaved)
        ctx.lastFileMessage = Translate(materialSaved ? "status.saved_with_material" : "status.scene_saved");
    else if (materialSaved)
        ctx.lastFileMessage = Translate("status.material_saved");
    else if (ctx.lastFileMessage.empty())
        ctx.lastFileMessage = Translate("status.save_failed");
}

// Delete acts on scene objects only while the hierarchy or the viewport has the
// focus; in the graph, the animation window or the inspector it would otherwise
// remove the selected object by accident.
static bool SceneSelectionHasFocus()
{
    const ImGuiWindow* focused = GImGui->NavWindow;
    if (!focused)
        return false;
    focused = focused->RootWindow;
    const std::string hierarchy = TranslateLabel("menu.hierarchy", "Hierarchie##editor");
    for (const char* name : {hierarchy.c_str(), "Viewport##editor"})
        if (const ImGuiWindow* window = ImGui::FindWindowByName(name); window && window == focused)
            return true;
    return false;
}

static void RunFileShortcutActions(EditorFrameContext& ctx)
{
    ImGuiIO& io = ImGui::GetIO();
    // Das Spiel hat die Tastatur (Game-Reiter fokussiert): keine Editor-Kuerzel.
    if (ctx.gameViewFocused)
        return;
    if (ImGui::IsKeyPressed(ImGuiKey_Escape, false))
    {
        CancelEditorInteraction(ctx);
        return;
    }

    if (io.WantTextInput)
        return;

    const auto& preferences = GetEditorPreferences();
    const auto pressed = [&](size_t index) { return ImGui::IsKeyPressed(EditorShortcutKey(preferences, index), false); };

    // Gizmo-Modus Tastaturkürzel (nur ohne Ctrl, nicht bei Kamera-Steuerung)
    if (!io.KeyCtrl && !ImGui::IsMouseDown(ImGuiMouseButton_Right))
    {
        if (pressed(3))
            ctx.state.gizmoMode = GizmoMode::Position;
        if (pressed(4))
            ctx.state.gizmoMode = GizmoMode::Rotation;
        if (pressed(5))
            ctx.state.gizmoMode = GizmoMode::Scale;
    }

    if (io.KeyCtrl && pressed(0))
        SaveCurrentSceneFromUI(ctx, true);
    if (ImGui::IsKeyPressed(ImGuiKey_Delete, false) && !GetSelectedEntities(ctx).empty() && SceneSelectionHasFocus())
        ctx.state.deleteConfirmOpen = true;
}

// Scoped styling also covers collapsed windows and closed popups, so none of
// these settings leak into the surrounding editor panels.
struct ProjectDialogStyle
{
    ProjectDialogStyle()
    {
        ImGui::SetNextWindowBgAlpha(1.f);
        ImGui::PushStyleVar(ImGuiStyleVar_Alpha, 1.f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(20.f, 18.f));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 6.f);
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 5.f);
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(10.f, 8.f));
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(12.f, 10.f));
        ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.f);
        ImGui::PushStyleColor(ImGuiCol_WindowBg, theme::kPanel);
        ImGui::PushStyleColor(ImGuiCol_PopupBg, theme::kPanel);
        ImGui::PushStyleColor(ImGuiCol_TitleBg, theme::kWindowBg);
        ImGui::PushStyleColor(ImGuiCol_TitleBgActive, theme::kWindowBg);
        ImGui::PushStyleColor(ImGuiCol_TitleBgCollapsed, theme::kWindowBg);
        ImGui::PushStyleColor(ImGuiCol_FrameBg, theme::kField);
        ImGui::PushStyleColor(ImGuiCol_Button, theme::kButton);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, theme::kButtonHover);
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, theme::kButtonActive);
        ImGui::PushStyleColor(ImGuiCol_Header, theme::kSelected);
        ImGui::PushStyleColor(ImGuiCol_HeaderHovered, theme::kMenuHover);
        ImGui::PushStyleColor(ImGuiCol_HeaderActive, theme::kMenuOpen);
        ImGui::PushStyleColor(ImGuiCol_CheckMark, theme::kText);
    }
    ~ProjectDialogStyle()
    {
        ImGui::PopStyleColor(13);
        ImGui::PopStyleVar(7);
    }
};

static void SetProjectDialogSize(float width, float height)
{
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    const float scale = ImGui::GetStyle().FontScaleMain;
    ImGui::SetNextWindowSize(ImVec2(std::min(width * scale, std::max(1.f, viewport->WorkSize.x - 32.f)),
                                  std::min(height * scale, std::max(1.f, viewport->WorkSize.y - 32.f))),
                             ImGuiCond_Appearing);
    ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x + viewport->WorkSize.x * .5f,
                                  viewport->WorkPos.y + viewport->WorkSize.y * .5f),
                            ImGuiCond_Appearing, ImVec2(.5f, .5f));
}

static void DrawCreateProjectDialog(EditorFrameContext& ctx)
{
    const auto title = LocalizedLabel("project.create_title");
    if (ctx.state.createProjectDialogOpen)
    {
        ImGui::OpenPopup(title.c_str());
        ctx.state.createProjectDialogOpen = false;
        ctx.lastFileMessage.clear();
        if (ctx.state.projectParentDirBuffer[0] == '\0' && !ctx.projectsRootDirectory.empty())
            std::snprintf(ctx.state.projectParentDirBuffer.data(),
                          ctx.state.projectParentDirBuffer.size(),
                          "%s", ctx.projectsRootDirectory.c_str());
    }

    ProjectDialogStyle dialogStyle;
    bool dialogOpen = true;
    if (ImGui::BeginPopupModal(title.c_str(),
                               &dialogOpen,
                               ImGuiWindowFlags_AlwaysAutoResize))
    {
        auto iconButton = [](const char* id, const char* label, const char* icon, ImVec2 size) {
            const bool clicked = ImGui::Button(id, size);
            const ImVec2 pos = ImGui::GetItemRectMin();
            ImDrawList* dl = ImGui::GetWindowDrawList();
            DrawIcon(dl, icon, ImVec2(pos.x + 16.f, pos.y + size.y * 0.5f), 18.f,
                     ImGui::GetColorU32(ImGuiCol_Text));
            if (*label)
                dl->AddText(ImVec2(pos.x + (size.x - ImGui::CalcTextSize(label).x) * 0.5f,
                                   pos.y + (size.y - ImGui::GetTextLineHeight()) * 0.5f),
                            ImGui::GetColorU32(ImGuiCol_Text), label);
            return clicked;
        };
        ImGui::TextUnformatted(Translate("project.name"));
        ImGui::SetNextItemWidth(480.f);
        ImGui::InputText("##ProjectName", ctx.state.projectNameBuffer.data(), ctx.state.projectNameBuffer.size());

        // Projektordner mit Browse-Button
        {
            const bool hasBrowse = static_cast<bool>(ctx.browseForProjectFolder);
            const float browseButtonWidth = hasBrowse ? ImGui::GetFrameHeight() : 0.f;
            const float spacing = hasBrowse ? ImGui::GetStyle().ItemSpacing.x : 0.f;
            ImGui::TextUnformatted(Translate("project.folder"));
            ImGui::SetNextItemWidth(480.f - browseButtonWidth - spacing);
            ImGui::InputText("##ProjectParentDir",
                             ctx.state.projectParentDirBuffer.data(),
                             ctx.state.projectParentDirBuffer.size());
            if (hasBrowse)
            {
                ImGui::SameLine();
                if (iconButton("##BrowseFolder", "", ICON_LC_FOLDER_OPEN, ImVec2(browseButtonWidth, ImGui::GetFrameHeight())))
                {
                    const std::string picked = ctx.browseForProjectFolder();
                    if (!picked.empty())
                        std::snprintf(ctx.state.projectParentDirBuffer.data(),
                                      ctx.state.projectParentDirBuffer.size(),
                                      "%s", picked.c_str());
                }
                if (ImGui::IsItemHovered())
                    ImGui::SetItemTooltip("%s", Translate("project.choose_parent"));
            }
        }



        const char* backendItems = "Vulkan\0DirectX11\0";
        ImGui::TextUnformatted("Backend");
        ImGui::SetNextItemWidth(480.f);
        ImGui::Combo("##ProjectBackend", &ctx.state.projectBackendSelection, backendItems);

        const renderer::DeviceFactory::BackendType selectedBackend =
            EditorProjectBackendFromSelection(ctx.state.projectBackendSelection);
        ImGui::Separator();
        ImGui::TextUnformatted(TranslateWithValue("project.current", ctx.currentProjectName.empty()
            ? Translate("project.none") : ctx.currentProjectName.c_str()).c_str());
        ImGui::TextUnformatted(TranslateWithValue("project.new_backend", EditorProjectBackendLabel(selectedBackend)).c_str());

        ImGui::Spacing();
        ImGui::PushStyleColor(ImGuiCol_Button, theme::kAccent);
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + std::max(0.f, ImGui::GetContentRegionAvail().x - 260.f - ImGui::GetStyle().ItemSpacing.x));
        const bool create = iconButton("##CreateProject", Translate("common.create"), ICON_LC_PLUS, ImVec2(130.f, 32.f));
        ImGui::PopStyleColor();
        if (create)
        {
            ctx.lastFileMessage.clear();
            const bool ok = ctx.createProject
                ? ctx.createProject(ctx.state.projectNameBuffer.data(),
                                    ctx.state.projectParentDirBuffer.data(),
                                    selectedBackend)
                : false;
            if (ok)
                ImGui::CloseCurrentPopup();
            else if (ctx.lastFileMessage.empty())
                ctx.lastFileMessage = Translate("project.create_failed");
        }
        ImGui::SameLine();
        if (ImGui::Button(LocalizedLabel("common.cancel").c_str(), ImVec2(130.f, 32.f)) ||
            ImGui::IsKeyPressed(ImGuiKey_Escape, false))
        {
            ImGui::CloseCurrentPopup();
        }
        if (!ctx.lastFileMessage.empty())
            ImGui::TextWrapped("%s", ctx.lastFileMessage.c_str());
        ImGui::EndPopup();
    }
}

static void DrawOpenProjectDialog(EditorFrameContext& ctx)
{
    const auto title = LocalizedLabel("project.open");
    if (ctx.state.openProjectDialogOpen)
    {
        ImGui::OpenPopup(title.c_str());
        ctx.state.openProjectDialogOpen = false;
        ctx.lastFileMessage.clear();
    }

    ProjectDialogStyle dialogStyle;
    const float dialogWidth = std::min(520.f, std::max(1.f, ImGui::GetMainViewport()->WorkSize.x - 32.f));
    ImGui::SetNextWindowSizeConstraints(ImVec2(dialogWidth, 0.f), ImVec2(dialogWidth, FLT_MAX));
    bool dialogOpen = true;
    if (ImGui::BeginPopupModal(title.c_str(),
                               &dialogOpen,
                               ImGuiWindowFlags_AlwaysAutoResize))
    {
        ImGui::TextUnformatted(Translate("project.path_prompt"));
        // Pfad-Eingabe mit optionalem Browse-Button
        const bool hasBrowse = static_cast<bool>(ctx.browseForProjectFile);
        const float browseButtonWidth = hasBrowse ? ImGui::GetFrameHeight() : 0.f;
        const float spacing = hasBrowse ? ImGui::GetStyle().ItemSpacing.x : 0.f;
        ImGui::SetNextItemWidth(std::max(1.f, ImGui::GetContentRegionAvail().x - browseButtonWidth - spacing));
        ImGui::InputText("##ProjectOpenPath",
                         ctx.state.projectOpenPathBuffer.data(),
                         ctx.state.projectOpenPathBuffer.size());
        if (hasBrowse)
        {
            ImGui::SameLine();
            const bool browse = ImGui::Button("##BrowseProject", ImVec2(browseButtonWidth, ImGui::GetFrameHeight()));
            {
                const ImVec2 pos = ImGui::GetItemRectMin();
                DrawIcon(ImGui::GetWindowDrawList(), ICON_LC_FOLDER_OPEN,
                         ImVec2(pos.x + 16.f, pos.y + ImGui::GetFrameHeight() * 0.5f), 18.f,
                         ImGui::GetColorU32(ImGuiCol_Text));
            }
            if (browse)
            {
                const std::string picked = ctx.browseForProjectFile();
                if (!picked.empty())
                    std::snprintf(ctx.state.projectOpenPathBuffer.data(),
                                  ctx.state.projectOpenPathBuffer.size(),
                                  "%s", picked.c_str());
            }
            if (ImGui::IsItemHovered())
                ImGui::SetItemTooltip("%s", Translate("project.choose_file"));
        }
        ImGui::TextDisabled("%s", Translate("project.path_hint"));

        ImGui::Spacing();
        const float actionWidth = std::min(150.f, std::max(1.f, (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x) * .5f));
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + std::max(0.f, ImGui::GetContentRegionAvail().x - 2.f * actionWidth - ImGui::GetStyle().ItemSpacing.x));
        ImGui::PushStyleColor(ImGuiCol_Button, theme::kAccent);
        const bool open = ImGui::Button(LocalizedLabel("common.open").c_str(), ImVec2(actionWidth, 32.f));
        ImGui::PopStyleColor();
        if (open)
        {
            ctx.lastFileMessage.clear();
            const std::string path = ctx.state.projectOpenPathBuffer.data();
            if (ctx.currentProjectName.empty() && !path.empty())
            {
                ctx.state.pendingStartProjectFile = path;
                ImGui::CloseCurrentPopup();
            }
            else
            {
                const bool ok = ctx.loadProject && ctx.loadProject(path);
                if (ok)
                    ImGui::CloseCurrentPopup();
                else if (ctx.lastFileMessage.empty())
                    ctx.lastFileMessage = Translate("project.load_failed");
            }
        }
        ImGui::SameLine();
        if (ImGui::Button(LocalizedLabel("common.cancel").c_str(), ImVec2(actionWidth, 32.f)) ||
            ImGui::IsKeyPressed(ImGuiKey_Escape, false))
        {
            ImGui::CloseCurrentPopup();
        }
        if (!ctx.lastFileMessage.empty())
            ImGui::TextWrapped("%s", ctx.lastFileMessage.c_str());
        ImGui::EndPopup();
    }
}

// ---------------------------------------------------------------------------
// Closing the editor (window close button, Alt+F4, File > Close, start screen).
// Used by DrawFileMenuBar and DrawNoProjectScreen.
// ---------------------------------------------------------------------------
static void CloseEditorNow(EditorFrameContext& ctx)
{
    ctx.state.closeSaveFailed = false;
    if (ctx.requestClose)
        ctx.requestClose();
}

static void DrawCloseConfirmDialogImpl(EditorFrameContext& ctx)
{
    const auto title = LocalizedLabel("close.title");
    if (ctx.state.closeConfirmOpen)
    {
        ctx.state.closeConfirmOpen = false;
        ctx.state.closeSaveFailed = false;
        // Closing ends Play anyway. Stopping first restores the scene the question is about.
        if (ctx.playActive && ctx.stopPlay)
            ctx.stopPlay();
        if (!detail::EditorCloseNeedsSaveQuestion(ctx))
        {
            CloseEditorNow(ctx);
            return;
        }
        ImGui::OpenPopup(title.c_str());
    }

    ImGui::SetNextWindowBgAlpha(1.f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(20.f, 18.f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 6.f);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 5.f);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(10.f, 8.f));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(12.f, 10.f));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.f);
    ImGui::PushStyleColor(ImGuiCol_PopupBg, theme::kPanel);
    ImGui::PushStyleColor(ImGuiCol_TitleBg, theme::kWindowBg);
    ImGui::PushStyleColor(ImGuiCol_TitleBgActive, theme::kWindowBg);
    ImGui::PushStyleColor(ImGuiCol_Button, theme::kPanel);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, theme::kMenuHover);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, theme::kMenuOpen);
    bool dialogOpen = true;
    if (ImGui::BeginPopupModal(title.c_str(),
                               &dialogOpen,
                               ImGuiWindowFlags_AlwaysAutoResize))
    {
        // An editable prefab cannot tell whether it changed; saving leaves the
        // prefab mode (writing the prefab) and then saves the scene.
        if (IsInPrefabMode(ctx.state) && !ctx.state.prefabModeReadOnly)
            ImGui::TextUnformatted(TranslateWithValue("close.unsaved_prefab_prompt",
                PathToUtf8(PathFromUtf8(ctx.state.prefabModePath).filename())).c_str());
        else
            ImGui::TextUnformatted(TranslateWithValue("close.unsaved_prompt", ctx.currentEditorSceneName).c_str());
        if (ctx.state.closeSaveFailed)
            ImGui::TextColored(ImVec4(0.96f, 0.45f, 0.40f, 1.f), "%s",
                ctx.lastFileMessage.empty() ? Translate("project.scene_save_failed") : ctx.lastFileMessage.c_str());
        ImGui::Separator();
        ImGui::Spacing();
        ImGui::PushStyleColor(ImGuiCol_Button, theme::kAccent);
        const bool save = ImGui::Button(LocalizedLabel("common.save").c_str(), ImVec2(130.f, 32.f));
        ImGui::PopStyleColor();
        if (save)
        {
            ctx.lastFileMessage.clear();
            if (ctx.saveScene && ctx.saveScene())
            {
                ImGui::CloseCurrentPopup();
                CloseEditorNow(ctx);
            }
            else
            {
                ctx.state.closeSaveFailed = true; // stays open: the user decides again
            }
        }
        ImGui::SameLine();
        if (ImGui::Button(LocalizedLabel("scene.dont_save").c_str(), ImVec2(130.f, 32.f)))
        {
            ImGui::CloseCurrentPopup();
            CloseEditorNow(ctx);
        }
        ImGui::SameLine();
        if (ImGui::Button(LocalizedLabel("common.cancel").c_str(), ImVec2(130.f, 32.f)) ||
            ImGui::IsKeyPressed(ImGuiKey_Escape, false))
        {
            ctx.state.closeSaveFailed = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
    ImGui::PopStyleColor(6);
    ImGui::PopStyleVar(6);
}

// ── Snap-Schalter in der Menueleiste (wie die Unreal-Viewport-Toolbar) ──────────
// Je Art ein Umschalter (hervorgehoben = an) plus Auswahl der Schrittweite.
enum class ToolbarGlyph { Save, Select, Move, Rotate, Scale, Magnet, Arrow, Play, Pause, Stop, Grid, Icons, Particles, CameraPreview };

static bool ToolbarButton(const char* id, ToolbarGlyph glyph, bool selected,
                          const char* tooltip, const char* label = nullptr)
{
    const float scale = ImGui::GetStyle().FontScaleMain;
    const float side = 28.f * scale;
    const float width = label ? ImGui::CalcTextSize(label).x + 38.f * scale : side;
    ImGui::PushStyleColor(ImGuiCol_Button, selected ? theme::kMenuOpen
                                                   : theme::kButton);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, selected ? theme::kAccent
                                                          : theme::kButtonHover);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, theme::kButtonActive);
    const bool clicked = ImGui::Button(id, ImVec2(width, side));
    ImGui::PopStyleColor(3);
    const ImVec2 p = ImGui::GetItemRectMin();
    const ImVec2 c(p.x + side * .5f, p.y + side * .5f);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImU32 color = ImGui::GetColorU32(ImGuiCol_Text);
    const char* icon = ICON_LC_MOUSE_POINTER_2;
    ImU32 iconColor = color;
    switch (glyph)
    {
    case ToolbarGlyph::Save:      icon = ICON_LC_SAVE; break;
    case ToolbarGlyph::Select:    icon = ICON_LC_MOUSE_POINTER_2; break;
    case ToolbarGlyph::Move:      icon = ICON_LC_MOVE; break;
    case ToolbarGlyph::Rotate:    icon = ICON_LC_ROTATE_CW; break;
    case ToolbarGlyph::Scale:     icon = ICON_LC_SCALING; break;
    case ToolbarGlyph::Magnet:    icon = ICON_LC_MAGNET; break;
    case ToolbarGlyph::Arrow:     icon = ICON_LC_CHEVRON_DOWN; break;
    case ToolbarGlyph::Play:      icon = ICON_LC_PLAY; iconColor = ImGui::GetColorU32(theme::kSuccess); break;
    case ToolbarGlyph::Pause:     icon = ICON_LC_PAUSE; break;
    case ToolbarGlyph::Stop:      icon = ICON_LC_SQUARE; iconColor = ImGui::GetColorU32(theme::kDanger); break;
    case ToolbarGlyph::Grid:      icon = ICON_LC_GRID_3X3; break;
    case ToolbarGlyph::Icons:     icon = ICON_LC_EYE; break;
    case ToolbarGlyph::Particles: icon = ICON_LC_SPARKLES; break;
    case ToolbarGlyph::CameraPreview: icon = ICON_LC_VIDEO; break;
    }
    DrawIcon(dl, icon, c, 18.f * scale, iconColor);
    if (label)
        dl->AddText(ImVec2(p.x + side, p.y + (side - ImGui::GetFontSize()) * 0.5f), color, label);
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("%s", tooltip);
    return clicked;
}

static void ToolbarSeparator()
{
    ImGui::SameLine(0.f, 8.f);
    const ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::GetWindowDrawList()->AddLine(ImVec2(p.x, p.y + 3), ImVec2(p.x, p.y + 25),
        ImGui::GetColorU32(theme::kBorder));
    ImGui::Dummy(ImVec2(1.f, 28.f));
    ImGui::SameLine(0.f, 8.f);
}

static void SnapValueCombo(const char* id, float& value, const float* steps, size_t count, const char* unit)
{
    const auto format = [unit](float v) {
        char text[64];
        std::snprintf(text, sizeof(text), "%g%s", v, unit);
        std::string result(text);
        std::replace(result.begin(), result.end(), '.', ',');
        return result;
    };
    ImGui::SetNextItemWidth(110.f);
    if (ImGui::BeginCombo(id, format(value).c_str()))
    {
        for (size_t i = 0; i < count; ++i)
            if (ImGui::Selectable(format(steps[i]).c_str(), std::abs(value - steps[i]) < 1e-5f))
                value = steps[i];
        ImGui::EndCombo();
    }
}

// The editor logo spans menu bar and toolbar; both bars leave room for it on its side.
static constexpr float kEditorLogoMargin = 5.f;
static constexpr bool kEditorLogoOnRight = false;

static const AssetBrowserState::Icon* GetEditorLogo(EditorFrameContext& ctx)
{
    if (!ctx.state.assetBrowser)
        return nullptr;
    LoadEditorIcons(ctx, *ctx.state.assetBrowser);
    const auto& logo = ctx.state.assetBrowser->icons.editorLogo;
    return logo.imguiId ? &logo : nullptr;
}

static float EditorLogoHeight()
{
    return (kEditorTopHeight - 2.f * kEditorLogoMargin) * ImGui::GetStyle().FontScaleMain;
}

// Horizontal space reserved for the logo at the left edge (0 without logo).
static float EditorLogoReserve(EditorFrameContext& ctx)
{
    const auto* logo = GetEditorLogo(ctx);
    if (!logo)
        return 0.f;
    const float uiScale = ImGui::GetStyle().FontScaleMain;
    return EditorLogoHeight() * logo->aspect + 2.f * kEditorLogoMargin * uiScale + 4.f * uiScale;
}

static void DrawEditorLogo(EditorFrameContext& ctx)
{
    const auto* logo = GetEditorLogo(ctx);
    if (!logo)
        return;
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    const float uiScale = ImGui::GetStyle().FontScaleMain;
    const float height = EditorLogoHeight();
    const float reserve = EditorLogoReserve(ctx);
    const float areaX = kEditorLogoOnRight ? viewport->Pos.x + viewport->Size.x - reserve : viewport->Pos.x;
    const ImVec2 areaMin(areaX, viewport->Pos.y);
    const ImVec2 areaMax(areaX + reserve, viewport->Pos.y + kEditorTopHeight * uiScale);
    // The extra 4px gap of the reserve sits on the side facing the bars.
    const float logoX = kEditorLogoOnRight ? areaMax.x - kEditorLogoMargin * uiScale - height * logo->aspect
                                           : areaMin.x + kEditorLogoMargin * uiScale;
    const ImVec2 min(logoX, viewport->Pos.y + kEditorLogoMargin * uiScale);
    const ImVec2 max(min.x + height * logo->aspect, min.y + height);
    // Foreground list: the logo overlaps two separate windows (menu bar and toolbar).
    ImDrawList* drawList = ImGui::GetForegroundDrawList(const_cast<ImGuiViewport*>(viewport));
    drawList->AddRectFilled(areaMin, areaMax, ImGui::GetColorU32(theme::kWindowBg));
    drawList->AddImage(reinterpret_cast<ImTextureID>(logo->imguiId), min, max);
}

static void DrawEditorToolbar(EditorFrameContext& ctx)
{
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    const float width = viewport->Size.x;
    const float uiScale = ImGui::GetStyle().FontScaleMain;
    const bool compact = width < 960.f * uiScale;
    ImGui::SetNextWindowPos(ImVec2(viewport->Pos.x, viewport->Pos.y + kEditorMenuHeight * uiScale));
    ImGui::SetNextWindowSize(ImVec2(width, kEditorToolbarHeight * uiScale));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(6, 6));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 4.f);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, theme::kWindowBg);
    constexpr ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoDocking |
        ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse;
    KeepNextWindowInMainViewport("##editorToolbar");
    ImGui::Begin("##editorToolbar", nullptr, flags);
    auto& state = ctx.state;
    const float logoWidth = EditorLogoReserve(ctx);
    const float leftReserve = kEditorLogoOnRight ? 0.f : logoWidth;
    const float rightReserve = kEditorLogoOnRight ? logoWidth : 0.f;
    ImGui::SetCursorPos(ImVec2(leftReserve > 0.f ? leftReserve : 12.f, 6.f * uiScale));
    // Save first: same as File > Save (the prefab in prefab mode).
    {
        const bool inPrefabMode = IsInPrefabMode(state);
        const bool canSave = inPrefabMode || static_cast<bool>(ctx.saveScene);
        const std::string saveTip = std::string(Translate(inPrefabMode ? "menu.save_prefab" : "menu.save_scene")) +
                                    " (" + EditorShortcutLabel(0) + ")";
        ImGui::BeginDisabled(!canSave);
        if (ToolbarButton("##toolSave", ToolbarGlyph::Save, false, saveTip.c_str()) && canSave)
            SaveCurrentSceneFromUI(ctx);
        ImGui::EndDisabled();
    }
    ToolbarSeparator();
    if (ToolbarButton("##toolSelect", ToolbarGlyph::Select, state.gizmoMode == GizmoMode::Select, Translate("toolbar.select")))
        state.gizmoMode = GizmoMode::Select;
    ImGui::SameLine(0, 4);
    if (ToolbarButton("##toolMove", ToolbarGlyph::Move, state.gizmoMode == GizmoMode::Position, (std::string(Translate("settings.shortcut_move")) + " (" + EditorShortcutLabel(3) + ")").c_str()))
        state.gizmoMode = GizmoMode::Position;
    ImGui::SameLine(0, 4);
    if (ToolbarButton("##toolRotate", ToolbarGlyph::Rotate, state.gizmoMode == GizmoMode::Rotation, (std::string(Translate("settings.shortcut_rotate")) + " (" + EditorShortcutLabel(4) + ")").c_str()))
        state.gizmoMode = GizmoMode::Rotation;
    ImGui::SameLine(0, 4);
    if (ToolbarButton("##toolScale", ToolbarGlyph::Scale, state.gizmoMode == GizmoMode::Scale, (std::string(Translate("settings.shortcut_scale")) + " (" + EditorShortcutLabel(5) + ")").c_str()))
        state.gizmoMode = GizmoMode::Scale;
    ToolbarSeparator();

    const bool world = state.transformSpace == EditorTransformSpace::World;
    const bool compactSpace = width - logoWidth < 760.f * uiScale;
    ImGui::SetNextItemWidth((compactSpace ? 56.f : 84.f) * uiScale);
    if (ImGui::BeginCombo("##toolbarSpace", compactSpace ? (world ? "W" : "L") : (world ? Translate("transform.world") : Translate("transform.local"))))
    {
        if (ImGui::Selectable(TranslateLabel("transform.local", "Lokal").c_str(), !world)) state.transformSpace = EditorTransformSpace::Local;
        if (ImGui::Selectable(TranslateLabel("transform.world", "Welt").c_str(), world)) state.transformSpace = EditorTransformSpace::World;
        ImGui::EndCombo();
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip(Translate("toolbar.space_tip"));
    ImGui::SameLine(0, 4);
    auto& snap = state.snap;
    const bool snapping = snap.moveEnabled || snap.rotateEnabled || snap.scaleEnabled;
    if (ToolbarButton("##toolbarSnap", ToolbarGlyph::Magnet, snapping,
                      Translate("toolbar.snap_tip")))
        snap.moveEnabled = snap.rotateEnabled = snap.scaleEnabled = !snapping;
    ImGui::SameLine(0, 2);
    if (ToolbarButton("##toolbarSnapOptions", ToolbarGlyph::Arrow, false, Translate("toolbar.snap_options")))
        ImGui::OpenPopup("##snapOptions");
    const float leftGroupEnd = ImGui::GetItemRectMax().x - ImGui::GetWindowPos().x;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(12, 10));
    if (ImGui::BeginPopup("##snapOptions"))
    {
        static constexpr float move[] = {0.01f, 0.05f, 0.1f, 0.25f, 0.5f, 1.f, 2.f, 5.f, 10.f};
        static constexpr float rotate[] = {1.f, 5.f, 10.f, 15.f, 22.5f, 30.f, 45.f, 90.f};
        static constexpr float scale[] = {0.01f, 0.05f, 0.1f, 0.25f, 0.5f, 1.f};
        ImGui::TextUnformatted(Translate("toolbar.snap"));
        ImGui::Separator();
        ImGui::Checkbox(TranslateLabel("transform.position_colon", "Position:").c_str(), &snap.moveEnabled); ImGui::SameLine(132);
        SnapValueCombo("##moveStep", snap.moveStep, move, std::size(move), " m");
        ImGui::Checkbox(TranslateLabel("transform.rotation_colon", "Drehung:").c_str(), &snap.rotateEnabled); ImGui::SameLine(132);
        SnapValueCombo("##rotateStep", snap.rotateStepDeg, rotate, std::size(rotate), "°");
        ImGui::Checkbox(TranslateLabel("transform.scale_colon", "Skalierung:").c_str(), &snap.scaleEnabled); ImGui::SameLine(132);
        SnapValueCombo("##scaleStep", snap.scaleStep, scale, std::size(scale), "");
        ImGui::EndPopup();
    }

    ImGui::PopStyleVar();

    // Play im Editor: Start (gesperrt waehrend Play), Pause (haelt Spielzeit, Scripts
    // und Physik an), Stop (stellt die Szene von vor dem Start wieder her).
    const float startWidth = ImGui::CalcTextSize("Start").x + 38.f * uiScale;
    const float pauseWidth = 28.f * uiScale;
    const float stopWidth = ImGui::CalcTextSize("Stop").x + 38.f * uiScale;
    const float spacing = ImGui::GetStyle().ItemSpacing.x;
    const float playGroupWidth = startWidth + pauseWidth + stopWidth + 2.f * spacing;
    const float playGroupX = std::max((width - playGroupWidth) * 0.5f, leftGroupEnd + 8.f * uiScale);
    ImGui::SetCursorPos(ImVec2(playGroupX, 6.f * uiScale));
    const bool canPlay = ctx.startPlay && !ctx.currentProjectName.empty() && !ctx.playActive;
    ImGui::BeginDisabled(!canPlay);
    if (ToolbarButton("##toolbarPlay", ToolbarGlyph::Play, false, Translate("toolbar.play_tip"), "Start"))
    {
        if (!ctx.startPlay() && ctx.lastFileMessage.empty()) ctx.lastFileMessage = Translate("status.run_failed");
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(!ctx.playActive || !ctx.togglePlayPause);
    if (ToolbarButton("##toolbarPause", ToolbarGlyph::Pause, ctx.playPaused,
                      ctx.playPaused ? Translate("toolbar.resume") : "Pause"))
        ctx.togglePlayPause();
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(!ctx.playActive || !ctx.stopPlay);
    if (ToolbarButton("##toolbarStop", ToolbarGlyph::Stop, false,
                      Translate("toolbar.stop_tip"), "Stop"))
        ctx.stopPlay();
    ImGui::EndDisabled();

    const float displayWidth = compact ? (112.f * uiScale + 12.f) :
        ImGui::CalcTextSize(Translate("toolbar.grid")).x + ImGui::CalcTextSize(Translate("toolbar.icons")).x + ImGui::CalcTextSize(Translate("toolbar.particles")).x +
        ImGui::CalcTextSize(Translate("toolbar.preview")).x + 152.f * uiScale + 12.f;
    ImGui::SetCursorPos(ImVec2(width - rightReserve - displayWidth - (rightReserve > 0.f ? 0.f : 12.f), 6.f * uiScale));
    if (ToolbarButton("##toolbarGrid", ToolbarGlyph::Grid, snap.showGrid, Translate("toolbar.grid_tip"), compact ? nullptr : Translate("toolbar.grid")))
        snap.showGrid = !snap.showGrid;
    ImGui::SameLine(0, 4);
    if (ToolbarButton("##toolbarIcons", ToolbarGlyph::Icons, state.showEditorIcons,
                      Translate("toolbar.icons_tip"), compact ? nullptr : Translate("toolbar.icons")))
        state.showEditorIcons = !state.showEditorIcons;
    ImGui::SameLine(0, 4);
    if (ToolbarButton("##toolbarParticles", ToolbarGlyph::Particles, state.playAllParticles,
                      Translate("toolbar.particles_tip"), compact ? nullptr : Translate("toolbar.particles")))
        state.playAllParticles = !state.playAllParticles;
    ImGui::SameLine(0, 4);
    if (ToolbarButton("##toolbarCameraPreview", ToolbarGlyph::CameraPreview, state.showCameraPreview,
                      Translate("toolbar.preview_tip"), compact ? nullptr : Translate("toolbar.preview")))
        state.showCameraPreview = !state.showCameraPreview;
    ImGui::End();
    ImGui::PopStyleColor();
    ImGui::PopStyleVar(3);
}

} // namespace

namespace detail {

bool SaveEntityAsPrefab(EditorFrameContext& ctx, EntityID entity, const std::filesystem::path& targetDir)
{
    if (!entity.IsValid() || !ctx.world.IsAlive(entity))
        return false;

    std::string prefabName = "Prefab";
    if (const auto* name = ctx.world.Get<NameComponent>(entity))
        if (!name->name.empty())
            prefabName = name->name;

    const engine::addons::prefab::PrefabAsset prefab =
        engine::addons::prefab::BuildPrefabFromWorldEntity(
            ctx.world, entity, prefabName, &ctx.registry, ctx.scriptRegistry);
    if (prefab.Empty())
    {
        ctx.lastFileMessage = Translate("prefab.create_failed");
        return false;
    }

    const std::filesystem::path projectRoot = PathFromUtf8(ctx.currentProjectRoot);
    const std::filesystem::path prefabDir = !targetDir.empty()
        ? targetDir
        : (projectRoot.empty()
            ? (std::filesystem::path("assets") / "Prefabs")
            : (projectRoot / "Assets" / "Prefabs"));
    const std::filesystem::path prefabPath =
        MakeUniqueFilesystemPath(prefabDir, SanitizeAssetStem(prefabName), ".prefab");

    std::string error;
    if (!engine::addons::prefab::SavePrefabToFile(prefab, prefabPath, &error))
    {
        ctx.lastFileMessage = error.empty() ? Translate("prefab.save_failed") : error;
        return false;
    }

    ctx.lastFileMessage = Translate("prefab.saved_prefix") + PathToUtf8(prefabPath);

    // The original scene object becomes an instance of the newly saved asset.
    engine::addons::prefab::PrefabInstance instance;
    instance.root = entity;
    std::vector<EntityID> stack{entity};
    while (!stack.empty())
    {
        const auto current = stack.back(); stack.pop_back();
        instance.entities.push_back(current);
        if (const auto* children = ctx.world.Get<ChildrenComponent>(current))
            for (auto it = children->children.rbegin(); it != children->children.rend(); ++it) stack.push_back(*it);
    }
    auto linkPath = std::filesystem::absolute(prefabPath);
    if (!projectRoot.empty())
    {
        std::error_code ec;
        const auto relative = std::filesystem::relative(prefabPath, projectRoot / "Assets", ec);
        if (!ec) linkPath = relative;
    }
    engine::addons::prefab::LinkPrefabInstance(ctx.world, prefab, instance, PathToGenericUtf8(linkPath));


    return true;
}

std::filesystem::path ResolveSelectedPrefabPath(const EditorFrameContext& ctx)
{
    std::filesystem::path path = PathFromUtf8(ctx.state.selectedPrefabAssetPath);
    if (path.empty() || path.is_absolute())
        return path;
    if (!ctx.currentProjectRoot.empty())
        return PathFromUtf8(ctx.currentProjectRoot) / "Assets" / path;
    return path;
}

bool SpawnPrefabAsset(EditorFrameContext& ctx,
                      const std::filesystem::path& path,
                      const math::Vec3* worldPosition,
                      EntityID parent)
{
    engine::addons::prefab::PrefabAsset prefab;
    std::string error;
    if (!engine::addons::prefab::LoadPrefabFromFile(path, ctx.registry, prefab, &error))
    {
        ctx.lastFileMessage = error.empty() ? Translate("prefab.load_failed") : error;
        return false;
    }

    const EntityID root = CreateEntityTree(ctx, "history.instantiate_prefab", [&](std::vector<EntityID>& created) {
        engine::addons::prefab::PrefabInstantiateOptions options{};
        options.scriptRegistry = ctx.scriptRegistry;
        const engine::addons::prefab::PrefabInstance instance =
            engine::addons::prefab::InstantiatePrefab(ctx.world, prefab, options);
        if (!instance.IsValid())
        {
            ctx.lastFileMessage = Translate("prefab.instance_failed");
            return NULL_ENTITY;
        }
        created = instance.entities;

        if (ctx.assetPipeline)
        {
            engine::addons::mesh_renderer::ResolveMeshAssetBindings(
                *ctx.assetPipeline, ctx.registry, ctx.world, instance.entities);
            ResolveMaterialAssetBindings(ctx, instance.entities);
        }

        if (parent.IsValid() && ctx.world.IsAlive(parent))
        {
            // Als Kind am Ursprung des Parents, gleiche Ausrichtung.
            const uint32_t order = GetNextSiblingOrder(ctx, parent);
            ctx.world.Add<ParentComponent>(instance.root, ParentComponent{parent});
            if (auto* children = ctx.world.Get<ChildrenComponent>(parent))
            {
                children->Add(instance.root);
            }
            else
            {
                ctx.world.Add<ChildrenComponent>(parent).Add(instance.root);
            }
            ctx.world.Add<EditorHierarchyComponent>(instance.root, EditorHierarchyComponent{order});
            if (auto* transform = ctx.world.Get<TransformComponent>(instance.root))
            {
                transform->localPosition = {0.f, 0.f, 0.f};
                transform->localRotation = math::Quat::Identity();
                transform->dirty = true;
                ++transform->localVersion;
            }
        }
        else if (worldPosition)
        {
            if (auto* transform = ctx.world.Get<TransformComponent>(instance.root))
            {
                transform->localPosition = *worldPosition;
                transform->dirty = true;
                ++transform->localVersion;
            }
        }

        // PrefabInstanceComponent: Live-Link zur Quell-Datei
        {
            std::string relPath = PathToGenericUtf8(path);
            if (!ctx.currentProjectRoot.empty())
            {
                const std::filesystem::path assetsRoot =
                    PathFromUtf8(ctx.currentProjectRoot) / "Assets";
                std::error_code ec;
                auto rel = std::filesystem::relative(path, assetsRoot, ec);
                if (!ec) relPath = PathToGenericUtf8(rel);
            }
            engine::addons::prefab::LinkPrefabInstance(ctx.world, prefab, instance, relPath);
        }

        ClearMultiSelection(ctx);
        ctx.state.selectedEntity = instance.root;
        ctx.lastFileMessage = Translate("prefab.loaded_prefix") + PathToUtf8(path.filename());
        return instance.root;
    });
    return root.IsValid();
}

void DrawSceneLoadStatus(EditorFrameContext& ctx)
{
    if (!ctx.sceneLoadActive)
        return;
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x + viewport->WorkSize.x * 0.5f,
                                  viewport->WorkPos.y + viewport->WorkSize.y * 0.5f),
                            ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(480.f, 0.f), ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(0.96f);
    constexpr ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration |
        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoSavedSettings |
        ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav;
    KeepNextWindowInMainViewport("##sceneLoadStatus");
    if (ImGui::Begin("##sceneLoadStatus", nullptr, flags))
    {
        ImGui::Text(Translate("scene.loading_format"), ctx.sceneLoadName.c_str());
        if (ctx.sceneLoadIndeterminate)
            ImGui::ProgressBar(-static_cast<float>(ImGui::GetTime()), ImVec2(-1.f, 0.f),
                               ctx.sceneLoadPhase.c_str());
        else
            ImGui::ProgressBar(ctx.sceneLoadProgress, ImVec2(-1.f, 0.f));
        if (!ctx.sceneLoadPhase.empty())
            ImGui::TextWrapped("%s", ctx.sceneLoadPhase.c_str());
    }
    ImGui::End();
}

float EditorStatusBarHeight(const EditorFrameContext& ctx)
{
    return ctx.shaderCompileStatus.Active() ? ImGui::GetFrameHeight() : 0.f;
}

void DrawEditorStatusBar(const EditorFrameContext& ctx)
{
    const EditorShaderCompileStatus& status = ctx.shaderCompileStatus;
    if (!status.Active())
        return;

    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    const float height = EditorStatusBarHeight(ctx);
    ImGui::SetNextWindowPos(ImVec2(viewport->Pos.x, viewport->Pos.y + viewport->Size.y - height));
    ImGui::SetNextWindowSize(ImVec2(viewport->Size.x, height));
    ImGui::SetNextWindowViewport(viewport->ID);
    const ImGuiWindowFlags flags =
        ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoDocking |
        ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoFocusOnAppearing |
        ImGuiWindowFlags_NoScrollWithMouse;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(ImGui::GetStyle().ItemSpacing.x, 0.f));
    KeepNextWindowInMainViewport("##editorStatusBar");
    const bool open = ImGui::Begin("##editorStatusBar", nullptr, flags);
    ImGui::PopStyleVar(3);
    if (open)
    {
        // Position of the compile shown in the text, 1-based like "3/12".
        const uint32_t index = std::min(status.completed + 1u, status.total);
        const float fraction = status.total > 0u
            ? static_cast<float>(status.completed) / static_cast<float>(status.total) : 0.f;
        ImGui::SetCursorPosY(0.f);
        // Orange so the running compile stands out against the dark UI.
        ImGui::PushStyleColor(ImGuiCol_PlotHistogram, theme::kWarning);
        ImGui::PushStyleColor(ImGuiCol_Text, theme::kWarning);
        ImGui::ProgressBar(fraction, ImVec2(120.f, height), "");
        ImGui::SameLine();
        ImGui::AlignTextToFramePadding();
        if (status.defines.empty())
            ImGui::Text(Translate("status.compiling_shader_plain"), status.shaderName.c_str(), index, status.total);
        else
            ImGui::Text(Translate("status.compiling_shader"), status.shaderName.c_str(), index, status.total,
                        status.defines.c_str());
        ImGui::PopStyleColor(2);
        if (status.running > 1u)
        {
            ImGui::SameLine();
            ImGui::TextDisabled(Translate("status.compiling_parallel"), status.running - 1u);
        }
    }
    ImGui::End();
}

void DrawRuntimeBuildStatus(EditorFrameContext& ctx)
{
    const auto popupLabel = TranslateLabel("build.scripts_title", "Scripts bauen##scriptBuild");
    const char* popup = popupLabel.c_str();
    if (ctx.scriptBuildActive && !ImGui::IsPopupOpen(popup)) ImGui::OpenPopup(popup);
    if (ImGui::IsPopupOpen(popup))
    {
        const auto* viewport = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(viewport->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
        // Fixed width, auto height. AlwaysAutoResize would shrink the window every
        // frame because the wrapped text and the progress bar follow its width.
        ImGui::SetNextWindowSize(ImVec2(540.f, 0.f), ImGuiCond_Always);
        if (ImGui::BeginPopupModal(popup, nullptr, ImGuiWindowFlags_NoDocking |
            ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoSavedSettings))
        {
            if (!ctx.scriptBuildActive)
                ImGui::CloseCurrentPopup();
            else
            {
                ImGui::TextWrapped("%s", ctx.runtimeBuildStatus.c_str());
                unsigned done = 0, total = 0;
                const auto newline = ctx.runtimeBuildStatus.find('\n');
                const bool fraction = newline != std::string::npos &&
                    std::sscanf(ctx.runtimeBuildStatus.c_str() + newline + 1, "[%u/%u]", &done, &total) == 2 && total;
                ImGui::ProgressBar(fraction ? static_cast<float>(done) / total :
                    -static_cast<float>(ImGui::GetTime()), ImVec2(-1.f, 0.f), fraction ? nullptr : Translate("build.preparing"));
                if (ImGui::Button(TranslateLabel("common.cancel", "Abbrechen").c_str()) && ctx.cancelBuild) ctx.cancelBuild();
            }
            ImGui::EndPopup();
        }
    }
    if (ctx.scriptBuildActive) return;
    const bool showSuccess = !ctx.runtimeBuildActive && ctx.runtimeBuildSuccessSeconds > 0.f;
    if (!ctx.runtimeBuildActive && !showSuccess)
        return;
    if (showSuccess)
        ctx.runtimeBuildSuccessSeconds -= ImGui::GetIO().DeltaTime;

    // Status: erste Zeile Ueberschrift, optional zweite Zeile die letzte
    // Ninja-Fortschrittszeile "[n/m] ...".
    const std::string& status = ctx.runtimeBuildStatus;
    const size_t newline = status.find('\n');
    const std::string headline = status.substr(0u, newline);
    const std::string detail = newline == std::string::npos ? std::string() : status.substr(newline + 1u);
    unsigned done = 0u;
    unsigned total = 0u;
    const bool hasFraction =
        std::sscanf(detail.c_str(), "[%u/%u]", &done, &total) == 2 && total > 0u;

    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x + viewport->WorkSize.x * 0.5f,
                                   viewport->Pos.y + kEditorTopHeight * ImGui::GetStyle().FontScaleMain + 8.f),
                            ImGuiCond_Always, ImVec2(0.5f, 0.f));
    ImGui::SetNextWindowSize(ImVec2(480.f, 0.f), ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(0.94f);
    constexpr ImGuiWindowFlags kFlags =
        ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoDocking |
        ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing |
        ImGuiWindowFlags_NoNav;
    KeepNextWindowInMainViewport("##runtimeBuildStatus");
    if (ImGui::Begin("##runtimeBuildStatus", nullptr, kFlags | (showSuccess ? ImGuiWindowFlags_NoInputs : 0)))
    {
        ImGui::TextUnformatted(headline.c_str());
        if (!showSuccess)
        {
            if (hasFraction)
                ImGui::ProgressBar(static_cast<float>(done) / static_cast<float>(total), ImVec2(-1.f, 0.f));
            else
                ImGui::ProgressBar(-1.f * static_cast<float>(ImGui::GetTime()), ImVec2(-1.f, 0.f),
                                   Translate("build.configuring"));
            if (!detail.empty())
            {
                ImGui::PushTextWrapPos(0.f);
                ImGui::TextDisabled("%s", detail.c_str());
                ImGui::PopTextWrapPos();
            }
            ImGui::TextDisabled(Translate("build.editor_available"));
            if (ctx.cancelBuild &&
                ImGui::Button(TranslateLabel("common.cancel", "Abbrechen").c_str()))
                ctx.cancelBuild();
        }
    }
    ImGui::End();
}

static void DrawExportConfirmation(EditorFrameContext& ctx)
{
    ProjectDialogStyle dialogStyle;
    if (ImGui::BeginPopupModal(LocalizedLabel("export.title").c_str(), nullptr,
                              ImGuiWindowFlags_AlwaysAutoResize))
    {
        ImGui::TextUnformatted(Translate("export.prompt"));
        ImGui::TextUnformatted(Translate("export.description"));
        ImGui::TextDisabled("%s", Translate("export.warning"));
        ImGui::Spacing();
        if (ImGui::Button(LocalizedLabel("common.cancel").c_str(), ImVec2(130.f, 32.f)) ||
            ImGui::IsKeyPressed(ImGuiKey_Escape, false))
            ImGui::CloseCurrentPopup();
        ImGui::SetItemDefaultFocus();
        ImGui::SameLine();
        const bool canExport = ctx.exportProject && !ctx.runtimeBuildActive && !ctx.sceneOrder.empty();
        ImGui::BeginDisabled(!canExport);
        if (ImGui::Button(LocalizedLabel("common.export").c_str(), ImVec2(130.f, 32.f)))
        {
            ImGui::CloseCurrentPopup();
            if (!ctx.exportProject() && ctx.lastFileMessage.empty())
                ctx.lastFileMessage = Translate("status.export_failed");
        }
        ImGui::EndDisabled();
        ImGui::EndPopup();
    }
}

static std::string InputKeyName(platform::Key key)
{
    using platform::Key;
    const int value = static_cast<int>(key);
    if (key >= Key::A && key <= Key::Z) return std::string(1, static_cast<char>('A' + value - static_cast<int>(Key::A)));
    if (key >= Key::Num0 && key <= Key::Num9) return std::string(1, static_cast<char>('0' + value - static_cast<int>(Key::Num0)));
    if (key >= Key::F1 && key <= Key::F12) return "F" + std::to_string(value - static_cast<int>(Key::F1) + 1);
    constexpr const char* names[] = {"Escape", "Space", "Enter", "Tab", "Backspace", "L Shift", "R Shift",
        "L Ctrl", "R Ctrl", "L Alt", "R Alt", "←", "→", "↑", "↓", "Home", "End", "Page Up", "Page Down", "Insert", "Delete"};
    if (key >= Key::Escape && key <= Key::Delete) return names[value - static_cast<int>(Key::Escape)];
    if (key == Key::Plus) return "+";
    if (key == Key::Minus) return "-";
    if (key == Key::Equals) return "=";
    return Translate("settings.unassigned");
}

static void DrawProjectInputSettings(ProjectSettings& settings)
{
    using platform::GamepadButton;
    using platform::Key;
    const char* buttons[] = {Translate("settings.unassigned"), "A / ×", "B / ○", "X / □", "Y / △",
        "LB / L1", "RB / R1", "Back", "Start", "L3", "R3", "D-Pad ↑", "D-Pad →", "D-Pad ↓", "D-Pad ←"};
    static_assert(std::size(buttons) == static_cast<size_t>(GamepadButton::Count));
    ImGui::TextWrapped("%s", Translate("settings.input_tip"));
    size_t remove = settings.actions.size();
    for (size_t i = 0; i < settings.actions.size(); ++i)
    {
        auto& action = settings.actions[i];
        ImGui::PushID(static_cast<int>(i));
        char name[128]{};
        std::snprintf(name, sizeof(name), "%s", action.name.c_str());
        if (ImGui::InputText(LocalizedLabel("settings.action_name").c_str(), name, sizeof(name)))
            action.name = name;
        if (ImGui::BeginCombo(LocalizedLabel("settings.keyboard").c_str(), InputKeyName(action.key).c_str()))
        {
            for (int key = 0; key < static_cast<int>(Key::Count); ++key)
            {
                const auto candidate = static_cast<Key>(key);
                if (ImGui::Selectable(InputKeyName(candidate).c_str(), action.key == candidate))
                    action.key = candidate;
            }
            ImGui::EndCombo();
        }
        int button = static_cast<int>(action.button);
        if (ImGui::Combo("Gamepad", &button, buttons, IM_ARRAYSIZE(buttons)))
            action.button = static_cast<GamepadButton>(button);
        if (ImGui::SmallButton(LocalizedLabel("settings.remove_action").c_str()))
            remove = i;
        ImGui::Separator();
        ImGui::PopID();
    }
    if (remove < settings.actions.size())
        settings.actions.erase(settings.actions.begin() + remove);
    if (ImGui::Button(LocalizedLabel("settings.add_action").c_str()))
        settings.actions.push_back({});
}

static void DrawLayerSettings(EditorFrameContext& ctx)
{
    ImGui::TextDisabled(Translate("layer.reserved"));
    ImGui::Separator();
    for (int b = 0; b < 32; ++b)
    {
        char label[16];
        std::snprintf(label, sizeof(label), "%d", b);
        ImGui::SetNextItemWidth(40.f);
        ImGui::TextDisabled("%s", label);
        ImGui::SameLine();
        char buf[32]{};
        std::snprintf(buf, sizeof(buf), "%s", ctx.state.layerNames[b].c_str());
        char inputId[16];
        std::snprintf(inputId, sizeof(inputId), "##ln%d", b);
        const bool isReserved = (b == 0 || b == 1); // Default + EditorGizmo gesperrt
        if (isReserved) ImGui::BeginDisabled();
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
        if (ImGui::InputText(inputId, buf, sizeof(buf)))
            ctx.state.layerNames[b] = buf;
        if (ImGui::IsItemDeactivatedAfterEdit() && ctx.saveEditorSettings)
            ctx.saveEditorSettings();
        if (isReserved) ImGui::EndDisabled();
    }
}

static void DrawCollisionSettings(EditorFrameContext& ctx)
{
    auto& settings = ctx.projectSettings;
    ImGui::DragFloat3(LocalizedLabel("settings.gravity").c_str(), &settings.gravity.x, .05f, -1000.f, 1000.f,
                      "%.2f", ImGuiSliderFlags_AlwaysClamp);
    ImGui::SeparatorText(Translate("settings.collision_matrix"));
    ImGui::TextWrapped("%s", Translate("settings.collision_tip"));
    static int group = 0;
    if (ctx.state.layerNames[group].empty())
        group = 0;
    if (ImGui::BeginCombo(LocalizedLabel("settings.collision_layer").c_str(), ctx.state.layerNames[group].c_str()))
    {
        for (int i = 0; i < 32; ++i)
        {
            ImGui::PushID(i);
            if (i != 1 && !ctx.state.layerNames[i].empty() && ImGui::Selectable(ctx.state.layerNames[i].c_str(), group == i))
                group = i;
            ImGui::PopID();
        }
        ImGui::EndCombo();
    }
    for (int i = 0; i < 32; ++i)
    {
        if (i == 1 || ctx.state.layerNames[i].empty())
            continue;
        ImGui::PushID(i);
        const uint32_t a = 1u << group;
        const uint32_t b = 1u << i;
        bool collide = (settings.collisionMask[group] & b) && (settings.collisionMask[i] & a);
        if (ImGui::Checkbox(ctx.state.layerNames[i].c_str(), &collide))
        {
            settings.collisionMask[group] = (settings.collisionMask[group] & ~b) | (collide ? b : 0u);
            settings.collisionMask[i] = (settings.collisionMask[i] & ~a) | (collide ? a : 0u);
        }
        ImGui::PopID();
    }
    ImGui::SeparatorText(Translate("settings.layer_names"));
    DrawLayerSettings(ctx);
}

static void DrawRuntimeWindowSettings(ProjectSettings& settings)
{
    char windowTitle[256]{};
    std::snprintf(windowTitle, sizeof(windowTitle), "%s", settings.windowTitle.c_str());
    if (ImGui::InputText(LocalizedLabel("settings.window_title").c_str(), windowTitle, sizeof(windowTitle)))
        settings.windowTitle = windowTitle;
    ImGui::Checkbox(LocalizedLabel("settings.system_resolution").c_str(), &settings.useSystemResolution);
    ImGui::SetItemTooltip("%s", Translate("settings.system_resolution_tip"));
    ImGui::BeginDisabled(settings.useSystemResolution);
    ImGui::DragInt(LocalizedLabel("settings.window_width").c_str(), &settings.width, 1.f, 320, 16384, "%d px", ImGuiSliderFlags_AlwaysClamp);
    ImGui::DragInt(LocalizedLabel("settings.window_height").c_str(), &settings.height, 1.f, 200, 16384, "%d px", ImGuiSliderFlags_AlwaysClamp);
    ImGui::EndDisabled();
    const char* windowModes[] = {Translate("settings.run_window"), Translate("settings.maximized"),
        Translate("settings.borderless"), Translate("settings.fullscreen")};
    int mode = static_cast<int>(settings.windowMode);
    if (ImGui::Combo(LocalizedLabel("settings.window_mode").c_str(), &mode, windowModes, IM_ARRAYSIZE(windowModes)))
        settings.windowMode = static_cast<platform::WindowMode>(mode);
    ImGui::Checkbox("VSync", &settings.vsync);
}

static void DrawProjectRuntimeSettings(EditorFrameContext& ctx)
{
    ImGui::TextUnformatted(Translate("settings.runtime_backend"));
    const char* backendItems[] = { "Vulkan", "DirectX11" };
    int backendSelection = 0;
    switch (ctx.projectBackend)
    {
    case renderer::DeviceFactory::BackendType::DirectX11: backendSelection = 1; break;
    case renderer::DeviceFactory::BackendType::Vulkan:
    default: backendSelection = 0; break;
    }
    ImGui::SetNextItemWidth(-1.f);
    if (ImGui::Combo("##RuntimeBackend", &backendSelection, backendItems, IM_ARRAYSIZE(backendItems)))
    {
        const renderer::DeviceFactory::BackendType nextBackend =
            EditorProjectBackendFromSelection(backendSelection);
        ctx.projectBackend = nextBackend;
        if (ctx.setProjectBackend)
            ctx.setProjectBackend(nextBackend);
    }
    ImGui::TextDisabled(Translate("settings.runtime_backend_tip"));
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();
    DrawRuntimeWindowSettings(ctx.projectSettings);
}

// Project-wide render options (ProjectSettings::rendering). They take effect
// with "Apply"; scripts and krom.h can change them at runtime.
static void DrawProjectRenderSettings(EditorFrameContext& ctx)
{
    ProjectRenderSettings& rendering = ctx.projectSettings.rendering;

    // Quality level: derived from the options below (no own key); picking a
    // level sets them, picking "Custom" keeps them.
    ImGui::SeparatorText(Translate("quality.title"));
    const char* qualityLevels[] = { Translate("quality.low"), Translate("quality.medium"),
                                    Translate("quality.high"), Translate("quality.custom") };
    int quality = static_cast<int>(GetRenderQualityLevel(rendering));
    if (ImGui::Combo(TranslateLabel("quality.level", "Level").c_str(), &quality, qualityLevels, IM_ARRAYSIZE(qualityLevels)))
        ApplyRenderQualityLevel(rendering, static_cast<RenderQualityLevel>(std::clamp(quality, 0, 3)));
    ImGui::SetItemTooltip("%s", Translate("quality.level_tip"));
    ImGui::PushTextWrapPos(0.f);
    ImGui::TextDisabled("%s", Translate("quality.hint"));
    ImGui::PopTextWrapPos();

    // Shadow maps: resolution and filter for all lights (part of the quality
    // level, as in Unity/Unreal); lights keep distance, cascades, bias.
    ImGui::SeparatorText(Translate("shadow_quality.title"));
    {
        const char* resolutionNames[] = { "256", "512", "1024", "2048", "4096" };
        auto resolutionCombo = [&](const char* key, const char* fallback, const char* tipKey, uint32_t& value) {
            int index = 0;
            for (int i = 0; i < IM_ARRAYSIZE(resolutionNames); ++i)
                if ((256u << i) == value)
                    index = i;
            if (ImGui::Combo(TranslateLabel(key, fallback).c_str(), &index, resolutionNames, IM_ARRAYSIZE(resolutionNames)))
                value = 256u << std::clamp(index, 0, IM_ARRAYSIZE(resolutionNames) - 1);
            ImGui::SetItemTooltip("%s", Translate(tipKey));
        };
        resolutionCombo("shadow_quality.sun_resolution", "Sun", "shadow_quality.sun_resolution_tip",
                        rendering.shadowSunResolution);
        resolutionCombo("shadow_quality.local_resolution", "Point/Spot", "shadow_quality.local_resolution_tip",
                        rendering.shadowLocalResolution);
        // List order (tent sizes together); values are engine::ShadowFilter ids.
        static constexpr uint8_t kFilterIds[] = { 0u, 1u, 2u, 5u, 3u, 4u };
        const char* filterNames[] = { Translate("light.shadow_filter_none"), "PCF 3x3", "PCF 5x5", "PCF 7x7",
                                      Translate("light.shadow_filter_poisson"), Translate("light.shadow_filter_pcss") };
        int filter = 2;
        for (int i = 0; i < IM_ARRAYSIZE(kFilterIds); ++i)
            if (kFilterIds[i] == rendering.shadowFilter)
                filter = i;
        if (ImGui::Combo(TranslateLabel("shadow_quality.filter", "Filter").c_str(), &filter, filterNames, IM_ARRAYSIZE(filterNames)))
            rendering.shadowFilter = kFilterIds[std::clamp(filter, 0, IM_ARRAYSIZE(kFilterIds) - 1)];
        ImGui::SetItemTooltip("%s", Translate("light.shadow_filter_tip"));
    }

    ImGui::SeparatorText(Translate("tonemap.title"));
    // Order of the list = TonemapOperator ids; the file stores the names.
    const char* operatorNames[] = { "reinhard", "aces", "agx", "pbrNeutral" };
    const char* operators[] = { "Reinhard", "ACES (Fitted)", "AgX", "Khronos PBR Neutral" };
    int op = 0;
    for (int i = 0; i < IM_ARRAYSIZE(operatorNames); ++i)
        if (rendering.tonemapOperator == operatorNames[i])
            op = i;
    if (ImGui::Combo(TranslateLabel("tonemap.operator", "Operator").c_str(), &op, operators, IM_ARRAYSIZE(operators)))
        rendering.tonemapOperator = operatorNames[std::clamp(op, 0, IM_ARRAYSIZE(operatorNames) - 1)];
    ImGui::TextDisabled("%s", Translate("settings.rendering_exposure_hint"));

    ImGui::SeparatorText(Translate("msaa.title"));
    const char* levels[] = { Translate("msaa.off"), "2x MSAA", "4x MSAA" };
    int level = rendering.msaaSamples >= 4u ? 2 : (rendering.msaaSamples == 2u ? 1 : 0);
    if (ImGui::Combo(TranslateLabel("msaa.level", "MSAA").c_str(), &level, levels, IM_ARRAYSIZE(levels)))
        rendering.msaaSamples = level == 2 ? 4u : (level == 1 ? 2u : 1u);
    // Order of the list = PostAAMode ids; the file stores the names.
    const char* postAANames[] = { "off", "fxaa", "smaa" };
    const char* postAAModes[] = { Translate("msaa.off"), "FXAA", "SMAA" };
    int postAA = 0;
    for (int i = 0; i < IM_ARRAYSIZE(postAANames); ++i)
        if (rendering.postAA == postAANames[i])
            postAA = i;
    if (ImGui::Combo(TranslateLabel("msaa.post_aa", "Post-AA").c_str(), &postAA, postAAModes, IM_ARRAYSIZE(postAAModes)))
        rendering.postAA = postAANames[std::clamp(postAA, 0, IM_ARRAYSIZE(postAANames) - 1)];
    ImGui::SetItemTooltip("%s", Translate("msaa.post_aa_tip"));

    ImGui::SeparatorText(Translate("gtao.title"));
    // Own ID scope: "Intensity" also labels the SSR slider below.
    ImGui::PushID("gtao");
    // Without the add-on (host started without GTAO) the values stay in the file but are greyed out here.
    if (!ctx.gtaoAvailable) ImGui::BeginDisabled();
    ImGui::Checkbox(TranslateLabel("gtao.enabled", "Enabled").c_str(), &rendering.gtaoEnabled);
    ImGui::SetItemTooltip("%s", Translate("gtao.enabled_tip"));
    const bool gtaoValuesDisabled = !rendering.gtaoEnabled;
    if (gtaoValuesDisabled) ImGui::BeginDisabled();
    ImGui::SliderFloat(TranslateLabel("gtao.radius", "Radius").c_str(), &rendering.gtaoRadius, 0.01f, 2.0f, "%.2f m",
                       ImGuiSliderFlags_Logarithmic);
    ImGui::SetItemTooltip("%s", Translate("gtao.radius_tip"));
    ImGui::SliderFloat(TranslateLabel("gtao.intensity", "Intensity").c_str(), &rendering.gtaoIntensity, 0.0f, 1.0f, "%.2f");
    ImGui::SetItemTooltip("%s", Translate("gtao.intensity_tip"));
    ImGui::SliderFloat(TranslateLabel("gtao.bias", "Bias").c_str(), &rendering.gtaoBias, 0.0f, 0.5f, "%.3f");
    ImGui::SetItemTooltip("%s", Translate("gtao.bias_tip"));
    if (gtaoValuesDisabled) ImGui::EndDisabled();
    if (!ctx.gtaoAvailable)
    {
        ImGui::EndDisabled();
        ImGui::TextDisabled("%s", Translate("settings.addon_unavailable"));
    }
    ImGui::PopID();

    ImGui::SeparatorText(Translate("bloom.title"));
    ImGui::PushID("bloom");
    ImGui::Checkbox(TranslateLabel("bloom.enabled", "Enabled").c_str(), &rendering.bloomEnabled);
    ImGui::SetItemTooltip("%s", Translate("bloom.enabled_tip"));
    if (!rendering.bloomEnabled) ImGui::BeginDisabled();
    ImGui::SliderFloat(TranslateLabel("bloom.intensity", "Intensity").c_str(), &rendering.bloomIntensity, 0.0f, 1.0f, "%.2f");
    ImGui::SetItemTooltip("%s", Translate("bloom.intensity_tip"));
    ImGui::SliderFloat(TranslateLabel("bloom.threshold", "Threshold").c_str(), &rendering.bloomThreshold, 0.0f, 16.0f, "%.2f",
                       ImGuiSliderFlags_Logarithmic);
    ImGui::SetItemTooltip("%s", Translate("bloom.threshold_tip"));
    if (!rendering.bloomEnabled) ImGui::EndDisabled();
    ImGui::PopID();

    ImGui::SeparatorText(Translate("contact_shadow.title"));
    // Own ID scope: "Enabled"/"Intensity"/"Thickness" also label GTAO and SSR controls.
    ImGui::PushID("contact_shadow");
    if (!ctx.contactShadowAvailable) ImGui::BeginDisabled();
    ImGui::Checkbox(TranslateLabel("contact_shadow.enabled", "Enabled").c_str(), &rendering.contactShadowsEnabled);
    ImGui::SetItemTooltip("%s", Translate("contact_shadow.enabled_tip"));
    if (!rendering.contactShadowsEnabled) ImGui::BeginDisabled();
    ImGui::SliderFloat(TranslateLabel("contact_shadow.length", "Length").c_str(), &rendering.contactShadowLength,
                       0.02f, 2.0f, "%.2f m", ImGuiSliderFlags_Logarithmic);
    ImGui::SetItemTooltip("%s", Translate("contact_shadow.length_tip"));
    ImGui::SliderFloat(TranslateLabel("contact_shadow.thickness", "Thickness").c_str(), &rendering.contactShadowThickness,
                       0.005f, 0.5f, "%.3f m", ImGuiSliderFlags_Logarithmic);
    ImGui::SetItemTooltip("%s", Translate("contact_shadow.thickness_tip"));
    ImGui::SliderFloat(TranslateLabel("contact_shadow.intensity", "Intensity").c_str(), &rendering.contactShadowIntensity,
                       0.0f, 1.0f, "%.2f");
    ImGui::SetItemTooltip("%s", Translate("contact_shadow.intensity_tip"));
    int contactSteps = static_cast<int>(rendering.contactShadowSteps);
    if (ImGui::SliderInt(TranslateLabel("contact_shadow.steps", "Steps").c_str(), &contactSteps, 4, 32))
        rendering.contactShadowSteps = static_cast<uint32_t>(std::clamp(contactSteps, 4, 32));
    ImGui::SetItemTooltip("%s", Translate("contact_shadow.steps_tip"));
    if (!rendering.contactShadowsEnabled) ImGui::EndDisabled();
    if (!ctx.contactShadowAvailable)
    {
        ImGui::EndDisabled();
        ImGui::TextDisabled("%s", Translate("settings.addon_unavailable"));
    }
    ImGui::PopID();

    ImGui::SeparatorText(Translate("ssr.title"));
    ImGui::Checkbox(TranslateLabel("ssr.enabled", "Enabled").c_str(), &rendering.ssrEnabled);
    ImGui::SetItemTooltip("%s", Translate("ssr.enabled_tip"));
    if (!rendering.ssrEnabled) ImGui::BeginDisabled();
    ImGui::SliderFloat(TranslateLabel("ssr.intensity", "Intensity").c_str(), &rendering.ssrIntensity, 0.0f, 1.0f, "%.2f");
    ImGui::SetItemTooltip("%s", Translate("ssr.intensity_tip"));
    ImGui::SliderFloat(TranslateLabel("ssr.max_roughness", "Max. roughness").c_str(), &rendering.ssrMaxRoughness, 0.05f, 1.0f, "%.2f");
    ImGui::SetItemTooltip("%s", Translate("ssr.max_roughness_tip"));
    ImGui::SliderFloat(TranslateLabel("ssr.max_distance", "Range").c_str(), &rendering.ssrMaxDistance, 0.5f, 200.0f, "%.1f",
                       ImGuiSliderFlags_Logarithmic);
    ImGui::SetItemTooltip("%s", Translate("ssr.max_distance_tip"));
    ImGui::SliderFloat(TranslateLabel("ssr.thickness", "Thickness").c_str(), &rendering.ssrThickness, 0.01f, 5.0f, "%.2f",
                       ImGuiSliderFlags_Logarithmic);
    ImGui::SetItemTooltip("%s", Translate("ssr.thickness_tip"));
    if (!rendering.ssrEnabled) ImGui::EndDisabled();

    ImGui::SeparatorText(Translate("shading.title"));
    ImGui::Checkbox(TranslateLabel("shading.visibility", "Height-correlated visibility").c_str(),
                    &rendering.brdfHeightCorrelated);
    ImGui::SetItemTooltip("%s", Translate("shading.visibility_tip"));
    ImGui::Checkbox(TranslateLabel("shading.diffuse_fresnel", "Diffuse Fresnel per light").c_str(),
                    &rendering.brdfPerLightFresnel);
    ImGui::SetItemTooltip("%s", Translate("shading.diffuse_fresnel_tip"));
    ImGui::Checkbox(TranslateLabel("shading.multi_scatter", "Multi-scatter compensation").c_str(),
                    &rendering.brdfMultiScatter);
    ImGui::SetItemTooltip("%s", Translate("shading.multi_scatter_tip"));
    ImGui::Checkbox(TranslateLabel("shading.burley", "Burley diffuse").c_str(),
                    &rendering.brdfBurleyDiffuse);
    ImGui::SetItemTooltip("%s", Translate("shading.burley_tip"));
    ImGui::Checkbox(TranslateLabel("shading.specular_aa", "Specular anti-aliasing").c_str(),
                    &rendering.specularAntiAliasing);
    ImGui::SetItemTooltip("%s", Translate("shading.specular_aa_tip"));

    ImGui::SeparatorText(Translate("probe.scene_title"));
    ImGui::Checkbox(TranslateLabel("probe.specular_occlusion", "Specular occlusion").c_str(),
                    &rendering.specularOcclusion);
    ImGui::SetItemTooltip("%s", Translate("probe.specular_occlusion_tip"));

    ImGui::Spacing();
    ImGui::TextDisabled("%s", Translate("settings.rendering_runtime_hint"));
}

static void DrawProjectSceneSettings(EditorFrameContext& ctx)
{
    const char* testBuilds[] = {Translate("settings.current_build"), "Debug", "RelWithDebInfo", "Release"};
    const char* exportBuilds[] = {"Release", "RelWithDebInfo"};
    ImGui::Combo(LocalizedLabel("settings.test_build").c_str(), &ctx.projectSettings.testBuild, testBuilds, IM_ARRAYSIZE(testBuilds));
    ImGui::Combo(LocalizedLabel("settings.export_build").c_str(), &ctx.projectSettings.exportBuild, exportBuilds, IM_ARRAYSIZE(exportBuilds));
    ImGui::Separator();
    ImGui::TextUnformatted(Translate("settings.scene_order"));
    ImGui::TextDisabled(Translate("settings.reorder_scenes"));
    ImGui::Spacing();

    std::vector<std::string> order = ctx.sceneOrder;
    bool orderChanged = false;
    int moveFrom = -1, moveTo = -1;
    int buildIndex = 0;

    for (int i = 0; i < static_cast<int>(order.size()); ++i)
    {
        ImGui::PushID(i);

        // Hoch-Button zuerst (links), dann Szenenname — so bleiben sie immer sichtbar.
        const float arrowPadding = std::max(0.f, (28.f - ImGui::GetFontSize()) * 0.5f);
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(arrowPadding, arrowPadding));
        const bool canMoveUp = (i > 0);
        if (!canMoveUp) ImGui::BeginDisabled();
        if (ImGui::ArrowButton("##moveUp", ImGuiDir_Up)) { moveFrom = i; moveTo = i - 1; orderChanged = true; }
        if (!canMoveUp) ImGui::EndDisabled();
        if (ImGui::IsItemHovered()) ImGui::SetTooltip(Translate("common.move_up"));
        ImGui::SameLine();

        // Runter-Button
        const bool canMoveDown = (i < static_cast<int>(order.size()) - 1);
        if (!canMoveDown) ImGui::BeginDisabled();
        if (ImGui::ArrowButton("##moveDown", ImGuiDir_Down)) { moveFrom = i; moveTo = i + 1; orderChanged = true; }
        if (!canMoveDown) ImGui::EndDisabled();
        if (ImGui::IsItemHovered()) ImGui::SetTooltip(Translate("common.move_down"));
        ImGui::PopStyleVar();
        ImGui::SameLine();

        const float startWidth = 62.f;
        const float rowWidth = std::max(1.f, ImGui::GetContentRegionAvail().x -
            startWidth - ImGui::GetStyle().ItemSpacing.x);
        const bool inBuild = std::find(ctx.buildExcludedScenes.begin(), ctx.buildExcludedScenes.end(), order[i]) == ctx.buildExcludedScenes.end();
        const std::string rowLabel = (inBuild ? std::to_string(++buildIndex) + ".  " : "–  ") + order[i];
        ImGui::Selectable(rowLabel.c_str(), false, ImGuiSelectableFlags_None, ImVec2(rowWidth, 28.f));
        if (ImGui::IsItemHovered())
            ImGui::SetItemTooltip(Translate("settings.drag_scene"));
        if (ImGui::BeginDragDropSource())
        {
            // Use the scene name rather than an index that could become stale
            // if the order changes while a drag is active.
            ImGui::SetDragDropPayload("KROM_SCENE_ORDER", order[i].c_str(), order[i].size() + 1u);
            ImGui::TextUnformatted(order[i].c_str());
            ImGui::EndDragDropSource();
        }
        ImGui::PushStyleColor(ImGuiCol_DragDropTarget, theme::kAccentHover);
        if (ImGui::BeginDragDropTarget())
        {
            if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("KROM_SCENE_ORDER"))
            {
                if (payload->Data && payload->DataSize > 0 &&
                    static_cast<const char*>(payload->Data)[payload->DataSize - 1] == '\0')
                {
                    const std::string scene(static_cast<const char*>(payload->Data),
                                            static_cast<size_t>(payload->DataSize - 1));
                    const auto source = std::find(order.begin(), order.end(), scene);
                    if (source != order.end() && source != order.begin() + i)
                    {
                        moveFrom = static_cast<int>(source - order.begin());
                        moveTo = i;
                        orderChanged = true;
                    }
                }
            }
            ImGui::EndDragDropTarget();
        }
        ImGui::PopStyleColor();
        ImGui::SameLine();
        if (inBuild && buildIndex == 1)
        {
            ImGui::TextDisabled("Start");
        }
        else if (inBuild)
        {
            if (ImGui::Button("Start", ImVec2(62.f, 28.f)))
            {
                moveFrom = i;
                moveTo = 0;
                orderChanged = true;
            }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip(Translate("settings.set_start_scene"));
        }

        ImGui::PopID();
    }

    if (orderChanged && moveFrom >= 0 && moveTo >= 0 &&
        moveTo < static_cast<int>(order.size()))
    {
        // Insert at the destination and keep every other scene in its relative
        // order. This also makes "Start" move a scene to the front instead of
        // swapping it with the previous start scene.
        std::string movedScene = std::move(order[moveFrom]);
        order.erase(order.begin() + moveFrom);
        order.insert(order.begin() + moveTo, std::move(movedScene));
        if (ctx.setSceneOrder)
            ctx.setSceneOrder(order);
        ctx.sceneOrder = order; // sofort im UI reflektieren
    }

    if (!order.empty())
    {
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::TextDisabled(Translate("settings.start_index"));
        ImGui::TextDisabled(Translate("settings.other_indices"));
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    // Build-Reihenfolge (Checkboxen)
    ImGui::TextUnformatted(Translate("settings.build_scenes"));
    const auto& excluded = ctx.buildExcludedScenes;
    for (const std::string& scene : ctx.sceneOrder)
    {
        const bool isExcluded = std::find(excluded.begin(), excluded.end(), scene) != excluded.end();
        bool included = !isExcluded;
        if (ImGui::Checkbox(scene.c_str(), &included))
        {
            if (ctx.toggleBuildExclude)
                ctx.toggleBuildExclude(scene, !included);
        }
    }
}

void DrawProjectSettingsDialog(EditorFrameContext& ctx)
{
    // ── Edit Layers Fenster ───────────────────────────────────────────────────
    if (ctx.state.layerNamesWindowOpen)
    {
        ImGui::SetNextWindowSize(ImVec2(300.f, 380.f), ImGuiCond_FirstUseEver);
        const std::string layerTitle = TranslateLabel("layer.edit", "Layer bearbeiten##layerEdit");
        KeepNextWindowInMainViewport(layerTitle);
        if (ImGui::Begin(layerTitle.c_str(), &ctx.state.layerNamesWindowOpen))
        {
            DrawLayerSettings(ctx);
        }
        ImGui::End();
    }

    const bool openScenes = ctx.state.settingsWindowOpen && ctx.state.projectSettingsScenePage;
    const std::string title = std::string(Translate("menu.project_settings")) + " · " + ctx.currentProjectName + "###ProjectSettings";
    if (ctx.state.settingsWindowOpen)
    {
        if (ctx.resetProjectSettingsDraft)
            ctx.resetProjectSettingsDraft();
        ImGui::OpenPopup(title.c_str());
        ctx.state.settingsWindowOpen = false;
        ctx.state.projectSettingsScenePage = false;
    }

    ProjectDialogStyle dialogStyle;
    SetProjectDialogSize(560.f, 780.f);
    bool dialogOpen = true;
    if (!ImGui::BeginPopupModal(title.c_str(),
                                &dialogOpen,
                                ImGuiWindowFlags_NoCollapse))
        return;

    ImGui::BeginChild("##ProjectSettingsBody", ImVec2(0.f, -52.f));
    ImGui::BeginDisabled(ctx.runtimeBuildActive);
    if (ImGui::BeginTabBar("##ProjectSettingsTabs", ImGuiTabBarFlags_FittingPolicyScroll))
    {
        if (ImGui::BeginTabItem(LocalizedLabel("settings.runtime_window").c_str()))
        {
            DrawProjectRuntimeSettings(ctx);
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem(LocalizedLabel("settings.scenes_build").c_str(), nullptr,
                               openScenes ? ImGuiTabItemFlags_SetSelected : ImGuiTabItemFlags_None))
        {
            DrawProjectSceneSettings(ctx);
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem(LocalizedLabel("settings.rendering").c_str()))
        {
            DrawProjectRenderSettings(ctx);
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem(LocalizedLabel("settings.layers_physics").c_str()))
        {
            DrawCollisionSettings(ctx);
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem(LocalizedLabel("settings.input").c_str()))
        {
            DrawProjectInputSettings(ctx.projectSettings);
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
    ImGui::EndDisabled();
    ImGui::Spacing();
    ImGui::EndChild();
    ImGui::Separator();
    const float actionWidth = std::max(1.f, (ImGui::GetContentRegionAvail().x - 3.f * ImGui::GetStyle().ItemSpacing.x) / 4.f);
    ImGui::BeginDisabled(ctx.runtimeBuildActive);
    if (ImGui::Button(LocalizedLabel("settings.apply").c_str(), ImVec2(actionWidth, 32.f)))
    {
        std::unordered_set<std::string> actionNames;
        const bool valid = std::all_of(ctx.projectSettings.actions.begin(), ctx.projectSettings.actions.end(), [&](const auto& action) {
            return action.name.find_first_not_of(" \t") != std::string::npos && actionNames.insert(action.name).second;
        });
        ctx.lastFileMessage = !valid ? Translate("settings.action_invalid") :
            (ctx.saveProjectSettings && ctx.saveProjectSettings(ctx.projectSettings)
                ? Translate("project.saved") : Translate("project.write_failed"));
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    const bool hasBuildScene = std::any_of(ctx.sceneOrder.begin(), ctx.sceneOrder.end(), [&](const auto& scene) {
        return std::find(ctx.buildExcludedScenes.begin(), ctx.buildExcludedScenes.end(), scene) == ctx.buildExcludedScenes.end();
    });
    const bool canPlay = static_cast<bool>(ctx.playGame) && hasBuildScene &&
                         !ctx.runtimeBuildActive && !ctx.playActive;
    if (!canPlay) ImGui::BeginDisabled();
    // Baut die Runtime und startet sie im eigenen Fenster (wie im Export).
    if (ImGui::Button(TranslateLabel("settings.run_window", "Im Fenster").c_str(), ImVec2(actionWidth, 32.f)))
    {
        const bool ok = ctx.playGame ? ctx.playGame() : false;
        if (!ok && ctx.lastFileMessage.empty())
            ctx.lastFileMessage = Translate("status.run_failed");
    }
    if (!canPlay) ImGui::EndDisabled();

    ImGui::SameLine();
    const bool canExport = static_cast<bool>(ctx.exportProject) && hasBuildScene &&
                           !ctx.runtimeBuildActive;
    if (!canExport) ImGui::BeginDisabled();
    if (ImGui::Button(TranslateLabel("export.action", "Spiel exportieren ...").c_str(), ImVec2(actionWidth, 32.f)))
    {
        ImGui::OpenPopup(LocalizedLabel("export.title").c_str());
    }
    if (!canExport) ImGui::EndDisabled();
    if (ImGui::IsItemHovered() && canExport)
        ImGui::SetTooltip(Translate("export.output_tip"));

    ImGui::SameLine();
    if (ImGui::Button(LocalizedLabel("common.close").c_str(), ImVec2(actionWidth, 32.f)) ||
        ImGui::IsKeyPressed(ImGuiKey_Escape, false))
        ImGui::CloseCurrentPopup();

    DrawExportConfirmation(ctx);
    ImGui::EndPopup();
}

void DrawDeleteConfirmDialog(EditorFrameContext& ctx)
{
    const auto popupLabel = TranslateLabel("hierarchy.delete_title", "Objekt löschen?##editor");
    const char* popupId = popupLabel.c_str();
    if (ctx.state.deleteConfirmOpen)
    {
        ImGui::OpenPopup(popupId);
        ctx.state.deleteConfirmOpen = false;
    }

    const float scale = ImGui::GetFontSize() / 13.f;
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(.5f, .5f));
    ImGui::SetNextWindowSizeConstraints(ImVec2(480.f * scale, 0.f), ImVec2(480.f * scale, FLT_MAX));
    ImGui::SetNextWindowBgAlpha(1.f);
    ImGui::PushStyleVar(ImGuiStyleVar_Alpha, 1.f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(20.f * scale, 18.f * scale));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 6.f * scale);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.f);
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(12.f * scale, 12.f * scale));
    // Same opaque slate surface as the updated close dialog and project windows.
    ImGui::PushStyleColor(ImGuiCol_PopupBg, theme::kPanel);
    ImGui::PushStyleColor(ImGuiCol_TitleBg, theme::kWindowBg);
    ImGui::PushStyleColor(ImGuiCol_TitleBgActive, theme::kWindowBg);
    ImGui::PushStyleColor(ImGuiCol_Border, theme::kBorder);
    bool dialogOpen = true;
    if (ImGui::BeginPopupModal(popupId, &dialogOpen,
            ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove))
    {
        const auto selection = GetSelectedEntities(ctx);
        if (selection.empty())
            ImGui::CloseCurrentPopup();
        else if (selection.size() > 1)
            ImGui::TextWrapped(Translate("hierarchy.delete_multiple_prompt"), selection.size());
        else
        {
            const auto* name = ctx.world.Get<NameComponent>(selection.front());
            ImGui::TextWrapped(Translate("hierarchy.delete_prompt"),
                name && !name->name.empty() ? name->name.c_str() : "(unbenannt)");
        }
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x);
        ImGui::TextDisabled(Translate("hierarchy.delete_children"));
        ImGui::PopTextWrapPos();
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        const ImVec2 buttonSize(200.f * scale, 44.f * scale);
        const float rowWidth = 2.f * buttonSize.x + ImGui::GetStyle().ItemSpacing.x;
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + std::max(0.f, ImGui::GetContentRegionAvail().x - rowWidth));
        if (EditorActionButton(TranslateLabel("common.cancel", "Abbrechen").c_str(), buttonSize) || ImGui::IsKeyPressed(ImGuiKey_Escape, false))
            ImGui::CloseCurrentPopup();
        ImGui::SetItemDefaultFocus();
        ImGui::SameLine();
        if (EditorActionButton(TranslateLabel("common.delete", "Löschen").c_str(), buttonSize, EditorButtonTone::Danger))
        {
            DestroySelectedEntityHierarchies(ctx);
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
    ImGui::PopStyleColor(4);
    ImGui::PopStyleVar(5);
}

// ---------------------------------------------------------------------------
// Startbildschirm — wird gezeigt solange kein Projekt geladen ist
// ---------------------------------------------------------------------------
static void DrawStartProjectLoadBar(const std::string& name, float width, float scale)
{
    const std::string message = name.empty() ? Translate("start.loading_project")
        : TranslateWithValue("start.loading_project_name", name);
    const float barHeight = std::max(3.f, 5.f * scale);
    const auto& style = ImGui::GetStyle();
    const float height = ImGui::CalcTextSize(message.c_str(), nullptr, false, width).y +
                         style.ItemSpacing.y + barHeight + 2.f;
    if (ImGui::BeginChild("##StartProjectLoadBar", ImVec2(width, height), ImGuiChildFlags_None,
                          ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoScrollbar))
    {
        ImGui::TextWrapped("%s", message.c_str());
        ImGui::PushStyleColor(ImGuiCol_PlotHistogram, theme::kAccent);
        ImGui::ProgressBar(-static_cast<float>(ImGui::GetTime()), ImVec2(-1.f, barHeight), "");
        ImGui::PopStyleColor();
    }
    ImGui::EndChild();
}

// Frameless launcher window: the UI draws the title bar. Zero when the OS frame is kept.
static float StartTitleBarHeight(const EditorFrameContext& ctx, float scale)
{
    if (!ctx.windowHasCustomFrame || !ctx.windowHasCustomFrame())
        return 0.f;
    return std::round(std::max(28.f, 34.f * scale));
}

enum class StartCaptionButton { Minimize, Maximize, Restore, Close };

static void DrawStartCaptionGlyph(ImDrawList* dl, StartCaptionButton kind, const ImVec2& center,
                                  float size, float thickness, ImU32 color)
{
    // Snap to pixel centers so the 1 px strokes stay crisp.
    const ImVec2 c(std::floor(center.x) + 0.5f, std::floor(center.y) + 0.5f);
    const float h = std::floor(size * 0.5f);
    switch (kind)
    {
    case StartCaptionButton::Minimize:
        dl->AddLine(ImVec2(c.x - h, c.y), ImVec2(c.x + h + 1.f, c.y), color, thickness);
        break;
    case StartCaptionButton::Maximize:
        dl->AddRect(ImVec2(c.x - h, c.y - h), ImVec2(c.x + h, c.y + h), color, 0.f, 0, thickness);
        break;
    case StartCaptionButton::Restore:
    {
        const float o = std::max(2.f, std::floor(size * 0.2f));
        dl->AddRect(ImVec2(c.x - h, c.y - h + o), ImVec2(c.x + h - o, c.y + h), color, 0.f, 0, thickness);
        dl->AddLine(ImVec2(c.x - h + o, c.y - h + o), ImVec2(c.x - h + o, c.y - h), color, thickness);
        dl->AddLine(ImVec2(c.x - h + o, c.y - h), ImVec2(c.x + h, c.y - h), color, thickness);
        dl->AddLine(ImVec2(c.x + h, c.y - h), ImVec2(c.x + h, c.y + h - o), color, thickness);
        dl->AddLine(ImVec2(c.x + h, c.y + h - o), ImVec2(c.x + h - o, c.y + h - o), color, thickness);
        break;
    }
    case StartCaptionButton::Close:
        dl->AddLine(ImVec2(c.x - h, c.y - h), ImVec2(c.x + h + 1.f, c.y + h + 1.f), color, thickness);
        dl->AddLine(ImVec2(c.x + h, c.y - h), ImVec2(c.x - h - 1.f, c.y + h + 1.f), color, thickness);
        break;
    }
}

// Drag region of a frameless main window (main viewport pixels). Popups and open
// menus may overlap the bar; they need the clicks there, not the OS.
static void UpdateWindowCaptionArea(EditorFrameContext& ctx, float x, float y, float width, float height)
{
    if (!ctx.setWindowCaptionArea)
        return;
    if (width <= 0.f || ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel))
        ctx.setWindowCaptionArea(0.f, 0.f, 0.f, 0.f);
    else
        ctx.setWindowCaptionArea(x, y, width, height);
}

// 1 px outline of a frameless main window; a maximized window has no edge.
static void DrawFramelessWindowBorder(EditorFrameContext& ctx)
{
    if (ctx.windowIsMaximized && ctx.windowIsMaximized())
        return;
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::GetForegroundDrawList(const_cast<ImGuiViewport*>(viewport))->AddRect(
        viewport->Pos, ImVec2(viewport->Pos.x + viewport->Size.x, viewport->Pos.y + viewport->Size.y),
        theme::U32(theme::kBorder));
}

// Minimize / maximize / close in the editor theme, laid out from screenMin in the
// current window. Close asks for confirmation like the OS close does.
static void DrawWindowCaptionButtons(EditorFrameContext& ctx, const ImVec2& screenMin, float buttonW,
                                     float barH, float scale, bool closeEnabled)
{
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    // The buttons reach the window edge; menu bars clip a little short of it.
    dl->PushClipRect(viewport->Pos, ImVec2(viewport->Pos.x + viewport->Size.x, viewport->Pos.y + viewport->Size.y));
    const bool maximized = ctx.windowIsMaximized && ctx.windowIsMaximized();
    const float glyphSize = std::round(std::max(8.f, 10.f * scale));
    const float thickness = std::max(1.f, std::floor(scale));
    auto button = [&](const char* id, StartCaptionButton kind, const char* tooltipKey, int index, bool enabled) {
        ImGui::SetCursorScreenPos(ImVec2(screenMin.x + index * buttonW, screenMin.y));
        if (!enabled) ImGui::BeginDisabled();
        const bool clicked = ImGui::InvisibleButton(id, ImVec2(buttonW, barH));
        const bool hovered = enabled && ImGui::IsItemHovered();
        const bool held = enabled && ImGui::IsItemActive();
        if (!enabled) ImGui::EndDisabled();
        const ImVec2 min = ImGui::GetItemRectMin();
        const ImVec2 max = ImGui::GetItemRectMax();
        const bool close = kind == StartCaptionButton::Close;
        if (hovered || held)
        {
            ImVec4 fill = close ? theme::kDanger : (held ? theme::kButtonActive : theme::kButtonHover);
            if (close && held)
                fill.w = 0.8f;
            // Leave the separator line visible below the hover fill.
            dl->AddRectFilled(min, ImVec2(max.x, max.y - 1.f), theme::U32(fill));
        }
        const ImVec4& glyph = !enabled ? theme::kTextDisabled
            : (hovered || held) ? (close ? ImVec4(1.f, 1.f, 1.f, 1.f) : theme::kText) : theme::kTextSecondary;
        DrawStartCaptionGlyph(dl, kind, ImVec2((min.x + max.x) * 0.5f, (min.y + max.y) * 0.5f),
                              glyphSize, thickness, theme::U32(glyph));
        if (hovered && ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal))
            ImGui::SetTooltip("%s", Translate(tooltipKey));
        return clicked && enabled;
    };
    if (button("##windowMinimize", StartCaptionButton::Minimize, "start.window_minimize", 0, true) && ctx.minimizeWindow)
        ctx.minimizeWindow();
    if (button("##windowMaximize", maximized ? StartCaptionButton::Restore : StartCaptionButton::Maximize,
               maximized ? "start.window_restore" : "start.window_maximize", 1, true) &&
        ctx.toggleMaximizeWindow)
        ctx.toggleMaximizeWindow();
    if (button("##windowClose", StartCaptionButton::Close, "common.close", 2, closeEnabled))
        ctx.state.closeConfirmOpen = true;
    dl->PopClipRect();
}

// Title bar across the full width: panel background, separator line, window
// title and the window buttons. Everything but the buttons is the OS drag
// region (move, snap, double-click maximizes).
static void DrawStartTitleBar(EditorFrameContext& ctx, float barH, float margin, float scale)
{
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    const float buttonW = std::round(barH * 1.35f);
    const float buttonsW = buttonW * 3.f;

    UpdateWindowCaptionArea(ctx, 0.f, 0.f, viewport->Size.x - buttonsW, barH);
    DrawFramelessWindowBorder(ctx);

    ImGui::SetNextWindowPos(viewport->Pos, ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(viewport->Size.x, barH), ImGuiCond_Always);
    constexpr ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBackground |
        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoNav;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.f, 0.f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.f);
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0.f, 0.f));
    KeepNextWindowInMainViewport("##KromStartTitleBar");
    const bool visible = ImGui::Begin("##KromStartTitleBar", nullptr, flags);
    ImGui::PopStyleVar(3);
    if (!visible)
    {
        ImGui::End();
        return;
    }

    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 origin = ImGui::GetWindowPos();
    const float barW = viewport->Size.x;
    dl->AddRectFilled(origin, ImVec2(origin.x + barW, origin.y + barH), theme::U32(theme::kPanel));
    dl->AddLine(ImVec2(origin.x, origin.y + barH - 0.5f), ImVec2(origin.x + barW, origin.y + barH - 0.5f),
                theme::U32(theme::kBorder));
    constexpr const char* kTitle = "KROM Editor";
    dl->AddText(ImVec2(std::round(origin.x + margin),
                       std::round(origin.y + (barH - ImGui::CalcTextSize(kTitle).y) * 0.5f)),
                theme::U32(theme::kTextSecondary), kTitle);

    // Same as the launcher's Close button: blocked while a project loads.
    DrawWindowCaptionButtons(ctx, ImVec2(origin.x + barW - buttonsW, origin.y), buttonW, barH, scale,
                             ctx.state.pendingStartProjectFile.empty());
    ImGui::End();
}

void DrawNoProjectScreen(EditorFrameContext& ctx)
{
    // Modals + Close-Confirm auch ohne volles Menueband verarbeiten
    DrawCreateProjectDialog(ctx);
    DrawOpenProjectDialog(ctx);
    DrawCloseConfirmDialog(ctx);

    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    const ImVec2 size = viewport->Size;
    const float scale = std::max(0.5f, std::min(size.x / 1272.f, size.y / 744.f));
    const float margin = 30.f * scale;
    const float launcherW = size.x * 0.645f;
    const float titleBarH = StartTitleBarHeight(ctx, scale);
    ImGui::SetNextWindowPos(viewport->Pos, ImGuiCond_Always);
    ImGui::SetNextWindowSize(size, ImGuiCond_Always);
    constexpr ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration |
        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoSavedSettings |
        ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoScrollbar |
        ImGuiWindowFlags_NoScrollWithMouse;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.f, 0.f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.f);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, GetEditorPreferences().lightTheme ? theme::kWindowBg : ImVec4(0.f, 0.f, 0.f, 1.f));
    KeepNextWindowInMainViewport("##KromStartScreen");
    const bool visible = ImGui::Begin("##KromStartScreen", nullptr, flags);
    ImGui::PopStyleColor();
    ImGui::PopStyleVar(3);
    if (!visible)
    {
        ImGui::End();
        return;
    }

    static std::string folderPath;
    static TextureHandle folderTexture;
    void* folderIcon = GetStartEditorTextureId(ctx, "folder.png", folderPath, folderTexture);

    ImGui::SetCursorPos(ImVec2(margin, std::max(36.f * scale, titleBarH + 16.f * scale)));
    ImGui::SetWindowFontScale(1.8f);
    ImGui::TextUnformatted(Translate("start.projects"));
    ImGui::SetWindowFontScale(1.f);
    ImGui::SetCursorPosX(margin);
    ImGui::TextDisabled("%s", ctx.state.projectParentDirBuffer.data());

    const float contentW = launcherW - margin - 20.f * scale;
    const float buttonY = size.y - 126.f * scale;
    const float listY = ImGui::GetCursorPosY() + 8.f * scale;
    ImGui::SetCursorPos(ImVec2(margin, listY));
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 6.f * scale);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(16.f * scale, 16.f * scale));
    ImGui::PushStyleColor(ImGuiCol_ChildBg, theme::kPanel);
    ImGui::BeginChild("##StartProjectList", ImVec2(contentW, std::max(60.f, buttonY - listY - 12.f * scale)), true);
    static std::vector<StartProjectEntry> projects;
    static std::string scannedParent;
    if (ctx.state.pendingStartProjectFile.empty() || scannedParent != ctx.state.projectParentDirBuffer.data())
    {
        scannedParent = ctx.state.projectParentDirBuffer.data();
        projects = ScanStartProjects(ctx);
    }
    bool drewLoadBar = false;
    if (projects.empty())
        ImGui::TextDisabled("%s", Translate("start.empty"));
    for (const StartProjectEntry& project : projects)
    {
        ImGui::PushID(project.projectFile.c_str());
        const ImVec2 row = ImGui::GetCursorScreenPos();
        const float width = ImGui::GetContentRegionAvail().x;
        const bool loading = !ctx.state.pendingStartProjectFile.empty();
        const bool selected = ctx.state.pendingStartProjectFile == project.projectFile;
        const ImGuiSelectableFlags projectFlags = ImGuiSelectableFlags_AllowDoubleClick |
            (loading ? ImGuiSelectableFlags_Disabled : ImGuiSelectableFlags_None);
        if (ImGui::Selectable("##project", selected, projectFlags, ImVec2(0.f, 80.f * scale)))
        {
            ctx.lastFileMessage.clear();
            ctx.state.pendingStartProjectFile = project.projectFile;
        }
        ImDrawList* dl = ImGui::GetWindowDrawList();
        if (folderIcon)
            dl->AddImage(reinterpret_cast<ImTextureID>(folderIcon),
                         ImVec2(row.x + 8.f * scale, row.y + 20.f * scale),
                         ImVec2(row.x + 56.f * scale, row.y + 68.f * scale));
        const float metadataW = std::max(205.f * scale, ImGui::CalcTextSize(project.lastOpened.c_str()).x);
        const float metadataX = row.x + width - metadataW - 8.f * scale;
        const bool showMetadata = metadataX > row.x + 200.f * scale;
        dl->PushClipRect(ImVec2(row.x + 72.f * scale, row.y),
                         ImVec2(showMetadata ? metadataX - 12.f * scale : row.x + width, row.y + 80.f * scale), true);
        dl->AddText(ImVec2(row.x + 78.f * scale, row.y + 22.f * scale),
                    ImGui::GetColorU32(ImGuiCol_Text), project.name.c_str());
        dl->AddText(ImVec2(row.x + 78.f * scale, row.y + 46.f * scale),
                    ImGui::GetColorU32(theme::kTextSecondary), project.path.c_str());
        dl->PopClipRect();
        if (showMetadata)
        {
            dl->AddText(ImVec2(metadataX, row.y + 22.f * scale),
                        ImGui::GetColorU32(ImGuiCol_Text), project.date.c_str());
            dl->AddText(ImVec2(metadataX, row.y + 46.f * scale),
                        ImGui::GetColorU32(ImGuiCol_Text), project.lastOpened.c_str());
        }
        if (ctx.state.pendingStartProjectFile.empty() && ImGui::IsItemHovered())
            ImGui::SetTooltip("%s\n%s\n%s", project.path.c_str(), project.date.c_str(), project.lastOpened.c_str());
        dl->AddLine(ImVec2(row.x, row.y + 80.f * scale),
                    ImVec2(row.x + width, row.y + 80.f * scale), ImGui::GetColorU32(ImGuiCol_Border));
        ImGui::PopID();
        if (ctx.state.pendingStartProjectFile == project.projectFile)
        {
            DrawStartProjectLoadBar(project.name, width, scale);
            if (!loading)
                ImGui::SetScrollHereY(1.f);
            drewLoadBar = true;
        }
    }
    ImGui::EndChild();
    ImGui::PopStyleColor();
    ImGui::PopStyleVar(2);

    const float gap = 12.f * scale;
    const float buttonW = (contentW - gap * 3.f) / 4.f;
    const float buttonH = 48.f * scale;
    auto button = [&](const char* label, const char* icon, int index) {
        ImGui::SetCursorPos(ImVec2(margin + index * (buttonW + gap), buttonY));
        ImGui::PushID(index);
        const bool clicked = ImGui::Button("##action", ImVec2(buttonW, buttonH));
        const ImVec2 pos = ImGui::GetItemRectMin();
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const ImVec2 textSize = ImGui::CalcTextSize(label);
        const float iconSpace = 30.f * scale;
        const bool showIcon = textSize.x + iconSpace + 12.f * scale <= buttonW;
        const float textX = pos.x + std::max(4.f * scale,
            (buttonW - textSize.x + (showIcon ? iconSpace : 0.f)) * .5f);
        dl->PushClipRect(pos, ImGui::GetItemRectMax(), true);
        if (showIcon)
            DrawIcon(dl, icon, ImVec2(textX - 16.f * scale, pos.y + buttonH * .5f), 22.f * scale,
                     ImGui::GetColorU32(ImGuiCol_Text));
        dl->AddText(ImVec2(textX,
                           pos.y + (buttonH - textSize.y) * 0.5f), ImGui::GetColorU32(ImGuiCol_Text), label);
        dl->PopClipRect();
        if (ctx.state.pendingStartProjectFile.empty() && textSize.x + 8.f * scale > buttonW &&
            ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("%s", label);
        ImGui::PopID();
        return clicked;
    };
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 5.f * scale);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.f);
    ImGui::PushStyleColor(ImGuiCol_Button, theme::kPanel);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, theme::kMenuHover);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, theme::kMenuOpen);
    ImGui::BeginDisabled(!ctx.state.pendingStartProjectFile.empty());
    const bool open = button(Translate("project.open"), ICON_LC_FOLDER_OPEN, 0);
    if (open)
    {
        if (ctx.browseForProjectFile)
        {
            const std::string picked = ctx.browseForProjectFile();
            if (!picked.empty())
            {
                ctx.lastFileMessage.clear();
                ctx.state.pendingStartProjectFile = picked;
            }
        }
        else
            ctx.state.openProjectDialogOpen = true;
    }
    if (button(Translate("project.new"), ICON_LC_PLUS, 1))
        ctx.state.createProjectDialogOpen = true;
    if (!ctx.browseForProjectFolder) ImGui::BeginDisabled();
    const bool changeFolder = button(Translate("start.change_folder"), ICON_LC_FOLDER, 2);
    if (!ctx.browseForProjectFolder) ImGui::EndDisabled();
    if (changeFolder && ctx.browseForProjectFolder)
    {
        const std::string picked = ctx.browseForProjectFolder();
        if (!picked.empty())
            std::snprintf(ctx.state.projectParentDirBuffer.data(),
                          ctx.state.projectParentDirBuffer.size(), "%s", picked.c_str());
    }
    if (button(Translate("common.close"), ICON_LC_X, 3))
        ctx.state.closeConfirmOpen = true;
    ImGui::EndDisabled();
    ImGui::PopStyleColor(3);
    ImGui::PopStyleVar(2);
    ImGui::SetCursorPos(ImVec2(margin, buttonY + buttonH + 14.f * scale));
    if (!ctx.state.pendingStartProjectFile.empty() && !drewLoadBar)
    {
        const auto path = PathFromUtf8(ctx.state.pendingStartProjectFile);
        const auto name = PathToUtf8((path.filename() == "krom-project.json" ? path.parent_path() : path).filename());
        DrawStartProjectLoadBar(name, contentW, scale);
    }
    else
        ImGui::TextDisabled("Version 0.5");
    if (!ctx.lastFileMessage.empty())
    {
        ImGui::SetCursorPosX(margin);
        ImGui::TextWrapped("%s", ctx.lastFileMessage.c_str());
    }
    // The preview starts below the title bar so the bar reads as its own strip.
    ImGui::SetCursorPos(ImVec2(launcherW, titleBarH));
    DrawStartEnginePreview(ctx, ImVec2(size.x - launcherW, size.y - titleBarH));
    ImGui::End();
    if (titleBarH > 0.f)
        DrawStartTitleBar(ctx, titleBarH, margin, scale);

}

// Settings pages follow the inspector: cards with label-left rows on one grid.
static void SettingsCard(const char* id, const char* titleKey, const char* glyph, const std::function<void()>& body)
{
    card::Card settingsCard;
    if (card::Begin(settingsCard, id, Translate(titleKey), glyph, IM_COL32(125, 185, 225, 255)))
        body();
    card::End(settingsCard);
}

static void SettingsTip(const char* key)
{
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    ImGui::TextWrapped("%s", Translate(key));
    ImGui::PopStyleColor();
}

static void DrawEditorGeneralSettings(EditorFrameContext& ctx)
{
    auto& preferences = ctx.state.editorPreferences;
    SettingsCard("##settings_general", "settings.general", ICON_LC_SETTINGS, [&] {
        value_ui::RowLabel(Translate("settings.interface_language"));
        if (ImGui::BeginCombo("##interface_language", GetEditorLanguage() == EditorLanguage::German ? "Deutsch" : "English"))
        {
            DrawLanguageChoices(ctx);
            ImGui::EndCombo();
        }
        const char* themes[] = {Translate("settings.theme_dark"), Translate("settings.theme_light")};
        int theme = preferences.lightTheme ? 1 : 0;
        if (value_ui::Combo(LocalizedLabel("settings.theme").c_str(), &theme, themes, IM_ARRAYSIZE(themes)))
            preferences.lightTheme = theme == 1;
        int scale = static_cast<int>(std::lround(preferences.uiScale * 100.f));
        if (value_ui::SliderInt(LocalizedLabel("settings.ui_scale").c_str(), &scale, 80, 150, "%d%%", ImGuiSliderFlags_AlwaysClamp))
            preferences.uiScale = scale / 100.f;
        int fontSize = static_cast<int>(std::lround(preferences.fontSize));
        if (value_ui::SliderInt(LocalizedLabel("settings.font_size").c_str(), &fontSize, 12, 24, "%d px", ImGuiSliderFlags_AlwaysClamp))
            preferences.fontSize = static_cast<float>(fontSize);
        value_ui::Checkbox(LocalizedLabel("settings.reopen_project").c_str(), &preferences.reopenProject);
    });
}

static void DrawEditorSnapSettings(EditorFrameContext& ctx)
{
    auto& snap = ctx.state.snap;
    SettingsCard("##settings_grid", "toolbar.grid", ICON_LC_GRID_3X3, [&] {
        value_ui::Checkbox(LocalizedLabel("toolbar.grid").c_str(), &snap.showGrid);
    });
    SettingsCard("##settings_snap", "toolbar.snap", ICON_LC_MAGNET, [&] {
        value_ui::Checkbox(LocalizedLabel("settings.snap_move_enabled").c_str(), &snap.moveEnabled);
        value_ui::DragFloat(LocalizedLabel("settings.move_step").c_str(), &snap.moveStep, .01f, .001f, 1000.f, "%.3f m", ImGuiSliderFlags_AlwaysClamp);
        value_ui::Checkbox(LocalizedLabel("settings.snap_rotate_enabled").c_str(), &snap.rotateEnabled);
        value_ui::DragFloat(LocalizedLabel("settings.rotation_step").c_str(), &snap.rotateStepDeg, 1.f, .01f, 360.f, "%.2f°", ImGuiSliderFlags_AlwaysClamp);
        value_ui::Checkbox(LocalizedLabel("settings.snap_scale_enabled").c_str(), &snap.scaleEnabled);
        value_ui::DragFloat(LocalizedLabel("settings.scale_step").c_str(), &snap.scaleStep, .01f, .001f, 1000.f, "%.3f", ImGuiSliderFlags_AlwaysClamp);
    });
    SettingsCard("##settings_snap_presets", "settings.snap_presets", ICON_LC_LAYERS, [&] {
        constexpr const char* presets[] = {"settings.snap_fine", "settings.snap_coarse"};
        for (size_t i = 0; i < std::size(presets); ++i)
        {
            auto& preset = ctx.state.editorPreferences.snapPresets[i];
            ImGui::PushID(static_cast<int>(i));
            value_ui::RowLabel(Translate(presets[i]), nullptr, false);
            if (ImGui::Button(LocalizedLabel("settings.snap_apply").c_str()))
            {
                snap.moveStep = preset[0];
                snap.rotateStepDeg = preset[1];
                snap.scaleStep = preset[2];
            }
            ImGui::SameLine();
            if (ImGui::Button(LocalizedLabel("settings.capture_steps").c_str()))
                preset = {snap.moveStep, snap.rotateStepDeg, snap.scaleStep};
            ImGui::PopID();
        }
    });
}

// Panel order in saved layout slots ("; KROM panels " + one digit per panel).
// Older slots have 3, 4 or 6 digits; missing panels count as open.
static bool* EditorLayoutPanelFlags(EditorFrameContext& ctx, size_t index)
{
    bool* flags[] = {&ctx.hierarchyPanelVisible, &ctx.assetsPanelVisible, &ctx.inspectorPanelVisible,
                     &ctx.consolePanelVisible, &ctx.viewportPanelVisible, &ctx.gamePanelVisible,
                     &ctx.sceneSettingsPanelVisible};
    return index < std::size(flags) ? flags[index] : nullptr;
}

static void ResetEditorLayout(EditorFrameContext& ctx)
{
    for (size_t i = 0; bool* flag = EditorLayoutPanelFlags(ctx, i); ++i)
        *flag = true;
    ctx.resetDockLayout = true;
}

// Asks before the layout is reset. Drawn at the popup level of the caller, so the
// settings dialog keeps its own copy (opening it at top level would close that modal).
static void DrawResetLayoutConfirm(EditorFrameContext& ctx, bool request)
{
    const std::string title = std::string(Translate("menu.reset_layout")) + "###reset_layout_confirm";
    if (request)
        ImGui::OpenPopup(title.c_str());

    ImGui::SetNextWindowBgAlpha(1.f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(20.f, 18.f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 6.f);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 5.f);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(10.f, 8.f));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(12.f, 10.f));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.f);
    ImGui::PushStyleColor(ImGuiCol_PopupBg, theme::kPanel);
    ImGui::PushStyleColor(ImGuiCol_TitleBg, theme::kWindowBg);
    ImGui::PushStyleColor(ImGuiCol_TitleBgActive, theme::kWindowBg);
    ImGui::PushStyleColor(ImGuiCol_Button, theme::kPanel);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, theme::kMenuHover);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, theme::kMenuOpen);
    bool dialogOpen = true;
    if (ImGui::BeginPopupModal(title.c_str(), &dialogOpen, ImGuiWindowFlags_AlwaysAutoResize))
    {
        ImGui::TextUnformatted(Translate("menu.reset_layout_confirm"));
        ImGui::Separator();
        ImGui::Spacing();
        ImGui::PushStyleColor(ImGuiCol_Button, theme::kAccent);
        const bool ok = ImGui::Button(LocalizedLabel("common.ok").c_str(), ImVec2(130.f, 32.f));
        ImGui::PopStyleColor();
        if (ok)
        {
            ResetEditorLayout(ctx);
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button(LocalizedLabel("common.cancel").c_str(), ImVec2(130.f, 32.f)) ||
            ImGui::IsKeyPressed(ImGuiKey_Escape, false))
            ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
    ImGui::PopStyleColor(6);
    ImGui::PopStyleVar(6);
}

static void DrawEditorWorkflowSettings(EditorFrameContext& ctx)
{
    auto& preferences = ctx.state.editorPreferences;
    SettingsCard("##settings_saving", "settings.saving", ICON_LC_SAVE, [&] {
        value_ui::Checkbox(LocalizedLabel("settings.auto_save").c_str(), &preferences.autoSaveEditors);
        SettingsTip("settings.auto_save_tip");
        value_ui::SliderInt(LocalizedLabel("settings.backup_interval").c_str(), &preferences.backupMinutes, 0, 120, "%d min", ImGuiSliderFlags_AlwaysClamp);
        SettingsTip("settings.backup_tip");
    });
    SettingsCard("##settings_layouts", "settings.layouts", ICON_LC_LAYOUT_TEMPLATE, [&] {
        static int slot = 0;
        const char* slots[] = {"1", "2", "3"};
        value_ui::Combo(LocalizedLabel("settings.layout_slot").c_str(), &slot, slots, IM_ARRAYSIZE(slots));
        const auto directory = EditorPreferencesDirectory();
        const auto file = directory.empty() ? std::filesystem::path{} : directory / ("layout-" + std::to_string(slot + 1) + ".ini");
        value_ui::RowLabel("", nullptr, false);
        if (ImGui::Button(LocalizedLabel("settings.save_layout").c_str()))
        {
            size_t bytes = 0;
            const char* ini = ImGui::SaveIniSettingsToMemory(&bytes);
            std::string layout = "; KROM panels ";
            for (size_t i = 0; const bool* flag = EditorLayoutPanelFlags(ctx, i); ++i)
                layout += *flag ? '1' : '0';
            layout += '\n';
            layout.append(ini, bytes);
            ctx.lastFileMessage = Translate(WriteTextFileAtomically(file, layout) ? "status.saved" : "status.save_failed");
        }
        ImGui::SameLine();
        std::error_code error;
        ImGui::BeginDisabled(!std::filesystem::is_regular_file(file, error));
        if (ImGui::Button(LocalizedLabel("settings.load_layout").c_str()))
        {
            std::ifstream in(file, std::ios::binary);
            if (in)
            {
                ctx.state.pendingLayoutIni.assign(std::istreambuf_iterator<char>(in), {});
                constexpr std::string_view prefix = "; KROM panels ";
                const auto& layout = ctx.state.pendingLayoutIni;
                const size_t end = layout.find('\n', prefix.size());
                const size_t count = end == std::string::npos ? 0u : end - prefix.size();
                if (layout.starts_with(prefix) && (count == 3u || count == 4u || count == 6u || count == 7u))
                {
                    for (size_t i = 0; bool* flag = EditorLayoutPanelFlags(ctx, i); ++i)
                        *flag = i >= count || layout[prefix.size() + i] == '1';
                    ctx.dockSceneSettingsWithInspector = count < 7u;
                }
            }
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        DrawResetLayoutConfirm(ctx, ImGui::Button(LocalizedLabel("menu.reset_layout").c_str()));
    });
    SettingsCard("##settings_shortcuts", "settings.shortcuts", ICON_LC_TERMINAL, [&] {
        constexpr const char* labels[] = {"menu.save_scene", "menu.undo", "menu.redo", "settings.shortcut_move", "settings.shortcut_rotate", "settings.shortcut_scale"};
        for (size_t i = 0; i < std::size(labels); ++i)
        {
            const char* prefix = i < 3 ? "Ctrl+" : "";
            const auto current = EditorShortcutKey(preferences, i);
            const std::string preview = std::string(prefix) + ImGui::GetKeyName(current);
            ImGui::PushID(static_cast<int>(i));
            value_ui::RowLabel(Translate(labels[i]));
            if (ImGui::BeginCombo("##shortcut", preview.c_str()))
            {
                for (int key = ImGuiKey_NamedKey_BEGIN; key < ImGuiKey_NamedKey_END; ++key)
                {
                    const auto candidate = static_cast<ImGuiKey>(key);
                    if (!IsEditorShortcutKey(candidate) || (i >= 3 && candidate == ImGuiKey_G)) continue;
                    bool occupied = false;
                    const size_t begin = i < 3 ? 0 : 3;
                    for (size_t other = begin; other < begin + 3; ++other)
                        occupied |= other != i && EditorShortcutKey(preferences, other) == candidate;
                    const std::string label = std::string(prefix) + ImGui::GetKeyName(candidate);
                    if (ImGui::Selectable(label.c_str(), current == candidate, occupied ? ImGuiSelectableFlags_Disabled : 0))
                        preferences.shortcutKeys[i] = key;
                }
                ImGui::EndCombo();
            }
            ImGui::PopID();
        }
        value_ui::RowLabel("", nullptr, false);
        if (ImGui::Button(LocalizedLabel("settings.reset_shortcuts").c_str()))
            preferences.shortcutKeys = {};
    });
}

static void DrawEditorViewportSettings(EditorFrameContext& ctx)
{
    EditorCameraState& cam = ctx.state.editorCamera;
    SettingsCard("##settings_camera", "component.camera", ICON_LC_CAMERA, [&] {
        value_ui::DragFloat("FOV##CameraFOV", &cam.fovDeg, 0.5f, 10.f, 170.f, "%.1f°");
        value_ui::DragFloat((std::string(Translate("camera.near")) + "##CameraNear").c_str(), &cam.nearPlane, 0.001f, 0.001f, 10.f, "%.3f m");
        value_ui::DragFloat((std::string(Translate("camera.far")) + "##CameraFar").c_str(), &cam.farPlane, 10.f, 10.f, 100000.f, "%.0f m");
        value_ui::DragFloat((std::string(Translate("camera.speed")) + "##CameraSpeed").c_str(), &cam.moveSpeed, 1.f, 0.1f, 10000.f, "%.1f");
        value_ui::RowLabel("", nullptr, false);
        if (ImGui::Button(LocalizedLabel("settings.reset_camera").c_str()))
        {
            cam.fovDeg = 60.f;
            cam.nearPlane = 0.05f;
            cam.farPlane = 2000.f;
            cam.moveSpeed = 50.f;
        }
    });
    cam.farPlane  = std::max(cam.nearPlane + 1.f, cam.farPlane);
    cam.nearPlane = std::clamp(cam.nearPlane, 0.001f, cam.farPlane - 1.f);
    cam.fovDeg    = std::clamp(cam.fovDeg, 10.f, 170.f);
    cam.moveSpeed = std::max(0.1f, cam.moveSpeed);

    SettingsCard("##settings_viewport", "settings.viewport", ICON_LC_MONITOR, [&] {
        if (value_ui::Checkbox(TranslateLabel("settings.enable_ibl", "IBL aktiv").c_str(), &ctx.state.environmentEnableIBL))
        {
            ctx.state.ambientSource = ctx.state.environmentEnableIBL
                ? EditorState::AmbientSource::Skybox : EditorState::AmbientSource::Color;
            ApplyEnvironmentFromState(ctx);
            if (ctx.setEditorAmbientLight)
                ctx.setEditorAmbientLight(ctx.state.ambientColor, ctx.state.ambientIntensity);
        }
        value_ui::Checkbox(TranslateLabel("settings.show_skybox", "Skybox anzeigen").c_str(), &ctx.state.showSkyboxBackground);
        value_ui::Checkbox(TranslateLabel("settings.show_performance", "Performance-Overlay anzeigen").c_str(), &ctx.state.showPerfOverlay);
        value_ui::Checkbox(LocalizedLabel("settings.show_profiler").c_str(), &ctx.state.showProfiler);
        value_ui::Checkbox(TranslateLabel("settings.show_selection_outline", "Auswahl-Outline anzeigen").c_str(), &ctx.state.showSelectionOutline);
        value_ui::Checkbox(LocalizedLabel("toolbar.icons").c_str(), &ctx.state.showEditorIcons);
        value_ui::Checkbox(LocalizedLabel("toolbar.preview").c_str(), &ctx.state.showCameraPreview);
        value_ui::Checkbox(LocalizedLabel("toolbar.particles").c_str(), &ctx.state.playAllParticles);
        // Reflection probe diagnostics (not saved).
        const char* iblViews[] = { Translate("settings.ibl_debug_normal"), Translate("settings.ibl_debug_diffuse"),
                                  Translate("settings.ibl_debug_specular") };
        const uint32_t iblFlags[] = { 0u, DBG_VIEW_IBL_DIFF, DBG_VIEW_IBL_SPEC };
        int iblView = ctx.state.editorDebugFlags == DBG_VIEW_IBL_DIFF ? 1
                    : ctx.state.editorDebugFlags == DBG_VIEW_IBL_SPEC ? 2 : 0;
        if (value_ui::Combo(LocalizedLabel("settings.ibl_debug").c_str(), &iblView, iblViews, IM_ARRAYSIZE(iblViews)))
        {
            ctx.state.editorDebugFlags = iblFlags[iblView];
            if (ctx.setEditorDebugFlags)
                ctx.setEditorDebugFlags(iblFlags[iblView]);
        }
        bool shadowCascadeView = ctx.state.editorDebugFlags == DBG_VIEW_SHADOW_CASCADE;
        if (value_ui::Checkbox(LocalizedLabel("settings.shadow_cascade_debug").c_str(), &shadowCascadeView))
        {
            ctx.state.editorDebugFlags = shadowCascadeView ? DBG_VIEW_SHADOW_CASCADE : 0u;
            if (ctx.setEditorDebugFlags)
                ctx.setEditorDebugFlags(ctx.state.editorDebugFlags);
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s", Translate("settings.shadow_cascade_debug_tip"));
        const char* probeViews[] = { Translate("probe.debug_none"), Translate("probe.debug_index"),
                                     Translate("probe.debug_weight"), Translate("probe.debug_so") };
        int probeView = static_cast<int>(std::min(ctx.state.probeDebugView, 3u));
        if (value_ui::Combo(LocalizedLabel("probe.debug_view").c_str(), &probeView, probeViews, IM_ARRAYSIZE(probeViews)))
            ctx.state.probeDebugView = static_cast<uint32_t>(probeView);
        // Surface cache GI test (packages 2-4): BVH trace on the GPU, radiance cache,
        // shading, debug views, measuring.
        if (!ctx.surfaceCacheAvailable) ImGui::BeginDisabled();
        value_ui::Checkbox(LocalizedLabel("surface_cache.enabled").c_str(), &ctx.state.surfaceCacheEnabled);
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("%s", Translate("surface_cache.enabled_tip"));
        if (ctx.state.surfaceCacheEnabled && ctx.surfaceCacheAvailable)
        {
            value_ui::Checkbox(LocalizedLabel("surface_cache.apply").c_str(), &ctx.state.surfaceCacheApply);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("%s", Translate("surface_cache.apply_tip"));
            if (ctx.state.surfaceCacheApply)
            {
                value_ui::SliderFloat(LocalizedLabel("surface_cache.strength").c_str(), &ctx.state.surfaceCacheStrength,
                                      0.f, 2.f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("%s", Translate("surface_cache.strength_tip"));
            }
            const char* giViews[] = { Translate("surface_cache.view_none"), Translate("surface_cache.view_normal"),
                                      Translate("surface_cache.view_distance"), Translate("surface_cache.view_instance"),
                                      Translate("surface_cache.view_nodes"), Translate("surface_cache.view_cells"),
                                      Translate("surface_cache.view_irradiance"), Translate("surface_cache.view_samples"),
                                      Translate("surface_cache.view_shaded_irradiance"),
                                      Translate("surface_cache.view_shaded_indirect"),
                                      Translate("surface_cache.view_shaded_coverage"),
                                      Translate("surface_cache.view_shaded_specular") };
            int giView = static_cast<int>(std::min(ctx.state.surfaceCacheDebugView, 11u));
            if (value_ui::Combo(LocalizedLabel("surface_cache.debug_view").c_str(), &giView, giViews, IM_ARRAYSIZE(giViews)))
                ctx.state.surfaceCacheDebugView = static_cast<uint32_t>(giView);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("%s", Translate("surface_cache.debug_view_tip"));
            if (ctx.state.surfaceCacheDebugView < 8u)
                value_ui::SliderFloat(LocalizedLabel("surface_cache.overlay").c_str(), &ctx.state.surfaceCacheOverlay,
                                      0.f, 1.f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
            if (ctx.state.surfaceCacheDebugView == 2u)
                value_ui::SliderFloat(LocalizedLabel("surface_cache.distance").c_str(), &ctx.state.surfaceCacheDistance,
                                      1.f, 500.f, "%.0f m", ImGuiSliderFlags_Logarithmic | ImGuiSliderFlags_AlwaysClamp);
            value_ui::SliderInt(LocalizedLabel("surface_cache.ray_budget").c_str(), &ctx.state.surfaceCacheRayBudgetK,
                                16, 1024, "%d k", ImGuiSliderFlags_Logarithmic | ImGuiSliderFlags_AlwaysClamp);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("%s", Translate("surface_cache.ray_budget_tip"));
            value_ui::SliderFloat(LocalizedLabel("surface_cache.ray_length").c_str(), &ctx.state.surfaceCacheRayLength,
                                  0.f, 100.f, "%.0f m", ImGuiSliderFlags_AlwaysClamp);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("%s", Translate("surface_cache.ray_length_tip"));
            value_ui::SliderFloat(LocalizedLabel("surface_cache.cell_size").c_str(), &ctx.state.surfaceCacheCellSize,
                                  0.1f, 4.f, "%.2f m", ImGuiSliderFlags_Logarithmic | ImGuiSliderFlags_AlwaysClamp);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("%s", Translate("surface_cache.cell_size_tip"));
            value_ui::SliderInt(LocalizedLabel("surface_cache.history").c_str(), &ctx.state.surfaceCacheHistory,
                                32, 2048, "%d", ImGuiSliderFlags_Logarithmic | ImGuiSliderFlags_AlwaysClamp);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("%s", Translate("surface_cache.history_tip"));
            value_ui::RowLabel("", nullptr, false);
            if (ctx.surfaceCacheBenchRunning) ImGui::BeginDisabled();
            if (ImGui::Button(LocalizedLabel("surface_cache.bench").c_str()) && ctx.startSurfaceCacheBench)
                ctx.startSurfaceCacheBench();
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                ImGui::SetTooltip("%s", Translate("surface_cache.bench_tip"));
            ImGui::SameLine();
            if (ImGui::Button(LocalizedLabel("surface_cache.check").c_str()) && ctx.checkSurfaceCache)
                ctx.checkSurfaceCache();
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                ImGui::SetTooltip("%s", Translate("surface_cache.check_tip"));
            ImGui::SameLine();
            if (ImGui::Button(LocalizedLabel("surface_cache.reset").c_str()) && ctx.resetSurfaceCache)
                ctx.resetSurfaceCache();
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                ImGui::SetTooltip("%s", Translate("surface_cache.reset_tip"));
            if (ctx.surfaceCacheBenchRunning) ImGui::EndDisabled();
            if (!ctx.surfaceCacheStatus.empty())
                ImGui::TextDisabled("%s", ctx.surfaceCacheStatus.c_str());
            if (!ctx.surfaceCacheCheckResult.empty())
                ImGui::TextWrapped("%s", ctx.surfaceCacheCheckResult.c_str());
            for (const std::string& line : ctx.surfaceCacheBenchResults)
                ImGui::TextWrapped("%s", line.c_str());
        }
        if (!ctx.surfaceCacheAvailable) ImGui::EndDisabled();
        if (value_ui::ColorEdit3(LocalizedLabel("settings.background").c_str(), ctx.state.backgroundColor.data()))
        {
            if (ctx.setEditorBackgroundColor)
                ctx.setEditorBackgroundColor(ctx.state.backgroundColor);
        }
    });
    auto& preferences = ctx.state.editorPreferences;
    SettingsCard("##settings_controls", "settings.controls", ICON_LC_MOUSE_POINTER_2, [&] {
        value_ui::SliderFloat(LocalizedLabel("settings.mouse_sensitivity").c_str(), &preferences.mouseSensitivity, .1f, 4.f, "%.2fx", ImGuiSliderFlags_AlwaysClamp);
        value_ui::Checkbox(LocalizedLabel("settings.invert_mouse_y").c_str(), &preferences.invertMouseY);
        value_ui::SliderFloat(LocalizedLabel("settings.gizmo_size").c_str(), &preferences.gizmoScale, .5f, 2.f, "%.2fx", ImGuiSliderFlags_AlwaysClamp);
    });
}

static int ResizeStringCallback(ImGuiInputTextCallbackData* data)
{
    if (data->EventFlag == ImGuiInputTextFlags_CallbackResize)
    {
        auto* text = static_cast<std::string*>(data->UserData);
        text->resize(static_cast<size_t>(data->BufTextLen));
        data->Buf = text->data();
    }
    return 0;
}

// Pasted paths often carry quotes ("Copy as path") or stray spaces.
static void CleanToolPath(std::string& path)
{
    constexpr const char* kTrim = " \t\"";
    const auto first = path.find_first_not_of(kTrim);
    const auto last = path.find_last_not_of(kTrim);
    path = first == std::string::npos ? std::string{} : path.substr(first, last - first + 1);
}

static void DrawToolPathRow(EditorFrameContext& ctx, const char* labelKey, const char* id,
                            std::string& path, bool folder)
{
    value_ui::RowLabel(Translate(labelKey), nullptr, false);
    const std::string browse = LocalizedLabel("settings.tool_browse") + "##" + id;
    const float browseWidth = ImGui::CalcTextSize(browse.c_str(), nullptr, true).x +
        ImGui::GetStyle().FramePadding.x * 2.f;
    ImGui::SetNextItemWidth(ctx.browseForToolPath
        ? -(browseWidth + ImGui::GetStyle().ItemSpacing.x) : -FLT_MIN);
    const std::string inputId = std::string("##") + id;
    ImGui::InputTextWithHint(inputId.c_str(), Translate("settings.tool_automatic"), path.data(),
                             path.capacity() + 1, ImGuiInputTextFlags_CallbackResize,
                             ResizeStringCallback, &path);
    if (ImGui::IsItemDeactivatedAfterEdit())
        CleanToolPath(path);
    if (!ctx.browseForToolPath)
        return;
    ImGui::SameLine();
    if (ImGui::Button(browse.c_str()))
    {
        std::string picked = ctx.browseForToolPath(folder);
        CleanToolPath(picked);
        if (!picked.empty())
            path = std::move(picked);
    }
}

static void DrawEditorBuildToolSettings(EditorFrameContext& ctx)
{
    auto& preferences = ctx.state.editorPreferences;
    SettingsCard("##settings_build_tools", "settings.build_tools", ICON_LC_WRENCH, [&] {
        SettingsTip("settings.build_tools_tip");
        DrawToolPathRow(ctx, "settings.tool_visual_studio", "tool_vs", preferences.toolVisualStudio, true);
        DrawToolPathRow(ctx, "settings.tool_cmake", "tool_cmake", preferences.toolCMake, false);
        DrawToolPathRow(ctx, "settings.tool_ninja", "tool_ninja", preferences.toolNinja, false);
        DrawToolPathRow(ctx, "settings.tool_dxc", "tool_dxc", preferences.toolDxc, false);
        DrawToolPathRow(ctx, "settings.tool_glslang", "tool_glslang", preferences.toolGlslang, false);

        if (!ctx.startBuildToolsCheck || !ctx.buildToolsCheckResult)
            return;
        std::string report;
        bool ok = false;
        const bool running = ctx.buildToolsCheckResult(report, ok);
        value_ui::RowLabel("", nullptr, false);
        ImGui::BeginDisabled(running);
        if (ImGui::Button(LocalizedLabel("settings.tool_check").c_str()))
            ctx.startBuildToolsCheck(preferences);
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::TextDisabled("%s", Translate(running ? "settings.tool_checking" : "settings.tool_check_hint"));
        if (!running && !report.empty())
        {
            if (!ok) ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.f, .45f, .4f, 1.f));
            ImGui::TextWrapped("%s", report.c_str());
            if (!ok) ImGui::PopStyleColor();
        }
    });
}

static void DrawEditorExternalTools(EditorFrameContext& ctx)
{
    SettingsCard("##settings_code_editor", "settings.code_editor", ICON_LC_CODE, [&] {
        value_ui::InputText(LocalizedLabel("settings.executable").c_str(), ctx.state.codeEditorExecutableBuffer.data(),
                            ctx.state.codeEditorExecutableBuffer.size());
        value_ui::InputText(LocalizedLabel("settings.arguments").c_str(), ctx.state.codeEditorArgumentsBuffer.data(),
                            ctx.state.codeEditorArgumentsBuffer.size());
        struct CodeEditorPreset { const char* label; const char* program; const char* arguments; };
        const CodeEditorPreset presets[] = {
            {"VS Code", "code", "\"{workspace}\" -g \"{file}:{line}\""},
            {"Visual Studio", "devenv", "/Edit \"{file}\""},
            {"Rider", "rider", "--line {line} \"{file}\""},
            {Translate("settings.system_default"), "", "\"{file}\""}
        };
        value_ui::RowLabel(Translate("settings.code_editor_presets"), nullptr, false);
        for (const auto& preset : presets)
        {
            if (ImGui::Button(preset.label))
            {
                std::snprintf(ctx.state.codeEditorExecutableBuffer.data(), ctx.state.codeEditorExecutableBuffer.size(), "%s", preset.program);
                std::snprintf(ctx.state.codeEditorArgumentsBuffer.data(), ctx.state.codeEditorArgumentsBuffer.size(), "%s", preset.arguments);
            }
            if (&preset != &presets[std::size(presets) - 1])
                ImGui::SameLine();
        }
        SettingsTip("settings.code_editor_tip");
    });
    DrawEditorBuildToolSettings(ctx);
}

static void DrawEditorSettingsDialog(EditorFrameContext& ctx)
{
    if (!ctx.state.cameraSettingsOpen)
        return;

    ProjectDialogStyle dialogStyle;
    SetProjectDialogSize(620.f, 660.f);
    const std::string settingsTitle = LocalizedLabel("menu.editor_settings");
    KeepNextWindowInMainViewport(settingsTitle);
    if (ImGui::Begin(settingsTitle.c_str(), &ctx.state.cameraSettingsOpen, ImGuiWindowFlags_NoCollapse))
    {
        ImGui::BeginChild("##EditorSettingsBody", ImVec2(0.f, -52.f));
        if (ImGui::BeginTabBar("##EditorSettingsTabs", ImGuiTabBarFlags_FittingPolicyScroll))
        {
            struct Tab { const char* name; void (*draw)(EditorFrameContext&); };
            const Tab tabs[] = {
                {"settings.general", DrawEditorGeneralSettings},
                {"settings.viewport", DrawEditorViewportSettings},
                {"settings.snap_grid", DrawEditorSnapSettings},
                {"settings.external_tools", DrawEditorExternalTools},
                {"settings.workflow", DrawEditorWorkflowSettings}
            };
            for (const auto& tab : tabs)
            {
                if (ImGui::BeginTabItem(LocalizedLabel(tab.name).c_str()))
                {
                    // One label column for every card of the page, as in the inspector.
                    value_ui::InlineLabelScope rows;
                    tab.draw(ctx);
                    ImGui::EndTabItem();
                }
            }
            ImGui::EndTabBar();
        }
        ImGui::EndChild();
        if (ImGui::Button(LocalizedLabel("settings.apply").c_str(), ImVec2(130.f, 32.f)))
        {
            auto& preferences = ctx.state.editorPreferences;
            preferences.lastProject = GetEditorPreferences().lastProject;
            const bool personalSaved = SaveEditorPreferences(preferences);
            const bool projectSaved = !ctx.saveEditorSettings || ctx.saveEditorSettings();
            ctx.lastFileMessage = Translate(personalSaved && projectSaved ? "status.saved" : "status.save_failed");
        }
        ImGui::SameLine();
        if (ImGui::Button(LocalizedLabel("common.close").c_str(), ImVec2(130.f, 32.f)) || ImGui::IsKeyPressed(ImGuiKey_Escape, false))
            ctx.state.cameraSettingsOpen = false;
    }
    ImGui::End();
}

void RunEditorHistoryShortcuts(EditorFrameContext& ctx)
{
    const auto& io = ImGui::GetIO();
    if (ctx.gameViewFocused || io.WantTextInput || !io.KeyCtrl || ImGui::IsKeyPressed(ImGuiKey_Escape, false))
        return;

    const auto& preferences = GetEditorPreferences();
    if (ImGui::IsKeyPressed(EditorShortcutKey(preferences, 1), false))
    {
        FinishViewportDragEdits(ctx);
        UndoEditorHistory(ctx);
    }
    else if (ImGui::IsKeyPressed(EditorShortcutKey(preferences, 2), false))
    {
        FinishViewportDragEdits(ctx);
        RedoEditorHistory(ctx);
    }
}

void DrawFileMenuBar(EditorFrameContext& ctx)
{
    bool requestExport = false;
    bool requestResetLayout = false;
    RunFileShortcutActions(ctx);
    const bool canUndo = CanUndoEditorHistory(ctx.state);
    const bool canRedo = CanRedoEditorHistory(ctx.state);
    const std::string undoActionLabel = GetUndoEditorHistoryLabel(ctx.state);
    const std::string redoActionLabel = GetRedoEditorHistoryLabel(ctx.state);
    const std::string undoLabel = std::string(ICON_LC_UNDO_2 "  ") +
        (canUndo ? TranslateWithValue("menu.undo_action", undoActionLabel) : Translate("menu.undo")) +
        "###menu.undo";
    const std::string redoLabel = std::string(ICON_LC_REDO_2 "  ") +
        (canRedo ? TranslateWithValue("menu.redo_action", redoActionLabel) : Translate("menu.redo")) +
        "###menu.redo";

    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding,
        ImVec2(8.f, std::max(0.f, (kEditorMenuHeight * ImGui::GetStyle().FontScaleMain - ImGui::GetFontSize()) * 0.5f)));
    if (ImGui::BeginMainMenuBar())
    {
        if constexpr (!kEditorLogoOnRight)
        {
            if (const float logoReserve = EditorLogoReserve(ctx); logoReserve > 0.f)
                ImGui::SetCursorPosX(logoReserve);
        }
        if (ImGui::BeginMenu(LocalizedLabel("menu.file").c_str()))
        {
            if (ImGui::MenuItem(LocalizedLabel("project.new", ICON_LC_FILE_PLUS "  ", "…").c_str(), nullptr, false, static_cast<bool>(ctx.createProject)))
                ctx.state.createProjectDialogOpen = true;
            if (ImGui::MenuItem(LocalizedLabel("project.open", ICON_LC_FOLDER_OPEN "  ", "…").c_str(), nullptr, false, static_cast<bool>(ctx.loadProject)))
                ctx.state.openProjectDialogOpen = true;
            if (ImGui::MenuItem(LocalizedLabel("menu.project_settings", ICON_LC_SETTINGS "  ", "…").c_str(), nullptr, false, !ctx.currentProjectName.empty()))
                ctx.state.settingsWindowOpen = true;
            if (ImGui::MenuItem(LocalizedLabel("menu.run_window", ICON_LC_MONITOR "  ").c_str(), nullptr, false,
                                static_cast<bool>(ctx.playGame) && !ctx.currentProjectName.empty() &&
                                !ctx.runtimeBuildActive && !ctx.playActive))
            {
                if (!ctx.playGame() && ctx.lastFileMessage.empty())
                    ctx.lastFileMessage = Translate("status.run_failed");
            }
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                ImGui::SetTooltip("%s", Translate("menu.run_window_hint"));
            if (ImGui::MenuItem(LocalizedLabel("menu.export_project", ICON_LC_PACKAGE "  ", "…").c_str(), nullptr, false,
                                static_cast<bool>(ctx.exportProject) && !ctx.currentProjectName.empty() &&
                                !ctx.runtimeBuildActive))
            {
                requestExport = true;
            }
            ImGui::Separator();
            const bool inPrefab = IsInPrefabMode(ctx.state);
            if (ImGui::MenuItem(LocalizedLabel(inPrefab ? "menu.save_prefab" : "menu.save_scene", ICON_LC_SAVE "  ").c_str(),
                                EditorShortcutLabel(0).c_str(), false, inPrefab || static_cast<bool>(ctx.saveScene)))
                SaveCurrentSceneFromUI(ctx);
            if (ImGui::MenuItem(LocalizedLabel("menu.save_project", ICON_LC_SAVE "  ").c_str(), nullptr, false,
                                ctx.saveProject && !ctx.currentProjectName.empty()))
            {
                ctx.lastFileMessage.clear();
                if (ctx.saveProject())
                    ctx.lastFileMessage = Translate("project.saved");
                else if (ctx.lastFileMessage.empty())
                    ctx.lastFileMessage = Translate("project.write_failed");
            }
            ImGui::Separator();
            if (ImGui::MenuItem(LocalizedLabel("common.close", ICON_LC_LOG_OUT "  ").c_str(), "Esc"))
                ctx.state.closeConfirmOpen = true;
            ImGui::EndMenu();
        }

        if (ImGui::BeginMenu(LocalizedLabel("menu.edit").c_str()))
        {
            if (ImGui::MenuItem(undoLabel.c_str(), EditorShortcutLabel(1).c_str(), false, canUndo))
                UndoEditorHistory(ctx);
            if (ImGui::MenuItem(redoLabel.c_str(), EditorShortcutLabel(2).c_str(), false, canRedo))
                RedoEditorHistory(ctx);
            ImGui::EndMenu();
        }

        if (ImGui::BeginMenu(LocalizedLabel("menu.view").c_str()))
        {
            // Viewport and Game share the center dock: a reopened one comes to the front.
            if (ImGui::MenuItem(LocalizedLabel("panel.viewport", ICON_LC_BOX "  ").c_str(), nullptr, &ctx.viewportPanelVisible) &&
                ctx.viewportPanelVisible)
                ctx.focusScenePanel = true;
            if (ImGui::MenuItem(LocalizedLabel("panel.game", ICON_LC_PLAY "  ").c_str(), nullptr, &ctx.gamePanelVisible) &&
                ctx.gamePanelVisible)
                ctx.focusGamePanel = true;
            ImGui::MenuItem(LocalizedLabel("menu.hierarchy", ICON_LC_LIST_TREE "  ").c_str(), nullptr, &ctx.hierarchyPanelVisible);
            ImGui::MenuItem(LocalizedLabel("panel.assets", ICON_LC_FOLDER "  ").c_str(), nullptr, &ctx.assetsPanelVisible);
            // Opening the console also selects its tab (it shares the dock with Assets).
            if (ImGui::MenuItem(LocalizedLabel("assets.console", ICON_LC_TERMINAL "  ").c_str(), nullptr, &ctx.consolePanelVisible) &&
                ctx.consolePanelVisible)
                ctx.state.consoleFocusRequest = true;
            ImGui::MenuItem(LocalizedLabel("panel.inspector", ICON_LC_INFO "  ").c_str(), nullptr, &ctx.inspectorPanelVisible);
            if (ImGui::MenuItem(LocalizedLabel("panel.scene", ICON_LC_GLOBE "  ").c_str(), nullptr, &ctx.sceneSettingsPanelVisible) &&
                ctx.sceneSettingsPanelVisible)
                ctx.focusSceneSettingsPanel = true;
            ImGui::MenuItem(LocalizedLabel("menu.animation", ICON_LC_PLAY "  ").c_str(), nullptr, &ctx.state.animationWindowOpen);
            ImGui::MenuItem(LocalizedLabel("menu.controller", ICON_LC_LIST_TREE "  ").c_str(), nullptr, &ctx.state.controllerWindowOpen);
            ImGui::MenuItem(LocalizedLabel("profiler.title", ICON_LC_ACTIVITY "  ").c_str(), nullptr, &ctx.state.showProfiler);
            if (ImGui::MenuItem(LocalizedLabel("menu.reset_layout", ICON_LC_LAYOUT_TEMPLATE "  ").c_str()))
                requestResetLayout = true;
            ImGui::Separator();
            if (ImGui::MenuItem(LocalizedLabel("menu.editor_settings", ICON_LC_WRENCH "  ", "…").c_str()))
            {
                ctx.state.editorPreferences = GetEditorPreferences();
                ctx.state.cameraSettingsOpen = true;
            }
            ImGui::EndMenu();
        }

        if (ImGui::BeginMenu(LocalizedLabel("menu.scene").c_str()))
        {
            if (ImGui::BeginMenu(LocalizedLabel("menu.switch_scene", ICON_LC_REFRESH_CW "  ").c_str()))
            {
                for (const auto& name : ctx.availableEditorScenes)
                    if (ImGui::MenuItem(name.c_str(), nullptr, name == ctx.currentEditorSceneName,
                                        ctx.switchEditorScene && name != ctx.currentEditorSceneName))
                        RunAfterSceneSaveCheck(ctx, name, [&ctx, name] { ctx.switchEditorScene(name, false); });
                ImGui::EndMenu();
            }
            ImGui::Separator();
            if (ImGui::MenuItem(LocalizedLabel("menu.scenes_build", ICON_LC_SETTINGS "  ", "…").c_str()))
            {
                ctx.state.projectSettingsScenePage = true;
                ctx.state.settingsWindowOpen = true;
            }
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu(LocalizedLabel("menu.help").c_str()))
        {
            ImGui::TextDisabled("%s", Translate("menu.tools"));
            for (size_t i = 0; i < 3; ++i)
            {
                constexpr const char* actions[] = {"settings.shortcut_move", "settings.shortcut_rotate", "settings.shortcut_scale"};
                ImGui::Text("%s  %s", EditorShortcutLabel(i + 3).c_str(), Translate(actions[i]));
            }
            ImGui::Separator();
            ImGui::TextUnformatted(Translate("menu.camera_keys"));
            ImGui::TextUnformatted(Translate("menu.snap_key"));
            ImGui::EndMenu();
        }

        // Frameless main window: the menu bar doubles as the title bar. The free
        // space between the menus and the window buttons drags the window.
        if (ctx.windowHasCustomFrame && ctx.windowHasCustomFrame())
        {
            const ImGuiViewport* viewport = ImGui::GetMainViewport();
            const float uiScale = ImGui::GetStyle().FontScaleMain;
            const float barH = ImGui::GetWindowHeight();
            const float buttonW = std::round(barH * 1.6f);
            const float buttonsX = viewport->Pos.x + viewport->Size.x - 3.f * buttonW;
            const float menusEnd = ImGui::GetCursorScreenPos().x + 8.f * uiScale;

            // Project and scene, centered in the bar when they fit between menus and buttons.
            std::string title = ctx.currentProjectName;
            if (!ctx.currentEditorSceneName.empty())
                title += (title.empty() ? "" : "  -  ") + ctx.currentEditorSceneName;
            const ImVec2 titleSize = ImGui::CalcTextSize(title.c_str());
            const float titleX = std::max(menusEnd + 16.f * uiScale,
                                          viewport->Pos.x + std::round((viewport->Size.x - titleSize.x) * 0.5f));
            if (!title.empty() && titleX + titleSize.x <= buttonsX - 16.f * uiScale)
                ImGui::GetWindowDrawList()->AddText(
                    ImVec2(titleX, viewport->Pos.y + std::round((barH - titleSize.y) * 0.5f)),
                    theme::U32(theme::kTextSecondary), title.c_str());

            UpdateWindowCaptionArea(ctx, menusEnd - viewport->Pos.x, 0.f, buttonsX - menusEnd, barH);
            DrawWindowCaptionButtons(ctx, ImVec2(buttonsX, viewport->Pos.y), buttonW, barH, uiScale, true);
            DrawFramelessWindowBorder(ctx);
        }

        ImGui::EndMainMenuBar();
    }
    ImGui::PopStyleVar();
    if (requestExport)
        ImGui::OpenPopup(LocalizedLabel("export.title").c_str());
    DrawExportConfirmation(ctx);
    DrawResetLayoutConfirm(ctx, requestResetLayout);
    DrawEditorToolbar(ctx);
    DrawEditorLogo(ctx);

    DrawEditorSettingsDialog(ctx);

    DrawCreateProjectDialog(ctx);
    DrawOpenProjectDialog(ctx);
    DrawCloseConfirmDialog(ctx);
}

bool EditorCloseNeedsSaveQuestion(const EditorFrameContext& ctx)
{
    // Start screen, project loading or a scene still loading: nothing to save.
    if (!ctx.state.pendingStartProjectFile.empty() || ctx.currentProjectName.empty() || ctx.sceneLoadActive)
        return false;
    // The prefab mode has no change tracking; closing without leaving it would
    // drop the prefab edits and any unsaved changes of the scene behind it.
    if (IsInPrefabMode(ctx.state) && !ctx.state.prefabModeReadOnly)
        return true;
    return ctx.isEditorSceneModified && ctx.isEditorSceneModified();
}

void DrawCloseConfirmDialog(EditorFrameContext& ctx)
{
    DrawCloseConfirmDialogImpl(ctx);
}

} // namespace detail

} // namespace engine::renderer::addons::editor

#endif // KROM_EDITOR_HAS_IMGUI
