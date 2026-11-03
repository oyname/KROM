// =============================================================================
// KROM Engine - addons/editor/EditorUIProject.cpp
// Projekt- und Szenen-Verwaltung: Startbildschirm, Dialoge, Menueleiste,
// Projekt-Einstellungen, Szenen-Manager, Prefab speichern/spawnen.
// =============================================================================
#include "addons/editor/EditorUIInternal.hpp"
#include "core/PathUtf8.hpp"
#include "addons/editor/EditorProjectHistory.hpp"

#include <cstdio>
#include <cfloat>

#ifdef KROM_EDITOR_HAS_IMGUI

namespace engine::renderer::addons::editor {

using namespace detail;

namespace {

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
    if (timestamp <= 0) return relative ? "Last opened: Unknown" : "";
    std::tm local{};
#ifdef _WIN32
    if (localtime_s(&local, &timestamp) != 0) return "";
#else
    if (!localtime_r(&timestamp, &local)) return "";
#endif
    char text[80]{};
    if (!relative)
        std::strftime(text, sizeof(text), "%d.%m.%Y", &local);
    else
    {
        const std::time_t now = std::time(nullptr);
        std::tm today{};
#ifdef _WIN32
        localtime_s(&today, &now);
#else
        localtime_r(&now, &today);
#endif
        const bool sameDay = today.tm_year == local.tm_year && today.tm_yday == local.tm_yday;
        std::strftime(text, sizeof(text), sameDay ? "Last opened: Today, %H:%M" :
                      "Last opened: %d.%m.%Y, %H:%M", &local);
    }
    return text;
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
    return GetStartEditorTextureId(ctx, "preview.png", s_previewPath, s_previewTexture);
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
            ctx.lastFileMessage = "Material gespeichert.";
        else if (sceneOk && materialSaved)
            ctx.lastFileMessage = "Gespeichert + Material gespeichert.";
        else if (sceneOk)
            ctx.lastFileMessage = "Gespeichert.";
        else
            ctx.lastFileMessage = "Speichern fehlgeschlagen.";
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
    if (ctx.state.createProjectDialogOpen)
    {
        ImGui::OpenPopup("Projekt erstellen##editor");
        ctx.state.createProjectDialogOpen = false;
        if (ctx.state.projectParentDirBuffer[0] == '\0' && !ctx.projectsRootDirectory.empty())
            std::snprintf(ctx.state.projectParentDirBuffer.data(),
                          ctx.state.projectParentDirBuffer.size(),
                          "%s", ctx.projectsRootDirectory.c_str());
    }

    ProjectDialogStyle dialogStyle;
    bool dialogOpen = true;
    if (ImGui::BeginPopupModal("Projekt erstellen##editor",
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
        ImGui::TextUnformatted("Projektname");
        ImGui::SetNextItemWidth(480.f);
        ImGui::InputText("##ProjectName", ctx.state.projectNameBuffer.data(), ctx.state.projectNameBuffer.size());

        // Projektordner mit Browse-Button
        {
            const bool hasBrowse = static_cast<bool>(ctx.browseForProjectFolder);
            const float browseButtonWidth = hasBrowse ? ImGui::GetFrameHeight() : 0.f;
            const float spacing = hasBrowse ? ImGui::GetStyle().ItemSpacing.x : 0.f;
            ImGui::Text("Projektordner");
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
                    ImGui::SetItemTooltip("Uebergeordneten Ordner auswaehlen");
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
                ImGui::TextColored(ImVec4(1.f, 0.7f, 0.2f, 1.f),
                    "Hinweis: Nur Projekte in %s\nbekommen eine Runtime - Play und Export\n"
                    "funktionieren in anderen Ordnern nicht.",
                    ctx.projectsRootDirectory.c_str());
            }
        }

        const char* backendItems = "Vulkan\0DirectX11\0OpenGL\0";
        ImGui::TextUnformatted("Backend");
        ImGui::SetNextItemWidth(480.f);
        ImGui::Combo("##ProjectBackend", &ctx.state.projectBackendSelection, backendItems);

        const renderer::DeviceFactory::BackendType selectedBackend =
            EditorProjectBackendFromSelection(ctx.state.projectBackendSelection);
        ImGui::Separator();
        ImGui::Text("Aktuell: %s", ctx.currentProjectName.empty() ? "(kein Projekt)" : ctx.currentProjectName.c_str());
        ImGui::Text("Neues Projekt-Backend: %s", EditorProjectBackendLabel(selectedBackend));

        ImGui::Spacing();
        ImGui::PushStyleColor(ImGuiCol_Button, theme::kAccent);
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + std::max(0.f, ImGui::GetContentRegionAvail().x - 260.f - ImGui::GetStyle().ItemSpacing.x));
        const bool create = iconButton("##CreateProject", "Erstellen", ICON_LC_PLUS, ImVec2(130.f, 32.f));
        ImGui::PopStyleColor();
        if (create)
        {
            const bool ok = ctx.createProject
                ? ctx.createProject(ctx.state.projectNameBuffer.data(),
                                    ctx.state.projectParentDirBuffer.data(),
                                    selectedBackend)
                : false;
            if (ok)
                ImGui::CloseCurrentPopup();
            else if (ctx.lastFileMessage.empty())
                ctx.lastFileMessage = "Projekt konnte nicht erstellt werden.";
        }
        ImGui::SameLine();
        if (ImGui::Button("Abbrechen", ImVec2(130.f, 32.f)) ||
            ImGui::IsKeyPressed(ImGuiKey_Escape, false))
        {
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}

static void DrawOpenProjectDialog(EditorFrameContext& ctx)
{
    if (ctx.state.openProjectDialogOpen)
    {
        ImGui::OpenPopup("Projekt oeffnen##editor");
        ctx.state.openProjectDialogOpen = false;
    }

    ProjectDialogStyle dialogStyle;
    const float dialogWidth = std::min(520.f, std::max(1.f, ImGui::GetMainViewport()->WorkSize.x - 32.f));
    ImGui::SetNextWindowSizeConstraints(ImVec2(dialogWidth, 0.f), ImVec2(dialogWidth, FLT_MAX));
    bool dialogOpen = true;
    if (ImGui::BeginPopupModal("Projekt oeffnen##editor",
                               &dialogOpen,
                               ImGuiWindowFlags_AlwaysAutoResize))
    {
        ImGui::TextUnformatted("Projektordner oder Projektdatei");
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
                ImGui::SetItemTooltip("Projektdatei auswaehlen (krom-project.json)");
        }
        ImGui::TextDisabled("Ordner oder direkte krom-project.json angeben.");

        ImGui::Spacing();
        const float actionWidth = std::min(150.f, std::max(1.f, (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x) * .5f));
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + std::max(0.f, ImGui::GetContentRegionAvail().x - 2.f * actionWidth - ImGui::GetStyle().ItemSpacing.x));
        ImGui::PushStyleColor(ImGuiCol_Button, theme::kAccent);
        const bool open = ImGui::Button("Oeffnen", ImVec2(actionWidth, 32.f));
        ImGui::PopStyleColor();
        if (open)
        {
            const bool ok = ctx.loadProject
                ? ctx.loadProject(ctx.state.projectOpenPathBuffer.data())
                : false;
            if (ok)
                ImGui::CloseCurrentPopup();
            else if (ctx.lastFileMessage.empty())
                ctx.lastFileMessage = "Projekt konnte nicht geladen werden.";
        }
        ImGui::SameLine();
        if (ImGui::Button("Abbrechen", ImVec2(actionWidth, 32.f)) ||
            ImGui::IsKeyPressed(ImGuiKey_Escape, false))
        {
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}

// ---------------------------------------------------------------------------
// Close-Confirm-Dialog — wird von DrawFileMenuBar UND DrawNoProjectScreen benutzt
// ---------------------------------------------------------------------------
static void DrawCloseConfirmDialog(EditorFrameContext& ctx)
{
    if (ctx.state.closeConfirmOpen)
    {
        ImGui::OpenPopup("Anwendung schliessen?##editor");
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
    if (ImGui::BeginPopupModal("Anwendung schliessen?##editor",
                               &dialogOpen,
                               ImGuiWindowFlags_AlwaysAutoResize))
    {
        ImGui::TextUnformatted("Editor wirklich schliessen?");
        ImGui::Separator();
        ImGui::Spacing();
        ImGui::PushStyleColor(ImGuiCol_Button, theme::kAccent);
        const bool close = ImGui::Button("Schliessen", ImVec2(130.f, 32.f));
        ImGui::PopStyleColor();
        if (close)
        {
            if (ctx.requestClose)
                ctx.requestClose();
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Abbrechen", ImVec2(130.f, 32.f)) ||
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
enum class ToolbarGlyph { Select, Move, Rotate, Scale, Magnet, Arrow, Play, Pause, Stop, Grid, Icons, Particles };

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
    if (ToolbarButton("##toolSelect", ToolbarGlyph::Select, state.gizmoMode == GizmoMode::Select, "Auswählen (0)"))
        state.gizmoMode = GizmoMode::Select;
    ImGui::SameLine(0, 4);
    if (ToolbarButton("##toolMove", ToolbarGlyph::Move, state.gizmoMode == GizmoMode::Position, "Verschieben (1)"))
        state.gizmoMode = GizmoMode::Position;
    ImGui::SameLine(0, 4);
    if (ToolbarButton("##toolRotate", ToolbarGlyph::Rotate, state.gizmoMode == GizmoMode::Rotation, "Drehen (2)"))
        state.gizmoMode = GizmoMode::Rotation;
    ImGui::SameLine(0, 4);
    if (ToolbarButton("##toolScale", ToolbarGlyph::Scale, state.gizmoMode == GizmoMode::Scale, "Skalieren (3)"))
        state.gizmoMode = GizmoMode::Scale;
    ToolbarSeparator();

    const bool world = state.transformSpace == EditorTransformSpace::World;
    ImGui::SetNextItemWidth(width < 760.f ? 56.f : 84.f);
    if (ImGui::BeginCombo("##toolbarSpace", width < 760.f ? (world ? "W" : "L") : (world ? "Welt" : "Lokal")))
    {
        if (ImGui::Selectable("Lokal", !world)) state.transformSpace = EditorTransformSpace::Local;
        if (ImGui::Selectable("Welt", world)) state.transformSpace = EditorTransformSpace::World;
        ImGui::EndCombo();
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Ausrichtung: lokale oder globale Achsen");
    ImGui::SameLine(0, 4);
    auto& snap = state.snap;
    const bool snapping = snap.moveEnabled || snap.rotateEnabled || snap.scaleEnabled;
    if (ToolbarButton("##toolbarSnap", ToolbarGlyph::Magnet, snapping,
                      "Einrasten ein/aus. Strg beim Ziehen kehrt die Einstellung um."))
        snap.moveEnabled = snap.rotateEnabled = snap.scaleEnabled = !snapping;
    ImGui::SameLine(0, 2);
    if (ToolbarButton("##toolbarSnapOptions", ToolbarGlyph::Arrow, false, "Einrastwerte einstellen"))
        ImGui::OpenPopup("##snapOptions");
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(12, 10));
    if (ImGui::BeginPopup("##snapOptions"))
    {
        static constexpr float move[] = {0.01f, 0.05f, 0.1f, 0.25f, 0.5f, 1.f, 2.f, 5.f, 10.f};
        static constexpr float rotate[] = {1.f, 5.f, 10.f, 15.f, 22.5f, 30.f, 45.f, 90.f};
        static constexpr float scale[] = {0.01f, 0.05f, 0.1f, 0.25f, 0.5f, 1.f};
        ImGui::TextUnformatted("Einrasten");
        ImGui::Separator();
        ImGui::Checkbox("Position:", &snap.moveEnabled); ImGui::SameLine(132);
        SnapValueCombo("##moveStep", snap.moveStep, move, std::size(move), " m");
        ImGui::Checkbox("Drehung:", &snap.rotateEnabled); ImGui::SameLine(132);
        SnapValueCombo("##rotateStep", snap.rotateStepDeg, rotate, std::size(rotate), "°");
        ImGui::Checkbox("Skalierung:", &snap.scaleEnabled); ImGui::SameLine(132);
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
    if (ToolbarButton("##toolbarPlay", ToolbarGlyph::Play, false, "Szene im Game-Reiter spielen", "Start"))
    {
        if (!ctx.startPlay() && ctx.lastFileMessage.empty()) ctx.lastFileMessage = "Start fehlgeschlagen.";
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(!ctx.playActive || !ctx.togglePlayPause);
    if (ToolbarButton("##toolbarPause", ToolbarGlyph::Pause, ctx.playPaused,
                      ctx.playPaused ? "Fortsetzen" : "Pause"))
        ctx.togglePlayPause();
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(!ctx.playActive || !ctx.stopPlay);
    if (ToolbarButton("##toolbarStop", ToolbarGlyph::Stop, false,
                      "Spiel beenden, Szene wiederherstellen", "Stop"))
        ctx.stopPlay();
    ImGui::EndDisabled();

    const float displayWidth = compact ? 92.f :
        ImGui::CalcTextSize("Raster").x + ImGui::CalcTextSize("Symbole").x + ImGui::CalcTextSize("Partikel").x + 122.f;
    ImGui::SetCursorPos(ImVec2(width - displayWidth - 12.f, 6));
    if (ToolbarButton("##toolbarGrid", ToolbarGlyph::Grid, snap.showGrid, "Bodenraster anzeigen", compact ? nullptr : "Raster"))
        snap.showGrid = !snap.showGrid;
    ImGui::SameLine(0, 4);
    if (ToolbarButton("##toolbarIcons", ToolbarGlyph::Icons, state.showEditorIcons,
                      "Symbole für Licht, Partikel und Audio anzeigen", compact ? nullptr : "Symbole"))
        state.showEditorIcons = !state.showEditorIcons;
    ImGui::SameLine(0, 4);
    if (ToolbarButton("##toolbarParticles", ToolbarGlyph::Particles, state.playAllParticles,
                      "Alle Partikel abspielen. Aus: nur die Auswahl und ihre Kinder.", compact ? nullptr : "Partikel"))
        state.playAllParticles = !state.playAllParticles;
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
        ctx.lastFileMessage = "Prefab konnte nicht erstellt werden.";
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
        ctx.lastFileMessage = error.empty() ? "Prefab konnte nicht gespeichert werden." : error;
        return false;
    }

    ctx.lastFileMessage = "Prefab gespeichert: " + PathToUtf8(prefabPath);

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
        ctx.lastFileMessage = error.empty() ? "Prefab konnte nicht geladen werden." : error;
        return false;
    }

    engine::addons::prefab::PrefabInstantiateOptions options{};
    options.scriptRegistry = ctx.scriptRegistry;
    const engine::addons::prefab::PrefabInstance instance =
        engine::addons::prefab::InstantiatePrefab(ctx.world, prefab, options);
    if (!instance.IsValid())
    {
        ctx.lastFileMessage = "Prefab konnte nicht instanziiert werden.";
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
    ctx.lastFileMessage = "Prefab geladen: " + PathToUtf8(path.filename());
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
        ImGui::Text("Lade Szene: %s", ctx.sceneLoadName.c_str());
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
    constexpr const char* popup = "Scripts bauen##scriptBuild";
    if (ctx.scriptBuildActive && !ImGui::IsPopupOpen(popup)) ImGui::OpenPopup(popup);
    if (ImGui::IsPopupOpen(popup))
    {
        const auto* viewport = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(viewport->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
        ImGui::SetNextWindowSize(ImVec2(540.f, 0.f), ImGuiCond_Appearing);
        if (ImGui::BeginPopupModal(popup, nullptr, ImGuiWindowFlags_NoDocking |
            ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings))
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
                    -static_cast<float>(ImGui::GetTime()), ImVec2(-1.f, 0.f), fraction ? nullptr : "Build wird vorbereitet...");
                if (ImGui::Button("Abbrechen") && ctx.cancelBuild) ctx.cancelBuild();
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
                               "Konfiguriere...");
        if (!detail.empty())
        {
            ImGui::PushTextWrapPos(0.f);
            ImGui::TextDisabled("%s", detail.c_str());
            ImGui::PopTextWrapPos();
        }
        ImGui::TextDisabled("Der Editor bleibt bedienbar. Play/Export sind bis zum Ende gesperrt.");
    }
    ImGui::End();
}

static void DrawExportConfirmation(EditorFrameContext& ctx)
{
    ProjectDialogStyle dialogStyle;
    if (ImGui::BeginPopupModal("Spiel exportieren?##confirmExport", nullptr,
                              ImGuiWindowFlags_AlwaysAutoResize))
    {
        ImGui::TextUnformatted("Möchtest du das Spiel jetzt exportieren?");
        ImGui::TextUnformatted("Der Export erstellt eine weitergebbare Spielversion.");
        ImGui::TextDisabled("Vorhandene Exportdateien werden ersetzt. Dies kann einige Minuten dauern.");
        ImGui::Spacing();
        if (ImGui::Button("Abbrechen", ImVec2(130.f, 32.f)) ||
            ImGui::IsKeyPressed(ImGuiKey_Escape, false))
            ImGui::CloseCurrentPopup();
        ImGui::SetItemDefaultFocus();
        ImGui::SameLine();
        const bool canExport = ctx.exportProject && !ctx.runtimeBuildActive && !ctx.sceneOrder.empty();
        ImGui::BeginDisabled(!canExport);
        if (ImGui::Button("Exportieren", ImVec2(130.f, 32.f)))
        {
            ImGui::CloseCurrentPopup();
            if (!ctx.exportProject() && ctx.lastFileMessage.empty())
                ctx.lastFileMessage = "Export fehlgeschlagen.";
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
        if (ImGui::Begin("Layer bearbeiten##layerEdit", &ctx.state.layerNamesWindowOpen))
        {
            ImGui::TextDisabled("Index 0 (Default) und 1 (EditorGizmo) sind reserviert.");
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
        ImGui::OpenPopup("Projekt-Einstellungen##settings");
        ctx.state.settingsWindowOpen = false;
    }

    ProjectDialogStyle dialogStyle;
    SetProjectDialogSize(560.f, 780.f);
    bool dialogOpen = true;
    if (!ImGui::BeginPopupModal("Projekt-Einstellungen##settings",
                                &dialogOpen,
                                ImGuiWindowFlags_NoCollapse))
        return;

    ImGui::BeginChild("##ProjectSettingsBody", ImVec2(0.f, -52.f));
    ImGui::TextUnformatted("Runtime-Backend");
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
    ImGui::TextDisabled("Play baut die Projekt-Runtime fuer dieses Backend.");
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    ImGui::TextUnformatted("Szenenreihenfolge");
    ImGui::TextDisabled("Szenen zum Umsortieren ziehen und ablegen.");
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
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Nach oben");
        ImGui::SameLine();

        // Runter-Button
        const bool canMoveDown = (i < static_cast<int>(order.size()) - 1);
        if (!canMoveDown) ImGui::BeginDisabled();
        if (ImGui::ArrowButton("##moveDown", ImGuiDir_Down)) { moveFrom = i; moveTo = i + 1; orderChanged = true; }
        if (!canMoveDown) ImGui::EndDisabled();
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Nach unten");
        ImGui::PopStyleVar();
        ImGui::SameLine();

        const float startWidth = 62.f;
        const float rowWidth = std::max(1.f, ImGui::GetContentRegionAvail().x -
            startWidth - ImGui::GetStyle().ItemSpacing.x);
        const std::string rowLabel = std::to_string(i + 1) + ".  " + order[i];
        ImGui::Selectable(rowLabel.c_str(), false, ImGuiSelectableFlags_None, ImVec2(rowWidth, 28.f));
        if (ImGui::IsItemHovered())
            ImGui::SetItemTooltip("Ziehen, um die Szenenreihenfolge zu aendern");
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
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Als Startszene setzen");
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
        ImGui::TextDisabled("Index 1 = Startszene (LoadScene(1))");
        ImGui::TextDisabled("Index 2 = LoadScene(2), usw.");
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    // Build-Reihenfolge (Checkboxen)
    ImGui::TextUnformatted("Szenen im Build");
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
    if (ImGui::Button("Im Fenster", ImVec2(actionWidth, 32.f)))
    {
        const bool ok = ctx.playGame ? ctx.playGame() : false;
        if (!ok && ctx.lastFileMessage.empty())
            ctx.lastFileMessage = "Play fehlgeschlagen.";
    }
    if (!canPlay) ImGui::EndDisabled();

    ImGui::SameLine();
    const bool canExport = static_cast<bool>(ctx.exportProject) && !ctx.sceneOrder.empty() &&
                           !ctx.runtimeBuildActive;
    if (!canExport) ImGui::BeginDisabled();
    if (ImGui::Button("Spiel exportieren ...", ImVec2(actionWidth, 32.f)))
    {
        ImGui::OpenPopup("Spiel exportieren?##confirmExport");
    }
    if (!canExport) ImGui::EndDisabled();
    if (ImGui::IsItemHovered() && canExport)
        ImGui::SetTooltip("Erstellt eine weitergebbare Release-Ausgabe unter\n"
                          "Build/<Config>-<Backend>. Der erste Export dauert einige Minuten.");

    ImGui::SameLine();
    if (ImGui::Button("Schliessen", ImVec2(actionWidth, 32.f)) ||
        ImGui::IsKeyPressed(ImGuiKey_Escape, false))
        ImGui::CloseCurrentPopup();

    DrawExportConfirmation(ctx);
    ImGui::EndPopup();
}

void DrawDeleteConfirmDialog(EditorFrameContext& ctx)
{
    constexpr const char* popupId = "Objekt löschen?##editor";
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

        ImGui::TextWrapped("Möchtest du das Objekt \"%s\" löschen?", name.c_str());
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x);
        ImGui::TextDisabled("Untergeordnete Objekte werden ebenfalls entfernt.");
        ImGui::PopTextWrapPos();
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        const ImVec2 buttonSize(200.f * scale, 44.f * scale);
        const float rowWidth = 2.f * buttonSize.x + ImGui::GetStyle().ItemSpacing.x;
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + std::max(0.f, ImGui::GetContentRegionAvail().x - rowWidth));
        if (EditorActionButton("Abbrechen", buttonSize) || ImGui::IsKeyPressed(ImGuiKey_Escape, false))
            ImGui::CloseCurrentPopup();
        ImGui::SetItemDefaultFocus();
        ImGui::SameLine();
        if (EditorActionButton("Löschen", buttonSize, EditorButtonTone::Danger))
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
    ImGui::TextUnformatted("Projects");
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
    const std::vector<StartProjectEntry> projects = ScanStartProjects(ctx);
    if (projects.empty())
        ImGui::TextDisabled("Keine Projekte gefunden.");
    for (const StartProjectEntry& project : projects)
    {
        ImGui::PushID(project.projectFile.c_str());
        const ImVec2 row = ImGui::GetCursorScreenPos();
        const float width = ImGui::GetContentRegionAvail().x;
        if (ImGui::Selectable("##project", false, ImGuiSelectableFlags_AllowDoubleClick,
                              ImVec2(0.f, 80.f * scale)))
        {
            const bool ok = ctx.loadProject ? ctx.loadProject(project.projectFile) : false;
            if (!ok && ctx.lastFileMessage.empty())
                ctx.lastFileMessage = "Projekt konnte nicht geladen werden.";
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
        ImGui::PushID(label);
        const bool clicked = ImGui::Button("##action", ImVec2(buttonW, buttonH));
        const ImVec2 pos = ImGui::GetItemRectMin();
        ImDrawList* dl = ImGui::GetWindowDrawList();
        DrawIcon(dl, icon, ImVec2(pos.x + 28.f * scale, pos.y + buttonH * 0.5f), 22.f * scale,
                 ImGui::GetColorU32(ImGuiCol_Text));
        const ImVec2 textSize = ImGui::CalcTextSize(label);
        dl->AddText(ImVec2(pos.x + 54.f * scale,
                           pos.y + (buttonH - textSize.y) * 0.5f), ImGui::GetColorU32(ImGuiCol_Text), label);
        ImGui::PopID();
        return clicked;
    };
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 5.f * scale);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.f);
    ImGui::PushStyleColor(ImGuiCol_Button, theme::kPanel);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, theme::kMenuHover);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, theme::kMenuOpen);
    const bool open = button("Open Project", ICON_LC_FOLDER_OPEN, 0);
    if (open)
    {
        if (ctx.browseForProjectFile)
        {
            const std::string picked = ctx.browseForProjectFile();
            if (!picked.empty())
            {
                const bool ok = ctx.loadProject ? ctx.loadProject(picked) : false;
                if (!ok && ctx.lastFileMessage.empty())
                    ctx.lastFileMessage = "Projekt konnte nicht geladen werden.";
            }
        }
        else
            ctx.state.openProjectDialogOpen = true;
    }
    if (button("New Project", ICON_LC_PLUS, 1))
        ctx.state.createProjectDialogOpen = true;
    if (!ctx.browseForProjectFolder) ImGui::BeginDisabled();
    const bool changeFolder = button("Change Folder", ICON_LC_FOLDER, 2);
    if (!ctx.browseForProjectFolder) ImGui::EndDisabled();
    if (changeFolder && ctx.browseForProjectFolder)
    {
        const std::string picked = ctx.browseForProjectFolder();
        if (!picked.empty())
            std::snprintf(ctx.state.projectParentDirBuffer.data(),
                          ctx.state.projectParentDirBuffer.size(), "%s", picked.c_str());
    }
    if (button("Close", ICON_LC_X, 3))
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
    const std::string undoLabel = std::string(ICON_LC_UNDO_2 "  ") + (canUndo ? ("Rückgängig: " + undoActionLabel) : "Rückgängig");
    const std::string redoLabel = std::string(ICON_LC_REDO_2 "  ") + (canRedo ? ("Wiederholen: " + redoActionLabel) : "Wiederholen");

    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding,
        ImVec2(8.f, std::max(0.f, (kEditorMenuHeight - ImGui::GetFontSize()) * 0.5f)));
    if (ImGui::BeginMainMenuBar())
    {
        if (ImGui::BeginMenu("Datei"))
        {
            if (ImGui::MenuItem(ICON_LC_FILE_PLUS "  Neues Projekt...", nullptr, false, static_cast<bool>(ctx.createProject)))
                ctx.state.createProjectDialogOpen = true;
            if (ImGui::MenuItem(ICON_LC_FOLDER_OPEN "  Projekt oeffnen...", nullptr, false, static_cast<bool>(ctx.loadProject)))
                ctx.state.openProjectDialogOpen = true;
            if (ImGui::MenuItem(ICON_LC_SETTINGS "  Einstellungen...", nullptr, false, !ctx.currentProjectName.empty()))
                ctx.state.settingsWindowOpen = true;
            if (ImGui::MenuItem(ICON_LC_MONITOR "  Im eigenen Fenster starten", nullptr, false,
                                static_cast<bool>(ctx.playGame) && !ctx.currentProjectName.empty() &&
                                !ctx.runtimeBuildActive && !ctx.playActive))
            {
                if (!ctx.playGame() && ctx.lastFileMessage.empty())
                    ctx.lastFileMessage = "Start fehlgeschlagen.";
            }
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                ImGui::SetTooltip("Baut die Projekt-Runtime und startet sie wie im Export.");
            if (ImGui::MenuItem(ICON_LC_PACKAGE "  Projekt exportieren", nullptr, false,
                                static_cast<bool>(ctx.exportProject) && !ctx.currentProjectName.empty() &&
                                !ctx.runtimeBuildActive))
            {
                requestExport = true;
            }
            ImGui::Separator();
            // Speichert Projektdatei (krom-project.json) UND Scene in einem Schritt.
            // saveProject ruft intern SaveEditorScene() auf — kein separater Eintrag noetig.
            if (ImGui::MenuItem(ICON_LC_SAVE "  Speichern", "Ctrl+S", false, static_cast<bool>(ctx.saveProject)))
            {
                const bool ok = ctx.saveProject ? ctx.saveProject() : false;
                ctx.lastFileMessage = ok ? "Gespeichert." : "Speichern fehlgeschlagen.";
            }
            ImGui::Separator();
            if (ImGui::MenuItem(ICON_LC_LOG_OUT "  Schliessen", "Esc"))
                ctx.state.closeConfirmOpen = true;
            ImGui::EndMenu();
        }

        if (ImGui::BeginMenu("Bearbeiten"))
        {
            if (ImGui::MenuItem(undoLabel.c_str(), "Ctrl+Z", false, canUndo))
                UndoEditorHistory(ctx);
            if (ImGui::MenuItem(redoLabel.c_str(), "Ctrl+Y", false, canRedo))
                RedoEditorHistory(ctx);
            ImGui::EndMenu();
        }

        if (ImGui::BeginMenu("Ansicht"))
        {
            ImGui::MenuItem(ICON_LC_LIST_TREE "  Hierarchie", nullptr, &ctx.hierarchyPanelVisible);
            ImGui::MenuItem(ICON_LC_FOLDER "  Assets", nullptr, &ctx.assetsPanelVisible);
            ImGui::MenuItem(ICON_LC_INFO "  Inspector", nullptr, &ctx.inspectorPanelVisible);
            if (ImGui::MenuItem(ICON_LC_LAYOUT_TEMPLATE "  Layout zuruecksetzen"))
            {
                ctx.hierarchyPanelVisible = true;
                ctx.assetsPanelVisible = true;
                ctx.inspectorPanelVisible = true;
                ctx.resetDockLayout = true;
            }
            ImGui::Separator();
            if (ImGui::MenuItem(ICON_LC_WRENCH "  Editor-Einstellungen..."))
                ctx.state.cameraSettingsOpen = true;
            ImGui::EndMenu();
        }

        if (ImGui::BeginMenu("Szene"))
        {
            if (ImGui::BeginMenu(ICON_LC_REFRESH_CW "  Wechseln"))
            {
                for (const auto& name : ctx.availableEditorScenes)
                    if (ImGui::MenuItem(name.c_str(), nullptr, name == ctx.currentEditorSceneName,
                                        ctx.switchEditorScene && name != ctx.currentEditorSceneName))
                        ctx.switchEditorScene(name, false);
                ImGui::EndMenu();
            }
            if (ImGui::BeginMenu(ICON_LC_LAYERS "  Additiv laden"))
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
            if (ImGui::MenuItem(ICON_LC_SETTINGS "  Szenen und Build…")) ctx.state.settingsWindowOpen = true;
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Hilfe"))
        {
            ImGui::TextDisabled("Werkzeuge");
            ImGui::TextUnformatted("0  Auswählen\\n1  Verschieben\\n2  Drehen\\n3  Skalieren");
            ImGui::Separator();
            ImGui::TextUnformatted("Kamera: rechte Maustaste + WASD");
            ImGui::TextUnformatted("Strg beim Ziehen: Einrasten umkehren");
            ImGui::EndMenu();
        }

        ImGui::EndMainMenuBar();
    }
    ImGui::PopStyleVar();
    if (requestExport)
        ImGui::OpenPopup("Spiel exportieren?##confirmExport");
    DrawExportConfirmation(ctx);
    DrawEditorToolbar(ctx);

    // ── Kamera-Einstellungen Dialog ───────────────────────────────────────────
    if (ctx.state.cameraSettingsOpen)
    {
        ProjectDialogStyle dialogStyle;
        SetProjectDialogSize(560.f, 760.f);
        if (ImGui::Begin("Editor-Einstellungen##camset", &ctx.state.cameraSettingsOpen, ImGuiWindowFlags_NoCollapse))
        {
            ImGui::BeginChild("##EditorSettingsBody", ImVec2(0.f, -52.f));
            EditorCameraState& cam = ctx.state.editorCamera;

            ImGui::SeparatorText("Kamera");
            if (ImGui::BeginTable("##CameraSettingsFields", 2, ImGuiTableFlags_SizingStretchSame))
            {
            // Separate label and input rows keep both columns on the same baseline.
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::TextUnformatted("FOV");
            ImGui::TableSetColumnIndex(1);
            ImGui::TextUnformatted("Near");
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::SetNextItemWidth(-1.f);
            ImGui::DragFloat("##CameraFOV", &cam.fovDeg, 0.5f, 10.f, 170.f, "%.1f°");
            ImGui::TableSetColumnIndex(1);
            ImGui::SetNextItemWidth(-1.f);
            ImGui::DragFloat("##CameraNear", &cam.nearPlane, 0.001f, 0.001f, 10.f, "%.3f m");
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::TextUnformatted("Far");
            ImGui::TableSetColumnIndex(1);
            ImGui::TextUnformatted("Geschwindigkeit");
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
            if (ImGui::Checkbox("IBL aktiv", &ctx.state.environmentEnableIBL))
            {
                ctx.state.ambientSource = ctx.state.environmentEnableIBL
                    ? EditorState::AmbientSource::Skybox : EditorState::AmbientSource::Color;
                ApplyEnvironmentFromState(ctx);
                if (ctx.setEditorAmbientLight)
                    ctx.setEditorAmbientLight(ctx.state.ambientColor, ctx.state.ambientIntensity);
            }
            ImGui::Checkbox("Skybox anzeigen", &ctx.state.showSkyboxBackground);
            ImGui::Checkbox("Performance-Overlay anzeigen", &ctx.state.showPerfOverlay);
            ImGui::Checkbox("Auswahl-Outline anzeigen", &ctx.state.showSelectionOutline);
            ImGui::PopStyleVar();
            ImGui::TextUnformatted("Hintergrund");
            ImGui::SetNextItemWidth(-1.f);
            if (ImGui::ColorEdit3("##Background", ctx.state.backgroundColor.data()))
            {
                if (ctx.setEditorBackgroundColor)
                    ctx.setEditorBackgroundColor(ctx.state.backgroundColor);
            }

            ImGui::Separator();

            ImGui::SeparatorText("Code Editor");
            ImGui::TextUnformatted("Programm");
            ImGui::SetNextItemWidth(-1.f);
            ImGui::InputText("##code_editor_executable", ctx.state.codeEditorExecutableBuffer.data(),
                             ctx.state.codeEditorExecutableBuffer.size());
            ImGui::TextUnformatted("Argumente");
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
            if (ImGui::Button("System Default", ImVec2(presetWidth, 32.f)))
            {
                ctx.state.codeEditorExecutableBuffer[0] = '\0';
                std::snprintf(ctx.state.codeEditorArgumentsBuffer.data(),
                              ctx.state.codeEditorArgumentsBuffer.size(), "%s", "\"{file}\"");
            }

            ImGui::Separator();

            ImGui::EndChild();
            const float actionWidth = std::min(150.f, std::max(1.f, (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x) * .5f));
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + std::max(0.f, ImGui::GetContentRegionAvail().x - 2.f * actionWidth - ImGui::GetStyle().ItemSpacing.x));
            if (ImGui::Button("Zuruecksetzen", ImVec2(actionWidth, 32.f)))
            {
                cam.fovDeg    = 60.f;
                cam.nearPlane = 0.05f;
                cam.farPlane  = 2000.f;
                cam.moveSpeed = 50.f;
            }
            ImGui::SameLine();
            ImGui::PushStyleColor(ImGuiCol_Button, theme::kAccent);
            if (ImGui::Button("Schliessen", ImVec2(actionWidth, 32.f)) || ImGui::IsKeyPressed(ImGuiKey_Escape, false))
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
