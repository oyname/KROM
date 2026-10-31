// =============================================================================
// KROM Engine - addons/editor/EditorUIProject.cpp
// Projekt- und Szenen-Verwaltung: Startbildschirm, Dialoge, Menueleiste,
// Projekt-Einstellungen, Szenen-Manager, Prefab speichern/spawnen.
// =============================================================================
#include "addons/editor/EditorUIInternal.hpp"
#include "core/PathUtf8.hpp"

#include <cstdio>

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

    constexpr float kPreviewAspect = 572.f / 852.f;
    const float pad = 0.f;
    const float maxW = std::max(1.f, size.x - pad * 2.f);
    const float maxH = std::max(1.f, size.y - pad * 2.f);
    float imageW = maxW;
    float imageH = imageW / kPreviewAspect;
    if (imageH > maxH)
    {
        imageH = maxH;
        imageW = imageH * kPreviewAspect;
    }
    const ImVec2 imageMin(min.x + (size.x - imageW) * 0.5f,
                          min.y + (size.y - imageH) * 0.5f);
    const ImVec2 imageMax(imageMin.x + imageW, imageMin.y + imageH);
    if (void* texId = GetStartPreviewTextureId(ctx))
        dl->AddImage(reinterpret_cast<ImTextureID>(texId), imageMin, imageMax);
    dl->AddRect(min, max, IM_COL32(56, 63, 68, 255));
    ImGui::EndChild();
}

static void* GetStartLogoTextureId(EditorFrameContext& ctx)
{
    static std::string s_logoPath;
    static TextureHandle s_logoTexture;
    return GetStartEditorTextureId(ctx, "logo.png", s_logoPath, s_logoTexture);
}

static void DrawStartLogo(EditorFrameContext& ctx, const ImVec2& size)
{
    ImGui::BeginChild("##StartLogoBox", size, false);
    constexpr float kLogoAspect = 629.f / 145.f;
    const float pad = 14.f;
    const float maxW = std::max(1.f, size.x - pad * 2.f);
    const float maxH = std::max(1.f, size.y - pad * 2.f);
    float logoW = maxW;
    float logoH = logoW / kLogoAspect;
    if (logoH > maxH)
    {
        logoH = maxH;
        logoW = logoH * kLogoAspect;
    }

    ImGui::SetCursorPos(ImVec2((size.x - logoW) * 0.5f, (size.y - logoH) * 0.5f));
    if (void* texId = GetStartLogoTextureId(ctx))
    {
        ImGui::Image(reinterpret_cast<ImTextureID>(texId), ImVec2(logoW, logoH));
    }
    else
    {
        ImGui::SetWindowFontScale(2.0f);
        ImGui::TextUnformatted("KROM ENGINE");
        ImGui::SetWindowFontScale(1.f);
    }
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

    if (ImGui::BeginPopupModal("Projekt erstellen##editor",
                               nullptr,
                               ImGuiWindowFlags_AlwaysAutoResize))
    {
        ImGui::InputText("Projektname", ctx.state.projectNameBuffer.data(), ctx.state.projectNameBuffer.size());

        // Projektordner mit Browse-Button
        {
            const bool hasBrowse = static_cast<bool>(ctx.browseForProjectFolder);
            const float browseButtonWidth = hasBrowse ? 28.f : 0.f;
            const float spacing = hasBrowse ? ImGui::GetStyle().ItemSpacing.x : 0.f;
            ImGui::Text("Projektordner");
            ImGui::SameLine();
            ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - browseButtonWidth - spacing);
            ImGui::InputText("##ProjectParentDir",
                             ctx.state.projectParentDirBuffer.data(),
                             ctx.state.projectParentDirBuffer.size());
            if (hasBrowse)
            {
                ImGui::SameLine();
                if (ImGui::Button("...##BrowseFolder", ImVec2(browseButtonWidth, 0.f)))
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
        ImGui::Combo("Backend", &ctx.state.projectBackendSelection, backendItems);

        const renderer::DeviceFactory::BackendType selectedBackend =
            EditorProjectBackendFromSelection(ctx.state.projectBackendSelection);
        ImGui::Separator();
        ImGui::Text("Aktuell: %s", ctx.currentProjectName.empty() ? "(kein Projekt)" : ctx.currentProjectName.c_str());
        ImGui::Text("Neues Projekt-Backend: %s", EditorProjectBackendLabel(selectedBackend));

        if (ImGui::Button("Erstellen", ImVec2(120.f, 0.f)))
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
        if (ImGui::Button("Abbrechen", ImVec2(120.f, 0.f)) ||
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

    if (ImGui::BeginPopupModal("Projekt oeffnen##editor",
                               nullptr,
                               ImGuiWindowFlags_AlwaysAutoResize))
    {
        // Pfad-Eingabe mit optionalem Browse-Button
        const bool hasBrowse = static_cast<bool>(ctx.browseForProjectFile);
        const float browseButtonWidth = hasBrowse ? 28.f : 0.f;
        const float spacing = hasBrowse ? ImGui::GetStyle().ItemSpacing.x : 0.f;
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - browseButtonWidth - spacing);
        ImGui::InputText("##ProjectOpenPath",
                         ctx.state.projectOpenPathBuffer.data(),
                         ctx.state.projectOpenPathBuffer.size());
        if (hasBrowse)
        {
            ImGui::SameLine();
            if (ImGui::Button("...##BrowseProject", ImVec2(browseButtonWidth, 0.f)))
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

        if (ImGui::Button("Oeffnen", ImVec2(120.f, 0.f)))
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
        if (ImGui::Button("Abbrechen", ImVec2(120.f, 0.f)) ||
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

    if (ImGui::BeginPopupModal("Anwendung schliessen?##editor",
                               nullptr,
                               ImGuiWindowFlags_AlwaysAutoResize))
    {
        ImGui::TextUnformatted("Editor wirklich schliessen?");
        ImGui::Separator();
        if (ImGui::Button("Schliessen", ImVec2(120.f, 0.f)))
        {
            if (ctx.requestClose)
                ctx.requestClose();
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Abbrechen", ImVec2(120.f, 0.f)) ||
            ImGui::IsKeyPressed(ImGuiKey_Escape, false))
        {
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}

// ── Snap-Schalter in der Menueleiste (wie die Unreal-Viewport-Toolbar) ──────────
// Je Art ein Umschalter (hervorgehoben = an) plus Auswahl der Schrittweite.
static bool SnapToggle(const char* label, bool& enabled, const char* tooltip)
{
    if (enabled)
        ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
    else
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.f, 0.f, 0.f, 0.f));
    const bool clicked = ImGui::SmallButton(label);
    ImGui::PopStyleColor();
    if (clicked)
        enabled = !enabled;
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", tooltip);
    return clicked;
}

static void SnapStepMenu(const char* id, float& value, const float* steps, size_t count, const char* format)
{
    char label[48];
    std::snprintf(label, sizeof(label), format, value);
    std::string menuLabel = std::string(label) + "##" + id;
    if (ImGui::BeginMenu(menuLabel.c_str()))
    {
        for (size_t i = 0; i < count; ++i)
        {
            char item[48];
            std::snprintf(item, sizeof(item), format, steps[i]);
            if (ImGui::MenuItem(item, nullptr, std::abs(value - steps[i]) < 1e-5f))
                value = steps[i];
        }
        ImGui::EndMenu();
    }
}

static void DrawSnapToolbar(EditorFrameContext& ctx)
{
    static constexpr float kMoveSteps[]   = {0.01f, 0.05f, 0.1f, 0.25f, 0.5f, 1.f, 2.f, 5.f, 10.f};
    static constexpr float kRotateSteps[] = {1.f, 5.f, 10.f, 15.f, 22.5f, 30.f, 45.f, 90.f};
    static constexpr float kScaleSteps[]  = {0.01f, 0.05f, 0.1f, 0.25f, 0.5f, 1.f};

    EditorSnapSettings& snap = ctx.state.snap;
    ImGui::Separator();
    ImGui::TextDisabled("Snap");

    SnapToggle("Pos##snap_move", snap.moveEnabled,
               "Positions-Raster beim Verschieben.\nStrg beim Ziehen kehrt um.");
    SnapStepMenu("snap_move_step", snap.moveStep, kMoveSteps, std::size(kMoveSteps), "%g m");

    SnapToggle("Rot##snap_rot", snap.rotateEnabled,
               "Winkel-Raster beim Drehen.\nStrg beim Ziehen kehrt um.");
    SnapStepMenu("snap_rot_step", snap.rotateStepDeg, kRotateSteps, std::size(kRotateSteps), "%g\xC2\xB0");

    SnapToggle("Skal##snap_scale", snap.scaleEnabled,
               "Skalierungs-Raster beim Skalieren.\nStrg beim Ziehen kehrt um.");
    SnapStepMenu("snap_scale_step", snap.scaleStep, kScaleSteps, std::size(kScaleSteps), "%g");

    ImGui::Separator();
    SnapToggle("Raster##snap_grid", snap.showGrid, "Bodenraster (y = 0) im Viewport anzeigen.");

    ImGui::Separator();
    SnapToggle("Symbole##show_editor_icons", ctx.state.showEditorIcons,
               "Symbole fuer Licht, Partikel und Audio im Viewport anzeigen.\n"
               "Aus: ausgeblendet und nicht anklickbar.");
    SnapToggle("Partikel##play_all_particles", ctx.state.playAllParticles,
               "An: alle Partikel-Emitter im Viewport abspielen.\n"
               "Aus: nur der ausgewaehlte Emitter und seine Kinder.\n"
               "Im Spiel laufen immer alle aktiven Emitter.");
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

    // Live-Link: alle Szenen-Instanzen synchronisieren
    engine::addons::editor::ResolvePrefabBindings(ctx);

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
        ctx.world.Add<PrefabInstanceComponent>(instance.root,
            PrefabInstanceComponent{ relPath, /*overrideTransform=*/true });
    }

    ctx.state.selectedEntity = instance.root;
    ctx.lastFileMessage = "Prefab geladen: " + PathToUtf8(path.filename());
    return true;
}

void DrawRuntimeBuildStatus(EditorFrameContext& ctx)
{
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
                                   viewport->WorkPos.y + 8.f),
                            ImGuiCond_Always, ImVec2(0.5f, 0.f));
    ImGui::SetNextWindowSize(ImVec2(480.f, 0.f), ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(0.94f);
    constexpr ImGuiWindowFlags kFlags =
        ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
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

    if (!ImGui::BeginPopupModal("Projekt-Einstellungen##settings",
                                nullptr,
                                ImGuiWindowFlags_AlwaysAutoResize))
        return;

    ImGui::TextDisabled("Runtime:");
    const char* backendItems[] = { "Vulkan", "DirectX11", "OpenGL" };
    int backendSelection = 0;
    switch (ctx.projectBackend)
    {
    case renderer::DeviceFactory::BackendType::DirectX11: backendSelection = 1; break;
    case renderer::DeviceFactory::BackendType::OpenGL: backendSelection = 2; break;
    case renderer::DeviceFactory::BackendType::Vulkan:
    default: backendSelection = 0; break;
    }
    ImGui::SetNextItemWidth(220.f);
    if (ImGui::Combo("Backend", &backendSelection, backendItems, IM_ARRAYSIZE(backendItems)))
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

    ImGui::TextDisabled("Szenenreihenfolge (Startszene = 1):");
    ImGui::Spacing();

    std::vector<std::string> order = ctx.sceneOrder;
    bool orderChanged = false;
    int moveFrom = -1, moveTo = -1;

    for (int i = 0; i < static_cast<int>(order.size()); ++i)
    {
        ImGui::PushID(i);

        // Hoch-Button zuerst (links), dann Szenenname — so bleiben sie immer sichtbar.
        const bool canMoveUp = (i > 0);
        if (!canMoveUp) ImGui::BeginDisabled();
        if (ImGui::SmallButton(" ^ ")) { moveFrom = i; moveTo = i - 1; orderChanged = true; }
        if (!canMoveUp) ImGui::EndDisabled();
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Nach oben");
        ImGui::SameLine();

        // Runter-Button
        const bool canMoveDown = (i < static_cast<int>(order.size()) - 1);
        if (!canMoveDown) ImGui::BeginDisabled();
        if (ImGui::SmallButton(" v ")) { moveFrom = i; moveTo = i + 1; orderChanged = true; }
        if (!canMoveDown) ImGui::EndDisabled();
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Nach unten");
        ImGui::SameLine();

        // Index + Name
        ImGui::TextDisabled("%d.", i + 1);
        ImGui::SameLine();
        ImGui::TextUnformatted(order[i].c_str());
        ImGui::SameLine();
        if (i == 0)
        {
            ImGui::TextDisabled("Start");
        }
        else
        {
            if (ImGui::SmallButton("Start"))
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
        std::swap(order[moveFrom], order[moveTo]);
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
    ImGui::TextDisabled("Build-Einschluss:");
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
    const bool canPlay = static_cast<bool>(ctx.playGame) && !ctx.sceneOrder.empty() &&
                         !ctx.runtimeBuildActive;
    if (!canPlay) ImGui::BeginDisabled();
    if (ImGui::Button("Play", ImVec2(-1.f, 0.f)))
    {
        const bool ok = ctx.playGame ? ctx.playGame() : false;
        if (!ok && ctx.lastFileMessage.empty())
            ctx.lastFileMessage = "Play fehlgeschlagen.";
    }
    if (!canPlay) ImGui::EndDisabled();

    ImGui::Spacing();
    const bool canBuild = static_cast<bool>(ctx.buildAllScenes) && !ctx.sceneOrder.empty();
    if (!canBuild) ImGui::BeginDisabled();
    ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.12f, 0.40f, 0.12f, 1.f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.18f, 0.56f, 0.18f, 1.f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(0.22f, 0.65f, 0.22f, 1.f));
    if (ImGui::Button("Spiel erstellen", ImVec2(-1.f, 0.f)))
    {
        const bool ok = ctx.buildAllScenes ? ctx.buildAllScenes() : false;
        ctx.lastFileMessage = ok ? "Build erfolgreich." : "Build fehlgeschlagen.";
    }
    ImGui::PopStyleColor(3);
    if (!canBuild) ImGui::EndDisabled();

    ImGui::Spacing();
    const bool canExport = static_cast<bool>(ctx.exportProject) && !ctx.sceneOrder.empty() &&
                           !ctx.runtimeBuildActive;
    if (!canExport) ImGui::BeginDisabled();
    if (ImGui::Button("Projekt exportieren", ImVec2(-1.f, 0.f)))
    {
        const bool ok = ctx.exportProject ? ctx.exportProject() : false;
        if (!ok && ctx.lastFileMessage.empty())
            ctx.lastFileMessage = "Export fehlgeschlagen.";
    }
    if (!canExport) ImGui::EndDisabled();
    if (ImGui::IsItemHovered() && canExport)
        ImGui::SetTooltip("Erstellt eine weitergebbare Release-Ausgabe unter\n"
                          "Build/<Config>-<Backend>. Der erste Export dauert einige Minuten.");

    ImGui::Spacing();
    if (ImGui::Button("Schliessen", ImVec2(-1.f, 0.f)) ||
        ImGui::IsKeyPressed(ImGuiKey_Escape, false))
        ImGui::CloseCurrentPopup();

    ImGui::EndPopup();
}

void DrawDeleteConfirmDialog(EditorFrameContext& ctx)
{
    if (ctx.state.deleteConfirmOpen)
    {
        ImGui::OpenPopup("Entity loeschen?##editor");
        ctx.state.deleteConfirmOpen = false;
    }

    if (ImGui::BeginPopupModal("Entity loeschen?##editor", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
    {
        const char* name = "(unbenannt)";
        if (ctx.state.selectedEntity.IsValid())
            if (const auto* nc = ctx.world.Get<NameComponent>(ctx.state.selectedEntity))
                if (!nc->name.empty())
                    name = nc->name.c_str();

        ImGui::Text("Entity \"%s\" loeschen?", name);
        ImGui::Separator();

        if (ImGui::Button("Loeschen", ImVec2(120.f, 0.f)))
        {
            DestroyEntityHierarchy(ctx, ctx.state.selectedEntity);
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Abbrechen", ImVec2(120.f, 0.f)) ||
            ImGui::IsKeyPressed(ImGuiKey_Escape, false))
        {
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
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

    const ImGuiIO& io = ImGui::GetIO();
    const ImVec2 windowSize{
        std::min(1030.f, io.DisplaySize.x - 12.f),
        std::min(560.f, io.DisplaySize.y - 12.f)
    };
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f),
                            ImGuiCond_Always,
                            ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(windowSize, ImGuiCond_Always);
    constexpr ImGuiWindowFlags kScreenFlags =
        ImGuiWindowFlags_NoTitleBar   |
        ImGuiWindowFlags_NoResize     |
        ImGuiWindowFlags_NoMove       |
        ImGuiWindowFlags_NoCollapse   |
        ImGuiWindowFlags_NoBringToFrontOnFocus |
        ImGuiWindowFlags_NoSavedSettings;

    if (!ImGui::Begin("##KromStartScreen", nullptr, kScreenFlags))
    {
        ImGui::End();
        return;
    }

    constexpr float kLauncherW = 640.f;
    constexpr float kLeftW = 290.f;
    constexpr float kHeaderH = 150.f;
    constexpr float kButtonW = 190.f;
    constexpr float kButtonH = 28.f;
    const float previewW = std::max(320.f, ImGui::GetContentRegionAvail().x - kLauncherW - ImGui::GetStyle().ItemSpacing.x);

    ImGui::BeginChild("##StartLauncher", ImVec2(kLauncherW, 0.f), false);
    DrawStartLogo(ctx, ImVec2(kLauncherW, kHeaderH));

    ImGui::Spacing();

    const float lowerH = ImGui::GetContentRegionAvail().y;
    ImGui::BeginChild("##StartActions", ImVec2(kLeftW, lowerH), false);
    if (ImGui::Button("Open Project", ImVec2(kButtonW, kButtonH)))
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
        {
            ctx.state.openProjectDialogOpen = true;
        }
    }

    ImGui::Spacing();
    if (ImGui::Button("New Project", ImVec2(kButtonW, kButtonH)))
        ctx.state.createProjectDialogOpen = true;

    if (ctx.browseForProjectFolder)
    {
        ImGui::Spacing();
        if (ImGui::Button("Change Folder", ImVec2(kButtonW, kButtonH)))
        {
            const std::string picked = ctx.browseForProjectFolder();
            if (!picked.empty())
                std::snprintf(ctx.state.projectParentDirBuffer.data(),
                              ctx.state.projectParentDirBuffer.size(),
                              "%s", picked.c_str());
        }
    }

    if (!ctx.lastFileMessage.empty())
    {
        ImGui::Dummy(ImVec2(1.f, 24.f));
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.96f, 0.55f, 0.35f, 1.f));
        ImGui::TextWrapped("%s", ctx.lastFileMessage.c_str());
            ImGui::PopStyleColor();
    }

    ImGui::Dummy(ImVec2(1.f, 24.f));
    ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.30f, 0.10f, 0.10f, 1.f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.50f, 0.15f, 0.15f, 1.f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(0.65f, 0.20f, 0.20f, 1.f));
    if (ImGui::Button("Close", ImVec2(kButtonW, kButtonH)))
        ctx.state.closeConfirmOpen = true;
    ImGui::PopStyleColor(3);

    const char* copyright = "KROM Editor Version 0.3";
    const float copyrightY = ImGui::GetWindowHeight() - ImGui::GetTextLineHeightWithSpacing() - 4.f;
    if (ImGui::GetCursorPosY() < copyrightY)
        ImGui::SetCursorPosY(copyrightY);
    ImGui::TextDisabled("%s", copyright);
    ImGui::EndChild();

    ImGui::SameLine();
    ImGui::BeginChild("##StartProjects", ImVec2(0.f, lowerH), false);
    ImGui::TextUnformatted("Projekte");
    ImGui::TextDisabled("%s", ctx.state.projectParentDirBuffer.data());
    ImGui::Separator();

    const std::vector<StartProjectEntry> projects = ScanStartProjects(ctx);
    const float listHeight = std::max(160.f, ImGui::GetContentRegionAvail().y - 2.f);
    ImGui::BeginChild("##StartProjectList", ImVec2(0.f, listHeight), false);
    if (projects.empty())
    {
        ImGui::Spacing();
        ImGui::TextDisabled("Keine Projekte gefunden.");
        ImGui::TextDisabled("Erstelle ein neues Projekt oder waehle einen anderen Projektordner.");
    }
    else
    {
        for (const StartProjectEntry& project : projects)
        {
            ImGui::PushID(project.projectFile.c_str());
            const bool selected = false;
            if (ImGui::Selectable(project.name.c_str(), selected, ImGuiSelectableFlags_AllowDoubleClick,
                                  ImVec2(0.f, 42.f)))
            {
                const bool ok = ctx.loadProject ? ctx.loadProject(project.projectFile) : false;
                if (!ok && ctx.lastFileMessage.empty())
                    ctx.lastFileMessage = "Projekt konnte nicht geladen werden.";
            }
            const ImVec2 itemMin = ImGui::GetItemRectMin();
            ImGui::SetCursorScreenPos(ImVec2(itemMin.x + 8.f, itemMin.y + 22.f));
            ImGui::TextDisabled("%s", project.path.c_str());
            ImGui::PopID();
        }
    }
    ImGui::EndChild();
    ImGui::EndChild();
    ImGui::EndChild();

    ImGui::SameLine();
    DrawStartEnginePreview(ctx, ImVec2(previewW, ImGui::GetContentRegionAvail().y));

    ImGui::End();
}

void DrawFileMenuBar(EditorFrameContext& ctx)
{
    RunFileShortcutActions(ctx);
    const bool canUndo = CanUndoEditorHistory(ctx.state);
    const bool canRedo = CanRedoEditorHistory(ctx.state);
    const std::string undoActionLabel = GetUndoEditorHistoryLabel(ctx.state);
    const std::string redoActionLabel = GetRedoEditorHistoryLabel(ctx.state);
    const std::string undoLabel = canUndo ? ("Undo " + undoActionLabel) : "Undo";
    const std::string redoLabel = canRedo ? ("Redo " + redoActionLabel) : "Redo";

    if (ImGui::BeginMainMenuBar())
    {
        if (ImGui::BeginMenu("Datei"))
        {
            if (ImGui::MenuItem("Neues Projekt...", nullptr, false, static_cast<bool>(ctx.createProject)))
                ctx.state.createProjectDialogOpen = true;
            if (ImGui::MenuItem("Projekt oeffnen...", nullptr, false, static_cast<bool>(ctx.loadProject)))
                ctx.state.openProjectDialogOpen = true;
            if (ImGui::MenuItem("Einstellungen...", nullptr, false, !ctx.currentProjectName.empty()))
                ctx.state.settingsWindowOpen = true;
            if (ImGui::MenuItem("Projekt exportieren", nullptr, false,
                                static_cast<bool>(ctx.exportProject) && !ctx.currentProjectName.empty() &&
                                !ctx.runtimeBuildActive))
            {
                const bool ok = ctx.exportProject();
                if (!ok && ctx.lastFileMessage.empty())
                    ctx.lastFileMessage = "Export fehlgeschlagen.";
            }
            ImGui::Separator();
            // Speichert Projektdatei (krom-project.json) UND Scene in einem Schritt.
            // saveProject ruft intern SaveEditorScene() auf — kein separater Eintrag noetig.
            if (ImGui::MenuItem("Speichern", "Ctrl+S", false, static_cast<bool>(ctx.saveProject)))
            {
                const bool ok = ctx.saveProject ? ctx.saveProject() : false;
                ctx.lastFileMessage = ok ? "Gespeichert." : "Speichern fehlgeschlagen.";
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Schliessen", "Esc"))
                ctx.state.closeConfirmOpen = true;
            ImGui::EndMenu();
        }

        if (ImGui::BeginMenu("Edit"))
        {
            if (ImGui::MenuItem(undoLabel.c_str(), "Ctrl+Z", false, canUndo))
                UndoEditorHistory(ctx);
            if (ImGui::MenuItem(redoLabel.c_str(), "Ctrl+Y", false, canRedo))
                RedoEditorHistory(ctx);
            ImGui::EndMenu();
        }

        if (ImGui::BeginMenu("Ansicht"))
        {
            ImGui::MenuItem("Hierarchie", nullptr, &ctx.hierarchyPanelVisible);
            ImGui::MenuItem("Assets", nullptr, &ctx.assetsPanelVisible);
            ImGui::MenuItem("Inspector", nullptr, &ctx.inspectorPanelVisible);
            ImGui::Separator();
            if (ImGui::MenuItem("Editor-Einstellungen..."))
                ctx.state.cameraSettingsOpen = true;
            ImGui::EndMenu();
        }

        const bool canPlay = static_cast<bool>(ctx.playGame) && !ctx.currentProjectName.empty() &&
                             !ctx.runtimeBuildActive;
        if (!canPlay)
            ImGui::BeginDisabled();
        if (ImGui::MenuItem("Play"))
        {
            const bool ok = ctx.playGame ? ctx.playGame() : false;
            if (!ok && ctx.lastFileMessage.empty())
                ctx.lastFileMessage = "Play fehlgeschlagen.";
        }
        if (!canPlay)
            ImGui::EndDisabled();

        DrawSnapToolbar(ctx);

        ImGui::EndMainMenuBar();
    }

    // ── Kamera-Einstellungen Dialog ───────────────────────────────────────────
    if (ctx.state.cameraSettingsOpen)
    {
        ImGui::SetNextWindowSize(ImVec2(300.f, 240.f), ImGuiCond_FirstUseEver);
        if (ImGui::Begin("Editor-Einstellungen##camset", &ctx.state.cameraSettingsOpen))
        {
            EditorCameraState& cam = ctx.state.editorCamera;

            ImGui::CollapsingHeader("Kamera", ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_Leaf);
            ImGui::DragFloat("FOV",               &cam.fovDeg,    0.5f,   10.f,    170.f, "%.1f°");
            ImGui::DragFloat("Near",              &cam.nearPlane, 0.001f, 0.001f,  10.f,  "%.3f m");
            ImGui::DragFloat("Far",               &cam.farPlane,  10.f,   10.f,    100000.f, "%.0f m");
            ImGui::DragFloat("Geschwindigkeit",   &cam.moveSpeed, 1.f,    0.1f,    10000.f, "%.1f");

            cam.farPlane  = std::max(cam.nearPlane + 1.f, cam.farPlane);
            cam.nearPlane = std::clamp(cam.nearPlane, 0.001f, cam.farPlane - 1.f);
            cam.fovDeg    = std::clamp(cam.fovDeg, 10.f, 170.f);
            cam.moveSpeed = std::max(0.1f, cam.moveSpeed);

            ImGui::Separator();

            ImGui::CollapsingHeader("Viewport", ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_Leaf);
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
            if (ImGui::ColorEdit3("Hintergrund", ctx.state.backgroundColor.data()))
            {
                if (ctx.setEditorBackgroundColor)
                    ctx.setEditorBackgroundColor(ctx.state.backgroundColor);
            }

            ImGui::Separator();

            ImGui::CollapsingHeader("Code Editor", ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_Leaf);
            constexpr float kCodeEditorLabelWidth = 82.f;
            ImGui::TextUnformatted("Executable");
            ImGui::SameLine(kCodeEditorLabelWidth);
            ImGui::SetNextItemWidth(-1.f);
            ImGui::InputText("##code_editor_executable", ctx.state.codeEditorExecutableBuffer.data(),
                             ctx.state.codeEditorExecutableBuffer.size());
            ImGui::TextUnformatted("Arguments");
            ImGui::SameLine(kCodeEditorLabelWidth);
            ImGui::SetNextItemWidth(-1.f);
            ImGui::InputText("##code_editor_arguments", ctx.state.codeEditorArgumentsBuffer.data(),
                             ctx.state.codeEditorArgumentsBuffer.size());
            if (ImGui::Button("VS Code"))
            {
                std::snprintf(ctx.state.codeEditorExecutableBuffer.data(),
                              ctx.state.codeEditorExecutableBuffer.size(), "%s", "code");
                std::snprintf(ctx.state.codeEditorArgumentsBuffer.data(),
                              ctx.state.codeEditorArgumentsBuffer.size(), "%s", "\"{workspace}\" -g \"{file}:{line}\"");
            }
            ImGui::SameLine();
            if (ImGui::Button("System Default"))
            {
                ctx.state.codeEditorExecutableBuffer[0] = '\0';
                std::snprintf(ctx.state.codeEditorArgumentsBuffer.data(),
                              ctx.state.codeEditorArgumentsBuffer.size(), "%s", "\"{file}\"");
            }

            ImGui::Separator();

            if (ImGui::Button("Zuruecksetzen"))
            {
                cam.fovDeg    = 60.f;
                cam.nearPlane = 0.05f;
                cam.farPlane  = 2000.f;
                cam.moveSpeed = 50.f;
            }
            ImGui::SameLine();
            if (ImGui::Button("Schliessen"))
                ctx.state.cameraSettingsOpen = false;
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
