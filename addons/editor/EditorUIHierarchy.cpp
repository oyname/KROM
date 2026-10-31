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
    const EditorLayoutRects rects = ComputeEditorLayout(ctx, ImGui::GetIO().DisplaySize);
    SetFixedEditorWindow(rects.hierarchyPos, rects.hierarchySize);
    SolidEditorPanelStyleScope solidPanelStyle;
    // Im Prefab-Modus leicht blau, damit klar ist, dass man das Prefab bearbeitet (wie Unity).
    const bool prefabMode = IsInPrefabMode(ctx.state);
    if (prefabMode)
        ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.10f, 0.14f, 0.21f, 1.f));
    ImGui::Begin("Hierarchie##editor", &ctx.hierarchyPanelVisible, kAlignedPanelFlags);
    if (prefabMode)
        ImGui::PopStyleColor();

    // Prefab-Modus: Pfad "< Szene  > Name" mit Zurueck, Speichern, Partikel neu starten.
    if (prefabMode)
    {
        const bool readOnly = ctx.state.prefabModeReadOnly;
        if (ImGui::SmallButton("< Szene"))
            ExitPrefabMode(ctx);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip(readOnly ? "Zurueck zur Szene (das Modell wird nicht veraendert)."
                                       : "Speichert das Prefab und kehrt zur Szene zurueck.");
        ImGui::SameLine();
        const std::string prefabName = PathToUtf8(PathFromUtf8(ctx.state.prefabModePath).filename());
        ImGui::TextColored(ImVec4(0.45f, 0.75f, 1.f, 1.f), readOnly ? "> %s (nur Ansicht)" : "> %s",
                           prefabName.c_str());
        if (readOnly)
        {
            if (ImGui::SmallButton("Als Prefab speichern##model_view"))
            {
                const EntityID modelRoot = PrefabModeRootEntity(ctx);
                if (modelRoot.IsValid())
                    SaveEntityAsPrefab(ctx, modelRoot);
            }
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Legt unter Assets/Prefabs ein bearbeitbares Prefab aus dem Modell an.");
        }
        else if (ImGui::SmallButton("Speichern##prefab_mode"))
        {
            SavePrefabMode(ctx);
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("Partikel neu starten##prefab_mode") && ctx.restartParticleEmitter)
        {
            ctx.world.View<ParticleEmitterComponent>([&](EntityID id, ParticleEmitterComponent&) {
                ctx.restartParticleEmitter(id);
            });
        }
        ImGui::Separator();
    }

    static bool showMeshes = true;
    static bool showCameras = true;
    static bool showLights = true;
    static bool showOther = true;

    ImGui::Checkbox("Mesh", &showMeshes);
    ImGui::SameLine();
    ImGui::Checkbox("Camera", &showCameras);
    ImGui::SameLine();
    ImGui::Checkbox("Light", &showLights);
    ImGui::Checkbox("Other", &showOther);
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

    auto makeLabel = [&](EntityID id, char* label, size_t labelSize) {
        const bool isCamera = ctx.world.Get<CameraComponent>(id) != nullptr;
        const bool isLight = ctx.world.Get<LightComponent>(id) != nullptr;
        const bool isMesh = ctx.world.Get<MeshComponent>(id) != nullptr;
        const char* typePrefix = "[Entity]";
        if (isCamera)
            typePrefix = "[Camera]";
        else if (isLight)
            typePrefix = "[Light]";
        else if (isMesh)
            typePrefix = "[Mesh]";

        const char* name = nullptr;
        if (const auto* nc = ctx.world.Get<NameComponent>(id))
            name = nc->name.c_str();

        if (name && name[0] != '\0')
            std::snprintf(label, labelSize, "%s %s", typePrefix, name);
        else
            std::snprintf(label, labelSize, "%s Entity %u", typePrefix, id.Index());
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
        makeLabel(id, label, sizeof(label));

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

        const bool open = ImGui::TreeNodeEx(
            reinterpret_cast<void*>(static_cast<uintptr_t>(id.value)),
            flags,
            "%s",
            label);

        // Rechtsklick-Kontextmenü pro Entity
        if (visibleSelf && ImGui::BeginPopupContextItem())
        {
            ClearMultiSelection(ctx);
            ctx.state.selectedEntity  = id;
            ctx.state.selectionSource = SelectionSource::Outliner;

            if (ImGui::MenuItem("Duplizieren"))
            {
                const EntityID dup = DuplicateEntity(ctx, id);
                if (dup.IsValid())
                    ctx.state.selectedEntity = dup;
            }
            if (ImGui::MenuItem("Nach oben"))
            {
                ExecuteSceneMutation(ctx, "Hierarchie-Reihenfolge geaendert", [&]() {
                    MoveEntityInSiblingOrder(ctx, id, -1);
                });
            }
            if (ImGui::MenuItem("Nach unten"))
            {
                ExecuteSceneMutation(ctx, "Hierarchie-Reihenfolge geaendert", [&]() {
                    MoveEntityInSiblingOrder(ctx, id, 1);
                });
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Loeschen"))
                ctx.state.deleteConfirmOpen = true;
            ImGui::Separator();
            if (ImGui::MenuItem("Als Prefab speichern"))
                SaveEntityAsPrefab(ctx, id);
            ImGui::Separator();
            if (ImGui::BeginMenu("Erstellen"))
            {
                if (ImGui::MenuItem("Leere Entity"))
                    CreateEmptyEntity(ctx);
                ImGui::Separator();
                if (ImGui::MenuItem("Camera"))
                    CreateCameraEntity(ctx);
                ImGui::Separator();
                if (ImGui::MenuItem("Directional Light"))
                    CreateLightEntity(ctx, LightType::Directional);
                if (ImGui::MenuItem("Point Light"))
                    CreateLightEntity(ctx, LightType::Point);
                if (ImGui::MenuItem("Spot Light"))
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
            ImGui::TextUnformatted(label);
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
                    ExecuteSceneMutation(ctx, "Material zugewiesen", [&]() {
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
            if (ImGui::MenuItem("Duplizieren"))
            {
                const EntityID dup = DuplicateEntity(ctx, ctx.state.selectedEntity);
                if (dup.IsValid())
                    ctx.state.selectedEntity = dup;
            }
            if (ImGui::MenuItem("Nach oben"))
            {
                ExecuteSceneMutation(ctx, "Hierarchie-Reihenfolge geaendert", [&]() {
                    MoveEntityInSiblingOrder(ctx, ctx.state.selectedEntity, -1);
                });
            }
            if (ImGui::MenuItem("Nach unten"))
            {
                ExecuteSceneMutation(ctx, "Hierarchie-Reihenfolge geaendert", [&]() {
                    MoveEntityInSiblingOrder(ctx, ctx.state.selectedEntity, 1);
                });
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Loeschen"))
                ctx.state.deleteConfirmOpen = true;
            ImGui::Separator();
            if (ImGui::MenuItem("Als Prefab speichern"))
                SaveEntityAsPrefab(ctx, ctx.state.selectedEntity);
            ImGui::Separator();
        }

        if (ImGui::BeginMenu("Erstellen"))
        {
            if (ImGui::MenuItem("Leere Entity"))
                CreateEmptyEntity(ctx);

            ImGui::Separator();
            if (ImGui::MenuItem("Camera"))
                CreateCameraEntity(ctx);

            ImGui::Separator();
            if (ImGui::MenuItem("Directional Light"))
                CreateLightEntity(ctx, LightType::Directional);
            if (ImGui::MenuItem("Point Light"))
                CreateLightEntity(ctx, LightType::Point);
            if (ImGui::MenuItem("Spot Light"))
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
