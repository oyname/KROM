// =============================================================================
// KROM Engine - addons/editor/EditorUIProject.cpp
// Projekt- und Szenen-Verwaltung: Startbildschirm, Dialoge, Menueleiste,
// Projekt-Einstellungen, Szenen-Manager, Prefab speichern/spawnen.
// =============================================================================
#include "addons/editor/EditorUIInternal.hpp"
#include "core/PathUtf8.hpp"
#include "addons/editor/EditorProjectHistory.hpp"
#include "addons/editor/EditorLocalization.hpp"

#include <cstdio>
#include <cfloat>

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
    case renderer::DeviceFactory::BackendType::OpenGL: return "OpenGL";
    case renderer::DeviceFactory::BackendType::Vulkan: return "Vulkan";
    default: return "Unknown";
    }
}

static renderer::DeviceFactory::BackendType EditorProjectBackendFromSelection(int selection) noexcept
{
    switch (selection)
    {
    case 1: return renderer::DeviceFactory::BackendType::DirectX11;
    case 2: return renderer::DeviceFactory::BackendType::OpenGL;
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
    EditorState& st = ctx.state;

    st.selectedEntity = NULL_ENTITY;
    st.multiSelection.clear();
    st.multiSelectionDragStartPositions.clear();
    st.multiSelectionDragStartRotations.clear();
    st.multiSelectionDragStartScales.clear();
    st.inspectorMaterialEntity = NULL_ENTITY;
    st.inspectorMaterialSlot = -1;
    st.inspectorMaterialAssetPath.clear();

    st.gizmoHoveredAxis = -1;
    st.gizmoDragAxis = -1;
    st.rotGizmoHoveredAxis = -1;
    st.rotGizmoDragAxis = -1;
    st.sclGizmoHoveredAxis = -1;
    st.sclGizmoDragAxis = -1;
    st.obbHoveredFace = -1;
    st.obbDragFaceIdx = -1;
    st.snapMoveLocked = false;

    st.deleteConfirmOpen = false;
    st.closeConfirmOpen = false;

    // Keine ImGui-Internals hier verwenden: ESC kann aus verschiedenen
    // Fenster-/Popup-Kontexten kommen, und Fokus-Internals assertieren dann.
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

    if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Z, false))
    {
        UndoEditorHistory(ctx);
    }
    if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Y, false))
    {
        RedoEditorHistory(ctx);
    }

    // Gizmo-Modus Tastaturkürzel (nur ohne Ctrl, nicht bei Kamera-Steuerung)
    if (!io.KeyCtrl && !ImGui::IsMouseDown(ImGuiMouseButton_Right))
    {
        if (ImGui::IsKeyPressed(ImGuiKey_1, false))
            ctx.state.gizmoMode = GizmoMode::Position;
        if (ImGui::IsKeyPressed(ImGuiKey_2, false))
            ctx.state.gizmoMode = GizmoMode::Rotation;
        if (ImGui::IsKeyPressed(ImGuiKey_3, false))
            ctx.state.gizmoMode = GizmoMode::Scale;
    }

    if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_S, false) && IsInPrefabMode(ctx.state))
    {
        // Im Prefab-Modus speichert Strg+S das Prefab, nicht die Szene.
        SavePrefabMode(ctx);
    }
    else if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_S, false))
    {
        // Projektdatei + Scene in einem Schritt speichern (saveProject ruft intern SaveEditorScene auf)
        const bool sceneOk = ctx.saveProject ? ctx.saveProject() : false;

        // Material mitsparen wenn das Material-Editor-Fenster offen und ein Asset geladen ist
        const bool materialSaved = ctx.state.materialWindowOpen &&
                                   ctx.state.selectedMaterialAssetLoaded
                                       ? SaveSelectedMaterialAsset(ctx)
                                       : false;

        if (materialSaved && !sceneOk)
            ctx.lastFileMessage = Translate("status.material_saved");
        else if (sceneOk && materialSaved)
            ctx.lastFileMessage = Translate("status.saved_with_material");
        else if (sceneOk)
            ctx.lastFileMessage = Translate("status.saved");
        else
            ctx.lastFileMessage = Translate("status.save_failed");
    }
    if (ImGui::IsKeyPressed(ImGuiKey_Delete, false))
    {
        if (ctx.state.selectedEntity.IsValid() && ctx.world.IsAlive(ctx.state.selectedEntity))
            ctx.state.deleteConfirmOpen = true;
    }
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
        ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, theme::Hex(0x1A2530));
        ImGui::PushStyleColor(ImGuiCol_FrameBgActive, theme::Hex(0x1E3342));
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
        ImGui::PopStyleColor(15);
        ImGui::PopStyleVar(7);
    }
};

static void SetProjectDialogSize(float width, float height)
{
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowSize(ImVec2(std::min(width, std::max(1.f, viewport->WorkSize.x - 32.f)),
                                  std::min(height, std::max(1.f, viewport->WorkSize.y - 32.f))),
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

        if (!ctx.projectsRootDirectory.empty())
        {
            std::error_code ec;
            const std::filesystem::path chosen =
                std::filesystem::weakly_canonical(PathFromUtf8(ctx.state.projectParentDirBuffer.data()), ec);
            const std::filesystem::path expected =
                std::filesystem::weakly_canonical(PathFromUtf8(ctx.projectsRootDirectory), ec);
            if (chosen != expected)
            {
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.f, 0.7f, 0.2f, 1.f));
                ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + 480.f);
                ImGui::TextUnformatted(TranslateWithValue("project.runtime_root", ctx.projectsRootDirectory).c_str());
                ImGui::PopTextWrapPos();
                ImGui::PopStyleColor();
            }
        }

        const char* backendItems = "Vulkan\0DirectX11\0OpenGL\0";
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
            const bool ok = ctx.loadProject
                ? ctx.loadProject(ctx.state.projectOpenPathBuffer.data())
                : false;
            if (ok)
                ImGui::CloseCurrentPopup();
            else if (ctx.lastFileMessage.empty())
                ctx.lastFileMessage = Translate("project.load_failed");
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
// Close-Confirm-Dialog — wird von DrawFileMenuBar UND DrawNoProjectScreen benutzt
// ---------------------------------------------------------------------------
static void DrawCloseConfirmDialog(EditorFrameContext& ctx)
{
    const auto title = LocalizedLabel("close.title");
    if (ctx.state.closeConfirmOpen)
    {
        ImGui::OpenPopup(title.c_str());
        ctx.state.closeConfirmOpen = false;
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
        ImGui::TextUnformatted(Translate("close.prompt"));
        ImGui::Separator();
        ImGui::Spacing();
        ImGui::PushStyleColor(ImGuiCol_Button, theme::kAccent);
        const bool close = ImGui::Button(LocalizedLabel("common.close").c_str(), ImVec2(130.f, 32.f));
        ImGui::PopStyleColor();
        if (close)
        {
            if (ctx.requestClose)
                ctx.requestClose();
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button(LocalizedLabel("common.cancel").c_str(), ImVec2(130.f, 32.f)) ||
            ImGui::IsKeyPressed(ImGuiKey_Escape, false))
        {
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
    ImGui::PopStyleColor(6);
    ImGui::PopStyleVar(6);
}

// ── Snap-Schalter in der Menueleiste (wie die Unreal-Viewport-Toolbar) ──────────
// Je Art ein Umschalter (hervorgehoben = an) plus Auswahl der Schrittweite.
enum class ToolbarGlyph { Select, Move, Rotate, Scale, Magnet, Arrow, Play, Pause, Stop, Grid, Icons, Particles, CameraPreview };

static bool ToolbarButton(const char* id, ToolbarGlyph glyph, bool selected,
                          const char* tooltip, const char* label = nullptr)
{
    const float width = label ? ImGui::CalcTextSize(label).x + 38.f : 28.f;
    ImGui::PushStyleColor(ImGuiCol_Button, selected ? theme::kMenuOpen
                                                   : theme::kButton);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, selected ? theme::kAccent
                                                          : theme::kButtonHover);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, theme::kButtonActive);
    const bool clicked = ImGui::Button(id, ImVec2(width, 28.f));
    ImGui::PopStyleColor(3);
    const ImVec2 p = ImGui::GetItemRectMin();
    const ImVec2 c(p.x + 14.f, p.y + 14.f);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImU32 color = ImGui::GetColorU32(ImGuiCol_Text);
    const char* icon = ICON_LC_MOUSE_POINTER_2;
    ImU32 iconColor = color;
    switch (glyph)
    {
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
    DrawIcon(dl, icon, c, 18.f, iconColor);
    if (label)
        dl->AddText(ImVec2(p.x + 28.f, p.y + (28.f - ImGui::GetFontSize()) * 0.5f), color, label);
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

static void DrawEditorToolbar(EditorFrameContext& ctx)
{
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    const float width = viewport->Size.x;
    const bool compact = width < 960.f;
    ImGui::SetNextWindowPos(ImVec2(viewport->Pos.x, viewport->Pos.y + kEditorMenuHeight));
    ImGui::SetNextWindowSize(ImVec2(width, kEditorToolbarHeight));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(6, 6));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 4.f);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, theme::kWindowBg);
    constexpr ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoDocking |
        ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse;
    ImGui::Begin("##editorToolbar", nullptr, flags);
    ImGui::SetCursorPos(ImVec2(12, 6));
    auto& state = ctx.state;
    if (ToolbarButton("##toolSelect", ToolbarGlyph::Select, state.gizmoMode == GizmoMode::Select, Translate("toolbar.select")))
        state.gizmoMode = GizmoMode::Select;
    ImGui::SameLine(0, 4);
    if (ToolbarButton("##toolMove", ToolbarGlyph::Move, state.gizmoMode == GizmoMode::Position, Translate("toolbar.move")))
        state.gizmoMode = GizmoMode::Position;
    ImGui::SameLine(0, 4);
    if (ToolbarButton("##toolRotate", ToolbarGlyph::Rotate, state.gizmoMode == GizmoMode::Rotation, Translate("toolbar.rotate")))
        state.gizmoMode = GizmoMode::Rotation;
    ImGui::SameLine(0, 4);
    if (ToolbarButton("##toolScale", ToolbarGlyph::Scale, state.gizmoMode == GizmoMode::Scale, Translate("toolbar.scale")))
        state.gizmoMode = GizmoMode::Scale;
    ToolbarSeparator();

    const bool world = state.transformSpace == EditorTransformSpace::World;
    ImGui::SetNextItemWidth(width < 760.f ? 56.f : 84.f);
    if (ImGui::BeginCombo("##toolbarSpace", width < 760.f ? (world ? "W" : "L") : (world ? Translate("transform.world") : Translate("transform.local"))))
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
    const float startWidth = ImGui::CalcTextSize("Start").x + 38.f;
    const float pauseWidth = 28.f;
    const float stopWidth = ImGui::CalcTextSize("Stop").x + 38.f;
    const float spacing = ImGui::GetStyle().ItemSpacing.x;
    const float playGroupWidth = startWidth + pauseWidth + stopWidth + 2.f * spacing;
    ImGui::SetCursorPos(ImVec2((width - playGroupWidth) * 0.5f, 6));
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

    const float displayWidth = compact ? 124.f :
        ImGui::CalcTextSize(Translate("toolbar.grid")).x + ImGui::CalcTextSize(Translate("toolbar.icons")).x + ImGui::CalcTextSize(Translate("toolbar.particles")).x +
        ImGui::CalcTextSize(Translate("toolbar.preview")).x + 164.f;
    ImGui::SetCursorPos(ImVec2(width - displayWidth - 12.f, 6));
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

bool SaveEntityAsPrefab(EditorFrameContext& ctx, EntityID entity)
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
    const std::filesystem::path prefabDir = projectRoot.empty()
        ? (std::filesystem::path("assets") / "Prefabs")
        : (projectRoot / "Assets" / "Prefabs");
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

    engine::addons::prefab::PrefabInstantiateOptions options{};
    options.scriptRegistry = ctx.scriptRegistry;
    const engine::addons::prefab::PrefabInstance instance =
        engine::addons::prefab::InstantiatePrefab(ctx.world, prefab, options);
    if (!instance.IsValid())
    {
        ctx.lastFileMessage = Translate("prefab.instance_failed");
        return false;
    }

    if (ctx.assetPipeline)
    {
        engine::addons::mesh_renderer::ResolveMeshAssetBindings(
            *ctx.assetPipeline, ctx.registry, ctx.world);
        ResolveMaterialAssetBindings(ctx);
    }

    if (parent.IsValid() && ctx.world.IsAlive(parent))
    {
        // Als Kind am Ursprung des Parents, gleiche Ausrichtung.
        ctx.world.Add<ParentComponent>(instance.root, ParentComponent{parent});
        uint32_t order = 0u;
        if (auto* children = ctx.world.Get<ChildrenComponent>(parent))
        {
            order = static_cast<uint32_t>(children->children.size());
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

    ctx.state.selectedEntity = instance.root;
    ctx.lastFileMessage = Translate("prefab.loaded_prefix") + PathToUtf8(path.filename());
    return true;
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
    if (!ctx.runtimeBuildActive)
        return;

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
                                   viewport->Pos.y + kEditorTopHeight + 8.f),
                            ImGuiCond_Always, ImVec2(0.5f, 0.f));
    ImGui::SetNextWindowSize(ImVec2(480.f, 0.f), ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(0.94f);
    constexpr ImGuiWindowFlags kFlags =
        ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoDocking |
        ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing |
        ImGuiWindowFlags_NoNav;
    if (ImGui::Begin("##runtimeBuildStatus", nullptr, kFlags))
    {
        ImGui::TextUnformatted(headline.c_str());
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

void DrawProjectSettingsDialog(EditorFrameContext& ctx)
{
    // ── Edit Layers Fenster ───────────────────────────────────────────────────
    if (ctx.state.layerNamesWindowOpen)
    {
        ImGui::SetNextWindowSize(ImVec2(300.f, 380.f), ImGuiCond_FirstUseEver);
        if (ImGui::Begin(TranslateLabel("layer.edit", "Layer bearbeiten##layerEdit").c_str(), &ctx.state.layerNamesWindowOpen))
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
                if (ImGui::IsItemDeactivatedAfterEdit() && ctx.saveProject)
                    ctx.saveProject();
                if (isReserved) ImGui::EndDisabled();
            }
        }
        ImGui::End();
    }

    if (ctx.state.settingsWindowOpen)
    {
        ImGui::OpenPopup(TranslateLabel("menu.project_settings", "Projekt-Einstellungen##settings").c_str());
        ctx.state.settingsWindowOpen = false;
    }

    ProjectDialogStyle dialogStyle;
    SetProjectDialogSize(560.f, 780.f);
    bool dialogOpen = true;
    if (!ImGui::BeginPopupModal(TranslateLabel("menu.project_settings", "Projekt-Einstellungen##settings").c_str(),
                                &dialogOpen,
                                ImGuiWindowFlags_NoCollapse))
        return;

    ImGui::BeginChild("##ProjectSettingsBody", ImVec2(0.f, -52.f));
    ImGui::TextUnformatted(Translate("settings.runtime_backend"));
    const char* backendItems[] = { "Vulkan", "DirectX11", "OpenGL" };
    int backendSelection = 0;
    switch (ctx.projectBackend)
    {
    case renderer::DeviceFactory::BackendType::DirectX11: backendSelection = 1; break;
    case renderer::DeviceFactory::BackendType::OpenGL: backendSelection = 2; break;
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

    ImGui::TextUnformatted(Translate("settings.scene_order"));
    ImGui::TextDisabled(Translate("settings.reorder_scenes"));
    ImGui::Spacing();

    std::vector<std::string> order = ctx.sceneOrder;
    bool orderChanged = false;
    int moveFrom = -1, moveTo = -1;

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
        const std::string rowLabel = std::to_string(i + 1) + ".  " + order[i];
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
        if (i == 0)
        {
            ImGui::TextDisabled("Start");
        }
        else
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

    ImGui::Spacing();
    ImGui::EndChild();
    ImGui::Separator();
    const float actionWidth = std::min(150.f, std::max(1.f, (ImGui::GetContentRegionAvail().x - 2.f * ImGui::GetStyle().ItemSpacing.x) / 3.f));
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + std::max(0.f, ImGui::GetContentRegionAvail().x - 3.f * actionWidth - 2.f * ImGui::GetStyle().ItemSpacing.x));
    const bool canPlay = static_cast<bool>(ctx.playGame) && !ctx.sceneOrder.empty() &&
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
    const bool canExport = static_cast<bool>(ctx.exportProject) && !ctx.sceneOrder.empty() &&
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
        std::string name = "(unbenannt)";
        if (const auto* nc = ctx.world.Get<NameComponent>(ctx.state.selectedEntity); nc && !nc->name.empty())
            name = nc->name;

        ImGui::TextWrapped(Translate("hierarchy.delete_prompt"), name.c_str());
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
            DestroyEntityHierarchy(ctx, ctx.state.selectedEntity);
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
    ImGui::SetNextWindowPos(viewport->Pos, ImGuiCond_Always);
    ImGui::SetNextWindowSize(size, ImGuiCond_Always);
    constexpr ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration |
        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoSavedSettings |
        ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoScrollbar |
        ImGuiWindowFlags_NoScrollWithMouse;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.f, 0.f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.f);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.f, 0.f, 0.f, 1.f) /* black launcher background */);
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

    ImGui::SetCursorPos(ImVec2(margin, 36.f * scale));
    ImGui::SetWindowFontScale(1.8f);
    ImGui::TextUnformatted(Translate("start.projects"));
    ImGui::SetWindowFontScale(1.f);
    const float languageWidth = ImGui::CalcTextSize(Translate("language.label")).x +
                                ImGui::GetFrameHeight() + 2.f * ImGui::GetStyle().FramePadding.x;
    ImGui::SameLine(std::max(margin, launcherW - 20.f * scale - languageWidth));
    ImGui::SetNextItemWidth(languageWidth);
    if (ImGui::BeginCombo("##StartLanguage", Translate("language.label")))
    {
        DrawLanguageChoices(ctx);
        ImGui::EndCombo();
    }
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
    const std::vector<StartProjectEntry> projects = ScanStartProjects(ctx);
    if (projects.empty())
        ImGui::TextDisabled("%s", Translate("start.empty"));
    for (const StartProjectEntry& project : projects)
    {
        ImGui::PushID(project.projectFile.c_str());
        const ImVec2 row = ImGui::GetCursorScreenPos();
        const float width = ImGui::GetContentRegionAvail().x;
        if (ImGui::Selectable("##project", false, ImGuiSelectableFlags_AllowDoubleClick,
                              ImVec2(0.f, 80.f * scale)))
        {
            ctx.lastFileMessage.clear();
            const bool ok = ctx.loadProject ? ctx.loadProject(project.projectFile) : false;
            if (!ok && ctx.lastFileMessage.empty())
                ctx.lastFileMessage = Translate("project.load_failed");
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
                    ImGui::GetColorU32(ImVec4(0.72f, 0.75f, 0.79f, 1.f)), project.path.c_str());
        dl->PopClipRect();
        if (showMetadata)
        {
            dl->AddText(ImVec2(metadataX, row.y + 22.f * scale),
                        ImGui::GetColorU32(ImGuiCol_Text), project.date.c_str());
            dl->AddText(ImVec2(metadataX, row.y + 46.f * scale),
                        ImGui::GetColorU32(ImGuiCol_Text), project.lastOpened.c_str());
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s\n%s\n%s", project.path.c_str(), project.date.c_str(), project.lastOpened.c_str());
        dl->AddLine(ImVec2(row.x, row.y + 80.f * scale),
                    ImVec2(row.x + width, row.y + 80.f * scale), ImGui::GetColorU32(ImGuiCol_Border));
        ImGui::PopID();
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
        if (textSize.x + 8.f * scale > buttonW && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("%s", label);
        ImGui::PopID();
        return clicked;
    };
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 5.f * scale);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.f);
    ImGui::PushStyleColor(ImGuiCol_Button, theme::kPanel);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, theme::kMenuHover);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, theme::kMenuOpen);
    const bool open = button(Translate("project.open"), ICON_LC_FOLDER_OPEN, 0);
    if (open)
    {
        if (ctx.browseForProjectFile)
        {
            const std::string picked = ctx.browseForProjectFile();
            if (!picked.empty())
            {
                ctx.lastFileMessage.clear();
                const bool ok = ctx.loadProject ? ctx.loadProject(picked) : false;
                if (!ok && ctx.lastFileMessage.empty())
                    ctx.lastFileMessage = Translate("project.load_failed");
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
    ImGui::PopStyleColor(3);
    ImGui::PopStyleVar(2);
    ImGui::SetCursorPos(ImVec2(margin, buttonY + buttonH + 14.f * scale));
    ImGui::TextDisabled("KROM Editor 0.3");
    if (!ctx.lastFileMessage.empty())
    {
        ImGui::SetCursorPosX(margin);
        ImGui::TextWrapped("%s", ctx.lastFileMessage.c_str());
    }
    ImGui::SetCursorPos(ImVec2(launcherW, 0.f));
    DrawStartEnginePreview(ctx, ImVec2(size.x - launcherW, size.y));
    ImGui::End();
}

void DrawFileMenuBar(EditorFrameContext& ctx)
{
    bool requestExport = false;
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
        ImVec2(8.f, std::max(0.f, (kEditorMenuHeight - ImGui::GetFontSize()) * 0.5f)));
    if (ImGui::BeginMainMenuBar())
    {
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
            // Speichert Projektdatei (krom-project.json) UND Scene in einem Schritt.
            // saveProject ruft intern SaveEditorScene() auf — kein separater Eintrag noetig.
            if (ImGui::MenuItem(LocalizedLabel("common.save", ICON_LC_SAVE "  ").c_str(), "Ctrl+S", false, static_cast<bool>(ctx.saveProject)))
            {
                const bool ok = ctx.saveProject ? ctx.saveProject() : false;
                ctx.lastFileMessage = ok ? Translate("status.saved") : Translate("status.save_failed");
            }
            ImGui::Separator();
            if (ImGui::MenuItem(LocalizedLabel("common.close", ICON_LC_LOG_OUT "  ").c_str(), "Esc"))
                ctx.state.closeConfirmOpen = true;
            ImGui::EndMenu();
        }

        if (ImGui::BeginMenu(LocalizedLabel("menu.edit").c_str()))
        {
            if (ImGui::MenuItem(undoLabel.c_str(), "Ctrl+Z", false, canUndo))
                UndoEditorHistory(ctx);
            if (ImGui::MenuItem(redoLabel.c_str(), "Ctrl+Y", false, canRedo))
                RedoEditorHistory(ctx);
            ImGui::EndMenu();
        }

        if (ImGui::BeginMenu(LocalizedLabel("menu.view").c_str()))
        {
            ImGui::MenuItem(LocalizedLabel("menu.hierarchy", ICON_LC_LIST_TREE "  ").c_str(), nullptr, &ctx.hierarchyPanelVisible);
            ImGui::MenuItem(LocalizedLabel("panel.assets", ICON_LC_FOLDER "  ").c_str(), nullptr, &ctx.assetsPanelVisible);
            ImGui::MenuItem(LocalizedLabel("panel.inspector", ICON_LC_INFO "  ").c_str(), nullptr, &ctx.inspectorPanelVisible);
            if (ImGui::MenuItem(LocalizedLabel("menu.reset_layout", ICON_LC_LAYOUT_TEMPLATE "  ").c_str()))
            {
                ctx.hierarchyPanelVisible = true;
                ctx.assetsPanelVisible = true;
                ctx.inspectorPanelVisible = true;
                ctx.resetDockLayout = true;
            }
            ImGui::Separator();
            if (ImGui::MenuItem(LocalizedLabel("menu.editor_settings", ICON_LC_WRENCH "  ", "…").c_str()))
                ctx.state.cameraSettingsOpen = true;
            if (ImGui::BeginMenu(LocalizedLabel("language.label").c_str()))
            {
                DrawLanguageChoices(ctx);
                ImGui::EndMenu();
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
                        ctx.switchEditorScene(name, false);
                ImGui::EndMenu();
            }
            if (ImGui::BeginMenu(LocalizedLabel("menu.additive", ICON_LC_LAYERS "  ").c_str()))
            {
                for (const auto& name : ctx.availableEditorScenes)
                {
                    const bool loaded = std::find(ctx.loadedEditorScenes.begin(), ctx.loadedEditorScenes.end(), name)
                                        != ctx.loadedEditorScenes.end();
                    if (ImGui::MenuItem(name.c_str(), nullptr, loaded, ctx.switchEditorScene && !loaded))
                        ctx.switchEditorScene(name, true);
                }
                ImGui::EndMenu();
            }
            ImGui::Separator();
            if (ImGui::MenuItem(LocalizedLabel("menu.scenes_build", ICON_LC_SETTINGS "  ", "…").c_str())) ctx.state.settingsWindowOpen = true;
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu(LocalizedLabel("menu.help").c_str()))
        {
            ImGui::TextDisabled("%s", Translate("menu.tools"));
            ImGui::TextUnformatted(Translate("menu.tool_keys"));
            ImGui::Separator();
            ImGui::TextUnformatted(Translate("menu.camera_keys"));
            ImGui::TextUnformatted(Translate("menu.snap_key"));
            ImGui::EndMenu();
        }

        ImGui::EndMainMenuBar();
    }
    ImGui::PopStyleVar();
    if (requestExport)
        ImGui::OpenPopup(LocalizedLabel("export.title").c_str());
    DrawExportConfirmation(ctx);
    DrawEditorToolbar(ctx);

    // ── Kamera-Einstellungen Dialog ───────────────────────────────────────────
    if (ctx.state.cameraSettingsOpen)
    {
        ProjectDialogStyle dialogStyle;
        SetProjectDialogSize(560.f, 760.f);
        if (ImGui::Begin(TranslateLabel("menu.editor_settings", "Editor-Einstellungen##camset").c_str(), &ctx.state.cameraSettingsOpen, ImGuiWindowFlags_NoCollapse))
        {
            ImGui::BeginChild("##EditorSettingsBody", ImVec2(0.f, -52.f));
            EditorCameraState& cam = ctx.state.editorCamera;

            ImGui::SeparatorText(Translate("component.camera"));
            if (ImGui::BeginTable("##CameraSettingsFields", 2, ImGuiTableFlags_SizingStretchSame))
            {
            // Separate label and input rows keep both columns on the same baseline.
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::TextUnformatted("FOV");
            ImGui::TableSetColumnIndex(1);
            ImGui::TextUnformatted(Translate("camera.near"));
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::SetNextItemWidth(-1.f);
            ImGui::DragFloat("##CameraFOV", &cam.fovDeg, 0.5f, 10.f, 170.f, "%.1f°");
            ImGui::TableSetColumnIndex(1);
            ImGui::SetNextItemWidth(-1.f);
            ImGui::DragFloat("##CameraNear", &cam.nearPlane, 0.001f, 0.001f, 10.f, "%.3f m");
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::TextUnformatted(Translate("camera.far"));
            ImGui::TableSetColumnIndex(1);
            ImGui::TextUnformatted(Translate("camera.speed"));
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::SetNextItemWidth(-1.f);
            ImGui::DragFloat("##CameraFar", &cam.farPlane, 10.f, 10.f, 100000.f, "%.0f m");
            ImGui::TableSetColumnIndex(1);
            ImGui::SetNextItemWidth(-1.f);
            ImGui::DragFloat("##CameraGeschwindigkeit", &cam.moveSpeed, 1.f, 0.1f, 10000.f, "%.1f");
            ImGui::EndTable();
            }

            cam.farPlane  = std::max(cam.nearPlane + 1.f, cam.farPlane);
            cam.nearPlane = std::clamp(cam.nearPlane, 0.001f, cam.farPlane - 1.f);
            cam.fovDeg    = std::clamp(cam.fovDeg, 10.f, 170.f);
            cam.moveSpeed = std::max(0.1f, cam.moveSpeed);

            ImGui::Separator();

            ImGui::SeparatorText("Viewport");
            ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(4.f, std::max(0.f, (24.f - ImGui::GetFontSize()) * .5f)));
            if (ImGui::Checkbox(TranslateLabel("settings.enable_ibl", "IBL aktiv").c_str(), &ctx.state.environmentEnableIBL))
            {
                ctx.state.ambientSource = ctx.state.environmentEnableIBL
                    ? EditorState::AmbientSource::Skybox : EditorState::AmbientSource::Color;
                ApplyEnvironmentFromState(ctx);
                if (ctx.setEditorAmbientLight)
                    ctx.setEditorAmbientLight(ctx.state.ambientColor, ctx.state.ambientIntensity);
            }
            ImGui::Checkbox(TranslateLabel("settings.show_skybox", "Skybox anzeigen").c_str(), &ctx.state.showSkyboxBackground);
            ImGui::Checkbox(TranslateLabel("settings.show_performance", "Performance-Overlay anzeigen").c_str(), &ctx.state.showPerfOverlay);
            ImGui::Checkbox(TranslateLabel("settings.show_selection_outline", "Auswahl-Outline anzeigen").c_str(), &ctx.state.showSelectionOutline);
            ImGui::PopStyleVar();
            ImGui::TextUnformatted(Translate("settings.background"));
            ImGui::SetNextItemWidth(-1.f);
            if (ImGui::ColorEdit3("##Background", ctx.state.backgroundColor.data()))
            {
                if (ctx.setEditorBackgroundColor)
                    ctx.setEditorBackgroundColor(ctx.state.backgroundColor);
            }

            ImGui::Separator();

            ImGui::SeparatorText(Translate("settings.code_editor"));
            ImGui::TextUnformatted(Translate("settings.executable"));
            ImGui::SetNextItemWidth(-1.f);
            ImGui::InputText("##code_editor_executable", ctx.state.codeEditorExecutableBuffer.data(),
                             ctx.state.codeEditorExecutableBuffer.size());
            ImGui::TextUnformatted(Translate("settings.arguments"));
            ImGui::SetNextItemWidth(-1.f);
            ImGui::InputText("##code_editor_arguments", ctx.state.codeEditorArgumentsBuffer.data(),
                             ctx.state.codeEditorArgumentsBuffer.size());
            const float presetWidth = std::max(1.f, (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x) * .5f);
            if (ImGui::Button("VS Code", ImVec2(presetWidth, 32.f)))
            {
                std::snprintf(ctx.state.codeEditorExecutableBuffer.data(),
                              ctx.state.codeEditorExecutableBuffer.size(), "%s", "code");
                std::snprintf(ctx.state.codeEditorArgumentsBuffer.data(),
                              ctx.state.codeEditorArgumentsBuffer.size(), "%s", "\"{workspace}\" -g \"{file}:{line}\"");
            }
            ImGui::SameLine();
            if (ImGui::Button(TranslateLabel("settings.system_default", "System Default").c_str(), ImVec2(presetWidth, 32.f)))
            {
                ctx.state.codeEditorExecutableBuffer[0] = '\0';
                std::snprintf(ctx.state.codeEditorArgumentsBuffer.data(),
                              ctx.state.codeEditorArgumentsBuffer.size(), "%s", "\"{file}\"");
            }

            ImGui::Separator();

            ImGui::EndChild();
            const float actionWidth = std::min(150.f, std::max(1.f, (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x) * .5f));
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + std::max(0.f, ImGui::GetContentRegionAvail().x - 2.f * actionWidth - ImGui::GetStyle().ItemSpacing.x));
            if (ImGui::Button(TranslateLabel("common.reset", "Zuruecksetzen").c_str(), ImVec2(actionWidth, 32.f)))
            {
                cam.fovDeg    = 60.f;
                cam.nearPlane = 0.05f;
                cam.farPlane  = 2000.f;
                cam.moveSpeed = 50.f;
            }
            ImGui::SameLine();
            ImGui::PushStyleColor(ImGuiCol_Button, theme::kAccent);
            if (ImGui::Button(LocalizedLabel("common.close").c_str(), ImVec2(actionWidth, 32.f)) || ImGui::IsKeyPressed(ImGuiKey_Escape, false))
                ctx.state.cameraSettingsOpen = false;
            ImGui::PopStyleColor();
        }
        ImGui::End();
    }

    DrawCreateProjectDialog(ctx);
    DrawOpenProjectDialog(ctx);
    DrawCloseConfirmDialog(ctx);
}

} // namespace detail

} // namespace engine::renderer::addons::editor

#endif // KROM_EDITOR_HAS_IMGUI
