// =============================================================================
// KROM Engine - addons/editor/EditorUIHierarchy.cpp
// Hierarchie-Panel (Entity-Liste, Drag & Drop, Kontextmenue).
// =============================================================================
#include "addons/editor/EditorUIInternal.hpp"
#include "core/PathUtf8.hpp"
#include "addons/particles/ParticleComponents.hpp"

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

} // namespace

namespace detail {

void DrawEntityList(EditorFrameContext& ctx)
{
    const EntityID reveal = ctx.state.hierarchyRevealEntity;
    std::unordered_set<uint32_t> revealAncestors;
    if (ctx.world.IsAlive(reveal))
    {
        ClearMultiSelection(ctx);
        ctx.state.selectedEntity = reveal;
        ctx.state.selectionSource = SelectionSource::Outliner;
        for (EntityID ancestor = reveal; ctx.world.IsAlive(ancestor); )
        {
            if (!revealAncestors.insert(ancestor.value).second)
                break;
            const auto* parent = ctx.world.Get<ParentComponent>(ancestor);
            ancestor = parent ? parent->parent : NULL_ENTITY;
        }
        ImGui::SetNextWindowFocus();
    }
    else
        ctx.state.hierarchyRevealEntity = NULL_ENTITY;
    SolidEditorPanelStyleScope solidPanelStyle;
    // Im Prefab-Modus leicht blau, damit klar ist, dass man das Prefab bearbeitet (wie Unity).
    const bool prefabMode = IsInPrefabMode(ctx.state);
    if (prefabMode)
        ImGui::PushStyleColor(ImGuiCol_WindowBg, theme::Hex(0x13202C));
    ImGui::Begin(TranslateLabel("menu.hierarchy", "Hierarchie##editor").c_str(), &ctx.hierarchyPanelVisible, kAlignedPanelFlags);
    if (prefabMode)
        ImGui::PopStyleColor();

    // Prefab-Modus: Pfad "< Szene  > Name" mit Zurueck, Speichern, Partikel neu starten.
    if (prefabMode)
    {
        const bool readOnly = ctx.state.prefabModeReadOnly;
        if (EditorActionButton(TranslateLabel("hierarchy.back", "< Zur Szene").c_str(), ImVec2(-1.f, 32.f)))
            ExitPrefabMode(ctx);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip(readOnly ? Translate("hierarchy.back_read_only")
                                       : Translate("hierarchy.back_save"));
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
        bool hasParticles = false;
        ctx.world.View<ParticleEmitterComponent>([&](EntityID, ParticleEmitterComponent&) { hasParticles = true; });
        ImGui::BeginDisabled(!hasParticles || !ctx.restartParticleEmitter);
        if (EditorActionButton(TranslateLabel("particles.restart", "Partikel neu starten##prefab_mode").c_str(), ImVec2(-1.f, 32.f)))
        {
            ctx.world.View<ParticleEmitterComponent>([&](EntityID id, ParticleEmitterComponent&) {
                ctx.restartParticleEmitter(id);
            });
        }
        ImGui::EndDisabled();
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();
    }

    static bool showMeshes = true;
    static bool showCameras = true;
    static bool showLights = true;
    static bool showOther = true;
    if (!revealAncestors.empty())
        showMeshes = showCameras = showLights = showOther = true;

    const int visibleTypes = static_cast<int>(showMeshes) + static_cast<int>(showCameras) +
                             static_cast<int>(showLights) + static_cast<int>(showOther);
    const bool filtered = visibleTypes != 4;
    const std::string summary = visibleTypes == 4 ? "Alle Typen" :
        (visibleTypes == 0 ? "Keine Typen" : std::to_string(visibleTypes) + " von 4 Typen");
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
        ImGui::Checkbox(TranslateLabel("hierarchy.other", "Sonstige Objekte").c_str(), &showOther);
        ImGui::Separator();
        if (ImGui::Button(TranslateLabel("hierarchy.show_all", "Alle anzeigen").c_str()))
            showMeshes = showCameras = showLights = showOther = true;
        ImGui::SameLine();
        if (ImGui::Button(TranslateLabel("common.none", "Keine").c_str()))
            showMeshes = showCameras = showLights = showOther = false;
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
        // Zweite Absicherung: Meshes auf dem Editor-Gizmo-Layer nie anzeigen
        if (const auto* mc = ctx.world.Get<MeshComponent>(id))
            if (mc->layerMask == renderer::LAYER_EDITOR_GIZMO ||
                mc->layerMask == renderer::LAYER_EDITOR_GIZMO_DEPTH ||
                mc->layerMask == renderer::LAYER_EDITOR_MATERIAL_PREVIEW ||
                mc->layerMask == renderer::LAYER_EDITOR_ASSET_THUMBNAIL)
                return false;
        const bool isCamera = ctx.world.Get<CameraComponent>(id) != nullptr;
        const bool isLight = ctx.world.Get<LightComponent>(id) != nullptr;
        const bool isMesh = ctx.world.Get<MeshComponent>(id) != nullptr;
        const bool isOther = !isCamera && !isLight && !isMesh;
        return !((isMesh && !showMeshes) || (isCamera && !showCameras) ||
                 (isLight && !showLights) || (isOther && !showOther));
    };

    if (ctx.state.assetBrowser)
        LoadEditorIcons(ctx, *ctx.state.assetBrowser);

    // Platz fuer das Symbol vor dem Namen: so viele Leerzeichen wie das Symbol breit ist
    const float iconSize = ImGui::GetFontSize();
    const float spaceWidth = std::max(1.f, ImGui::CalcTextSize(" ").x);
    const int iconSpaces = static_cast<int>(std::ceil((iconSize + 4.f) / spaceWidth));

    // Liefert das Typ-Symbol oder nullptr, dann bleibt das Text-Praefix im Label.
    auto makeLabel = [&](EntityID id, char* label, size_t labelSize) -> const AssetBrowserState::Icon* {
        // Rangfolge: Prefab-Instanz, Camera, Light (nach Typ), Partikel, Audio, Mesh, sonst Entity
        const AssetBrowserState::Icons* icons = ctx.state.assetBrowser ? &ctx.state.assetBrowser->icons : nullptr;
        const char* typePrefix = "[Entity]";
        const AssetBrowserState::Icon* icon = icons ? &icons->entity : nullptr;
        if (ctx.world.Get<PrefabInstanceComponent>(id) != nullptr)
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
            if (name && name[0] != '\0')
                std::snprintf(label, labelSize, "%*s%s", iconSpaces, "", name);
            else
                std::snprintf(label, labelSize, "%*sEntity %u", iconSpaces, "", id.Index());
        }
        else if (name && name[0] != '\0')
            std::snprintf(label, labelSize, "%s %s", typePrefix, name);
        else
            std::snprintf(label, labelSize, "%s Entity %u", typePrefix, id.Index());
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

    auto drawEntity = [&](auto&& self, EntityID id, uint32_t depth) -> void {
        if (depth > 32u)
            return;

        const bool visibleSelf = passesFilter(id);
        const bool visibleDescendant = hasVisibleDescendant(hasVisibleDescendant, id, depth);
        if (!visibleSelf && !visibleDescendant)
            return;

        char label[112];
        const AssetBrowserState::Icon* icon = makeLabel(id, label, sizeof(label));

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

        if (revealAncestors.count(id.value) > 0)
            ImGui::SetNextItemOpen(true);
        const bool open = ImGui::TreeNodeEx(
            reinterpret_cast<void*>(static_cast<uintptr_t>(id.value)),
            flags,
            "%s",
            label);
        if (id == reveal)
        {
            ImGui::SetScrollHereY(0.5f);
            ctx.state.hierarchyRevealEntity = NULL_ENTITY;
        }

        // Typ-Symbol an die Stelle der fuehrenden Leerzeichen zeichnen
        if (icon)
        {
            const ImVec2 itemMin = ImGui::GetItemRectMin();
            const ImVec2 itemMax = ImGui::GetItemRectMax();
            const float textX = itemMin.x + ImGui::GetFontSize() + ImGui::GetStyle().FramePadding.x * 2.f;
            ImVec2 size(iconSize, iconSize);
            if (icon->aspect > 1.f)
                size.y = iconSize / icon->aspect;
            else
                size.x = iconSize * icon->aspect;
            const ImVec2 iconMin(textX + (iconSize - size.x) * 0.5f,
                                 itemMin.y + (itemMax.y - itemMin.y - size.y) * 0.5f);
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
            ImGui::Separator();
            if (ImGui::MenuItem(TranslateLabel("prefab.save_as", "Als Prefab speichern").c_str()))
                SaveEntityAsPrefab(ctx, id);
            ImGui::Separator();
            if (ImGui::BeginMenu(TranslateLabel("common.create", "Erstellen").c_str()))
            {
                if (ImGui::MenuItem(TranslateLabel("hierarchy.empty_entity", "Leere Entity").c_str()))
                    CreateEmptyEntity(ctx);
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
            ClearMultiSelection(ctx);
            ctx.state.selectedEntity  = id;
            ctx.state.selectionSource = SelectionSource::Outliner;
            const math::Vec3 target   = WorldPosition(ctx, id);
            EditorCameraState& cam    = ctx.state.editorCamera;
            const math::Vec3 toTarget = target - cam.position;
            const float      dist     = toTarget.Length();
            if (dist > 0.001f)
            {
                const math::Vec3 dir = toTarget.Normalized();
                cam.yawDeg   = std::atan2(-dir.x, -dir.z) * math::RAD_TO_DEG;
                cam.pitchDeg = std::clamp(
                    std::asin(std::clamp(dir.y, -1.f, 1.f)) * math::RAD_TO_DEG,
                    -89.f, 89.f);
            }
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

        if (visibleSelf && ImGui::BeginDragDropTarget())
        {
            if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("KROM_EDITOR_ENTITY"))
            {
                if (payload->DataSize == sizeof(EntityID))
                {
                    EntityID dragged = *static_cast<const EntityID*>(payload->Data);
                    const auto* dp = ctx.world.Get<ParentComponent>(dragged);
                    const auto* tp = ctx.world.Get<ParentComponent>(id);
                    Debug::Log("[Drop] dragged=%u(parent=%u) -> target=%u(parent=%u)",
                               dragged.value, dp ? dp->parent.value : 0u,
                               id.value,      tp ? tp->parent.value : 0u);
                    const bool ok = ReparentEntity(ctx, dragged, id);
                    if (!ok)
                    {
                        const auto* dpAfter = ctx.world.Get<ParentComponent>(dragged);
                        Debug::Log("[Drop] FAILED dragged=%u (currentParent=%u, target=%u) – bereits Kind oder Zyklus",
                                   dragged.value,
                                   dpAfter ? dpAfter->parent.value : 0u,
                                   id.value);
                    }
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

    const ImVec2 rootDropSize{
        std::max(1.f, ImGui::GetContentRegionAvail().x),
        std::max(32.f, ImGui::GetContentRegionAvail().y)
    };
    ImGui::InvisibleButton("##SceneRootDropZone", rootDropSize);
    if (ImGui::BeginDragDropTarget())
    {
        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("KROM_EDITOR_ENTITY"))
        {
            if (payload->DataSize == sizeof(EntityID))
            {
                EntityID dragged = *static_cast<const EntityID*>(payload->Data);
                ReparentEntity(ctx, dragged, NULL_ENTITY);
            }
        }
        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("KROM_ASSET_PREFAB"))
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
            ImGui::Separator();
            if (ImGui::MenuItem(TranslateLabel("prefab.save_as", "Als Prefab speichern").c_str()))
                SaveEntityAsPrefab(ctx, ctx.state.selectedEntity);
            ImGui::Separator();
        }

        if (ImGui::BeginMenu(TranslateLabel("common.create", "Erstellen").c_str()))
        {
            if (ImGui::MenuItem(TranslateLabel("hierarchy.empty_entity", "Leere Entity").c_str()))
                CreateEmptyEntity(ctx);

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

    ctx.hierarchyPanelWidth = ImGui::GetWindowWidth();
    ImGui::End();
}

} // namespace detail

} // namespace engine::renderer::addons::editor

#endif // KROM_EDITOR_HAS_IMGUI
