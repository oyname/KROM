// =============================================================================
// KROM Engine - addons/editor/EditorUIHierarchy.cpp
// Hierarchie-Panel (Entity-Liste, Drag & Drop, Kontextmenue).
// =============================================================================
#include "addons/editor/EditorUIInternal.hpp"
#include "core/PathUtf8.hpp"
#include "addons/particles/ParticleComponents.hpp"
#include "addons/probes/ProbeComponents.hpp"

#include "imgui_internal.h"

#ifdef KROM_EDITOR_HAS_IMGUI

namespace engine::renderer::addons::editor {

using namespace detail;

namespace {

// Pfad aus dem Prefab-Drag-Payload (relativ zu Assets) als Dateipfad.
std::filesystem::path PrefabPayloadPath(const EditorFrameContext& ctx, const char* payloadPath)
{
    std::filesystem::path path = PathFromUtf8(payloadPath);
    if (path.is_relative() && !ctx.currentProjectRoot.empty())
        path = PathFromUtf8(ctx.currentProjectRoot) / "Assets" / path;
    return path;
}

void FrameHierarchyEntity(EditorFrameContext& ctx, EntityID entity)
{
    math::Vec3 target = WorldPosition(ctx, entity);
    math::Vec3 lower, upper;
    bool hasMeshBounds = false;
    std::vector<EntityID> pending{entity};
    while (!pending.empty())
    {
        const EntityID id = pending.back();
        pending.pop_back();
        if (const auto* children = ctx.world.Get<ChildrenComponent>(id))
            pending.insert(pending.end(), children->children.begin(), children->children.end());

        const auto* mesh = ctx.world.Get<MeshComponent>(id);
        const auto* bounds = ctx.world.Get<BoundsComponent>(id);
        if (!mesh || !mesh->mesh.IsValid() || !mesh->visible || !bounds)
            continue;

        const math::Vec3 minPoint = bounds->centerWorld - bounds->extentsWorld;
        const math::Vec3 maxPoint = bounds->centerWorld + bounds->extentsWorld;
        if (hasMeshBounds)
        {
            lower = {std::min(lower.x, minPoint.x), std::min(lower.y, minPoint.y), std::min(lower.z, minPoint.z)};
            upper = {std::max(upper.x, maxPoint.x), std::max(upper.y, maxPoint.y), std::max(upper.z, maxPoint.z)};
        }
        else
        {
            lower = minPoint;
            upper = maxPoint;
            hasMeshBounds = true;
        }
    }

    float radius = 0.f;
    if (hasMeshBounds)
    {
        target = (lower + upper) * 0.5f;
        radius = (upper - lower).Length() * 0.5f;
    }

    EditorCameraState& cam = ctx.state.editorCamera;
    const math::Vec3 toTarget = target - cam.position;
    if (toTarget.LengthSq() > 1e-6f)
    {
        const math::Vec3 direction = toTarget.Normalized();
        cam.yawDeg = std::atan2(-direction.x, -direction.z) * math::RAD_TO_DEG;
        cam.pitchDeg = std::clamp(
            std::asin(std::clamp(direction.y, -1.f, 1.f)) * math::RAD_TO_DEG, -89.f, 89.f);
    }

    const float aspect = ctx.renderViewHeight > 0.f && ctx.renderViewWidth > 0.f
        ? ctx.renderViewWidth / ctx.renderViewHeight : 1.f;
    const float halfVerticalFov = cam.fovDeg * math::DEG_TO_RAD * 0.5f;
    const float halfFov = std::atan(std::tan(halfVerticalFov) * std::min(aspect, 1.f));
    const float distance = std::max({2.f, radius * 1.15f / std::sin(halfFov), radius + cam.nearPlane});
    const math::Vec3 forward =
        math::Quat::FromEulerDeg(cam.pitchDeg, cam.yawDeg, 0.f).Rotate({0.f, 0.f, -1.f});
    cam.position = target - forward * distance;
}

} // namespace

namespace detail {

void DrawEntityList(EditorFrameContext& ctx)
{
    auto& highlighted = ctx.state.hierarchyHighlightedEntity;
    if (!ctx.world.IsAlive(highlighted))
        highlighted = NULL_ENTITY;

    const EntityID reveal = ctx.state.hierarchyRevealEntity;
    const bool focusOnReveal = ctx.state.hierarchyRevealFocus;
    std::unordered_set<uint32_t> revealAncestors;
    if (ctx.world.IsAlive(reveal))
    {
        highlighted = reveal;
        for (EntityID ancestor = reveal; ctx.world.IsAlive(ancestor); )
        {
            if (!revealAncestors.insert(ancestor.value).second)
                break;
            const auto* parent = ctx.world.Get<ParentComponent>(ancestor);
            ancestor = parent ? parent->parent : NULL_ENTITY;
        }
        ImGui::SetNextWindowCollapsed(false);
        if (focusOnReveal)
            ImGui::SetNextWindowFocus();
    }
    else
        ctx.state.hierarchyRevealEntity = NULL_ENTITY;
    SolidEditorPanelStyleScope solidPanelStyle;
    // Im Prefab-Modus leicht blau, damit klar ist, dass man das Prefab bearbeitet (wie Unity).
    const bool prefabMode = IsInPrefabMode(ctx.state);
    if (prefabMode)
        ImGui::PushStyleColor(ImGuiCol_WindowBg, theme::Hex(0x13202C));
    const std::string hierarchyTitle = TranslateLabel("menu.hierarchy", "Hierarchie##editor");
    KeepNextWindowInMainViewport(hierarchyTitle);
    const bool hierarchyVisible = ImGui::Begin(hierarchyTitle.c_str(), &ctx.hierarchyPanelVisible, kAlignedPanelFlags);
    if (prefabMode)
        ImGui::PopStyleColor();
    if (!hierarchyVisible)
    {
        ImGui::End();
        return;
    }

    // Prefab-Modus: Name und Speichern. "Zur Szene" sitzt oben links im
    // Viewport, Partikel steuert die Leiste unten im Viewport.
    if (prefabMode)
    {
        const bool readOnly = ctx.state.prefabModeReadOnly;
        const std::string prefabName = PathToUtf8(PathFromUtf8(ctx.state.prefabModePath).filename());
        ImGui::Spacing();
        ImGui::PushStyleColor(ImGuiCol_Text, theme::kAccentHover);
        ImGui::TextWrapped("%s", prefabName.c_str());
        ImGui::PopStyleColor();
        ImGui::TextDisabled(readOnly ? Translate("hierarchy.model_preview") : Translate("hierarchy.edit_prefab"));
        ImGui::Spacing();
        if (readOnly)
        {
            if (EditorActionButton(TranslateLabel("prefab.save_as", "Als Prefab speichern##model_view").c_str(), ImVec2(-1.f, 34.f), EditorButtonTone::Primary))
            {
                const EntityID modelRoot = PrefabModeRootEntity(ctx);
                if (modelRoot.IsValid())
                    SaveEntityAsPrefab(ctx, modelRoot);
            }
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip(Translate("hierarchy.model_to_prefab"));
        }
        else if (EditorActionButton(TranslateLabel("prefab.save", "Prefab speichern##prefab_mode").c_str(), ImVec2(-1.f, 34.f), EditorButtonTone::Primary))
        {
            SavePrefabMode(ctx);
        }
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();
    }

    static bool showMeshes = true;
    static bool showCameras = true;
    static bool showLights = true;
    static bool showProbes = true;
    static bool showOther = true;
    if (focusOnReveal && !revealAncestors.empty())
        showMeshes = showCameras = showLights = showProbes = showOther = true;

    constexpr int kTypeCount = 5;
    const int visibleTypes = static_cast<int>(showMeshes) + static_cast<int>(showCameras) +
                             static_cast<int>(showLights) + static_cast<int>(showProbes) +
                             static_cast<int>(showOther);
    const bool filtered = visibleTypes != kTypeCount;
    const std::string summary = visibleTypes == kTypeCount ? "Alle Typen" :
        (visibleTypes == 0 ? "Keine Typen" :
         std::to_string(visibleTypes) + " von " + std::to_string(kTypeCount) + " Typen");
    const int filterIconSpaces = static_cast<int>(std::ceil(24.f / std::max(1.f, ImGui::CalcTextSize(" ").x)));
    const std::string preview = std::string(filterIconSpaces, ' ') + summary;
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(8, 6));
    if (filtered)
    {
        ImGui::PushStyleColor(ImGuiCol_FrameBg, theme::kSelected);
        ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, theme::kMenuOpen);
    }
    const ImVec2 filterPos = ImGui::GetCursorScreenPos();
    const float filterHeight = ImGui::GetFrameHeight();
    ImDrawList* filterDrawList = ImGui::GetWindowDrawList();
    const ImU32 filterColor = ImGui::GetColorU32(ImGuiCol_Text);
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
    const bool filterOpen = ImGui::BeginCombo("##hierarchyTypeFilter", preview.c_str());
    // Im Parent zeichnen: bei offenem Combo ist das aktuelle Fenster bereits das Popup.
    DrawIcon(filterDrawList, ICON_LC_FILTER, ImVec2(filterPos.x + 17.f, filterPos.y + filterHeight * 0.5f),
             16.f, filterColor);
    if (filterOpen)
    {
        ImGui::TextUnformatted(Translate("hierarchy.filters"));
        ImGui::Separator();
        ImGui::Checkbox("Meshes", &showMeshes);
        ImGui::Checkbox(TranslateLabel("hierarchy.cameras", "Kameras").c_str(), &showCameras);
        ImGui::Checkbox(TranslateLabel("hierarchy.lights", "Lichter").c_str(), &showLights);
        ImGui::Checkbox(TranslateLabel("hierarchy.reflection_probes", "Reflection Probes").c_str(), &showProbes);
        ImGui::Checkbox(TranslateLabel("hierarchy.other", "Sonstige Objekte").c_str(), &showOther);
        ImGui::Separator();
        if (ImGui::Button(TranslateLabel("hierarchy.show_all", "Alle anzeigen").c_str()))
            showMeshes = showCameras = showLights = showProbes = showOther = true;
        ImGui::SameLine();
        if (ImGui::Button(TranslateLabel("common.none", "Keine").c_str()))
            showMeshes = showCameras = showLights = showProbes = showOther = false;
        ImGui::EndCombo();
    }
    else if (ImGui::IsItemHovered())
        ImGui::SetTooltip(Translate("hierarchy.filter_tip"));
    if (filtered) ImGui::PopStyleColor(2);
    ImGui::PopStyleVar();
    ImGui::Spacing();
    ImGui::Separator();

    std::vector<EntityID> entities;
    ctx.world.ForEachAlive([&](EntityID id) {
        entities.push_back(id);
    });
    std::sort(entities.begin(), entities.end(), [&](EntityID a, EntityID b) {
        const uint32_t orderA = GetEntityHierarchyOrder(ctx, a);
        const uint32_t orderB = GetEntityHierarchyOrder(ctx, b);
        if (orderA != orderB)
            return orderA < orderB;
        return a.value < b.value;
    });

    auto passesFilter = [&](EntityID id) {
        // Laufzeit-Gizmo-Entities sind rein visuelle Hilfsobjekte — nie in der Liste anzeigen
        if (ctx.world.Get<EditorRuntimeGizmoTag>(id) != nullptr)
            return false;
        if (ctx.world.Get<EditorMaterialPreviewComponent>(id) != nullptr)
            return false;
        if (ctx.world.Get<EditorAssetThumbnailComponent>(id) != nullptr)
            return false;
        if (ctx.world.Get<EditorPreviewLightComponent>(id) != nullptr)
            return false;
        // Zweite Absicherung: Meshes auf dem Editor-Gizmo-Layer nie anzeigen
        if (const auto* mc = ctx.world.Get<MeshComponent>(id))
            if (mc->layerMask == renderer::LAYER_EDITOR_GIZMO ||
                mc->layerMask == renderer::LAYER_EDITOR_GIZMO_DEPTH ||
                mc->layerMask == renderer::LAYER_EDITOR_MATERIAL_PREVIEW ||
                mc->layerMask == renderer::LAYER_EDITOR_ASSET_THUMBNAIL)
                return false;
        if (id == highlighted)
            return true;
        const bool isCamera = ctx.world.Get<CameraComponent>(id) != nullptr;
        const bool isLight = ctx.world.Get<LightComponent>(id) != nullptr;
        const bool isMesh = ctx.world.Get<MeshComponent>(id) != nullptr;
        const bool isProbe = ctx.world.Get<ReflectionProbeComponent>(id) != nullptr;
        const bool isOther = !isCamera && !isLight && !isMesh && !isProbe;
        // An object shows when any of its types is selected, so a mesh with a
        // reflection probe is found by "Reflection Probes" alone.
        return (isMesh && showMeshes) || (isCamera && showCameras) ||
               (isLight && showLights) || (isProbe && showProbes) ||
               (isOther && showOther);
    };

    if (ctx.state.assetBrowser)
        LoadEditorIcons(ctx, *ctx.state.assetBrowser);

    // Platz fuer das Symbol vor dem Namen: so viele Leerzeichen wie das Symbol breit ist
    const float iconSize = ImGui::GetFontSize();
    const float spaceWidth = std::max(1.f, ImGui::CalcTextSize(" ").x);
    const int iconSpaces = static_cast<int>(std::ceil((iconSize + 4.f) / spaceWidth));
    const int markerSpaces = static_cast<int>(std::ceil((iconSize * 0.6f + 4.f) / spaceWidth));

    // Liefert das Typ-Symbol oder nullptr, dann bleibt das Text-Praefix im Label.
    auto makeLabel = [&](EntityID id, int prefixSpaces, char* label, size_t labelSize) -> const AssetBrowserState::Icon* {
        // Rangfolge: Ordner, Prefab-Instanz, Camera, Light (nach Typ), Partikel, Audio, Probe,
        // Mesh, sonst Entity
        const AssetBrowserState::Icons* icons = ctx.state.assetBrowser ? &ctx.state.assetBrowser->icons : nullptr;
        const char* typePrefix = "[Entity]";
        const AssetBrowserState::Icon* icon = icons ? &icons->entity : nullptr;
        if (ctx.world.Get<FolderComponent>(id) != nullptr)
        {
            typePrefix = "[Folder]";
            icon = icons ? &icons->folder : nullptr;
        }
        else if (ctx.world.Get<PrefabInstanceComponent>(id) != nullptr)
        {
            typePrefix = "[Prefab]";
            icon = icons ? &icons->prefab : nullptr;
        }
        else if (ctx.world.Get<CameraComponent>(id) != nullptr)
        {
            typePrefix = "[Camera]";
            icon = icons ? &icons->camera : nullptr;
        }
        else if (const auto* light = ctx.world.Get<LightComponent>(id))
        {
            typePrefix = "[Light]";
            if (icons)
            {
                switch (light->type)
                {
                case LightType::Directional: icon = &icons->lightDirectional; break;
                case LightType::Spot:        icon = &icons->lightSpot;        break;
                default:                     icon = &icons->lightPoint;       break;
                }
            }
        }
        else if (ctx.world.Get<ParticleEmitterComponent>(id) != nullptr)
        {
            typePrefix = "[Particle]";
            icon = icons ? &icons->particleEmitter : nullptr;
        }
        else if (ctx.world.Get<AudioSourceComponent>(id) != nullptr)
        {
            typePrefix = "[Audio]";
            icon = icons ? &icons->audioSource : nullptr;
        }
        else if (ctx.world.Get<ReflectionProbeComponent>(id) != nullptr)
        {
            typePrefix = "[Probe]";
            icon = icons ? &icons->reflectionProbe : nullptr;
        }
        else if (ctx.world.Get<MeshComponent>(id) != nullptr)
        {
            typePrefix = "[Mesh]";
            icon = icons ? &icons->mesh : nullptr;
        }
        if (icon && !icon->imguiId)
            icon = nullptr;

        const char* name = nullptr;
        if (const auto* nc = ctx.world.Get<NameComponent>(id))
            name = nc->name.c_str();

        if (icon)
        {
            const int spaces = prefixSpaces + iconSpaces;
            if (name && name[0] != '\0')
                std::snprintf(label, labelSize, "%*s%s", spaces, "", name);
            else
                std::snprintf(label, labelSize, "%*sEntity %u", spaces, "", id.Index());
        }
        else if (name && name[0] != '\0')
            std::snprintf(label, labelSize, "%*s%s %s", prefixSpaces, "", typePrefix, name);
        else
            std::snprintf(label, labelSize, "%*s%s Entity %u", prefixSpaces, "", typePrefix, id.Index());
        return icon;
    };

    auto isDirectChildOf = [&](EntityID child, EntityID parent) {
        const auto* pc = ctx.world.Get<ParentComponent>(child);
        return pc && pc->parent == parent;
    };

    auto hasVisibleDescendant = [&](auto&& self, EntityID parent, uint32_t depth) -> bool {
        if (depth > 32u)
            return false;

        for (EntityID candidate : entities)
        {
            if (!isDirectChildOf(candidate, parent))
                continue;
            if (passesFilter(candidate) || self(self, candidate, depth + 1u))
                return true;
        }
        return false;
    };

    // Set when the mouse is over a row; the panel-wide root drop then stays off,
    // so releasing a drag on its own row does not move the entity to the root.
    bool mouseOverRow = false;

    auto drawEntity = [&](auto&& self, EntityID id, uint32_t depth) -> void {
        if (depth > 32u)
            return;

        const bool visibleSelf = passesFilter(id);
        const bool visibleDescendant = hasVisibleDescendant(hasVisibleDescendant, id, depth);
        if (!visibleSelf && !visibleDescendant)
            return;

        const auto* origin = ctx.world.Get<EditorSceneTagComponent>(id);
        const bool carriedOver = !prefabMode && !ctx.currentEditorSceneName.empty() &&
            origin && !origin->sceneName.empty() && origin->sceneName != ctx.currentEditorSceneName &&
            ctx.world.Has<EditorPersistentComponent>(id) &&
            std::find(ctx.loadedEditorScenes.begin(), ctx.loadedEditorScenes.end(), origin->sceneName) ==
                ctx.loadedEditorScenes.end();
        const int prefixSpaces = carriedOver ? markerSpaces : 0;

        char label[112];
        const AssetBrowserState::Icon* icon = makeLabel(id, prefixSpaces, label, sizeof(label));

        bool selected = (ctx.state.selectedEntity == id)
                     || IsEntityInMultiSelection(ctx.state, id);
        ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow |
                                   ImGuiTreeNodeFlags_SpanAvailWidth |
                                   ImGuiTreeNodeFlags_DefaultOpen;
        if (selected)
            flags |= ImGuiTreeNodeFlags_Selected;
        if (!visibleDescendant)
            flags |= ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen;

        if (!visibleSelf)
            ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));

        if (id != reveal && revealAncestors.count(id.value) > 0)
            ImGui::SetNextItemOpen(true);
        const bool open = ImGui::TreeNodeEx(
            reinterpret_cast<void*>(static_cast<uintptr_t>(GetStableEntityId(ctx.state, id))),
            flags,
            "%s",
            label);
        const ImVec2 rowMin = ImGui::GetItemRectMin();
        const ImVec2 rowMax = ImGui::GetItemRectMax();
        const ImGuiID rowId = ImGui::GetItemID();
        // Drop area of the row: full panel width and half the item spacing above
        // and below, so the gaps between rows and the indent belong to a row and
        // never fall through to the panel-wide root target (that flickered).
        const float halfGap = ImGui::GetStyle().ItemSpacing.y * 0.5f + 1.f;
        const ImRect dropRect(ImVec2(ImGui::GetWindowPos().x, rowMin.y - halfGap),
                              ImVec2(std::max(rowMax.x, ImGui::GetWindowPos().x + ImGui::GetWindowWidth()),
                                     rowMax.y + halfGap));
        if (visibleSelf && ImGui::IsMouseHoveringRect(dropRect.Min, dropRect.Max, false))
            mouseOverRow = true;
        if (id == reveal)
        {
            const ImRect& clipRect = ImGui::GetCurrentWindow()->ClipRect;
            if (focusOnReveal || rowMin.y < clipRect.Min.y || rowMax.y > clipRect.Max.y)
                ImGui::SetScrollHereY(0.5f);
            ctx.state.hierarchyRevealEntity = NULL_ENTITY;
        }
        if (id == highlighted && ImGui::IsItemVisible())
        {
            ImGui::GetWindowDrawList()->AddRect(
                ImGui::GetItemRectMin(), ImGui::GetItemRectMax(),
                ImGui::GetColorU32(theme::kAccentHover), 2.f, 2.f);
        }

        const float labelX = rowMin.x + iconSize + ImGui::GetStyle().FramePadding.x * 2.f;
        const float rowCenterY = (rowMin.y + rowMax.y) * 0.5f;
        if (carriedOver && ImGui::IsItemVisible())
        {
            ImGui::GetWindowDrawList()->AddCircleFilled(
                ImVec2(labelX + prefixSpaces * spaceWidth * 0.5f, rowCenterY),
                iconSize * 0.22f, ImGui::GetColorU32(theme::kWarning));
            ImGui::SetItemTooltip(Translate("hierarchy.carried_persistent"), origin->sceneName.c_str());
        }

        // Typ-Symbol an die Stelle der fuehrenden Leerzeichen zeichnen
        if (icon)
        {
            const float textX = labelX + prefixSpaces * spaceWidth;
            ImVec2 size(iconSize, iconSize);
            if (icon->aspect > 1.f)
                size.y = iconSize / icon->aspect;
            else
                size.x = iconSize * icon->aspect;
            const ImVec2 iconMin(textX + (iconSize - size.x) * 0.5f,
                                 rowCenterY - size.y * 0.5f);
            ImGui::GetWindowDrawList()->AddImage(
                reinterpret_cast<ImTextureID>(icon->imguiId),
                iconMin,
                ImVec2(iconMin.x + size.x, iconMin.y + size.y),
                ImVec2(0.f, 0.f),
                ImVec2(1.f, 1.f),
                visibleSelf ? IM_COL32_WHITE : IM_COL32(255, 255, 255, 110));
        }

        // Rechtsklick-Kontextmenü pro Entity
        if (visibleSelf && ImGui::BeginPopupContextItem())
        {
            highlighted = NULL_ENTITY;
            ClearMultiSelection(ctx);
            ctx.state.selectedEntity  = id;
            ctx.state.selectionSource = SelectionSource::Outliner;

            if (ImGui::MenuItem(TranslateLabel("common.duplicate", "Duplizieren").c_str()))
            {
                const EntityID dup = DuplicateEntity(ctx, id);
                if (dup.IsValid())
                    ctx.state.selectedEntity = dup;
            }
            if (ImGui::MenuItem(TranslateLabel("common.move_up", "Nach oben").c_str()))
            {
                ExecuteSceneMutation(ctx, "history.reorder_hierarchy", [&]() {
                    MoveEntityInSiblingOrder(ctx, id, -1);
                });
            }
            if (ImGui::MenuItem(TranslateLabel("common.move_down", "Nach unten").c_str()))
            {
                ExecuteSceneMutation(ctx, "history.reorder_hierarchy", [&]() {
                    MoveEntityInSiblingOrder(ctx, id, 1);
                });
            }
            ImGui::Separator();
            if (ImGui::MenuItem(TranslateLabel("common.delete", "Loeschen").c_str()))
                ctx.state.deleteConfirmOpen = true;
            if (FindOutermostPrefabInstanceRoot(ctx, id).IsValid())
            {
                if (ImGui::MenuItem(TranslateLabel("prefab.reset_instance", "Instanz auf Prefab zuruecksetzen").c_str()))
                    engine::addons::editor::ResetPrefabInstanceOverrides(ctx, id);
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("%s", Translate("prefab.reset_instance_tip"));
                if (ImGui::MenuItem(TranslateLabel("prefab.unpack", "Prefab entkoppeln").c_str()))
                    UnpackPrefabInstance(ctx, id);
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("%s", Translate("prefab.unpack_tip"));
            }
            ImGui::Separator();
            if (ImGui::BeginMenu(TranslateLabel("common.create", "Erstellen").c_str()))
            {
                if (ImGui::MenuItem(TranslateLabel("hierarchy.empty_entity", "Leere Entity").c_str()))
                    CreateEmptyEntity(ctx);
                if (ImGui::MenuItem(TranslateLabel("hierarchy.folder", "Ordner").c_str()))
                    CreateFolderEntity(ctx, Translate("hierarchy.folder_name"));
                ImGui::Separator();
                if (ImGui::MenuItem(TranslateLabel("component.camera", "Camera").c_str()))
                    CreateCameraEntity(ctx);
                ImGui::Separator();
                if (ImGui::MenuItem(TranslateLabel("component.directional_light", "Directional Light").c_str()))
                    CreateLightEntity(ctx, LightType::Directional);
                if (ImGui::MenuItem(TranslateLabel("component.point_light", "Point Light").c_str()))
                    CreateLightEntity(ctx, LightType::Point);
                if (ImGui::MenuItem(TranslateLabel("component.spot_light", "Spot Light").c_str()))
                    CreateLightEntity(ctx, LightType::Spot);
                ImGui::EndMenu();
            }

            ImGui::EndPopup();
        }

        if (ImGui::IsItemHovered() &&
            ImGui::IsMouseReleased(ImGuiMouseButton_Left) &&
            ImGui::GetIO().MouseDragMaxDistanceSqr[ImGuiMouseButton_Left] < 36.0f &&
            visibleSelf &&
            !ShouldCancelSelectionChangeForActiveInput())
        {
            highlighted = NULL_ENTITY;
            if (ImGui::GetIO().KeyCtrl)
                AddToMultiSelection(ctx, id);
            else
            {
                ClearMultiSelection(ctx);
                ctx.state.selectedEntity  = id;
                ctx.state.selectionSource = SelectionSource::Outliner;
            }
        }

        if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) &&
            visibleSelf && !ShouldCancelSelectionChangeForActiveInput())
        {
            highlighted = NULL_ENTITY;
            ClearMultiSelection(ctx);
            ctx.state.selectedEntity  = id;
            ctx.state.selectionSource = SelectionSource::Outliner;
            FrameHierarchyEntity(ctx, id);
        }

        if (visibleSelf && ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceAllowNullID))
        {
            ImGui::SetDragDropPayload("KROM_EDITOR_ENTITY", &id, sizeof(EntityID));
            const char* dragLabel = label;
            while (*dragLabel == ' ')
                ++dragLabel;
            ImGui::TextUnformatted(dragLabel);
            ImGui::EndDragDropSource();
        }

        if (visibleSelf && ImGui::BeginDragDropTargetCustom(dropRect, rowId))
        {
            // Entity drag: upper quarter = insert before, lower quarter = insert
            // after (as sibling of this row), middle = become its last child.
            const ImGuiPayload* active = ImGui::GetDragDropPayload();
            const bool entityDrag = active && active->IsDataType("KROM_EDITOR_ENTITY") &&
                                    active->DataSize == sizeof(EntityID);
            const EntityID draggedPreview = entityDrag
                ? *static_cast<const EntityID*>(active->Data) : NULL_ENTITY;
            enum class DropZone { Before, Inside, After };
            DropZone zone = DropZone::Inside;
            if (entityDrag && draggedPreview != id)
            {
                const float rowHeight = rowMax.y - rowMin.y;
                const float mouseY = ImGui::GetIO().MousePos.y;
                if (mouseY < rowMin.y + rowHeight * 0.25f)
                    zone = DropZone::Before;
                else if (mouseY > rowMax.y - rowHeight * 0.25f)
                    zone = DropZone::After;
            }
            if (zone != DropZone::Inside)
            {
                const float lineY = zone == DropZone::Before ? rowMin.y : rowMax.y;
                ImGui::GetWindowDrawList()->AddLine(ImVec2(rowMin.x, lineY), ImVec2(rowMax.x, lineY),
                                                    ImGui::GetColorU32(theme::kAccentHover), 2.f);
            }
            const ImGuiDragDropFlags entityFlags =
                zone == DropZone::Inside ? ImGuiDragDropFlags_None : ImGuiDragDropFlags_AcceptNoDrawDefaultRect;
            if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("KROM_EDITOR_ENTITY", entityFlags))
            {
                if (payload->DataSize == sizeof(EntityID))
                {
                    const EntityID dragged = *static_cast<const EntityID*>(payload->Data);
                    if (zone == DropZone::Inside)
                        MoveEntityInHierarchy(ctx, dragged, id, NULL_ENTITY, true);
                    else
                        MoveEntityInHierarchy(ctx, dragged, GetEntityParent(ctx, id), id,
                                              zone == DropZone::After);
                }
            }
            if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("KROM_ASSET_PREFAB"))
            {
                // Prefab als Kind dieses Objekts anlegen.
                if (const char* prefabPath = static_cast<const char*>(payload->Data); prefabPath && prefabPath[0] != '\0')
                    SpawnPrefabAsset(ctx, PrefabPayloadPath(ctx, prefabPath), nullptr, id);
            }
            if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("KROM_ASSET_MATERIAL"))
            {
                const char* materialPath = static_cast<const char*>(payload->Data);
                if (materialPath && materialPath[0] != '\0')
                {
                    ExecuteSceneMutation(ctx, "history.assign_material", [&]() {
                        SelectMaterialAsset(ctx, PathFromUtf8(materialPath));
                        ApplyMaterialAssetToEntity(ctx, id, PathFromUtf8(materialPath));
                    });
                }
            }
            ImGui::EndDragDropTarget();
        }

        if (!visibleSelf)
            ImGui::PopStyleColor();

        if (visibleDescendant && open)
        {
            for (EntityID candidate : entities)
            {
                if (isDirectChildOf(candidate, id))
                    self(self, candidate, depth + 1u);
            }
            ImGui::TreePop();
        }
    };

    for (EntityID id : entities)
    {
        const auto* pc = ctx.world.Get<ParentComponent>(id);
        if (pc && pc->parent.IsValid() && ctx.world.IsAlive(pc->parent))
            continue;
        drawEntity(drawEntity, id, 0u);
    }

    // Always leave some empty space below the last row to drop onto.
    const ImVec2 listEnd = ImGui::GetCursorScreenPos();
    ImGui::Dummy(ImVec2(1.f, std::max(32.f, ImGui::GetContentRegionAvail().y)));

    // Fallback target covering the whole panel: anything not dropped onto a row
    // (empty space, header) goes to the scene root, appended at the end.
    // Rows are smaller targets and take precedence.
    ImGuiWindow* hierarchyWindow = ImGui::GetCurrentWindow();
    if (!mouseOverRow &&
        ImGui::BeginDragDropTargetCustom(hierarchyWindow->InnerRect, ImGui::GetID("##SceneRootDropZone")))
    {
        // No frame around the whole panel; a line at the end of the list shows
        // where the entity will land.
        if (const ImGuiPayload* active = ImGui::GetDragDropPayload();
            active && active->IsDataType("KROM_EDITOR_ENTITY"))
        {
            ImGui::GetWindowDrawList()->AddLine(
                ImVec2(hierarchyWindow->InnerRect.Min.x, listEnd.y),
                ImVec2(hierarchyWindow->InnerRect.Max.x, listEnd.y),
                ImGui::GetColorU32(theme::kAccentHover), 2.f);
        }
        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(
                "KROM_EDITOR_ENTITY", ImGuiDragDropFlags_AcceptNoDrawDefaultRect))
        {
            if (payload->DataSize == sizeof(EntityID))
            {
                const EntityID dragged = *static_cast<const EntityID*>(payload->Data);
                MoveEntityInHierarchy(ctx, dragged, NULL_ENTITY, NULL_ENTITY, true);
            }
        }
        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(
                "KROM_ASSET_PREFAB", ImGuiDragDropFlags_AcceptNoDrawDefaultRect))
        {
            // Prefab auf oberster Ebene anlegen.
            if (const char* prefabPath = static_cast<const char*>(payload->Data); prefabPath && prefabPath[0] != '\0')
                SpawnPrefabAsset(ctx, PrefabPayloadPath(ctx, prefabPath));
        }
        ImGui::EndDragDropTarget();
    }

    if (ImGui::BeginPopupContextWindow("SceneCreateContext##editor",
                                       ImGuiPopupFlags_MouseButtonRight))
    {
        if (ctx.state.selectedEntity.IsValid() &&
            ctx.world.IsAlive(ctx.state.selectedEntity))
        {
            if (ImGui::MenuItem(TranslateLabel("common.duplicate", "Duplizieren").c_str()))
            {
                const EntityID dup = DuplicateEntity(ctx, ctx.state.selectedEntity);
                if (dup.IsValid())
                    ctx.state.selectedEntity = dup;
            }
            if (ImGui::MenuItem(TranslateLabel("common.move_up", "Nach oben").c_str()))
            {
                ExecuteSceneMutation(ctx, "history.reorder_hierarchy", [&]() {
                    MoveEntityInSiblingOrder(ctx, ctx.state.selectedEntity, -1);
                });
            }
            if (ImGui::MenuItem(TranslateLabel("common.move_down", "Nach unten").c_str()))
            {
                ExecuteSceneMutation(ctx, "history.reorder_hierarchy", [&]() {
                    MoveEntityInSiblingOrder(ctx, ctx.state.selectedEntity, 1);
                });
            }
            ImGui::Separator();
            if (ImGui::MenuItem(TranslateLabel("common.delete", "Loeschen").c_str()))
                ctx.state.deleteConfirmOpen = true;
            if (FindOutermostPrefabInstanceRoot(ctx, ctx.state.selectedEntity).IsValid())
            {
                if (ImGui::MenuItem(TranslateLabel("prefab.reset_instance", "Instanz auf Prefab zuruecksetzen").c_str()))
                    engine::addons::editor::ResetPrefabInstanceOverrides(ctx, ctx.state.selectedEntity);
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("%s", Translate("prefab.reset_instance_tip"));
                if (ImGui::MenuItem(TranslateLabel("prefab.unpack", "Prefab entkoppeln").c_str()))
                    UnpackPrefabInstance(ctx, ctx.state.selectedEntity);
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("%s", Translate("prefab.unpack_tip"));
            }
            ImGui::Separator();
        }

        if (ImGui::BeginMenu(TranslateLabel("common.create", "Erstellen").c_str()))
        {
            if (ImGui::MenuItem(TranslateLabel("hierarchy.empty_entity", "Leere Entity").c_str()))
                CreateEmptyEntity(ctx);
            if (ImGui::MenuItem(TranslateLabel("hierarchy.folder", "Ordner").c_str()))
                CreateFolderEntity(ctx, Translate("hierarchy.folder_name"));

            ImGui::Separator();
            if (ImGui::MenuItem(TranslateLabel("component.camera", "Camera").c_str()))
                CreateCameraEntity(ctx);

            ImGui::Separator();
            if (ImGui::MenuItem(TranslateLabel("component.directional_light", "Directional Light").c_str()))
                CreateLightEntity(ctx, LightType::Directional);
            if (ImGui::MenuItem(TranslateLabel("component.point_light", "Point Light").c_str()))
                CreateLightEntity(ctx, LightType::Point);
            if (ImGui::MenuItem(TranslateLabel("component.spot_light", "Spot Light").c_str()))
                CreateLightEntity(ctx, LightType::Spot);
            ImGui::EndMenu();
        }
        ImGui::EndPopup();
    }

    // Auto-scroll while dragging an entity near the top or bottom edge.
    if (const ImGuiPayload* active = ImGui::GetDragDropPayload();
        active && active->IsDataType("KROM_EDITOR_ENTITY"))
    {
        const ImRect inner = hierarchyWindow->InnerRect;
        const ImGuiIO& io = ImGui::GetIO();
        const float edge = std::max(24.f, ImGui::GetFrameHeight() * 1.5f);
        if (io.MousePos.x >= inner.Min.x && io.MousePos.x <= inner.Max.x &&
            io.MousePos.y >= inner.Min.y && io.MousePos.y <= inner.Max.y)
        {
            float strength = 0.f;
            if (io.MousePos.y < inner.Min.y + edge)
                strength = -(inner.Min.y + edge - io.MousePos.y) / edge;
            else if (io.MousePos.y > inner.Max.y - edge)
                strength = (io.MousePos.y - (inner.Max.y - edge)) / edge;
            if (strength != 0.f)
            {
                constexpr float kMaxScrollSpeed = 900.f; // pixels per second at the very edge
                const float step = std::clamp(strength, -1.f, 1.f) * kMaxScrollSpeed * io.DeltaTime;
                ImGui::SetScrollY(std::clamp(ImGui::GetScrollY() + step, 0.f, ImGui::GetScrollMaxY()));
            }
        }
    }

    ctx.hierarchyPanelWidth = ImGui::GetWindowWidth();
    ImGui::End();
}

} // namespace detail

} // namespace engine::renderer::addons::editor

#endif // KROM_EDITOR_HAS_IMGUI
