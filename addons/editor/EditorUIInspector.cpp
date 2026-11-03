// =============================================================================
// KROM Engine - addons/editor/EditorUIInspector.cpp
// Inspector: Komponenten-Sektionen (Transform, Kamera, Licht, Mesh,
// Material, Script, Physik/Kollision) und Environment.
// =============================================================================
#include "addons/editor/EditorUIInternal.hpp"
#include "core/PathUtf8.hpp"
#include "addons/editor/EditorValueClipboard.hpp"
#include "addons/particles/ParticleComponents.hpp"

#ifdef KROM_EDITOR_HAS_IMGUI

#include "addons/editor/EditorComponentClipboard.hpp"

#include <cstring>
#include <functional>
#include <array>
#include "imgui_internal.h"

namespace engine::renderer::addons::editor {

using namespace detail;

namespace {

// Reine Editor-Anordnung, gemeinsam fuer alle Entities waehrend dieser Sitzung.
static int currentInspectorSection = -1;
static int movedInspectorSection = -1;
static int targetInspectorSection = -1;
static bool insertInspectorSectionAfter = false;

enum class ComponentIcon { Transform, Mesh, Material, Camera, Light, Physics, Collider,
                           Character, Audio, Particles, Script, Environment };

static ImU32 ComponentColor(ComponentIcon icon)
{
    switch (icon)
    {
    case ComponentIcon::Transform: return IM_COL32(105, 170, 245, 255);
    case ComponentIcon::Mesh: return IM_COL32(75, 205, 220, 255);
    case ComponentIcon::Material: return IM_COL32(235, 170, 85, 255);
    case ComponentIcon::Light: return IM_COL32(245, 215, 100, 255);
    case ComponentIcon::Physics: case ComponentIcon::Collider: return IM_COL32(110, 200, 145, 255);
    case ComponentIcon::Audio: return IM_COL32(185, 150, 235, 255);
    case ComponentIcon::Particles: return IM_COL32(235, 160, 105, 255);
    case ComponentIcon::Script: return IM_COL32(225, 200, 125, 255);
    default: return IM_COL32(125, 185, 225, 255);
    }
}

static void DrawComponentIcon(ImDrawList& dl, ComponentIcon icon, const ImVec2& c, ImU32 color)
{
    const char* glyph = ICON_LC_BOX;
    switch (icon)
    {
    case ComponentIcon::Transform:   glyph = ICON_LC_MOVE_3D; break;
    case ComponentIcon::Mesh:        glyph = ICON_LC_BOX; break;
    case ComponentIcon::Material:    glyph = ICON_LC_CONTRAST; break;
    case ComponentIcon::Camera:      glyph = ICON_LC_VIDEO; break;
    case ComponentIcon::Light:       glyph = ICON_LC_LIGHTBULB; break;
    case ComponentIcon::Physics:     glyph = ICON_LC_WEIGHT; break;
    case ComponentIcon::Collider:    glyph = ICON_LC_SQUARE_DASHED; break;
    case ComponentIcon::Character:   glyph = ICON_LC_PERSON_STANDING; break;
    case ComponentIcon::Audio:       glyph = ICON_LC_VOLUME_2; break;
    case ComponentIcon::Particles:   glyph = ICON_LC_SPARKLES; break;
    case ComponentIcon::Script:      glyph = ICON_LC_FILE_CODE; break;
    case ComponentIcon::Environment: glyph = ICON_LC_GLOBE; break;
    }
    DrawIcon(&dl, glyph, c, 17.f, color);
}

// Ein gemeinsamer Header erhaelt die bestehenden IDs und Kontextmenues.
// Die Beschriftung wird mit Platz fuer ein Icon neu gezeichnet; kein Asset-
// Laden und keine GPU-Ressourcenerzeugung innerhalb des Inspector-Passes.
static bool ComponentHeader(const char* label, ComponentIcon icon, ImGuiTreeNodeFlags flags = 0)
{
    ImGui::Dummy(ImVec2(0, 9.f));
    ImGui::PushStyleColor(ImGuiCol_Separator, theme::kBorder);
    ImGui::Separator();
    ImGui::PopStyleColor();
    ImGui::Dummy(ImVec2(0, 3.f));
    const ImU32 textColor = ImGui::GetColorU32(ImGuiCol_Text);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(10, 7));
    ImGui::PushStyleColor(ImGuiCol_Header, theme::kPanel);
    ImGui::PushStyleColor(ImGuiCol_HeaderHovered, theme::kMenuHover);
    ImGui::PushStyleColor(ImGuiCol_HeaderActive, theme::kSelected);
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0, 0, 0, 0));
    const bool open = ImGui::CollapsingHeader(label, flags | ImGuiTreeNodeFlags_FramePadding |
        (currentInspectorSection >= 0 ? ImGuiTreeNodeFlags_AllowOverlap : 0));
    // Capture before drag/drop widgets can alter the last-item hover metadata.
    const ImGuiLastItemData headerItem = ImGui::GetCurrentContext()->LastItemData;
    ImGui::PopStyleColor(4);
    ImGui::PopStyleVar();
    if (!ImGui::IsItemVisible())
        return open;
    const ImVec2 min = ImGui::GetItemRectMin();
    const ImVec2 max = ImGui::GetItemRectMax();
    const float mid = (min.y + max.y) * 0.5f;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->PushClipRect(min, max, true);
    dl->AddRectFilled(min, ImVec2(min.x + 3.f, max.y), ComponentColor(icon));
    if (open)
        dl->AddTriangleFilled(ImVec2(min.x + 10, mid - 3), ImVec2(min.x + 18, mid - 3),
                              ImVec2(min.x + 14, mid + 3), textColor);
    else
        dl->AddTriangleFilled(ImVec2(min.x + 11, mid - 4), ImVec2(min.x + 11, mid + 4),
                              ImVec2(min.x + 17, mid), textColor);
    DrawComponentIcon(*dl, icon, ImVec2(min.x + 33.f, mid), ComponentColor(icon));
    std::string title(label);
    if (const auto suffix = title.find("##"); suffix != std::string::npos) title.resize(suffix);
    dl->PushClipRect(min, ImVec2(max.x - (currentInspectorSection >= 0 ? 28.f : 0.f), max.y), true);
    dl->AddText(ImVec2(min.x + 49.f, mid - ImGui::GetFontSize() * 0.5f), textColor, title.c_str());
    dl->PopClipRect();
    dl->PopClipRect();
    if (currentInspectorSection >= 0)
    {
        // Drop auf die obere/untere Haelfte fuegt vor/nach der Sektion ein.
        if (ImGui::BeginDragDropTarget())
        {
            if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("KROM_INSPECTOR_SECTION"))
            {
                if (payload->DataSize == sizeof(int))
                {
                    movedInspectorSection = *static_cast<const int*>(payload->Data);
                    targetInspectorSection = currentInspectorSection;
                    insertInspectorSectionAfter = ImGui::GetIO().MousePos.y >= mid;
                }
            }
            const float lineY = ImGui::GetIO().MousePos.y < mid ? min.y : max.y;
            dl->AddLine(ImVec2(min.x, lineY), ImVec2(max.x, lineY), IM_COL32(90, 175, 255, 255), 2.f);
            ImGui::EndDragDropTarget();
        }
        // LastItem erhalten: die folgenden Komponenten-Kontextmenues gehoeren zum Header.
        const ImVec2 cursor = ImGui::GetCursorScreenPos();
        ImGui::SetCursorScreenPos(ImVec2(max.x - 26.f, min.y));
        ImGui::PushID(label);
        ImGui::InvisibleButton("##reorder", ImVec2(26.f, max.y - min.y));
        const bool hovered = ImGui::IsItemHovered();
        for (int row = 0; row < 3; ++row)
            for (int col = 0; col < 2; ++col)
                dl->AddCircleFilled(ImVec2(max.x - 18.f + col * 6.f, mid - 6.f + row * 6.f),
                                    1.5f, hovered ? IM_COL32(100, 190, 255, 255) : textColor);
        if (hovered)
        {
            ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeAll);
            ImGui::SetTooltip("Ziehen, um die Inspector-Reihenfolge zu aendern");
        }
        if (ImGui::BeginDragDropSource())
        {
            ImGui::SetDragDropPayload("KROM_INSPECTOR_SECTION", &currentInspectorSection, sizeof(int));
            value_ui::TextUnformatted(title.c_str());
            ImGui::EndDragDropSource();
        }
        ImGui::PopID();
        ImGui::SetCursorScreenPos(cursor);
        ImGui::GetCurrentContext()->LastItemData = headerItem;
    }
    ImGui::GetCurrentContext()->LastItemData = headerItem;
    return open;
}

// Reorder handles overlap the header. Explicitly allow this overlap when
// requesting its clipboard popup; keep the standard keyboard context action.
static bool BeginComponentContextMenu(const char* popupId)
{
    if (ImGui::IsMouseReleased(ImGuiMouseButton_Right) &&
        ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenBlockedByPopup |
                             ImGuiHoveredFlags_AllowWhenOverlappedByItem))
        ImGui::OpenPopup(popupId);
    return ImGui::BeginPopupContextItem(popupId);
}

// Undo fuer die Felder einer Komponente: merkt sich den Wert vor dem Zeichnen
// der Sektion. Aendert ein Widget ihn, wird der Schnappschuss mit dem alten Wert
// aufgenommen (BeginPendingSceneEdit); sobald kein Widget mehr aktiv ist, wird
// der Schritt abgeschlossen. Ein ganzer Zieh-Vorgang ergibt so einen Undo-Schritt.
template<typename T, typename Equal = std::equal_to<T>>
class ComponentUndoScope
{
public:
    ComponentUndoScope(EditorFrameContext& ctx, EntityID entity, std::string_view field,
                       const char* label, Equal equal = {})
        : m_ctx(ctx), m_entity(entity), m_key(MakeEditHistoryKey(entity, field)),
          m_label(label), m_equal(equal)
    {
        if (const T* component = ctx.world.Get<T>(entity))
        {
            m_before = *component;
            m_valid  = true;
        }
    }

    ~ComponentUndoScope()
    {
        if (!m_valid || !m_ctx.world.IsAlive(m_entity))
            return;
        T* component = m_ctx.world.Get<T>(m_entity);
        if (!component)
            return;   // in dieser Sektion entfernt (eigener Undo-Schritt)
        if (!m_equal(*component, m_before))
        {
            const T after = *component;
            *component = m_before;                       // Schnappschuss mit altem Wert
            BeginPendingSceneEdit(m_ctx, m_key, m_label);
            if (T* again = m_ctx.world.Get<T>(m_entity))
                *again = after;
        }
        if (!ImGui::IsAnyItemActive())
            CommitPendingSceneEdit(m_ctx, m_key);
    }

    ComponentUndoScope(const ComponentUndoScope&)            = delete;
    ComponentUndoScope& operator=(const ComponentUndoScope&) = delete;

private:
    EditorFrameContext& m_ctx;
    EntityID            m_entity;
    std::string         m_key;
    const char*         m_label;
    Equal               m_equal;
    T                   m_before{};
    bool                m_valid = false;
};

// OBBComponent (Core) hat keinen operator== (Quat ohne Vergleich).
struct ObbEqual
{
    bool operator()(const OBBComponent& a, const OBBComponent& b) const noexcept
    {
        return a.centerOffset == b.centerOffset && a.halfExtents == b.halfExtents &&
               a.orientation.x == b.orientation.x && a.orientation.y == b.orientation.y &&
               a.orientation.z == b.orientation.z && a.orientation.w == b.orientation.w &&
               a.showInEditor == b.showInEditor && a.isTrigger == b.isTrigger;
    }
};

std::string RuntimeScriptFieldKey(const std::string& entityGuid,
                                  uint32_t scriptIndex,
                                  const std::string& fieldName)
{
    return entityGuid + "|" + std::to_string(scriptIndex) + "|" + fieldName;
}

std::string RuntimeScriptFieldValueText(const engine::script::ScriptFieldValue& value)
{
    std::ostringstream out;
    switch (value.type)
    {
    case engine::script::ScriptFieldType::Float:
        out << value.floatValue;
        break;
    case engine::script::ScriptFieldType::Int:
        out << value.intValue;
        break;
    case engine::script::ScriptFieldType::Bool:
        out << (value.boolValue ? "true" : "false");
        break;
    case engine::script::ScriptFieldType::Vec3:
        out << value.vec3Value.x << ", " << value.vec3Value.y << ", " << value.vec3Value.z;
        break;
    case engine::script::ScriptFieldType::String:
    case engine::script::ScriptFieldType::Prefab:
    case engine::script::ScriptFieldType::Entity:
        out << value.stringValue;
        break;
    }
    return out.str();
}

EntityID FindEntityByGuid(ecs::World& world, const std::string& guid)
{
    if (guid.empty())
        return NULL_ENTITY;

    EntityID result = NULL_ENTITY;
    world.ForEachAlive([&](EntityID entity)
    {
        if (result.IsValid())
            return;
        const auto* component = world.Get<GuidComponent>(entity);
        if (component && component->guid == guid)
            result = entity;
    });
    return result;
}

std::string EntityLabel(const ecs::World& world, EntityID entity)
{
    if (!entity.IsValid())
        return "(None)";
    std::string label;
    if (const auto* name = world.Get<NameComponent>(entity))
        label = name->name;
    if (label.empty())
        label = "Entity";
    return label + " #" + std::to_string(entity.value);
}

std::string EnsureEntityGuid(ecs::World& world, EntityID entity)
{
    if (!entity.IsValid() || !world.IsAlive(entity))
        return {};

    if (auto* guid = world.Get<GuidComponent>(entity))
    {
        if (!guid->guid.empty())
            return guid->guid;
    }

    static uint32_t counter = 1u;
    std::string value;
    do
    {
        value = "entity-" + std::to_string(entity.value) + "-" + std::to_string(counter++);
    } while (FindEntityByGuid(world, value).IsValid());

    if (auto* guid = world.Get<GuidComponent>(entity))
        guid->guid = value;
    else
        world.Add<GuidComponent>(entity, GuidComponent{value});
    return value;
}

bool DrawScriptFieldEditor(const char* fieldName,
                           EditorFrameContext& ctx,
                           engine::script::ScriptFieldType fieldType,
                           engine::script::ScriptFieldValue& value,
                           bool hasLiveValue)
{
    using engine::script::ScriptFieldType;

    bool changed = false;
    ImGui::PushID(fieldName);
    const float labelWidth = std::min(118.f, ImGui::GetContentRegionAvail().x * 0.42f);
    value_ui::TextUnformatted(fieldName);
    const char* clipboardType = "string";
    void* clipboardData = nullptr;
    size_t clipboardSize = 0;
    switch (fieldType)
    {
    case ScriptFieldType::Float: clipboardType = "float"; clipboardData = &value.floatValue; clipboardSize = sizeof(float); break;
    case ScriptFieldType::Int: clipboardType = "int"; clipboardData = &value.intValue; clipboardSize = sizeof(int32_t); break;
    case ScriptFieldType::Bool: clipboardType = "bool"; clipboardData = &value.boolValue; clipboardSize = sizeof(bool); break;
    case ScriptFieldType::Vec3: clipboardType = "float3"; clipboardData = &value.vec3Value; clipboardSize = sizeof(math::Vec3); break;
    case ScriptFieldType::Prefab: clipboardType = "asset-prefab"; break;
    case ScriptFieldType::Entity: clipboardType = "entity-guid"; break;
    default: break;
    }
    const bool compatible = clipboardData ? value_ui::Compatible(clipboardType, clipboardSize)
                                         : value_ui::Clipboard().type == clipboardType;
    changed |= value_ui::Menu("##script_field_value", [&] {
        if (clipboardData) value_ui::Copy(clipboardType, clipboardData, clipboardSize);
        else
        {
            std::string text = value.stringValue;
            if (fieldType == ScriptFieldType::Entity && text.empty() && value.entityValue.IsValid())
                if (const auto* guid = ctx.world.Get<GuidComponent>(value.entityValue)) text = guid->guid;
            value_ui::CopyText(clipboardType, text);
        }
    }, compatible, [&] {
        if (clipboardData) return value_ui::Paste(clipboardType, clipboardData, clipboardSize);
        const bool pasted = value_ui::PasteText(clipboardType, value.stringValue);
        if (pasted && fieldType == ScriptFieldType::Entity) value.entityValue = NULL_ENTITY;
        return pasted;
    }, !hasLiveValue);
    value_ui::ForgetLabel();
    if (hasLiveValue && ImGui::IsItemHovered())
        ImGui::SetTooltip("Aktueller Live-Wert des Scripts");
    ImGui::SameLine(labelWidth);
    ImGui::SetNextItemWidth(-1.f);
    if (hasLiveValue && fieldType != ScriptFieldType::Entity)
        ImGui::BeginDisabled();
    switch (fieldType)
    {
    case ScriptFieldType::Float:
        changed |= value_ui::DragFloat("##value", &value.floatValue, 0.1f);
        break;
    case ScriptFieldType::Int:
        changed |= value_ui::DragInt("##value", &value.intValue, 1.0f);
        break;
    case ScriptFieldType::Bool:
        changed |= value_ui::Checkbox("##value", &value.boolValue);
        break;
    case ScriptFieldType::Vec3:
        changed |= value_ui::DragFloat3("##value", &value.vec3Value.x, 0.1f);
        break;
    case ScriptFieldType::String:
    case ScriptFieldType::Prefab:
    {
        std::array<char, 512> text{};
        std::snprintf(text.data(), text.size(), "%s", value.stringValue.c_str());
        changed |= value_ui::InputText("##value", text.data(), text.size());
        if (changed)
            value.stringValue = text.data();
        // Dateien aus dem Asset-Browser hineinziehen (Pfad relativ zum Asset-Root).
        if (ImGui::BeginDragDropTarget())
        {
            static constexpr const char* kStringPayloads[] = {
                "KROM_ASSET_PREFAB", "KROM_ASSET_AUDIO", "KROM_ASSET_TEXTURE", "KROM_ASSET_MATERIAL",
            };
            for (const char* payloadType : kStringPayloads)
            {
                if (fieldType == ScriptFieldType::Prefab && std::strcmp(payloadType, "KROM_ASSET_PREFAB") != 0)
                    continue;
                if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload(payloadType))
                {
                    if (const char* path = static_cast<const char*>(p->Data); path && path[0] != '\0')
                    {
                        value.stringValue = path;
                        changed = true;
                    }
                }
            }
            ImGui::EndDragDropTarget();
        }
        if (fieldType == ScriptFieldType::Prefab && ImGui::IsItemHovered())
            ImGui::SetTooltip("Relativer Pfad zu Assets, z.B. Prefabs/Bullet.prefab");
        else if (ImGui::IsItemHovered() && !ImGui::IsItemActive())
            ImGui::SetTooltip("Text oder Datei aus dem Asset-Browser hierher ziehen.");
        break;
    }
    case ScriptFieldType::Entity:
    {
        if (!ctx.world.IsAlive(value.entityValue))
            value.entityValue = FindEntityByGuid(ctx.world, value.stringValue);

        const std::string preview = EntityLabel(ctx.world, value.entityValue);
        const float spacing = ImGui::GetStyle().ItemSpacing.x;
        const float clearWidth = ImGui::CalcTextSize("X").x + ImGui::GetStyle().FramePadding.x * 2.f;
        const float nameWidth = std::max(1.f, ImGui::GetContentRegionAvail().x -
            ImGui::GetFrameHeight() - clearWidth - spacing * 2.f);
        ImGui::BeginGroup();
        ImGui::BeginDisabled(!ctx.world.IsAlive(value.entityValue));
        ImGui::PushStyleVar(ImGuiStyleVar_ButtonTextAlign, ImVec2(0.f, 0.5f));
        ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_FrameBg));
        if (ImGui::Button((preview + "###reveal").c_str(), ImVec2(nameWidth, 0.f)))
        {
            ctx.state.hierarchyRevealEntity = value.entityValue;
            ctx.hierarchyPanelVisible = true;
        }
        ImGui::PopStyleColor();
        ImGui::PopStyleVar();
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Objekt in der Hierarchie anzeigen");
        ImGui::SameLine();
        ImGui::BeginDisabled(hasLiveValue);
        if (ImGui::BeginCombo("##value", nullptr, ImGuiComboFlags_NoPreview))
        {
            const bool noneSelected = !value.entityValue.IsValid();
            if (ImGui::Selectable("(None)", noneSelected))
            {
                value.entityValue = NULL_ENTITY;
                value.stringValue.clear();
                changed = true;
            }
            if (noneSelected)
                ImGui::SetItemDefaultFocus();

            ctx.world.ForEachAlive([&](EntityID candidate)
            {
                if (!IsScenePickEntity(ctx.world, candidate))
                    return;
                const bool selected = candidate == value.entityValue;
                const std::string label = EntityLabel(ctx.world, candidate);
                if (ImGui::Selectable(label.c_str(), selected))
                {
                    value.entityValue = candidate;
                    value.stringValue = EnsureEntityGuid(ctx.world, candidate);
                    changed = true;
                }
                if (selected)
                    ImGui::SetItemDefaultFocus();
            });
            ImGui::EndCombo();
        }
        ImGui::EndDisabled();
        ImGui::EndGroup();
        if (ImGui::BeginDragDropTarget())
        {
            if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("KROM_EDITOR_ENTITY"))
            {
                if (payload->DataSize == sizeof(EntityID))
                {
                    const EntityID dropped = *static_cast<const EntityID*>(payload->Data);
                    if (!hasLiveValue && IsScenePickEntity(ctx.world, dropped))
                    {
                        value.entityValue = dropped;
                        value.stringValue = EnsureEntityGuid(ctx.world, dropped);
                        changed = true;
                    }
                }
            }
            ImGui::EndDragDropTarget();
        }
        ImGui::SameLine();
        ImGui::BeginDisabled(hasLiveValue);
        if (ImGui::SmallButton("X"))
        {
            value.entityValue = NULL_ENTITY;
            value.stringValue.clear();
            changed = true;
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Entity-Referenz loeschen");
        ImGui::EndDisabled();
        break;
    }
    }
    if (hasLiveValue)
    {
        if (fieldType != ScriptFieldType::Entity)
            ImGui::EndDisabled();
        ImGui::SameLine();
        value_ui::TextDisabled("Live");
    }
    ImGui::PopID();
    return changed && !hasLiveValue;
}

// Liefert den MaterialHandle der Entity: erst MaterialComponent, dann MeshAsset-Fallback.
static MaterialHandle GetEntityMaterial(EditorFrameContext& ctx, EntityID entity)
{
    if (auto* mc = ctx.world.Get<MaterialComponent>(entity))
        if (mc->material.IsValid())
            return mc->material;

    if (auto* meshComp = ctx.world.Get<MeshComponent>(entity))
        if (auto* mesh = ctx.registry.meshes.Get(meshComp->mesh))
            if (!mesh->materialHandles.empty())
                return mesh->materialHandles[0];

    return MaterialHandle::Invalid();
}

static const assets::MaterialAsset* GetMaterialAsset(EditorFrameContext& ctx, MaterialHandle handle)
{
    return handle.IsValid() ? ctx.registry.materials.Get(handle) : nullptr;
}

static std::string MaterialDisplayName(const assets::MaterialAsset* material,
                                       MaterialHandle materialHandle)
{
    if (material)
    {
        if (!material->debugName.empty())
            return material->debugName;
        if (!material->path.empty())
            return PathToUtf8(PathFromUtf8(material->path).filename());
    }
    return materialHandle.IsValid() ? "Runtime Material" : "None";
}

static bool CanOpenMaterialAssetPath(const std::string& path)
{
    return !path.empty() && PathFromUtf8(path).extension() == ".mat";
}

static bool IsInspectorMaterialSelection(EditorFrameContext& ctx,
                                         EntityID entity,
                                         uint32_t submeshIndex,
                                         const std::string& materialPath)
{
    return ctx.state.inspectorMaterialEntity == entity &&
           ctx.state.inspectorMaterialSlot == static_cast<int>(submeshIndex) &&
           PathToGenericUtf8(PathFromUtf8(ctx.state.inspectorMaterialAssetPath)) ==
               PathToGenericUtf8(PathFromUtf8(materialPath));
}

static void SelectInspectorMaterial(EditorFrameContext& ctx,
                                    EntityID entity,
                                    uint32_t submeshIndex,
                                    const std::string& materialPath)
{
    if (!CanOpenMaterialAssetPath(materialPath))
        return;
    ctx.state.inspectorMaterialEntity = entity;
    ctx.state.inspectorMaterialSlot = static_cast<int>(submeshIndex);
    ctx.state.inspectorMaterialAssetPath = materialPath;
    SelectMaterialAsset(ctx, PathFromUtf8(materialPath));
}

static const char* PrimaryEntityType(EditorFrameContext& ctx, EntityID entity)
{
    if (ctx.world.Get<CameraComponent>(entity))
        return "Camera";
    if (ctx.world.Get<LightComponent>(entity))
        return "Light";
    if (ctx.world.Get<MeshComponent>(entity))
        return "Mesh";
    return "Entity";
}

static std::string ComponentSummary(EditorFrameContext& ctx, EntityID entity)
{
    std::string summary;
    auto append = [&](const char* name) {
        if (!summary.empty())
            summary += ", ";
        summary += name;
    };

    if (ctx.world.Get<TransformComponent>(entity))
        append("Transform");
    if (ctx.world.Get<CameraComponent>(entity))
        append("Camera");
    if (ctx.world.Get<LightComponent>(entity))
        append("Light");
    if (ctx.world.Get<MeshComponent>(entity))
        append("Mesh");
    if (ctx.world.Get<MaterialComponent>(entity))
        append("Material");
    if (ctx.world.Get<ParentComponent>(entity))
        append("Parent");
    if (ctx.world.Get<ChildrenComponent>(entity))
        append("Children");

    return summary.empty() ? std::string("None") : summary;
}

// ─── Script-Section ──────────────────────────────────────────────────────────

static void RevealScriptAsset(EditorFrameContext& ctx, const std::string& className)
{
    if (!ctx.state.assetBrowser)
        return;
    const auto path = FindCppScriptAssetPath(ctx, className);
    if (path.empty())
    {
        ctx.lastFileMessage = "Script-Datei nicht gefunden: " + className;
        return;
    }
    ctx.state.assetBrowser->highlightPath = path;
    ctx.assetsPanelVisible = true;
    ctx.state.consoleFocusRequest = false;
}

static void DrawScriptSection(EditorFrameContext& ctx, EntityID entity)
{
    using namespace engine::script;

    auto* sl = ctx.world.Get<ScriptList>(entity);
    std::vector<std::string> scriptClassNames;
    if (ctx.scriptRegistry)
        scriptClassNames = ctx.scriptRegistry->GetClassNames();
    for (const std::string& markerClassName : FindCppScriptClassNames(ctx))
    {
        if (std::find(scriptClassNames.begin(), scriptClassNames.end(), markerClassName) ==
            scriptClassNames.end())
        {
            scriptClassNames.push_back(markerClassName);
        }
    }
    std::sort(scriptClassNames.begin(), scriptClassNames.end());

    if (!sl && scriptClassNames.empty())
        return;  // kein Script-System aktiv
    const auto* runtimeGuid = ctx.world.Get<GuidComponent>(entity);
    const std::string runtimeEntityGuid = runtimeGuid ? runtimeGuid->guid : std::string{};

    const bool open = ComponentHeader("Scripts##scripts_hdr", ComponentIcon::Script,
                                              ImGuiTreeNodeFlags_DefaultOpen);
    if (!open) return;

    ImGui::PushID("ScriptSection");

    if (sl)
    {
        auto& instances = sl->Instances_Mutable();
        int toRemove = -1;

        for (int i = 0; i < (int)instances.size(); ++i)
        {
            ScriptInstance& inst = instances[i];
            ImGui::PushID(i);

            // Kopfzeile: fetter Instanzname + X-Button
            char headerBuf[256];
            std::snprintf(headerBuf, sizeof(headerBuf),
                "%s##si%d", inst.instanceName.c_str(), i);
            const bool instOpen = ImGui::TreeNodeEx(headerBuf,
                ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_AllowOverlap |
                ImGuiTreeNodeFlags_OpenOnArrow);
            if (ImGui::IsItemClicked(ImGuiMouseButton_Left) && !ImGui::IsItemToggledOpen())
                RevealScriptAsset(ctx, inst.className);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Script im Asset-Browser anzeigen (Pfeil zum Ein-/Ausklappen)");
            if (ImGui::BeginPopupContextItem("##script_ctx"))
            {
                DrawScriptClipboardMenuItems(ctx, entity, static_cast<size_t>(i));
                ImGui::EndPopup();
            }

            // X-Button rechts
            ImGui::SameLine(ImGui::GetContentRegionAvail().x - 18.f +
                            ImGui::GetCursorPosX());
            if (ImGui::SmallButton("X"))
                toRemove = i;

            if (instOpen)
            {
                value_ui::TextDisabled("Klasse:");
                ImGui::SameLine();
                if (ImGui::SmallButton(inst.className.c_str()))
                    RevealScriptAsset(ctx, inst.className);
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Script im Asset-Browser anzeigen");

                // Platzhalter: Klasse weder registriert noch in den Projektquellen.
                // Eintrag und gespeicherte Werte bleiben beim Speichern erhalten.
                if (!inst.script &&
                    !std::binary_search(scriptClassNames.begin(), scriptClassNames.end(), inst.className))
                {
                    ImGui::PushStyleColor(ImGuiCol_Text, theme::kWarning);
                    ImGui::TextWrapped("Script-Klasse im Projekt nicht gefunden. Eintrag und %zu "
                                       "gespeicherte Werte bleiben beim Speichern erhalten.",
                                       inst.fieldValues.size());
                    ImGui::PopStyleColor();
                }

                // Instanzname editierbar
                char buf[128];
                std::snprintf(buf, sizeof(buf), "%s", inst.instanceName.c_str());
                ImGui::SetNextItemWidth(-1.f);
                if (value_ui::InputText("##instname", buf, sizeof(buf)))
                {
                    inst.instanceName = buf;
                    // Änderung als Undo-Snapshot markieren
                    if (ctx.saveScene) {} // Snapshot wird beim nächsten Save erfasst
                }

                if (ctx.scriptRegistry)
                {
                    std::unordered_set<std::string> displayedFields;
                    const std::vector<EditorScriptFieldMarker> markerFields =
                        FindCppScriptFields(ctx, inst.className);
                    std::unordered_set<std::string> markerFieldNames;
                    for (const EditorScriptFieldMarker& markerField : markerFields)
                        markerFieldNames.insert(markerField.name);
                    const bool markersAreAuthoritative = !markerFieldNames.empty();

                    if (const auto* fields = ctx.scriptRegistry->GetFields(inst.className))
                    {
                        for (const ScriptFieldMeta& field : *fields)
                        {
                            if (markersAreAuthoritative &&
                                markerFieldNames.count(field.name) == 0u)
                            {
                                inst.fieldValues.erase(field.name);
                                continue;
                            }
                            displayedFields.insert(field.name);
                            ScriptFieldValue value{};
                            const auto storedIt = inst.fieldValues.find(field.name);
                            if (storedIt != inst.fieldValues.end())
                            {
                                // Gespeicherter Typ kann abgeleitet sein (Platzhalter: Int statt Float).
                                value = ConvertScriptFieldValue(storedIt->second, field.type);
                            }
                            else if (inst.script)
                            {
                                value.type = field.type;
                                (void)ctx.scriptRegistry->ReadField(*inst.script, inst.className, field.name, value);
                            }
                            else
                            {
                                value.type = field.type;
                            }

                            const bool editorLiveValue = ReadLiveEditorScriptField(ctx, inst, field, value);
                            bool hasLiveValue = editorLiveValue;
                            if (!hasLiveValue && !runtimeEntityGuid.empty())
                            {
                                const std::string liveKey = RuntimeScriptFieldKey(
                                    runtimeEntityGuid,
                                    static_cast<uint32_t>(i),
                                    field.name);
                                const auto liveIt = ctx.state.runtimeScriptFieldValues.find(liveKey);
                                if (liveIt != ctx.state.runtimeScriptFieldValues.end())
                                {
                                    value = ConvertScriptFieldValue(liveIt->second, field.type);
                                    hasLiveValue = true;
                                }
                            }

                            if (DrawScriptFieldEditor(field.name.c_str(), ctx, field.type, value, hasLiveValue && !editorLiveValue))
                            {
                                value.type = field.type;
                                inst.fieldValues[field.name] = value;
                                if (inst.script)
                                    (void)ctx.scriptRegistry->WriteField(*inst.script, inst.className, field.name, value);
                            }
                        }
                    }
                    for (const EditorScriptFieldMarker& markerField : markerFields)
                    {
                        if (displayedFields.count(markerField.name) > 0u)
                            continue;
                        displayedFields.insert(markerField.name);

                        ScriptFieldValue value{};
                        const auto storedIt = inst.fieldValues.find(markerField.name);
                        if (storedIt != inst.fieldValues.end())
                            value = ConvertScriptFieldValue(storedIt->second, markerField.type);
                        else if (markerField.hasDefaultValue)
                            value = markerField.defaultValue;
                        else
                            value.type = markerField.type;

                        bool hasLiveValue = false;
                        if (!runtimeEntityGuid.empty())
                        {
                            const std::string liveKey = RuntimeScriptFieldKey(
                                runtimeEntityGuid,
                                static_cast<uint32_t>(i),
                                markerField.name);
                            const auto liveIt = ctx.state.runtimeScriptFieldValues.find(liveKey);
                            if (liveIt != ctx.state.runtimeScriptFieldValues.end())
                            {
                                value = ConvertScriptFieldValue(liveIt->second, markerField.type);
                                hasLiveValue = true;
                            }
                        }

                        if (DrawScriptFieldEditor(markerField.name.c_str(), ctx, markerField.type, value, hasLiveValue))
                        {
                            value.type = markerField.type;
                            inst.fieldValues[markerField.name] = value;
                        }
                    }
                    if (!runtimeEntityGuid.empty())
                    {
                        const std::string livePrefix =
                            runtimeEntityGuid + "|" + std::to_string(static_cast<uint32_t>(i)) + "|";
                        for (const auto& [key, liveValue] : ctx.state.runtimeScriptFieldValues)
                        {
                            if (key.rfind(livePrefix, 0u) != 0u)
                                continue;
                            const std::string fieldName = key.substr(livePrefix.size());
                            if (displayedFields.count(fieldName) > 0u)
                                continue;
                            const std::string liveText =
                                "Live: " + fieldName + " = " + RuntimeScriptFieldValueText(liveValue);
                            value_ui::TextDisabled("%s", liveText.c_str());
                        }
                    }
                }

                ImGui::TreePop();
            }

            ImGui::PopID();
        }

        if (toRemove >= 0)
        {
            sl->RemoveAt(static_cast<size_t>(toRemove), entity);
        }
    }

    // "Script hinzufügen" Button
    if (!scriptClassNames.empty())
    {
        ImGui::Spacing();
        if (EditorActionButton("+ Script hinzufuegen", ImVec2(-1.f, 34.f)))
            ImGui::OpenPopup("AddScriptPopup");

        if (ImGui::BeginPopup("AddScriptPopup"))
        {
            value_ui::TextDisabled("Script-Klasse waehlen");
            ImGui::Separator();

            for (const auto& name : scriptClassNames)
            {
                if (ImGui::MenuItem(name.c_str()))
                {
                    if (!sl)
                    {
                        engine::script::ScriptList newList;
                        newList.SetOwnerEntity(entity);
                        ctx.world.Add<ScriptList>(entity, std::move(newList));
                        sl = ctx.world.Get<ScriptList>(entity);
                    }
                    if (sl)
                    {
                        const bool hasLiveFactory = ctx.scriptRegistry && ctx.scriptRegistry->Has(name);
                        if (hasLiveFactory)
                        {
                            sl->Add(name, entity, *ctx.scriptRegistry);
                        }
                        else
                        {
                            sl->AddNameOnly(name);
                            ScriptInstance& inst = sl->Instances_Mutable().back();
                            for (const EditorScriptFieldMarker& field : FindCppScriptFields(ctx, name))
                            {
                                ScriptFieldValue value{};
                                value.type = field.type;
                                if (field.hasDefaultValue)
                                    value = field.defaultValue;
                                inst.fieldValues[field.name] = std::move(value);
                            }
                            ctx.lastFileMessage =
                                "Script hinzugefuegt: " + name + " (Build noetig fuer Live-Code).";
                        }
                    }
                }
            }
            ImGui::EndPopup();
        }
    }

    ImGui::PopID();
}

// ─────────────────────────────────────────────────────────────────────────────

static void DrawTransformSection(EditorFrameContext& ctx, EntityID entity)
{
    auto* t = ctx.world.Get<TransformComponent>(entity);
    if (!t)
        return;

    const bool transformOpen = ComponentHeader("Transform", ComponentIcon::Transform, ImGuiTreeNodeFlags_DefaultOpen);
    if (BeginComponentContextMenu("##transform_hdr_ctx"))
    {
        DrawTransformClipboardMenuItems(ctx, entity);
        ImGui::EndPopup();
    }
    if (!transformOpen)
        return;

    int space = ctx.state.transformSpace == EditorTransformSpace::World ? 1 : 0;
    if (ImGui::RadioButton("Local", space == 0))
        space = 0;
    if (ImGui::IsItemHovered())
        ImGui::SetItemTooltip("Local Space: Werte relativ zum Parent (bzw. World, wenn kein Parent vorhanden).");
    ImGui::SameLine();
    if (ImGui::RadioButton("World", space == 1))
        space = 1;
    if (ImGui::IsItemHovered())
        ImGui::SetItemTooltip("World Space: globale Achsen und globale Position/Rotation.");
    ctx.state.transformSpace = space == 1 ? EditorTransformSpace::World : EditorTransformSpace::Local;

    // Modus-Anzeige: Viewport-Selektion = "Nur Parent", Outliner = "Mit Kindern"
    {
        const bool isViewport = ctx.state.selectionSource == SelectionSource::Viewport;
        ImGui::SameLine();
        ImGui::Spacing(); ImGui::SameLine();
        if (isViewport)
        {
            ImGui::TextColored({0.4f, 0.9f, 1.0f, 1.f}, "* Nur Parent");
            if (ImGui::IsItemHovered())
                ImGui::SetItemTooltip(
                    "Viewport-Klick: nur diese Entity wird transformiert.\n"
                    "Kinder bleiben an ihrer World-Position.\n"
                    "Outliner-Klick: Parent + Kinder bewegen sich gemeinsam.");
        }
        else
        {
            value_ui::TextDisabled("* Mit Kindern");
            if (ImGui::IsItemHovered())
                ImGui::SetItemTooltip(
                    "Outliner-Klick: Parent + Kinder bewegen sich gemeinsam.\n"
                    "Viewport-Klick: nur diese Entity wird transformiert,\n"
                    "Kinder bleiben an ihrer World-Position.");
        }
    }

    // Hilfsfunktion: Label links (klickbar für Gizmo-Modus), DragFloat3 rechts
    constexpr float kLabelWidth = 72.f;

    // Gibt {valueChanged, labelClicked} zurück.
    // labelColor: wenn gesetzt, wird das Label in dieser Farbe gezeichnet (Modus-Anzeige).
    struct DragResult { bool changed; bool labelClicked; };
    auto LabeledDragFloat3 = [&](const char* label, const char* id,
                                 float* v, float speed,
                                 const ImVec4* labelColor = nullptr,
                                 float vmin = 0.f, float vmax = 0.f) -> DragResult
    {
        if (labelColor) ImGui::PushStyleColor(ImGuiCol_Text, *labelColor);
        value_ui::Text("%s", label);
        if (labelColor) ImGui::PopStyleColor();
        const auto clipboardPart = std::strcmp(id, "##pos") == 0 ? TransformClipboardPart::Position
            : (std::strcmp(id, "##rot") == 0 ? TransformClipboardPart::Rotation : TransformClipboardPart::Scale);
        DrawTransformValueClipboardMenu(ctx, entity, clipboardPart);
        const bool labelClicked = ImGui::IsItemClicked();
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Klicken um Gizmo-Modus zu wechseln");
        ImGui::SameLine(kLabelWidth);
        ImGui::SetNextItemWidth(-1.f);
        const ImGuiSliderFlags clampFlag = (vmin < vmax) ? ImGuiSliderFlags_AlwaysClamp : ImGuiSliderFlags_None;
        return { value_ui::DragFloat3(id, v, speed, vmin, vmax, "%.3f", clampFlag), labelClicked };
    };

    if (ctx.state.transformSpace == EditorTransformSpace::Local)
    {
        // sodass Ziehen an Z die Entity tatsächlich in ihrer lokalen Z-Richtung bewegt.
        const ImVec4 kPosActive{0.4f, 1.0f, 0.4f, 1.f};
        const ImVec4 kRotActive{0.4f, 0.6f, 1.0f, 1.f};
        const ImVec4 kSclActive{1.0f, 0.8f, 0.2f, 1.f};
        const bool posMode = ctx.state.gizmoMode == GizmoMode::Position;
        const bool rotMode = ctx.state.gizmoMode == GizmoMode::Rotation;
        const bool sclMode = ctx.state.gizmoMode == GizmoMode::Scale;

        const std::string localPositionKey = MakeEditHistoryKey(entity, "transform.local.position");
        auto posResult = LabeledDragFloat3("Position", "##pos", &t->localPosition.x, 0.1f,
                                           posMode ? &kPosActive : nullptr);
        if (posResult.labelClicked) ctx.state.gizmoMode = GizmoMode::Position;
        if (posResult.changed)
        {
            BeginPendingSceneEdit(ctx, localPositionKey, "Position geaendert");
            t->dirty = true;
        }
        if (ImGui::IsItemDeactivatedAfterEdit())
            CommitPendingSceneEdit(ctx, localPositionKey);

        math::Quat localRotation = t->localRotation;
        math::Vec3 euler = PrepareRotationEditEuler(
            ctx, entity, EditorTransformSpace::Local, localRotation);
        const std::string localRotationKey = MakeEditHistoryKey(entity, "transform.local.rotation");
        auto rotResult = LabeledDragFloat3("Rotation", "##rot", &euler.x, 1.0f,
                                           rotMode ? &kRotActive : nullptr, 0.f, 360.f);
        if (rotResult.labelClicked) ctx.state.gizmoMode = GizmoMode::Rotation;
        if (rotResult.changed)
        {
            BeginPendingSceneEdit(ctx, localRotationKey, "Rotation geaendert");
            localRotation = math::Quat::FromEulerDeg(euler.x, euler.y, euler.z);
            t->localRotation = localRotation;
            t->dirty = true;
        }
        if (ImGui::IsItemDeactivatedAfterEdit())
            CommitPendingSceneEdit(ctx, localRotationKey);
        CommitRotationEditState(ctx, entity, EditorTransformSpace::Local, euler, localRotation);

        const std::string localScaleKey = MakeEditHistoryKey(entity, "transform.local.scale");
        auto sclResult = LabeledDragFloat3("Scale", "##scl", &t->localScale.x, 0.01f,
                                           sclMode ? &kSclActive : nullptr, 0.0001f, 1000.f);
        if (sclResult.labelClicked) ctx.state.gizmoMode = GizmoMode::Scale;
        if (sclResult.changed)
        {
            BeginPendingSceneEdit(ctx, localScaleKey, "Skalierung geaendert");
            t->dirty = true;
        }
        if (ImGui::IsItemDeactivatedAfterEdit())
            CommitPendingSceneEdit(ctx, localScaleKey);

        // Checkbox "Inherit Parent Scale" — nur sichtbar wenn Entity einen Parent hat
        if (ctx.world.Has<ParentComponent>(entity))
        {
            ImGui::SetCursorPosX(ImGui::GetCursorPosX() + kLabelWidth + 4.f);
            bool inherit = t->inheritParentScale;
            if (value_ui::Checkbox("Scale erben", &inherit))
            {
                const std::string key = MakeEditHistoryKey(entity, "transform.inheritParentScale");
                BeginPendingSceneEdit(ctx, key, "Scale-Vererbung geaendert");
                t->inheritParentScale = inherit;
                t->dirty = true;
                CommitPendingSceneEdit(ctx, key);
            }
            if (ImGui::IsItemHovered())
                ImGui::SetItemTooltip(
                    "An: Dieses Objekt skaliert mit dem Parent mit.\n"
                    "Aus: Dieses Objekt behaelt seine eigene Weltgroesse.");
        }
    }
    else
    {
        const ImVec4 kPosActive{0.4f, 1.0f, 0.4f, 1.f};
        const ImVec4 kRotActive{0.4f, 0.6f, 1.0f, 1.f};
        const ImVec4 kSclActive{1.0f, 0.8f, 0.2f, 1.f};
        const bool posMode = ctx.state.gizmoMode == GizmoMode::Position;
        const bool rotMode = ctx.state.gizmoMode == GizmoMode::Rotation;
        const bool sclMode = ctx.state.gizmoMode == GizmoMode::Scale;

        math::Vec3 position = WorldPosition(ctx, entity);
        const std::string worldPositionKey = MakeEditHistoryKey(entity, "transform.world.position");
        auto posResult = LabeledDragFloat3("Position", "##pos", &position.x, 0.1f,
                                           posMode ? &kPosActive : nullptr);
        if (posResult.labelClicked) ctx.state.gizmoMode = GizmoMode::Position;
        if (posResult.changed)
        {
            BeginPendingSceneEdit(ctx, worldPositionKey, "Position geaendert");
            SetWorldPosition(ctx, entity, *t, position);
        }
        if (ImGui::IsItemDeactivatedAfterEdit())
            CommitPendingSceneEdit(ctx, worldPositionKey);

        const bool worldRotationBlocked = HasNonUniformScaledParent(ctx, entity);
        math::Quat worldRotation = WorldRotation(ctx, entity);
        math::Vec3 euler = PrepareRotationEditEuler(
            ctx, entity, EditorTransformSpace::World, worldRotation);
        const std::string worldRotationKey = MakeEditHistoryKey(entity, "transform.world.rotation");
        if (worldRotationBlocked)
            ImGui::BeginDisabled();
        auto rotResult = LabeledDragFloat3("Rotation", "##rot", &euler.x, 1.0f,
                                           rotMode ? &kRotActive : nullptr, 0.f, 360.f);
        if (rotResult.labelClicked) ctx.state.gizmoMode = GizmoMode::Rotation;
        if (rotResult.changed && !worldRotationBlocked)
        {
            BeginPendingSceneEdit(ctx, worldRotationKey, "Rotation geaendert");
            worldRotation = math::Quat::FromEulerDeg(euler.x, euler.y, euler.z);
            SetWorldRotation(ctx, entity, *t, worldRotation);
        }
        if (ImGui::IsItemDeactivatedAfterEdit())
            CommitPendingSceneEdit(ctx, worldRotationKey);
        CommitRotationEditState(ctx, entity, EditorTransformSpace::World, euler, worldRotation);
        if (worldRotationBlocked)
        {
            ImGui::EndDisabled();
            if (ImGui::IsItemHovered())
                ImGui::SetItemTooltip("WORLD-Rotation ist bei nicht-uniform skalierten Parents deaktiviert, weil das Child sonst in einem reinen TRS-System geschert/verzerrt wird.");
        }

        // World-scale edits must be converted to local scale, including roots
        // without meshes whose transform controls an entire prefab hierarchy.
        math::Vec3 scale = WorldScale(ctx, entity);
        {
            const std::string worldScaleKey = MakeEditHistoryKey(entity, "transform.world.scale");
            auto sclResult = LabeledDragFloat3("Scale", "##scl", &scale.x, 0.01f,
                                               sclMode ? &kSclActive : nullptr, 0.0001f, 1000.f);
            if (sclResult.labelClicked) ctx.state.gizmoMode = GizmoMode::Scale;
            if (sclResult.changed)
            {
                BeginPendingSceneEdit(ctx, worldScaleKey, "Skalierung geaendert");
                SetWorldScale(ctx, entity, *t, scale);
            }
            if (ImGui::IsItemDeactivatedAfterEdit())
                CommitPendingSceneEdit(ctx, worldScaleKey);
        }
    }
}

static uint32_t VisibleUserLayerMask(const EditorFrameContext& ctx)
{
    uint32_t mask = 0u;
    for (int b = 0; b < 32; ++b)
    {
        if (b == 1)
            continue;
        const std::string& name = ctx.state.layerNames[b];
        if (name.empty() && b >= 8)
            continue;
        mask |= (1u << b);
    }
    return mask;
}

static uint32_t NormalizeSingleUserLayerMask(const EditorFrameContext& ctx, uint32_t mask)
{
    const uint32_t selected = mask & VisibleUserLayerMask(ctx);
    for (int b = 0; b < 32; ++b)
    {
        if ((selected & (1u << b)) != 0u)
            return (1u << b);
    }
    return renderer::LAYER_DEFAULT;
}

static int LayerIndexFromSingleMask(uint32_t mask)
{
    for (int b = 0; b < 32; ++b)
        if ((mask & (1u << b)) != 0u)
            return b;
    return 0;
}

static std::string BuildLayerMaskPreview(const EditorFrameContext& ctx, uint32_t mask)
{
    const uint32_t visibleMask = VisibleUserLayerMask(ctx);
    const uint32_t selected = mask & visibleMask;
    if (selected == 0u)
        return "Nothing";
    if ((selected & visibleMask) == visibleMask)
        return "Everything";

    std::string preview;
    int listed = 0;
    int total = 0;
    for (int b = 0; b < 32; ++b)
    {
        if ((selected & (1u << b)) == 0u)
            continue;
        if (b == 1)
            continue;
        const std::string& name = ctx.state.layerNames[b];
        if (name.empty() && b >= 8)
            continue;

        ++total;
        if (listed >= 3)
            continue;
        if (!preview.empty())
            preview += ", ";
        preview += name.empty() ? ("Layer " + std::to_string(b)) : name;
        ++listed;
    }

    if (total > 3)
        return std::to_string(total) + " Layers";
    return preview;
}

static bool DrawLayerMaskCombo(EditorFrameContext& ctx,
                               const char* label,
                               const char* id,
                               uint32_t& mask)
{
    const uint32_t visibleMask = VisibleUserLayerMask(ctx);
    uint32_t nextMask = mask & ~renderer::LAYER_EDITOR_GIZMO;
    bool changed = false;

    const std::string preview = BuildLayerMaskPreview(ctx, nextMask);
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
    if (ImGui::BeginCombo(label, preview.c_str()))
    {
        if (ImGui::Selectable(("Everything##" + std::string(id)).c_str(),
                              (nextMask & visibleMask) == visibleMask))
        {
            nextMask = visibleMask;
            changed = true;
        }
        if (ImGui::Selectable(("Nothing##" + std::string(id)).c_str(),
                              (nextMask & visibleMask) == 0u))
        {
            nextMask = renderer::LAYER_NONE;
            changed = true;
        }

        ImGui::Separator();
        for (int b = 0; b < 32; ++b)
        {
            if (b == 1)
                continue;
            const std::string& name = ctx.state.layerNames[b];
            if (name.empty() && b >= 8)
                continue;

            bool checked = (nextMask & (1u << b)) != 0u;
            const std::string itemLabel =
                (name.empty() ? ("Layer " + std::to_string(b)) : name) +
                "##" + id + std::to_string(b);
            if (value_ui::Checkbox(itemLabel.c_str(), &checked))
            {
                if (checked)
                    nextMask |= (1u << b);
                else
                    nextMask &= ~(1u << b);
                changed = true;
            }
        }
        ImGui::EndCombo();
    }

    nextMask &= ~renderer::LAYER_EDITOR_GIZMO;
    if (changed && nextMask != mask)
        mask = nextMask;
    return changed;
}

static void DrawCameraSection(EditorFrameContext& ctx, EntityID entity)
{
    auto* camera = ctx.world.Get<CameraComponent>(entity);
    if (!camera)
        return;

    const bool camOpen = ComponentHeader("Camera", ComponentIcon::Camera, ImGuiTreeNodeFlags_DefaultOpen);
    if (BeginComponentContextMenu("##cam_hdr_ctx"))
    {
        DrawComponentClipboardMenuItems<CameraComponent>(ctx, entity, "Kamera");
        ImGui::Separator();
        if (ImGui::MenuItem("Camera entfernen"))
            ExecuteSceneMutation(ctx, "Komponente entfernt", [&]() { ctx.world.Remove<CameraComponent>(entity); });
        ImGui::EndPopup();
    }
    if (!camOpen)
        return;

    int projection = camera->projection == ProjectionType::Orthographic ? 1 : 0;
    const std::string projectionKey = MakeEditHistoryKey(entity, "camera.projection");
    if (value_ui::Combo("Projection", &projection, "Perspective\0Orthographic\0"))
    {
        BeginPendingSceneEdit(ctx, projectionKey, "Kamera-Projektion geaendert");
        camera->projection = projection == 1 ? ProjectionType::Orthographic : ProjectionType::Perspective;
    }
    if (ImGui::IsItemDeactivatedAfterEdit())
        CommitPendingSceneEdit(ctx, projectionKey);

    const std::string primaryValueKey = camera->projection == ProjectionType::Perspective
        ? MakeEditHistoryKey(entity, "camera.fov")
        : MakeEditHistoryKey(entity, "camera.orthoSize");
    if (camera->projection == ProjectionType::Perspective)
    {
        if (value_ui::DragFloat("FOV", &camera->fovYDeg, 0.25f, 1.f, 179.f))
            BeginPendingSceneEdit(ctx, primaryValueKey, "Kamera-FOV geaendert");
        if (ImGui::IsItemDeactivatedAfterEdit())
            CommitPendingSceneEdit(ctx, primaryValueKey);
    }
    else
    {
        if (value_ui::DragFloat("Ortho Size", &camera->orthoSize, 0.05f, 0.01f, 10000.f))
            BeginPendingSceneEdit(ctx, primaryValueKey, "Kamera-Orthogroesse geaendert");
        if (ImGui::IsItemDeactivatedAfterEdit())
            CommitPendingSceneEdit(ctx, primaryValueKey);
    }

    const std::string nearKey = MakeEditHistoryKey(entity, "camera.near");
    if (value_ui::DragFloat("Near", &camera->nearPlane, 0.01f, 0.001f, camera->farPlane - 0.001f))
        BeginPendingSceneEdit(ctx, nearKey, "Near Plane geaendert");
    if (ImGui::IsItemDeactivatedAfterEdit())
        CommitPendingSceneEdit(ctx, nearKey);

    const std::string farKey = MakeEditHistoryKey(entity, "camera.far");
    if (value_ui::DragFloat("Far", &camera->farPlane, 1.0f, camera->nearPlane + 0.001f, 100000.f))
        BeginPendingSceneEdit(ctx, farKey, "Far Plane geaendert");
    if (ImGui::IsItemDeactivatedAfterEdit())
        CommitPendingSceneEdit(ctx, farKey);


    bool activeCamera = camera->isMainCamera;
    const std::string mainKey = MakeEditHistoryKey(entity, "camera.main");
    if (value_ui::Checkbox("Active Camera", &activeCamera))
    {
        BeginPendingSceneEdit(ctx, mainKey, "Aktive Kamera geaendert");
        if (activeCamera)
            SetExclusiveMainCamera(ctx, entity);
        else
            camera->isMainCamera = false;
    }
    if (ImGui::IsItemDeactivatedAfterEdit())
        CommitPendingSceneEdit(ctx, mainKey);

    {
        uint32_t cm = camera->cullingMask;
        const std::string cmKey = MakeEditHistoryKey(entity, "camera.cullingMask");
        if (DrawLayerMaskCombo(ctx, "Culling Mask", "cameraCulling", cm))
        {
            BeginPendingSceneEdit(ctx, cmKey, "Culling Mask geaendert");
            camera->cullingMask = cm;
            CommitPendingSceneEdit(ctx, cmKey);
        }
    }

    ImGui::Separator();
    value_ui::TextDisabled("Hintergrund");

    {
        int bgMode = static_cast<int>(camera->backgroundMode);
        const std::string bgModeKey = MakeEditHistoryKey(entity, "camera.backgroundMode");
        if (value_ui::Combo("##bgMode", &bgMode, "Solid\0Skybox\0"))
        {
            BeginPendingSceneEdit(ctx, bgModeKey, "Hintergrundmodus geaendert");
            camera->backgroundMode = static_cast<BackgroundMode>(bgMode);
            CommitPendingSceneEdit(ctx, bgModeKey);
        }
    }

    if (camera->backgroundMode == BackgroundMode::ClearColor)
    {
        const std::string colorKey = MakeEditHistoryKey(entity, "camera.clearColor");
        if (value_ui::ColorEdit4("Farbe##bgColor", camera->clearColor.data()))
            BeginPendingSceneEdit(ctx, colorKey, "Hintergrundfarbe geaendert");
        if (ImGui::IsItemDeactivatedAfterEdit())
            CommitPendingSceneEdit(ctx, colorKey);
    }

}

static void DrawLightSection(EditorFrameContext& ctx, EntityID entity)
{
    auto* light = ctx.world.Get<LightComponent>(entity);
    if (!light)
        return;

    const bool lightOpen = ComponentHeader("Light", ComponentIcon::Light, ImGuiTreeNodeFlags_DefaultOpen);
    if (BeginComponentContextMenu("##light_hdr_ctx"))
    {
        DrawComponentClipboardMenuItems<LightComponent>(ctx, entity, "Licht");
        ImGui::Separator();
        if (ImGui::MenuItem("Light entfernen"))
            ExecuteSceneMutation(ctx, "Komponente entfernt", [&]() { ctx.world.Remove<LightComponent>(entity); });
        ImGui::EndPopup();
    }
    if (!lightOpen)
        return;

    int type = static_cast<int>(light->type);
    const std::string typeKey = MakeEditHistoryKey(entity, "light.type");
    if (value_ui::Combo("Type", &type, "Directional\0Point\0Spot\0"))
    {
        BeginPendingSceneEdit(ctx, typeKey, "Lichttyp geaendert");
        const LightType previousType = light->type;
        light->type = static_cast<LightType>(std::clamp(type, 0, 2));
        if (light->type != previousType)
        {
            if (light->type == LightType::Directional)
            {
                light->range = 100.f;
                light->castShadows = true;
                light->shadowSettings.enabled = true;
                light->shadowSettings.resolution = 2048u;
                light->shadowSettings.maxDistance = 100.f;
            }
            else if (light->type == LightType::Spot)
            {
                light->range = 12.f;
                light->spotInnerDeg = 18.f;
                light->spotOuterDeg = 35.f;
                light->castShadows = true;
                light->shadowSettings.enabled = true;
                light->shadowSettings.resolution = 1024u;
                light->shadowSettings.maxDistance = light->range;
            }
            else
            {
                light->range = 12.f;
                light->castShadows = true;
                light->shadowSettings.enabled = true;
                light->shadowSettings.resolution = 1024u;
                light->shadowSettings.maxDistance = light->range;
            }
        }
    }
    if (ImGui::IsItemDeactivatedAfterEdit())
        CommitPendingSceneEdit(ctx, typeKey);

    const std::string colorKey = MakeEditHistoryKey(entity, "light.color");
    if (value_ui::ColorEdit3("Color", &light->color.x))
        BeginPendingSceneEdit(ctx, colorKey, "Lichtfarbe geaendert");
    if (ImGui::IsItemDeactivatedAfterEdit())
        CommitPendingSceneEdit(ctx, colorKey);

    const std::string intensityKey = MakeEditHistoryKey(entity, "light.intensity");
    if (value_ui::DragFloat("Intensity", &light->intensity, 0.05f, 0.f, 100000.f))
        BeginPendingSceneEdit(ctx, intensityKey, "Lichtintensitaet geaendert");
    if (ImGui::IsItemDeactivatedAfterEdit())
        CommitPendingSceneEdit(ctx, intensityKey);

    if (light->type != LightType::Directional)
    {
        const float oldRange = light->range;
        const std::string rangeKey = MakeEditHistoryKey(entity, "light.range");
        if (value_ui::DragFloat("Range", &light->range, 0.1f, 0.01f, 100000.f))
        {
            const bool shadowDistanceFollowedRange =
                std::fabs(light->shadowSettings.maxDistance - oldRange) <= 0.01f ||
                std::fabs(light->shadowSettings.maxDistance - 100.f) <= 0.01f;
            if (shadowDistanceFollowedRange)
                light->shadowSettings.maxDistance = light->range;
            BeginPendingSceneEdit(ctx, rangeKey, "Lichtreichweite geaendert");
        }
        if (ImGui::IsItemDeactivatedAfterEdit())
            CommitPendingSceneEdit(ctx, rangeKey);
    }

    if (light->type == LightType::Spot)
    {
        const std::string innerKey = MakeEditHistoryKey(entity, "light.innerAngle");
        if (value_ui::DragFloat("Inner Angle", &light->spotInnerDeg, 0.25f, 0.f, 179.f))
            BeginPendingSceneEdit(ctx, innerKey, "Spot-Innenwinkel geaendert");
        if (ImGui::IsItemDeactivatedAfterEdit())
            CommitPendingSceneEdit(ctx, innerKey);

        const std::string outerKey = MakeEditHistoryKey(entity, "light.outerAngle");
        if (value_ui::DragFloat("Outer Angle", &light->spotOuterDeg, 0.25f, light->spotInnerDeg, 179.f))
            BeginPendingSceneEdit(ctx, outerKey, "Spot-Aussenwinkel geaendert");
        if (ImGui::IsItemDeactivatedAfterEdit())
            CommitPendingSceneEdit(ctx, outerKey);
    }

    bool shadows = light->castShadows || light->shadowSettings.enabled;
    const std::string castShadowsKey = MakeEditHistoryKey(entity, "light.castShadows");
    if (value_ui::Checkbox("Cast Shadows", &shadows))
    {
        BeginPendingSceneEdit(ctx, castShadowsKey, "Schattenoption geaendert");
        light->castShadows = shadows;
        light->shadowSettings.enabled = shadows;
        if (shadows && light->type != LightType::Directional)
        {
            const float range = std::max(light->range, 0.1f);
            if (light->shadowSettings.maxDistance <= 0.1f)
                light->shadowSettings.maxDistance = range;
        }
    }
    if (ImGui::IsItemDeactivatedAfterEdit())
        CommitPendingSceneEdit(ctx, castShadowsKey);

    if (shadows)
    {
        int resolution = static_cast<int>(light->shadowSettings.resolution);
        const std::string resolutionKey = MakeEditHistoryKey(entity, "light.shadowResolution");
        if (value_ui::DragInt("Shadow Resolution", &resolution, 16.f, 128, 8192))
        {
            BeginPendingSceneEdit(ctx, resolutionKey, "Schattenaufloesung geaendert");
            light->shadowSettings.resolution = static_cast<uint32_t>(std::max(resolution, 128));
        }
        if (ImGui::IsItemDeactivatedAfterEdit())
            CommitPendingSceneEdit(ctx, resolutionKey);

        const float minShadowBias = light->type == LightType::Directional ? 0.0005f : 0.0001f;
        const float minNormalBias = light->type == LightType::Directional ? 0.0010f : 0.0010f;

        const std::string biasKey = MakeEditHistoryKey(entity, "light.shadowBias");
        if (value_ui::DragFloat("Shadow Bias", &light->shadowSettings.bias, 0.0001f, minShadowBias, 1.f, "%.5f"))
        {
            light->shadowSettings.bias = std::max(light->shadowSettings.bias, minShadowBias);
            BeginPendingSceneEdit(ctx, biasKey, "Shadow Bias geaendert");
        }
        if (ImGui::IsItemDeactivatedAfterEdit())
            CommitPendingSceneEdit(ctx, biasKey);

        const std::string normalBiasKey = MakeEditHistoryKey(entity, "light.normalBias");
        if (value_ui::DragFloat("Normal Bias", &light->shadowSettings.normalBias, 0.0001f, minNormalBias, 1.f, "%.5f"))
        {
            light->shadowSettings.normalBias = std::max(light->shadowSettings.normalBias, minNormalBias);
            BeginPendingSceneEdit(ctx, normalBiasKey, "Normal Bias geaendert");
        }
        if (ImGui::IsItemDeactivatedAfterEdit())
            CommitPendingSceneEdit(ctx, normalBiasKey);

        const std::string strengthKey = MakeEditHistoryKey(entity, "light.shadowStrength");
        if (value_ui::DragFloat("Shadow Strength", &light->shadowSettings.strength, 0.01f, 0.f, 1.f))
            BeginPendingSceneEdit(ctx, strengthKey, "Shadow Strength geaendert");
        if (ImGui::IsItemDeactivatedAfterEdit())
            CommitPendingSceneEdit(ctx, strengthKey);

        const std::string distanceKey = MakeEditHistoryKey(entity, "light.shadowDistance");
        if (value_ui::DragFloat("Shadow Distanz", &light->shadowSettings.maxDistance, 0.5f, 1.f, 10000.f, "%.1f"))
            BeginPendingSceneEdit(ctx, distanceKey, "Schatten-Distanz geaendert");
        if (ImGui::IsItemDeactivatedAfterEdit())
            CommitPendingSceneEdit(ctx, distanceKey);
        if (ImGui::IsItemHovered())
        {
            if (light->type == LightType::Directional)
            {
                ImGui::SetItemTooltip(
                    "Maximale Kameradistanz, bis zu der Directional-Schatten berechnet werden.\n"
                    "Directional Lights haben keine Range; diese Distanz begrenzt den Schattenbereich vor der Kamera.");
            }
            else
            {
                ImGui::SetItemTooltip(
                    "Maximale Entfernung, bis zu der dieses lokale Licht Schatten berechnet.\n"
                    "Neue Point/Spot-Lights starten mit Shadow Distanz = Range.\n"
                    "Wenn die Distanz noch der Range folgt, wird sie bei Range-Aenderung automatisch mitgezogen.");
            }
        }

        if (light->type == LightType::Directional)
        {
            int cascades = static_cast<int>(std::max<uint32_t>(1u, light->shadowSettings.cascadeCount));
            const std::string cascadeKey = MakeEditHistoryKey(entity, "light.cascadeCount");
            if (value_ui::SliderInt("Cascades", &cascades, 1, 4))
            {
                BeginPendingSceneEdit(ctx, cascadeKey, "Cascade-Anzahl geaendert");
                light->shadowSettings.cascadeCount = static_cast<uint32_t>(std::clamp(cascades, 1, 4));
            }
            if (ImGui::IsItemDeactivatedAfterEdit())
                CommitPendingSceneEdit(ctx, cascadeKey);
            if (ImGui::IsItemHovered())
                ImGui::SetItemTooltip(
                    "Cascaded Shadow Maps: Anzahl der Schatten-Stufen.\n"
                    "1 = eine einzelne Map ueber die ganze Distanz (nah = unscharf,\n"
                    "wenn die Distanz gross ist). 2-4 = nahe Bereiche bekommen eine\n"
                    "eigene, scharfe Map, ferne eine groebere. Behebt unscharfe\n"
                    "Schatten in Kameranaehe bei grosser Shadow-Distanz.");

            if (light->shadowSettings.cascadeCount > 1u)
            {
                const std::string lambdaKey = MakeEditHistoryKey(entity, "light.cascadeLambda");
                if (value_ui::SliderFloat("Cascade-Verteilung", &light->shadowSettings.cascadeLambda, 0.f, 1.f, "%.2f"))
                    BeginPendingSceneEdit(ctx, lambdaKey, "Cascade-Verteilung geaendert");
                if (ImGui::IsItemDeactivatedAfterEdit())
                    CommitPendingSceneEdit(ctx, lambdaKey);
                if (ImGui::IsItemHovered())
                    ImGui::SetItemTooltip(
                        "Verteilung der Cascade-Grenzen ueber die Shadow-Distanz.\n"
                        "0.0 = gleichmaessig (uniform) — ferne Cascades scharf, nahe groeber.\n"
                        "1.0 = logarithmisch — nahe Cascades sehr scharf (klein), ferne groeber.\n"
                        "Hoeher stellen, wenn Schatten direkt vor der Kamera noch unscharf sind.");
            }
        }
    }
}

static void DrawMeshSection(EditorFrameContext& ctx, EntityID entity)
{
    auto* meshComp = ctx.world.Get<MeshComponent>(entity);
    if (!meshComp)
        return;

    const bool meshOpen = ComponentHeader("Mesh", ComponentIcon::Mesh, ImGuiTreeNodeFlags_DefaultOpen);
    if (BeginComponentContextMenu("##mesh_hdr_ctx"))
    {
        DrawComponentClipboardMenuItems<MeshComponent>(ctx, entity, "Mesh");
        ImGui::EndPopup();
    }
    if (!meshOpen)
        return;

    const bool handleValid = meshComp->mesh.IsValid() &&
                             ctx.registry.meshes.Get(meshComp->mesh) != nullptr;

    if (!handleValid)
    {
        // Kein gueltiger Mesh-Handle — unterscheide zwei Faelle:
        if (!meshComp->meshAssetPath.empty())
        {
            // Pfad vorhanden aber Handle ungueltig → Scene wurde mit altem Code gespeichert
            // oder ResolveMeshAssetBindings ist noch nicht gelaufen.
            ImGui::PushStyleColor(ImGuiCol_Text, theme::kWarning);
            ImGui::TextWrapped("Mesh nicht gebunden!");
            ImGui::PopStyleColor();
            value_ui::TextDisabled("Pfad: %s", meshComp->meshAssetPath.c_str());
            value_ui::TextDisabled("Scene neu speichern oder Modell erneut aus dem");
            value_ui::TextDisabled("Asset Browser in die Scene ziehen.");
            if (ctx.syncSceneState && ImGui::Button("Erneut binden##mesh"))
                ctx.syncSceneState();
        }
        else
        {
            value_ui::TextDisabled("(Kein Mesh zugewiesen)");
        }
        return;
    }

    const auto* mesh = ctx.registry.meshes.Get(meshComp->mesh);
    value_ui::Text("Name: %s", mesh->debugName.c_str());
    value_ui::Text("Submeshes: %u", static_cast<uint32_t>(mesh->submeshes.size()));
    if (!meshComp->meshAssetPath.empty())
        value_ui::TextDisabled("Pfad: %s", meshComp->meshAssetPath.c_str());

}

static void DrawMaterialSlotsSection(EditorFrameContext& ctx, EntityID entity)
{
    auto* meshComp = ctx.world.Get<MeshComponent>(entity);
    if (!meshComp)
    {
        EntityID meshChild = NULL_ENTITY;
        ctx.world.ForEachAlive([&](EntityID id)
        {
            if (meshChild.IsValid() || !ctx.world.Get<MeshComponent>(id))
                return;

            EntityID cur = id;
            while (cur.IsValid())
            {
                if (cur == entity)
                {
                    meshChild = id;
                    return;
                }
                const auto* parent = ctx.world.Get<ParentComponent>(cur);
                cur = parent ? parent->parent : NULL_ENTITY;
            }
        });

        if (!meshChild.IsValid())
            return;

        entity = meshChild;
        meshComp = ctx.world.Get<MeshComponent>(entity);
        if (!meshComp)
            return;
    }

    assets::MeshAsset* mesh = ctx.registry.meshes.Get(meshComp->mesh);
    if (!mesh)
        return;

    const bool materialsOpen = ComponentHeader("Materials", ComponentIcon::Material, ImGuiTreeNodeFlags_DefaultOpen);
    if (BeginComponentContextMenu("##materials_hdr_ctx"))
    {
        if (ctx.world.Has<MaterialComponent>(entity))
            DrawComponentClipboardMenuItems<MaterialComponent>(ctx, entity, "Materialien");
        else if (ClipboardHolds<MaterialComponent>())
            DrawPasteComponentAsNewItem(ctx, entity);
        else
            value_ui::TextDisabled("Keine Material-Overrides zum Kopieren");
        ImGui::EndPopup();
    }
    if (!materialsOpen)
        return;

    const MaterialComponent* overrideMaterial = ctx.world.Get<MaterialComponent>(entity);
    const bool hasEntityOverride = overrideMaterial &&
        (overrideMaterial->material.IsValid() ||
         !overrideMaterial->materialAssetPath.empty() ||
         !overrideMaterial->baseColorTexturePath.empty());

    const uint32_t submeshCount = static_cast<uint32_t>(mesh->submeshes.size());
    if (submeshCount == 0u)
    {
        value_ui::TextDisabled("(No material slots)");
        return;
    }

    // Element 0 automatisch vorauswählen wenn diese Entity selektiert wird
    // und noch kein Slot für sie aktiv ist.
    if (ctx.state.inspectorMaterialEntity != entity)
    {
        const uint32_t materialIndex0 = mesh->submeshes[0].materialIndex;
        std::string autoPath;
        if (overrideMaterial)
        {
            if (const MaterialComponent::SlotOverride* s = overrideMaterial->FindSlotOverride(0u))
                autoPath = s->materialAssetPath;
            if (autoPath.empty() && !overrideMaterial->materialAssetPath.empty())
                autoPath = overrideMaterial->materialAssetPath;
        }
        if (autoPath.empty() && materialIndex0 < mesh->materialHandles.size())
        {
            const assets::MaterialAsset* ma = GetMaterialAsset(ctx, mesh->materialHandles[materialIndex0]);
            if (ma) autoPath = ma->path;
        }
        if (CanOpenMaterialAssetPath(autoPath))
            SelectInspectorMaterial(ctx, entity, 0u, autoPath);
    }

    const float rowH = ImGui::GetFrameHeightWithSpacing();
    ImGui::BeginChild("##material_slots",
                      ImVec2(0.f, std::min(160.f, rowH * submeshCount + 8.f)),
                      true);

    for (uint32_t si = 0u; si < submeshCount; ++si)
    {
        const uint32_t materialIndex = mesh->submeshes[si].materialIndex;
        MaterialHandle slotHandle = MaterialHandle::Invalid();
        if (materialIndex < mesh->materialHandles.size())
            slotHandle = mesh->materialHandles[materialIndex];

        const MaterialComponent::SlotOverride* slotOverride =
            overrideMaterial ? overrideMaterial->FindSlotOverride(si) : nullptr;
        const bool hasSlotOverride = slotOverride &&
            (slotOverride->material.IsValid() || !slotOverride->materialAssetPath.empty());

        const MaterialHandle visibleHandle = hasSlotOverride
            ? slotOverride->material
            : (hasEntityOverride ? overrideMaterial->material : slotHandle);
        const assets::MaterialAsset* visibleMaterial = GetMaterialAsset(ctx, visibleHandle);
        std::string visibleName = MaterialDisplayName(visibleMaterial, visibleHandle);
        std::string visiblePath = (visibleMaterial && !visibleMaterial->path.empty())
            ? visibleMaterial->path
            : std::string{};

        if (hasSlotOverride && !slotOverride->materialAssetPath.empty())
        {
            visiblePath = slotOverride->materialAssetPath;
            visibleName = PathToUtf8(PathFromUtf8(visiblePath).filename());
        }
        else if (hasEntityOverride && !overrideMaterial->materialAssetPath.empty())
        {
            visiblePath = overrideMaterial->materialAssetPath;
            visibleName = PathToUtf8(PathFromUtf8(visiblePath).filename());
        }
        else if (hasEntityOverride && !overrideMaterial->baseColorTexturePath.empty())
        {
            visiblePath = overrideMaterial->baseColorTexturePath;
            visibleName = PathToUtf8(PathFromUtf8(visiblePath).filename());
        }

        const float buttonW = 54.f;
        const float rowW = std::max(1.f, ImGui::GetContentRegionAvail().x - buttonW - 8.f);
        std::string rowLabel = "Element " + std::to_string(si) + "  [" +
            (hasSlotOverride ? "Slot" : (hasEntityOverride ? "Override" : "Mesh")) +
            "]  " + visibleName;

        ImGui::PushID(static_cast<int>(si));
        const bool selectable = CanOpenMaterialAssetPath(visiblePath);
        const bool selected = selectable && IsInspectorMaterialSelection(ctx, entity, si, visiblePath);
        if (!selectable)
            ImGui::BeginDisabled();
        if (ImGui::Selectable(rowLabel.c_str(), selected, ImGuiSelectableFlags_AllowOverlap, ImVec2(rowW, 0.f)))
            SelectInspectorMaterial(ctx, entity, si, visiblePath);
        if (!selectable)
            ImGui::EndDisabled();

        if (ImGui::IsItemHovered() && !visiblePath.empty())
            ImGui::SetItemTooltip("%s", visiblePath.c_str());
        if (ImGui::IsMouseReleased(ImGuiMouseButton_Right) &&
            ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled | ImGuiHoveredFlags_AllowWhenBlockedByPopup))
            ImGui::OpenPopup("##material_slot_ctx");
        if (ImGui::BeginPopupContextItem("##material_slot_ctx"))
        {
            if (ImGui::MenuItem("Wert kopieren")) value_ui::CopyText("asset-material", visiblePath);
            const bool canPasteMaterial = value_ui::Clipboard().type == "asset-material" &&
                CanOpenMaterialAssetPath(value_ui::Clipboard().text);
            if (ImGui::MenuItem("Wert einfuegen", nullptr, false, canPasteMaterial))
            {
                const std::string path = value_ui::Clipboard().text;
                ExecuteSceneMutation(ctx, "Materialwert eingefuegt", [&] {
                    SelectMaterialAsset(ctx, PathFromUtf8(path));
                    ApplyMaterialAssetToEntitySlot(ctx, entity, si, PathFromUtf8(path));
                });
            }
            ImGui::Separator();
            if (hasSlotOverride && ImGui::MenuItem("Remove Slot Override"))
            {
                ExecuteSceneMutation(ctx, "Material-Slot entfernt", [&]() {
                    if (MaterialComponent* material = ctx.world.Get<MaterialComponent>(entity))
                    {
                        material->slotOverrides.erase(
                            std::remove_if(material->slotOverrides.begin(),
                                           material->slotOverrides.end(),
                                           [&](const MaterialComponent::SlotOverride& slot) {
                                               return slot.submeshIndex == si;
                                           }),
                            material->slotOverrides.end());
                    }
                });
            }
            ImGui::EndPopup();
        }
        if (ImGui::BeginDragDropTarget())
        {
            if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("KROM_ASSET_MATERIAL"))
            {
                const char* matPath = static_cast<const char*>(payload->Data);
                if (matPath && matPath[0] != '\0')
                {
                    ExecuteSceneMutation(ctx, "Material-Slot zugewiesen", [&]() {
                        SelectMaterialAsset(ctx, PathFromUtf8(matPath));
                        ApplyMaterialAssetToEntitySlot(ctx, entity, si, PathFromUtf8(matPath));
                    });
                }
            }
            ImGui::EndDragDropTarget();
        }

        ImGui::SameLine();
        const bool canOpen = CanOpenMaterialAssetPath(visiblePath);
        if (!canOpen)
            ImGui::BeginDisabled();
        if (ImGui::SmallButton("Open"))
            OpenMaterialAsset(ctx, PathFromUtf8(visiblePath));
        if (!canOpen)
            ImGui::EndDisabled();

        if (!hasSlotOverride && !hasEntityOverride && materialIndex >= mesh->materialHandles.size())
        {
            ImGui::SameLine();
            value_ui::TextDisabled("(missing)");
        }

        ImGui::PopID();
    }

    ImGui::EndChild();

    if (hasEntityOverride && !overrideMaterial->materialAssetPath.empty())
        value_ui::TextDisabled("Entity override: %s", overrideMaterial->materialAssetPath.c_str());
    if (overrideMaterial && !overrideMaterial->slotOverrides.empty())
        value_ui::TextDisabled("Slot overrides: %u", static_cast<uint32_t>(overrideMaterial->slotOverrides.size()));
}

static void DrawMaterialSection(EditorFrameContext& ctx, EntityID entity)
{
    const bool hasMesh = ctx.world.Get<MeshComponent>(entity) != nullptr;

    MaterialComponent* materialComp = ctx.world.Get<MaterialComponent>(entity);
    const MaterialHandle matHandle = GetEntityMaterial(ctx, entity);
    const bool hasSlotSelection =
        ctx.state.inspectorMaterialEntity == entity &&
        ctx.state.inspectorMaterialSlot >= 0 &&
        !ctx.state.inspectorMaterialAssetPath.empty();
    // Fuer Entities OHNE Mesh den Material-Bereich nur anzeigen wenn ein Material
    // zugewiesen ist. Fuer Mesh-Entities immer anzeigen (Drag-Drop-Ziel muss sichtbar
    // bleiben, auch nachdem das Material entfernt wurde).
    if (!hasMesh && !hasSlotSelection && !matHandle.IsValid() &&
        (!materialComp || materialComp->materialAssetPath.empty()))
        return;

    const bool matOpen = ComponentHeader("Material", ComponentIcon::Material, ImGuiTreeNodeFlags_DefaultOpen);
    if (BeginComponentContextMenu("##mat_hdr_ctx"))
    {
        DrawComponentClipboardMenuItems<MaterialComponent>(ctx, entity, "Material");
        ImGui::Separator();
        if (materialComp && !materialComp->materialAssetPath.empty())
        {
            if (ImGui::MenuItem("In eigenem Fenster oeffnen"))
                OpenMaterialAsset(ctx, PathFromUtf8(materialComp->materialAssetPath));
            ImGui::Separator();
            if (ImGui::MenuItem("Material entfernen"))
            {
                ExecuteSceneMutation(ctx, "Material entfernt", [&]() {
                    if (auto* mc = ctx.world.Get<MaterialComponent>(entity))
                    {
                        mc->material = MaterialHandle{};
                        mc->materialAssetPath.clear();
                    }
                });
            }
        }
        ImGui::EndPopup();
    }
    if (!matOpen)
        return;

    // Materialpfad anzeigen — die gesamte Zeile ist Drag-Drop-Ziel
    if (hasSlotSelection)
    {
        value_ui::TextDisabled("Element %d: %s",
                            ctx.state.inspectorMaterialSlot,
                            ctx.state.inspectorMaterialAssetPath.c_str());

        if (PathToGenericUtf8(PathFromUtf8(ctx.state.inspectorMaterialAssetPath)) !=
            PathToGenericUtf8(PathFromUtf8(ctx.state.selectedMaterialAssetPath)))
        {
            ctx.state.inspectorMaterialEntity = NULL_ENTITY;
            ctx.state.inspectorMaterialSlot = -1;
            ctx.state.inspectorMaterialAssetPath.clear();
            return;
        }

        ImGui::Separator();
        DrawSelectedMaterialInspector(ctx, entity);
        return;
    }

    if (materialComp && !materialComp->materialAssetPath.empty())
    {
        value_ui::TextDisabled("%s", materialComp->materialAssetPath.c_str());
    }
    else
    {
        value_ui::TextDisabled("Material hierher ziehen");
    }

    // Drag-Drop auf den Materialpfad / Hinweistext
    if (ImGui::BeginDragDropTarget())
    {
        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("KROM_ASSET_MATERIAL"))
        {
            const char* matPath = static_cast<const char*>(payload->Data);
            if (matPath && matPath[0] != '\0')
            {
                ExecuteSceneMutation(ctx, "Material zugewiesen", [&]() {
                    SelectMaterialAsset(ctx, PathFromUtf8(matPath));
                    ApplyMaterialAssetToEntity(ctx, entity, PathFromUtf8(matPath));
                });
            }
        }
        ImGui::EndDragDropTarget();
    }

    // Inline-Material-Editor (nur wenn ein Asset-Material zugewiesen ist)
    if (materialComp && !materialComp->materialAssetPath.empty())
    {
        if (PathToGenericUtf8(PathFromUtf8(materialComp->materialAssetPath)) ==
            PathToGenericUtf8(PathFromUtf8(ctx.state.selectedMaterialAssetPath)))
        {
            ImGui::Separator();
            DrawSelectedMaterialInspector(ctx, entity);
        }
    }
}

static bool HasPhysicsCollider(const ecs::World& world, EntityID entity)
{
    const auto* obb = world.Get<OBBComponent>(entity);
    return (obb && !obb->isTrigger) ||
           world.Has<SphereColliderComponent>(entity) ||
           world.Has<CapsuleColliderComponent>(entity);
}

static void CountChildColliders(const ecs::World& world, EntityID entity, uint32_t& count, uint32_t depth = 0u)
{
    const auto* children = world.Get<ChildrenComponent>(entity);
    if (!children || depth > 1024u)
        return;
    for (const EntityID child : children->children)
    {
        if (!world.IsAlive(child))
            continue;
        if (HasPhysicsCollider(world, child))
            ++count;
        CountChildColliders(world, child, count, depth + 1u);
    }
}

static void DrawRigidBodySection(EditorFrameContext& ctx, EntityID entity)
{
    // Hinzufügen läuft über "+ Komponente" → "Rigidbody".
    auto* rb = ctx.world.Get<RigidBodyComponent>(entity);
    if (!rb)
        return;

    const bool open = ComponentHeader("Rigidbody##rb_hdr", ComponentIcon::Physics);
    if (BeginComponentContextMenu("##rb_hdr_ctx"))
    {
        DrawComponentClipboardMenuItems<RigidBodyComponent>(ctx, entity, "Rigidbody");
        ImGui::Separator();
        if (ImGui::MenuItem("Rigidbody entfernen"))
        {
            ExecuteSceneMutation(ctx, "Komponente entfernt", [&]() { ctx.world.Remove<RigidBodyComponent>(entity); });
            ImGui::EndPopup();
            return;
        }
        ImGui::EndPopup();
    }
    if (!open)
        return;
    ComponentUndoScope<RigidBodyComponent> undo(ctx, entity, "rigidbody", "Rigidbody geaendert");

    value_ui::Checkbox("Aktiv##rb_enabled", &rb->enabled);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Aus: Objekt und seine Kollisionen nehmen nicht an der Physik teil.\n"
                          "Einstellungen bleiben erhalten.");

    static const char* kTypeNames[] = { "Statisch", "Dynamisch", "Kinematisch" };
    int type = static_cast<int>(rb->type);
    if (value_ui::Combo("Typ##rb_type", &type, kTypeNames, IM_ARRAYSIZE(kTypeNames)))
        rb->type = static_cast<RigidBodyType>(type);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Statisch: bewegt sich nie, blockiert nur.\n"
                          "Dynamisch: Schwerkraft, Kraefte, Kollisionen.\n"
                          "Kinematisch: folgt der Transform (Script/Animation)\n"
                          "und schiebt dynamische Objekte weg.");

    // Kollisionen dieses Koerpers (eigene + Kinder) und Hinweise.
    uint32_t childColliders = 0u;
    CountChildColliders(ctx.world, entity, childColliders);
    const bool ownCollider = HasPhysicsCollider(ctx.world, entity);
    const uint32_t total = childColliders + (ownCollider ? 1u : 0u);
    value_ui::TextDisabled("Kollisionen: %u (%u eigene, %u von Kindern)",
                        total, ownCollider ? 1u : 0u, childColliders);
    const ImVec4 warnColor(1.f, 0.7f, 0.2f, 1.f);
    if (total == 0u)
        ImGui::TextColored(warnColor, "Keine Kollision vorhanden - wird nicht simuliert.");
    if (const auto* obb = ctx.world.Get<OBBComponent>(entity); obb && obb->isTrigger)
        ImGui::TextColored(warnColor, "Trigger-Box wird nicht simuliert.");
    const EntityID ancestor = FindRigidBodyAncestor(ctx.world, entity);
    if (ancestor.IsValid())
    {
        ImGui::TextColored(warnColor, "Elternobjekt '%s' hat einen Rigidbody -\n"
                                      "dieser hier wird ignoriert.", EntityLabel(ctx.world, ancestor).c_str());
    }
    else if (ctx.world.Has<ParentComponent>(entity) && rb->type == RigidBodyType::Dynamic)
    {
        value_ui::TextDisabled("Hat ein Elternobjekt: Position wird zur Laufzeit\n"
                            "relativ zum Parent umgerechnet.");
    }
    if (ImGui::SmallButton("+ Kollisions-Kind (Box)##rb_child_box"))
    {
        CreateColliderChildEntity(ctx, entity, ColliderChildShape::Box);
        return;   // Selektion wechselt auf das neue Kind
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("+ Kollisions-Kind (Kugel)##rb_child_sphere"))
    {
        CreateColliderChildEntity(ctx, entity, ColliderChildShape::Sphere);
        return;
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Weitere Formen als Kind-Objekte - alle zusammen\n"
                          "bilden einen Koerper (z.B. L-Form).");
    ImGui::Separator();

    value_ui::DragFloat("Reibung##rb_friction", &rb->friction, 0.01f, 0.f, 10.f, "%.2f");
    value_ui::SliderFloat("Elastizitaet##rb_restitution", &rb->restitution, 0.f, 1.f, "%.2f");
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("0 = kein Abprallen, 1 = voll elastisch.");

    if (rb->type != RigidBodyType::Dynamic)
        return;

    value_ui::DragFloat("Masse (kg)##rb_mass", &rb->mass, 0.05f, 0.001f, 100000.f, "%.3f");
    rb->mass = std::max(rb->mass, 0.001f);
    value_ui::DragFloat("Schwerkraft-Faktor##rb_gravity", &rb->gravityScale, 0.01f, -10.f, 10.f, "%.2f");
    value_ui::DragFloat("Daempfung linear##rb_lin", &rb->linearDamping, 0.01f, 0.f, 10.f, "%.2f");
    value_ui::DragFloat("Daempfung Drehung##rb_ang", &rb->angularDamping, 0.01f, 0.f, 10.f, "%.2f");
    value_ui::TextDisabled("Rotation sperren");
    value_ui::Checkbox("X##rb_lockx", &rb->lockRotationX); ImGui::SameLine();
    value_ui::Checkbox("Y##rb_locky", &rb->lockRotationY); ImGui::SameLine();
    value_ui::Checkbox("Z##rb_lockz", &rb->lockRotationZ);
}

// Physik-Zuordnung eines Colliders (siehe DrawRigidBodySection).
static void DrawColliderOwnerInfo(EditorFrameContext& ctx, EntityID entity, const char* idSuffix)
{
    const EntityID owner = FindRigidBodyAncestor(ctx.world, entity);
    if (owner.IsValid())
    {
        value_ui::TextDisabled("Gehoert zu Rigidbody von '%s'", EntityLabel(ctx.world, owner).c_str());
        const std::string label = std::string("Zum Rigidbody springen##goto_rb_") + idSuffix;
        if (ImGui::SmallButton(label.c_str()))
            ctx.state.selectedEntity = owner;
    }
    else if (!ctx.world.Has<RigidBodyComponent>(entity))
    {
        value_ui::TextDisabled("Ohne Rigidbody = statisches Hindernis (Wand/Boden)");
    }
}

static void DrawSphereColliderSection(EditorFrameContext& ctx, EntityID entity)
{
    auto* sphere = ctx.world.Get<SphereColliderComponent>(entity);
    if (!sphere)
        return;

    const bool open = ComponentHeader("Kollision (Kugel)##sphere_hdr", ComponentIcon::Collider);
    if (BeginComponentContextMenu("##sphere_hdr_ctx"))
    {
        DrawComponentClipboardMenuItems<SphereColliderComponent>(ctx, entity, "Kollision (Kugel)");
        ImGui::Separator();
        if (ImGui::MenuItem("Kugel-Kollision entfernen"))
        {
            ExecuteSceneMutation(ctx, "Komponente entfernt", [&]() { ctx.world.Remove<SphereColliderComponent>(entity); });
            ImGui::EndPopup();
            return;
        }
        ImGui::EndPopup();
    }
    if (!open)
        return;
    ComponentUndoScope<SphereColliderComponent> undo(ctx, entity, "sphere", "Kugel-Kollision geaendert");

    value_ui::Checkbox("Sichtbar (Kugel anzeigen)##sphere_vis", &sphere->showInEditor);
    DrawColliderOwnerInfo(ctx, entity, "sphere");
    ImGui::Separator();

    value_ui::DragFloat("Radius##sphere_radius", &sphere->radius, 0.01f, 0.001f, 10000.f, "%.3f");
    sphere->radius = std::max(sphere->radius, 0.001f);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Bei ungleichmaessiger Skalierung zaehlt die groesste Achse.");
    value_ui::TextDisabled("Mittelpunkt-Offset");
    value_ui::DragFloat3("##sphere_center", &sphere->centerOffset.x, 0.01f, -10000.f, 10000.f, "%.3f");

    ImGui::Separator();
    if (ImGui::Button("Aus Mesh berechnen##sphere_auto"))
    {
        if (const auto* b = ctx.world.Get<BoundsComponent>(entity))
        {
            sphere->centerOffset = b->centerLocal;
            sphere->radius = std::max(0.001f, std::max({b->extentsLocal.x, b->extentsLocal.y, b->extentsLocal.z}));
        }
    }
}

static void DrawCharacterControllerSection(EditorFrameContext& ctx, EntityID entity)
{
    auto* cc = ctx.world.Get<CharacterControllerComponent>(entity);
    if (!cc)
        return;

    const bool open = ComponentHeader("Spielfigur (Character Controller)##cc_hdr", ComponentIcon::Character);
    if (BeginComponentContextMenu("##cc_hdr_ctx"))
    {
        DrawComponentClipboardMenuItems<CharacterControllerComponent>(ctx, entity, "Spielfigur");
        ImGui::Separator();
        if (ImGui::MenuItem("Spielfigur entfernen"))
        {
            ExecuteSceneMutation(ctx, "Komponente entfernt", [&]() { ctx.world.Remove<CharacterControllerComponent>(entity); });
            ImGui::EndPopup();
            return;
        }
        ImGui::EndPopup();
    }
    if (!open)
        return;
    ComponentUndoScope<CharacterControllerComponent> undo(ctx, entity, "character", "Spielfigur geaendert");

    value_ui::Checkbox("Aktiv##cc_enabled", &cc->enabled);
    ImGui::SameLine();
    value_ui::Checkbox("Sichtbar##cc_vis", &cc->showInEditor);
    value_ui::TextDisabled("Ursprung = Fuesse. Steuerung per Script:\nSetMoveVelocity, Jump, IsGrounded.");
    const ImVec4 warnColor(1.f, 0.7f, 0.2f, 1.f);
    if (ctx.world.Has<RigidBodyComponent>(entity))
        ImGui::TextColored(warnColor, "Hat zusaetzlich einen Rigidbody -\nbeides zusammen ist nicht sinnvoll.");
    ImGui::Separator();

    value_ui::DragFloat("Radius##cc_radius", &cc->radius, 0.01f, 0.05f, 10.f, "%.2f");
    value_ui::DragFloat("Hoehe##cc_height", &cc->height, 0.01f, 0.1f, 20.f, "%.2f");
    cc->radius = std::max(cc->radius, 0.05f);
    cc->height = std::max(cc->height, cc->radius * 2.f);
    value_ui::DragFloat("Max. Steigung (Grad)##cc_slope", &cc->maxSlopeDeg, 0.5f, 0.f, 89.f, "%.0f");
    value_ui::DragFloat("Stufenhoehe##cc_step", &cc->stepHeight, 0.01f, 0.f, 2.f, "%.2f");
    value_ui::DragFloat("Masse (kg)##cc_mass", &cc->mass, 0.5f, 1.f, 10000.f, "%.0f");
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Bestimmt, wie stark die Figur dynamische Objekte wegschiebt.");
    value_ui::DragFloat("Schwerkraft-Faktor##cc_gravity", &cc->gravityScale, 0.01f, -10.f, 10.f, "%.2f");
}

static void DrawAudioSourceSection(EditorFrameContext& ctx, EntityID entity)
{
    auto* source = ctx.world.Get<AudioSourceComponent>(entity);
    if (!source)
        return;

    const bool open = ComponentHeader("Audio-Quelle##audio_src_hdr", ComponentIcon::Audio);
    if (BeginComponentContextMenu("##audio_src_hdr_ctx"))
    {
        DrawComponentClipboardMenuItems<AudioSourceComponent>(ctx, entity, "Audio-Quelle");
        ImGui::Separator();
        if (ImGui::MenuItem("Audio-Quelle entfernen"))
        {
            ExecuteSceneMutation(ctx, "Komponente entfernt", [&]() { ctx.world.Remove<AudioSourceComponent>(entity); });
            ImGui::EndPopup();
            return;
        }
        ImGui::EndPopup();
    }
    if (!open)
        return;
    ComponentUndoScope<AudioSourceComponent> undo(ctx, entity, "audio_source", "Audio-Quelle geaendert");

    // Datei: Textfeld + Drop-Ziel fuer Audiodateien aus dem Asset-Browser.
    char buffer[512];
    std::snprintf(buffer, sizeof(buffer), "%s", source->clipPath.c_str());
    value_ui::TextDisabled("Datei (wav, mp3, ogg, flac)");
    ImGui::SetNextItemWidth(-1.f);
    if (value_ui::InputText("##audio_clip", buffer, sizeof(buffer)))
        source->clipPath = buffer;
    if (ImGui::BeginDragDropTarget())
    {
        if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload("KROM_ASSET_AUDIO"))
        {
            if (const char* path = static_cast<const char*>(p->Data); path && path[0] != '\0')
                source->clipPath = PathToGenericUtf8(PathFromUtf8(path).lexically_normal());
        }
        ImGui::EndDragDropTarget();
    }
    if (source->clipPath.empty())
        ImGui::TextColored(ImVec4(1.f, 0.7f, 0.2f, 1.f), "Keine Datei - Audiodatei aus dem Asset-Browser hierher ziehen.");
    ImGui::Separator();

    value_ui::SliderFloat("Lautstaerke##audio_vol", &source->volume, 0.f, 2.f, "%.2f");
    value_ui::DragFloat("Tonhoehe##audio_pitch", &source->pitch, 0.01f, 0.1f, 4.f, "%.2f");
    value_ui::Checkbox("Beim Start abspielen##audio_play", &source->playOnStart);
    ImGui::SameLine();
    value_ui::Checkbox("Schleife##audio_loop", &source->loop);
    value_ui::Checkbox("Raeumlich (3D)##audio_spatial", &source->spatial);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Aus: 2D, ueberall gleich laut (Musik, UI).\n"
                          "An: leiser mit Abstand zum Audio-Listener.");
    if (source->spatial)
    {
        value_ui::DragFloat("Min. Abstand##audio_min", &source->minDistance, 0.1f, 0.01f, 1000.f, "%.1f");
        value_ui::DragFloat("Max. Abstand##audio_max", &source->maxDistance, 0.1f, 0.01f, 10000.f, "%.1f");
        source->maxDistance = std::max(source->maxDistance, source->minDistance);
    }
    value_ui::TextDisabled("Wiedergabe nur im Play-Modus.");
}

static void DrawAudioListenerSection(EditorFrameContext& ctx, EntityID entity)
{
    auto* listener = ctx.world.Get<AudioListenerComponent>(entity);
    if (!listener)
        return;

    const bool open = ComponentHeader("Audio-Listener##audio_lst_hdr", ComponentIcon::Audio);
    if (BeginComponentContextMenu("##audio_lst_hdr_ctx"))
    {
        DrawComponentClipboardMenuItems<AudioListenerComponent>(ctx, entity, "Audio-Listener");
        ImGui::Separator();
        if (ImGui::MenuItem("Audio-Listener entfernen"))
        {
            ExecuteSceneMutation(ctx, "Komponente entfernt", [&]() { ctx.world.Remove<AudioListenerComponent>(entity); });
            ImGui::EndPopup();
            return;
        }
        ImGui::EndPopup();
    }
    if (!open)
        return;
    ComponentUndoScope<AudioListenerComponent> undo(ctx, entity, "audio_listener", "Audio-Listener geaendert");
    value_ui::SliderFloat("Gesamtlautstaerke##audio_lst_vol", &listener->volume, 0.f, 2.f, "%.2f");
    value_ui::TextDisabled("Das \"Ohr\" der Szene, meist an der Kamera.\nNur ein Listener pro Szene.");
}

static void DrawParticleEmitterSection(EditorFrameContext& ctx, EntityID entity)
{
    auto* emitter = ctx.world.Get<ParticleEmitterComponent>(entity);
    if (!emitter)
        return;

    const bool open = ComponentHeader("Partikel-Emitter##particles_hdr", ComponentIcon::Particles);
    if (BeginComponentContextMenu("##particles_hdr_ctx"))
    {
        DrawComponentClipboardMenuItems<ParticleEmitterComponent>(ctx, entity, "Partikel-Emitter");
        ImGui::Separator();
        if (ImGui::MenuItem("Partikel-Emitter entfernen"))
        {
            ExecuteSceneMutation(ctx, "Komponente entfernt", [&]() { ctx.world.Remove<ParticleEmitterComponent>(entity); });
            ImGui::EndPopup();
            return;
        }
        ImGui::EndPopup();
    }
    if (!open)
        return;
    ComponentUndoScope<ParticleEmitterComponent> undo(ctx, entity, "particle_emitter", "Partikel-Emitter geaendert");

    // Wiedergabe
    value_ui::Checkbox("Aktiv##pe_playing", &emitter->playing);
    ImGui::SameLine();
    value_ui::Checkbox("Schleife##pe_loop", &emitter->looping);
    ImGui::SameLine();
    if (ImGui::SmallButton("Neu starten##pe_restart") && ctx.restartParticleEmitter)
        ctx.restartParticleEmitter(entity);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Vorschau von vorn. Bei \"Aktiv\" aus (Vorlage fuer Scripts)\n"
                          "wird einmal der Burst gezeigt.");
    value_ui::DragFloat("Dauer (s)##pe_duration", &emitter->duration, 0.05f, 0.f, 600.f, "%.2f");
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip(emitter->looping
            ? "Laenge eines Durchgangs: danach beginnt er von vorn, der Burst kommt erneut."
            : "So lange wird mit der Rate emittiert; danach endet der Emitter.");

    // Emission
    ImGui::SeparatorText("Emission");
    value_ui::DragFloat("Rate (pro s)##pe_rate", &emitter->emitRate, 0.5f, 0.f, 10000.f, "%.1f");
    int burst = static_cast<int>(emitter->burstCount);
    if (value_ui::DragInt("Burst beim Start##pe_burst", &burst, 1.f, 0, 100000))
        emitter->burstCount = static_cast<uint32_t>(std::max(burst, 0));
    int maxParticles = static_cast<int>(emitter->maxParticles);
    if (value_ui::DragInt("Max. Partikel##pe_max", &maxParticles, 10.f, 1, 100000))
        emitter->maxParticles = static_cast<uint32_t>(std::max(maxParticles, 1));
    value_ui::DragFloat("Lebensdauer (s)##pe_life", &emitter->particleLifetime, 0.01f, 0.01f, 60.f, "%.2f");
    value_ui::SliderFloat("Lebensdauer-Zufall##pe_liferand", &emitter->lifetimeRandom, 0.f, 1.f, "%.2f");
    value_ui::DragFloat("Radius##pe_radius", &emitter->emitterRadius, 0.01f, 0.f, 100.f, "%.2f");
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Partikel entstehen zufaellig in einer Kugel um den Ursprung.");

    // Bewegung
    ImGui::SeparatorText("Bewegung");
    value_ui::DragFloat3("Startgeschwindigkeit##pe_vel", &emitter->initialVelocity.x, 0.05f, -1000.f, 1000.f, "%.2f");
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("In Ausrichtung des Objekts (dreht mit).");
    value_ui::DragFloat3("Geschw.-Zufall (+/-)##pe_velrand", &emitter->velocityRandom.x, 0.05f, 0.f, 1000.f, "%.2f");
    value_ui::DragFloat3("Schwerkraft##pe_gravity", &emitter->gravity.x, 0.05f, -1000.f, 1000.f, "%.2f");
    value_ui::DragFloat("Luftwiderstand##pe_drag", &emitter->drag, 0.01f, 0.f, 50.f, "%.2f");

    // Boden
    ImGui::SeparatorText("Boden");
    value_ui::Checkbox("Bodenkollision##pe_floor", &emitter->collideWithFloor);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Partikel prallen an einer waagrechten Ebene ab (im Viewport als oranges Gitter).");
    if (emitter->collideWithFloor)
    {
        value_ui::Checkbox("Relativ zum Emitter##pe_floor_rel", &emitter->floorRelative);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("An: Hoehe als Abstand zum Emitter (wandert mit). Aus: feste Welt-Hoehe (Y).");
        value_ui::DragFloat(emitter->floorRelative ? "Hoehe (relativ)##pe_floor_h" : "Hoehe (Welt-Y)##pe_floor_h",
                         &emitter->floorHeight, 0.01f, -1000.f, 1000.f, "%.2f m");
        value_ui::Checkbox("Beim Aufprall verschwinden##pe_floor_die", &emitter->dieOnFloor);
        if (!emitter->dieOnFloor)
        {
            value_ui::SliderFloat("Abprallen##pe_bounce", &emitter->bounce, 0.f, 1.f, "%.2f");
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("0 = bleibt liegen, 1 = springt voll zurueck.");
            value_ui::SliderFloat("Reibung##pe_floor_friction", &emitter->floorFriction, 0.f, 1.f, "%.2f");
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Bremst die seitliche Bewegung bei jedem Aufprall.");
        }
    }

    // Aussehen
    ImGui::SeparatorText("Aussehen");
    int blend = emitter->blendMode == ParticleBlendMode::Additive ? 1 : 0;
    if (value_ui::Combo("Mischung##pe_blend", &blend, "Alpha (Rauch, Staub)\0Additiv (Feuer, Funken)\0"))
        emitter->blendMode = blend == 1 ? ParticleBlendMode::Additive : ParticleBlendMode::Alpha;
    value_ui::DragFloat("Startgroesse##pe_size0", &emitter->startSize, 0.005f, 0.f, 100.f, "%.3f");
    value_ui::DragFloat("Endgroesse##pe_size1", &emitter->endSize, 0.005f, 0.f, 100.f, "%.3f");
    value_ui::ColorEdit4("Startfarbe##pe_col0", &emitter->startColor.x);
    value_ui::ColorEdit4("Endfarbe##pe_col1", &emitter->endColor.x);
    value_ui::DragFloat("Intensitaet##pe_intensity", &emitter->intensity, 0.05f, 0.f, 100.f, "%.2f");
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Werte ueber 1 leuchten (Bloom).");

    // Textur: Textfeld + Drop-Ziel fuer Bilder aus dem Asset-Browser.
    ImGui::SeparatorText("Textur");
    char textureBuffer[512];
    std::snprintf(textureBuffer, sizeof(textureBuffer), "%s", emitter->texturePath.c_str());
    ImGui::SetNextItemWidth(-1.f);
    if (value_ui::InputText("##pe_texture", textureBuffer, sizeof(textureBuffer)))
        emitter->texturePath = textureBuffer;
    if (ImGui::BeginDragDropTarget())
    {
        if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload("KROM_ASSET_TEXTURE"))
        {
            if (const char* path = static_cast<const char*>(p->Data); path && path[0] != '\0')
            {
                const std::filesystem::path dropped = PathFromUtf8(path).lexically_normal();
                emitter->texturePath = ctx.assetPipeline ? ctx.assetPipeline->ToAssetRelative(dropped)
                                                         : PathToGenericUtf8(dropped);
            }
        }
        ImGui::EndDragDropTarget();
    }
    if (emitter->texturePath.empty())
        value_ui::TextDisabled("Keine - weiche Scheibe. Bild aus dem Asset-Browser hierher ziehen.");
    else if (ImGui::SmallButton("Textur entfernen##pe_tex_clear"))
        emitter->texturePath.clear();

    int columns = static_cast<int>(emitter->flipbookColumns);
    int rows = static_cast<int>(emitter->flipbookRows);
    ImGui::SetNextItemWidth(ImGui::CalcItemWidth() * 0.5f);
    if (value_ui::DragInt("##pe_fb_cols", &columns, 0.1f, 1, 64))
        emitter->flipbookColumns = static_cast<uint32_t>(std::clamp(columns, 1, 64));
    ImGui::SameLine();
    ImGui::SetNextItemWidth(ImGui::CalcItemWidth() * 0.5f);
    if (value_ui::DragInt("Flipbook (Spalten x Zeilen)##pe_fb_rows", &rows, 0.1f, 1, 64))
        emitter->flipbookRows = static_cast<uint32_t>(std::clamp(rows, 1, 64));
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Bild aus mehreren Einzelbildern (Zeile fuer Zeile).\n1 x 1 = ganzes Bild.");
    if (emitter->flipbookColumns * emitter->flipbookRows > 1u)
    {
        value_ui::Checkbox("Zufaelliges Einzelbild##pe_fb_random", &emitter->flipbookRandomFrame);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("An: jedes Partikel zeigt ein festes, zufaelliges Bild (z. B. Rauchwolken).\n"
                              "Aus: die Bilder laufen ueber die Lebensdauer ab (z. B. Explosion).");
    }
}

static void DrawCapsuleColliderSection(EditorFrameContext& ctx, EntityID entity)
{
    auto* capsule = ctx.world.Get<CapsuleColliderComponent>(entity);
    if (!capsule)
        return;

    const bool open = ComponentHeader("Kollision (Kapsel)##capsule_hdr", ComponentIcon::Collider);
    if (BeginComponentContextMenu("##capsule_hdr_ctx"))
    {
        DrawComponentClipboardMenuItems<CapsuleColliderComponent>(ctx, entity, "Kollision (Kapsel)");
        ImGui::Separator();
        if (ImGui::MenuItem("Kapsel-Kollision entfernen"))
        {
            ExecuteSceneMutation(ctx, "Komponente entfernt", [&]() { ctx.world.Remove<CapsuleColliderComponent>(entity); });
            ImGui::EndPopup();
            return;
        }
        ImGui::EndPopup();
    }
    if (!open)
        return;
    ComponentUndoScope<CapsuleColliderComponent> undo(ctx, entity, "capsule", "Kapsel-Kollision geaendert");

    value_ui::Checkbox("Sichtbar (Kapsel anzeigen)##capsule_vis", &capsule->showInEditor);
    DrawColliderOwnerInfo(ctx, entity, "capsule");
    ImGui::Separator();

    static const char* kAxisNames[] = { "X", "Y", "Z" };
    int axis = static_cast<int>(capsule->axis);
    if (value_ui::Combo("Achse##capsule_axis", &axis, kAxisNames, IM_ARRAYSIZE(kAxisNames)))
        capsule->axis = static_cast<CapsuleAxis>(axis);
    value_ui::DragFloat("Radius##capsule_radius", &capsule->radius, 0.01f, 0.001f, 10000.f, "%.3f");
    capsule->radius = std::max(capsule->radius, 0.001f);
    value_ui::DragFloat("Hoehe##capsule_height", &capsule->height, 0.01f, 0.002f, 10000.f, "%.3f");
    capsule->height = std::max(capsule->height, 0.002f);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Gesamthoehe inklusive beider Halbkugeln.\n"
                          "Kleiner als 2x Radius = Kugel.");
    value_ui::TextDisabled("Mittelpunkt-Offset");
    value_ui::DragFloat3("##capsule_center", &capsule->centerOffset.x, 0.01f, -10000.f, 10000.f, "%.3f");
}

static void DrawOBBSection(EditorFrameContext& ctx, EntityID entity)
{
    // Sektion nur anzeigen wenn eine OBB-Komponente vorhanden ist.
    // Hinzufügen läuft über Rechtsklick im Inspector → "+ Komponente".
    auto* obb = ctx.world.Get<OBBComponent>(entity);
    if (!obb)
        return;

    const bool obbOpen = ComponentHeader("Kollision##obb_hdr", ComponentIcon::Collider);
    if (BeginComponentContextMenu("##obb_hdr_ctx"))
    {
        DrawComponentClipboardMenuItems<OBBComponent>(ctx, entity, "Kollision (Box)");
        ImGui::Separator();
        if (ImGui::MenuItem("Kollision entfernen"))
        {
            ExecuteSceneMutation(ctx, "Komponente entfernt", [&]() { ctx.world.Remove<OBBComponent>(entity); });
            ctx.state.obbDragFaceIdx = -1;
        }
        ImGui::EndPopup();
    }
    if (!obbOpen)
        return;
    ComponentUndoScope<OBBComponent, ObbEqual> undo(ctx, entity, "obb", "Kollision geaendert");

    value_ui::Checkbox("Sichtbar (OBB anzeigen)##obb_vis", &obb->showInEditor);
    value_ui::Checkbox("Trigger##obb_trigger", &obb->isTrigger);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Trigger blockieren nichts und tauchen nicht in Raycasts auf.\n"
                          "Scripts bekommen OnTriggerEnter/OnTriggerExit,\n"
                          "wenn ein anderes Objekt die Box betritt oder verlaesst.");

    if (!obb->isTrigger)
        DrawColliderOwnerInfo(ctx, entity, "obb");
    ImGui::Separator();

    // Vollgroesse anzeigen, intern als halfExtents speichern
    math::Vec3 size = obb->halfExtents * 2.f;
    value_ui::TextDisabled("Groesse  X / Y / Z");
    if (value_ui::DragFloat3("##obb_size", &size.x, 0.01f, 0.001f, 10000.f, "%.3f"))
    {
        obb->halfExtents.x = std::max(0.001f, size.x * 0.5f);
        obb->halfExtents.y = std::max(0.001f, size.y * 0.5f);
        obb->halfExtents.z = std::max(0.001f, size.z * 0.5f);
    }

    value_ui::TextDisabled("Mittelpunkt-Offset");
    value_ui::DragFloat3("##obb_center", &obb->centerOffset.x, 0.01f, -10000.f, 10000.f, "%.3f");

    ImGui::Separator();
    if (ImGui::Button("Aus Mesh berechnen##obb_auto"))
    {
        if (const auto* b = ctx.world.Get<BoundsComponent>(entity))
        {
            obb->centerOffset = b->centerLocal;
            obb->halfExtents  = b->extentsLocal;
            obb->orientation  = math::Quat::Identity();
        }
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Berechnet OBB aus der Mesh-AABB.\nKollisionsdaten bleiben erhalten.");
}

struct InspectorDragResult { bool changed; bool labelClicked; };
static InspectorDragResult InspectorDragFloat3(const char* label, const char* id,
                                      float* v, float speed,
                                      const ImVec4* labelColor = nullptr,
                                      float vmin = 0.f, float vmax = 0.f)
{
    constexpr float kLabelWidth = 72.f;
    if (labelColor) ImGui::PushStyleColor(ImGuiCol_Text, *labelColor);
    value_ui::Text("%s", label);
    if (labelColor) ImGui::PopStyleColor();
    const bool labelClicked = ImGui::IsItemClicked();
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Klicken um Gizmo-Modus zu wechseln");
    ImGui::SameLine(kLabelWidth);
    ImGui::SetNextItemWidth(-1.f);
    const ImGuiSliderFlags clampFlag = (vmin < vmax) ? ImGuiSliderFlags_AlwaysClamp : ImGuiSliderFlags_None;
    return InspectorDragResult{ value_ui::DragFloat3(id, v, speed, vmin, vmax, "%.3f", clampFlag), labelClicked };
}

static bool MultiSelectionContains(EditorState& state, EntityID entity)
{
    return std::find(state.multiSelection.begin(), state.multiSelection.end(), entity) !=
           state.multiSelection.end();
}

static bool HasSelectedAncestor(EditorFrameContext& ctx, EntityID entity)
{
    const auto* parent = ctx.world.Get<ParentComponent>(entity);
    uint32_t depth = 0u;
    while (parent && parent->parent.IsValid() && ctx.world.IsAlive(parent->parent) && depth++ < 1024u)
    {
        if (MultiSelectionContains(ctx.state, parent->parent))
            return true;
        parent = ctx.world.Get<ParentComponent>(parent->parent);
    }
    return false;
}

static bool IsMultiTransformTarget(EditorFrameContext& ctx, EntityID entity)
{
    return entity.IsValid() && ctx.world.IsAlive(entity) && !HasSelectedAncestor(ctx, entity);
}

static bool IsHdrEnvironmentTexturePath(const std::filesystem::path& path)
{
    std::string ext = PathToUtf8(path.extension());
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return ext == ".hdr";
}

static void DrawSkyboxDropZone(EditorFrameContext& ctx)
{
    const std::string preview = ctx.state.environmentTexturePath.empty()
        ? std::string("<keine HDR-Textur>")
        : ctx.state.environmentTexturePath;

    char pathBuffer[512]{};
    std::snprintf(pathBuffer, sizeof(pathBuffer), "%s", preview.c_str());
    ImGui::SetNextItemWidth(-1.f);
    value_ui::InputText("##EnvironmentTexturePath", pathBuffer, sizeof(pathBuffer),
                     ImGuiInputTextFlags_ReadOnly);

    const ImVec2 dropMin = ImGui::GetCursorScreenPos();
    const float dropWidth = std::max(1.f, ImGui::GetContentRegionAvail().x);
    const ImVec2 dropSize(dropWidth, 44.f);
    ImGui::InvisibleButton("##EnvironmentHdrDropZone", dropSize);
    const ImVec2 dropMax(dropMin.x + dropSize.x, dropMin.y + dropSize.y);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const bool hovered = ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem);
    dl->AddRectFilled(dropMin, dropMax, hovered ? IM_COL32(70,92,112,255) : IM_COL32(48,56,64,255), 4.f);
    dl->AddRect      (dropMin, dropMax, IM_COL32(130,154,176,255), 4.f, 0, 1.5f);
    const char* dropText = "HDR-Textur hier ablegen";
    const ImVec2 ts = ImGui::CalcTextSize(dropText);
    dl->AddText(ImVec2(dropMin.x + (dropSize.x - ts.x) * 0.5f,
                       dropMin.y + (dropSize.y - ts.y) * 0.5f),
                ImGui::GetColorU32(ImGuiCol_Text), dropText);

    if (ImGui::BeginDragDropTarget())
    {
        if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload("KROM_ASSET_TEXTURE"))
        {
            const char* texPath = static_cast<const char*>(p->Data);
            if (texPath && texPath[0] != '\0')
            {
                const std::filesystem::path dropped = PathFromUtf8(texPath).lexically_normal();
                if (IsHdrEnvironmentTexturePath(dropped))
                {
                    ctx.state.environmentTexturePath = PathToUtf8(dropped);
                    ApplyEnvironmentFromState(ctx);
                }
            }
        }
        ImGui::EndDragDropTarget();
    }

    if (!ctx.state.environmentTexturePath.empty())
    {
        if (ImGui::SmallButton("Entfernen##skybox"))
        {
            ctx.state.environmentTexturePath.clear();
            if (ctx.clearEditorEnvironment)
                ctx.clearEditorEnvironment();
        }
    }
}

static void DrawEnvironmentInspector(EditorFrameContext& ctx)
{
    using AmbientSource = EditorState::AmbientSource;

    // ── Environment ───────────────────────────────────────────────────────────
    if (ComponentHeader("Environment", ComponentIcon::Environment, ImGuiTreeNodeFlags_DefaultOpen))
    {
        DrawSkyboxDropZone(ctx);
    }

    // ── Environment Lighting ──────────────────────────────────────────────────
    if (ComponentHeader("Environment Lighting", ComponentIcon::Environment, ImGuiTreeNodeFlags_DefaultOpen))
    {
        int src = static_cast<int>(ctx.state.ambientSource);
        if (value_ui::Combo("Source", &src, "Skybox\0Gradient\0Color\0"))
        {
            ctx.state.ambientSource        = static_cast<AmbientSource>(src);
            ctx.state.environmentEnableIBL = (ctx.state.ambientSource == AmbientSource::Skybox);
            ApplyEnvironmentFromState(ctx);
        }

        // IBL Intensity – nur wenn Skybox-Quelle aktiv; Rebake nur beim Loslassen der Maus
        if (ctx.state.ambientSource == AmbientSource::Skybox)
        {
            float envIntensity = ctx.state.environmentIntensity;
            value_ui::TextUnformatted("IBL Intensity");
            ImGui::SameLine();
            ImGui::SetNextItemWidth(-1.f);
            value_ui::DragFloat("##iblIntensity", &envIntensity, 0.01f, 0.0f, 4.0f, "%.2f",
                             ImGuiSliderFlags_AlwaysClamp);
            ctx.state.environmentIntensity = envIntensity;   // immer State aktualisieren
            if (ImGui::IsItemDeactivatedAfterEdit())          // Rebake nur beim Loslassen
                ApplyEnvironmentFromState(ctx);
        }

        bool ambientChanged = false;

        // Farbfelder + Fallback-Intensität – nur relevant wenn IBL NICHT aktiv.
        // Bei Skybox (IBL aktiv): IBL ist das Ambient-Licht → ambientIntensity hat keine Wirkung.
        // Die IBL-Helligkeit wird ausschliesslich durch environmentIntensity (IBL Intensity, oben) gesteuert.
        if (ctx.state.ambientSource == AmbientSource::Gradient)
        {
            value_ui::TextUnformatted("Sky");
            ImGui::SameLine();
            ambientChanged |= value_ui::ColorEdit3("##ambSky", &ctx.state.ambientColor.x,
                                                ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_NoLabel);
            ImGui::SameLine();
            value_ui::TextUnformatted("Ground");
            ImGui::SameLine();
            ambientChanged |= value_ui::ColorEdit3("##ambGnd", &ctx.state.ambientColorGround.x,
                                                ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_NoLabel);

            float ambientIntensity = ctx.state.ambientIntensity;
            value_ui::TextUnformatted("Helligkeit");
            ImGui::SameLine();
            ImGui::SetNextItemWidth(-1.f);
            if (value_ui::DragFloat("##ambientIntensity", &ambientIntensity, 0.02f, 0.0f, 10.0f, "%.2f",
                                 ImGuiSliderFlags_AlwaysClamp))
            {
                ctx.state.ambientIntensity = ambientIntensity;
                ambientChanged = true;
            }
        }
        else if (ctx.state.ambientSource == AmbientSource::Color)
        {
            value_ui::TextUnformatted("Farbe");
            ImGui::SameLine();
            ambientChanged |= value_ui::ColorEdit3("##ambColor", &ctx.state.ambientColor.x,
                                                ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_NoLabel);

            float ambientIntensity = ctx.state.ambientIntensity;
            value_ui::TextUnformatted("Helligkeit");
            ImGui::SameLine();
            ImGui::SetNextItemWidth(-1.f);
            if (value_ui::DragFloat("##ambientIntensity", &ambientIntensity, 0.02f, 0.0f, 10.0f, "%.2f",
                                 ImGuiSliderFlags_AlwaysClamp))
            {
                ctx.state.ambientIntensity = ambientIntensity;
                ambientChanged = true;
            }
        }
        // AmbientSource::Skybox → IBL aktiv: kein Fallback-Slider.
        // Helligkeit wird ausschliesslich durch IBL Intensity (oben) gesteuert.

        if (ambientChanged && ctx.setEditorAmbientLight)
        {
            // Gradient: Durchschnitt aus Sky und Ground als Näherung
            const math::Vec3 effectiveColor =
                (ctx.state.ambientSource == AmbientSource::Gradient)
                ? math::Vec3{
                    (ctx.state.ambientColor.x + ctx.state.ambientColorGround.x) * 0.5f,
                    (ctx.state.ambientColor.y + ctx.state.ambientColorGround.y) * 0.5f,
                    (ctx.state.ambientColor.z + ctx.state.ambientColorGround.z) * 0.5f }
                : ctx.state.ambientColor;
            ctx.setEditorAmbientLight(effectiveColor, ctx.state.ambientIntensity);
        }
    }

    if (!ctx.lastFileMessage.empty())
    {
        ImGui::Separator();
        ImGui::TextWrapped("%s", ctx.lastFileMessage.c_str());
    }
}

} // namespace

namespace detail {

// ── Physik (Rigidbody) ───────────────────────────────────────────────────────
// Regel wie im Physics-Addon: der oberste Rigidbody einer Hierarchie besitzt
// alle Collider darunter; Collider ohne Rigidbody sind statische Hindernisse.

EntityID FindRigidBodyAncestor(const ecs::World& world, EntityID entity)
{
    EntityID found = NULL_ENTITY;
    EntityID cursor = entity;
    for (uint32_t depth = 0u; cursor.IsValid() && depth < 1024u; ++depth)
    {
        const auto* parent = world.Get<ParentComponent>(cursor);
        if (!parent || !world.IsAlive(parent->parent))
            break;
        cursor = parent->parent;
        if (world.Has<RigidBodyComponent>(cursor))
            found = cursor;
    }
    return found;   // oberster Rigidbody oberhalb von entity
}

void ApplyEnvironmentFromState(EditorFrameContext& ctx)
{
    if (ctx.state.environmentTexturePath.empty() || !ctx.setEditorEnvironment)
        return;

    if (ctx.setEditorEnvironment(ctx.state.environmentTexturePath,
                                 ctx.state.environmentIntensity,
                                 ctx.state.environmentEnableIBL))
    {
        ctx.lastFileMessage = "Environment aktualisiert.";
    }
    else
    {
        ctx.lastFileMessage = "Environment konnte nicht geladen werden.";
    }
}

void DrawInspector(EditorFrameContext& ctx)
{
    const std::string pasteKey = MakeEditHistoryKey(ctx.state.selectedEntity, "property.paste");
    value_ui::Scope valueScope(
        [&] { BeginPendingSceneEdit(ctx, pasteKey, "Wert eingefuegt"); },
        [&] { CommitPendingSceneEdit(ctx, pasteKey); });
    SolidEditorPanelStyleScope solidPanelStyle;
    ImGui::Begin("Inspector##editor", &ctx.inspectorPanelVisible, kAlignedPanelFlags);

    bool drawEntityTab = true;
    bool drawEnvironmentTab = false;
    if (ImGui::BeginTabBar("##InspectorTabs"))
    {
        drawEntityTab = false;
        if (ImGui::BeginTabItem("Entity"))
        {
            drawEntityTab = true;
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Szene"))
        {
            drawEnvironmentTab = true;
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }

    if (drawEnvironmentTab)
    {
        DrawEnvironmentInspector(ctx);
        ImGui::End();
        return;
    }

    if (!drawEntityTab)
    {
        ImGui::End();
        return;
    }

    if (!ctx.state.selectedEntity.IsValid())
    {
        if (!ctx.state.selectedPrefabAssetPath.empty())
        {
            const std::filesystem::path prefabPath = ResolveSelectedPrefabPath(ctx);
            engine::addons::prefab::PrefabAsset prefab;
            std::string error;
            const bool loaded =
                engine::addons::prefab::LoadPrefabFromFile(prefabPath, ctx.registry, prefab, &error);

            value_ui::TextDisabled("Prefab");
            ImGui::TextWrapped("%s", ctx.state.selectedPrefabAssetPath.c_str());
            ImGui::Separator();

            if (loaded)
            {
                value_ui::Text("Name: %s", prefab.name.c_str());
                value_ui::Text("Entities: %zu", prefab.records.size());
                value_ui::Text("Root Index: %u", prefab.rootIndex);
                ImGui::Separator();
                if (ImGui::Button("In Szene laden", ImVec2(-1.f, 0.f)))
                    SpawnPrefabAsset(ctx, prefabPath);
            }
            else
            {
                ImGui::TextWrapped("%s", error.empty() ? "Prefab konnte nicht gelesen werden." : error.c_str());
            }

            ImGui::End();
            return;
        }

        value_ui::TextDisabled("Keine Entity ausgewaehlt");
        ImGui::End();
        return;
    }

    const EntityID selected = ctx.state.selectedEntity;

    // Kamera-Gizmo-Entities können nicht ausgewählt werden (Entity-Liste filtert sie aus),
    // aber sicherheitshalber auch hier abfangen.
    if (ctx.world.Get<EditorCameraGizmoComponent>(selected) != nullptr)
    {
        value_ui::TextDisabled("(Editor-internes Kamera-Gizmo)");
        ImGui::End();
        return;
    }
    if (ctx.world.Get<EditorRuntimeGizmoTag>(selected) != nullptr)
    {
        value_ui::TextDisabled("(Editor-internes Gizmo)");
        ImGui::End();
        return;
    }
    if (ctx.world.Get<EditorMaterialPreviewComponent>(selected) != nullptr)
    {
        value_ui::TextDisabled("(Editor-interne Materialvorschau)");
        ImGui::End();
        return;
    }
    if (ctx.world.Get<EditorAssetThumbnailComponent>(selected) != nullptr)
    {
        value_ui::TextDisabled("(Editor-internes Asset-Thumbnail)");
        ImGui::End();
        return;
    }

    if (!IsInPrefabMode(ctx.state))
    {
        EntityID prefabRoot=selected;
        for (size_t depth=0; prefabRoot.IsValid() && depth<=ctx.world.EntityCount(); ++depth)
        {
            if (const auto* link=ctx.world.Get<PrefabInstanceComponent>(prefabRoot))
            {
                value_ui::TextDisabled("Prefab: %s",PathToUtf8(PathFromUtf8(link->prefabAssetPath).filename()).c_str());
                if (EditorActionButton("Instanz auf Prefab zuruecksetzen",ImVec2(-1.f,0.f)))
                {
                    engine::addons::editor::ResetPrefabInstanceOverrides(ctx,selected);
                    ImGui::End(); return;
                }
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("Entfernt lokale Abweichungen dieser Instanz und ihrer Kinder. Position und Rotation der Instanz bleiben erhalten. Rueckgaengig mit Undo.");
                ImGui::Separator(); break;
            }
            const auto* parent=ctx.world.Get<ParentComponent>(prefabRoot);
            prefabRoot=parent ? parent->parent : NULL_ENTITY;
        }
    }

    if (EditorActionButton("Als Prefab speichern", ImVec2(-1.f, 36.f), EditorButtonTone::Primary))
        SaveEntityAsPrefab(ctx, selected);
    ImGui::Separator();

    // Multi-Selektion: Inspector zeigt Gruppeninfo + editierbare Transform
    if (ctx.state.multiSelection.size() >= 2)
    {
        value_ui::TextDisabled("Gruppe (%zu Objekte)", ctx.state.multiSelection.size());
        ImGui::Separator();

        // ── Position (Pivot verschieben → alle Entities um Delta bewegen) ────
        const bool mPosMode = ctx.state.gizmoMode == GizmoMode::Position;
        const bool mRotMode = ctx.state.gizmoMode == GizmoMode::Rotation;
        const bool mSclMode = ctx.state.gizmoMode == GizmoMode::Scale;
        const ImVec4 kPosActive{0.4f, 1.0f, 0.4f, 1.f};
        const ImVec4 kRotActive{0.4f, 0.6f, 1.0f, 1.f};
        const ImVec4 kSclActive{1.0f, 0.8f, 0.2f, 1.f};

        math::Vec3 pivot = ctx.state.multiSelectionPivot;
        auto mPosResult = InspectorDragFloat3("Position", "##mpos", &pivot.x, 0.1f,
                                              mPosMode ? &kPosActive : nullptr);
        if (mPosResult.labelClicked) ctx.state.gizmoMode = GizmoMode::Position;
        if (mPosResult.changed)
        {
            BeginPendingSceneEdit(ctx, "multisel.pivot.pos", "Gruppe bewegt");
            const math::Vec3 delta = pivot - ctx.state.multiSelectionPivot;
            for (EntityID e : ctx.state.multiSelection)
            {
                if (!IsMultiTransformTarget(ctx, e)) continue;
                auto* tc = ctx.world.Get<TransformComponent>(e);
                if (!tc) continue;
                SetWorldPosition(ctx, e, *tc, WorldPosition(ctx, e) + delta);
            }
            ctx.state.multiSelectionPivot = pivot;
        }
        if (ImGui::IsItemDeactivatedAfterEdit())
            CommitPendingSceneEdit(ctx, "multisel.pivot.pos");

        // ── Rotation (Delta um Pivot) — zeigt immer 0/0/0 (relativer Delta) ──
        math::Vec3 eulerDelta{0.f, 0.f, 0.f};
        auto mRotResult = InspectorDragFloat3("Rotation", "##mrot", &eulerDelta.x, 1.0f,
                                              mRotMode ? &kRotActive : nullptr);
        if (mRotResult.labelClicked) ctx.state.gizmoMode = GizmoMode::Rotation;
        if (mRotResult.changed)
        {
            BeginPendingSceneEdit(ctx, "multisel.pivot.rot", "Gruppe rotiert");
            const math::Quat deltaRot = math::Quat::FromEulerDeg(
                eulerDelta.x, eulerDelta.y, eulerDelta.z);
            for (EntityID e : ctx.state.multiSelection)
            {
                if (!IsMultiTransformTarget(ctx, e)) continue;
                auto* tc = ctx.world.Get<TransformComponent>(e);
                if (!tc) continue;
                const math::Vec3 offset   = WorldPosition(ctx, e) - ctx.state.multiSelectionPivot;
                const math::Vec3 newPos   = ctx.state.multiSelectionPivot + deltaRot.Rotate(offset);
                const math::Quat newRot   = (deltaRot * WorldRotation(ctx, e)).Normalized();
                const math::Mat4 world    = math::Mat4::TRS(newPos, newRot, WorldScale(ctx, e));
                SetWorldTransform(ctx, e, *tc, world);
            }
        }
        if (ImGui::IsItemDeactivatedAfterEdit())
            CommitPendingSceneEdit(ctx, "multisel.pivot.rot");

        // ── Scale — zeigt den Wert der ersten Entity; Änderungen werden als
        //           absoluter Delta auf alle Objekte angewendet. So verhält
        //           sich die Eingabe konsistent mit der Einzelauswahl:
        //           "5 eingeben" setzt alle auf 5, Drag-Bewegungen sind relativ.
        {
            // Anzeigewert: Scale der ersten Entity in der Auswahl
            math::Vec3 displayScale{1.f, 1.f, 1.f};
            const EntityID first = ctx.state.multiSelection.empty()
                ? NULL_ENTITY : ctx.state.multiSelection[0];
            if (first.IsValid() && ctx.world.IsAlive(first))
                displayScale = WorldScale(ctx, first);

            const math::Vec3 displayScaleBefore = displayScale;
            auto mSclResult = InspectorDragFloat3("Scale", "##mscl", &displayScale.x, 0.01f,
                                                  mSclMode ? &kSclActive : nullptr, 0.001f, 1000.f);
            if (mSclResult.labelClicked) ctx.state.gizmoMode = GizmoMode::Scale;
            if (mSclResult.changed)
            {
                BeginPendingSceneEdit(ctx, "multisel.pivot.scl", "Gruppe skaliert");

                // Delta = Differenz zwischen neuem und altem Anzeigewert
                const math::Vec3 delta{
                    displayScale.x - displayScaleBefore.x,
                    displayScale.y - displayScaleBefore.y,
                    displayScale.z - displayScaleBefore.z };

                for (EntityID e : ctx.state.multiSelection)
                {
                    if (!IsMultiTransformTarget(ctx, e)) continue;
                    if (!e.IsValid() || !ctx.world.IsAlive(e)) continue;
                    auto* tc = ctx.world.Get<TransformComponent>(e);
                    if (!tc) continue;
                    const math::Vec3 oldScale = WorldScale(ctx, e);
                    const math::Vec3 newScale{
                        std::max(0.0001f, oldScale.x + delta.x),
                        std::max(0.0001f, oldScale.y + delta.y),
                        std::max(0.0001f, oldScale.z + delta.z) };
                    const math::Mat4 world = math::Mat4::TRS(
                        WorldPosition(ctx, e), WorldRotation(ctx, e), newScale);
                    SetWorldTransform(ctx, e, *tc, world);
                }
            }
            if (ImGui::IsItemDeactivatedAfterEdit())
                CommitPendingSceneEdit(ctx, "multisel.pivot.scl");
        }

        ImGui::Separator();

        // ── Active / Visible / Persistent ─────────────────────────────────────
        // Zeigt den Wert der ersten Entity; Änderung wird auf alle angewendet.
        {
            const EntityID first = ctx.state.multiSelection.empty()
                ? NULL_ENTITY : ctx.state.multiSelection[0];

            // Active
            bool activeVal = true;
            if (first.IsValid() && ctx.world.IsAlive(first))
                if (const auto* ac = ctx.world.Get<ActiveComponent>(first))
                    activeVal = ac->active;
            if (value_ui::Checkbox("Active", &activeVal))
            {
                BeginPendingSceneEdit(ctx, "multisel.active", "Active geaendert");
                for (EntityID e : ctx.state.multiSelection)
                {
                    if (!e.IsValid() || !ctx.world.IsAlive(e)) continue;
                    if (auto* ac = ctx.world.Get<ActiveComponent>(e))
                        ac->active = activeVal;
                }
                CommitPendingSceneEdit(ctx, "multisel.active");
            }

            ImGui::SameLine();

            // Visible (nur wenn erste Entity ein Mesh hat)
            const bool firstHasMesh = first.IsValid() && ctx.world.IsAlive(first)
                                   && ctx.world.Get<MeshComponent>(first) != nullptr;
            bool visibleVal = true;
            if (first.IsValid() && ctx.world.IsAlive(first))
                if (const auto* mc = ctx.world.Get<MeshComponent>(first))
                    visibleVal = mc->visible;
            if (!firstHasMesh) ImGui::BeginDisabled();
            if (value_ui::Checkbox("Visible", &visibleVal))
            {
                BeginPendingSceneEdit(ctx, "multisel.visible", "Visible geaendert");
                for (EntityID e : ctx.state.multiSelection)
                {
                    if (!e.IsValid() || !ctx.world.IsAlive(e)) continue;
                    if (auto* mc = ctx.world.Get<MeshComponent>(e))
                        mc->visible = visibleVal;
                }
                CommitPendingSceneEdit(ctx, "multisel.visible");
            }
            if (!firstHasMesh) ImGui::EndDisabled();

            // Persistent
            bool persistVal = false;
            if (first.IsValid() && ctx.world.IsAlive(first))
                persistVal = ctx.world.Has<EditorPersistentComponent>(first);
            if (value_ui::Checkbox("Persistent", &persistVal))
            {
                BeginPendingSceneEdit(ctx, "multisel.persistent", "Persistent geaendert");
                for (EntityID e : ctx.state.multiSelection)
                {
                    if (!e.IsValid() || !ctx.world.IsAlive(e)) continue;
                    if (persistVal && !ctx.world.Has<EditorPersistentComponent>(e))
                        ctx.world.Add<EditorPersistentComponent>(e);
                    else if (!persistVal && ctx.world.Has<EditorPersistentComponent>(e))
                        ctx.world.Remove<EditorPersistentComponent>(e);
                }
                CommitPendingSceneEdit(ctx, "multisel.persistent");
            }

            // Scale erben (nur relevant wenn mind. eine Entity einen Parent hat)
            bool anyHasParent = false;
            for (EntityID e : ctx.state.multiSelection)
                if (e.IsValid() && ctx.world.IsAlive(e) && ctx.world.Has<ParentComponent>(e))
                    { anyHasParent = true; break; }

            if (anyHasParent)
            {
                bool inheritVal = true;
                if (first.IsValid() && ctx.world.IsAlive(first))
                    if (const auto* tc = ctx.world.Get<TransformComponent>(first))
                        inheritVal = tc->inheritParentScale;
                if (value_ui::Checkbox("Scale erben", &inheritVal))
                {
                    BeginPendingSceneEdit(ctx, "multisel.inheritScale", "Scale-Vererbung geaendert");
                    for (EntityID e : ctx.state.multiSelection)
                    {
                        if (!e.IsValid() || !ctx.world.IsAlive(e)) continue;
                        if (!ctx.world.Has<ParentComponent>(e)) continue;
                        if (auto* tc = ctx.world.Get<TransformComponent>(e))
                        {
                            tc->inheritParentScale = inheritVal;
                            tc->dirty = true;
                        }
                    }
                    CommitPendingSceneEdit(ctx, "multisel.inheritScale");
                }
            }
        }

        ImGui::Separator();
        value_ui::TextDisabled("STRG+Klick zum Abwaehlen.\nKlick ohne STRG hebt Gruppenauswahl auf.");
        ImGui::End();
        return;
    }
    ImGui::PushID(static_cast<int>(selected.value));

    value_ui::TextDisabled("Name");
    const float deleteWidth = ImGui::CalcTextSize("Loeschen").x + 24.f;
    const float spacing = ImGui::GetStyle().ItemSpacing.x;
    const float nameWidth = std::max(1.f, ImGui::GetContentRegionAvail().x - deleteWidth - spacing);

    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(8, 7));
    ImGui::SetNextItemWidth(nameWidth);
    if (auto* nc = ctx.world.Get<NameComponent>(selected))
    {
        char nameBuffer[128]{};
        std::snprintf(nameBuffer, sizeof(nameBuffer), "%s", nc->name.c_str());
        const std::string nameKey = MakeEditHistoryKey(selected, "name");
        if (value_ui::InputText("##EntityName", nameBuffer, sizeof(nameBuffer)))
        {
            BeginPendingSceneEdit(ctx, nameKey, "Name geaendert");
            nc->name = nameBuffer;
        }
        if (ImGui::IsItemDeactivatedAfterEdit())
            CommitPendingSceneEdit(ctx, nameKey);
    }
    else
    {
        char nameBuffer[128]{};
        std::snprintf(nameBuffer, sizeof(nameBuffer), "Entity %u", selected.Index());
        const std::string nameKey = MakeEditHistoryKey(selected, "name");
        if (value_ui::InputText("##EntityName", nameBuffer, sizeof(nameBuffer)))
        {
            BeginPendingSceneEdit(ctx, nameKey, "Name geaendert");
            ctx.world.Add<NameComponent>(selected, NameComponent{nameBuffer});
        }
        if (ImGui::IsItemDeactivatedAfterEdit())
            CommitPendingSceneEdit(ctx, nameKey);
    }
    ImGui::PopStyleVar();
    ImGui::SameLine();
    const bool deleteClicked = EditorActionButton("Loeschen", ImVec2(deleteWidth, ImGui::GetFontSize() + 14.f),
                                                   EditorButtonTone::Danger);
    value_ui::TextDisabled("Type: %s", PrimaryEntityType(ctx, selected));
    value_ui::TextDisabled("Components: %s", ComponentSummary(ctx, selected).c_str());

    // ── Tag | Layer  (eine Zeile, wie Unity) ─────────────────────────────────
    {
        auto* mesh = ctx.world.Get<MeshComponent>(selected);

        // Aktuellen Layer-Index bestimmen
        int layerIdx = 0;
        if (mesh)
        {
            mesh->layerMask = NormalizeSingleUserLayerMask(ctx, mesh->layerMask);
            layerIdx = LayerIndexFromSingleMask(mesh->layerMask);
        }

        const float totalW = ImGui::GetContentRegionAvail().x;
        const float tagLabelW   = ImGui::CalcTextSize("Tag").x   + ImGui::GetStyle().ItemSpacing.x;
        const float layerLabelW = ImGui::CalcTextSize("Layer").x + ImGui::GetStyle().ItemSpacing.x;
        const float halfW = (totalW - tagLabelW - layerLabelW) * 0.5f;

        // ── Tag: Dropdown mit verwalteter Liste ──────────────────────────────
        value_ui::Text("Tag");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(halfW);
        const auto* tc = ctx.world.Get<TagComponent>(selected);
        const std::string currentTag = tc ? tc->tag : "";
        const std::string tagKey = MakeEditHistoryKey(selected, "tag");
        std::string pastedTag = currentTag;
        if (value_ui::StringMenu("##tag_value", pastedTag, "tag"))
        {
            if (auto* tag = ctx.world.Get<TagComponent>(selected)) tag->tag = pastedTag;
            else ctx.world.Add<TagComponent>(selected, TagComponent{pastedTag});
            if (!pastedTag.empty() && std::find(ctx.state.knownTags.begin(), ctx.state.knownTags.end(), pastedTag) == ctx.state.knownTags.end())
                ctx.state.knownTags.push_back(pastedTag);
            mesh = ctx.world.Get<MeshComponent>(selected);
        }
        value_ui::ForgetLabel();

        if (ImGui::BeginCombo("##TagCombo", currentTag.empty() ? "Untagged" : currentTag.c_str()))
        {
            // Bestehende Tags — Klick = zuweisen, Rechtsklick = löschen
            std::string tagToDelete;
            for (const auto& t : ctx.state.knownTags)
            {
                const bool sel = (currentTag == t || (currentTag.empty() && t == "Untagged"));
                if (ImGui::Selectable(t.c_str(), sel))
                {
                    BeginPendingSceneEdit(ctx, tagKey, "Tag geaendert");
                    const std::string newTag = (t == "Untagged") ? "" : t;
                    if (!ctx.world.Has<TagComponent>(selected))
                        ctx.world.Add<TagComponent>(selected, TagComponent{newTag});
                    else
                        ctx.world.Get<TagComponent>(selected)->tag = newTag;
                    CommitPendingSceneEdit(ctx, tagKey);
                }
                if (ImGui::IsItemHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Right))
                    ImGui::OpenPopup(("##del_" + t).c_str());
                if (ImGui::BeginPopup(("##del_" + t).c_str()))
                {
                    if (t != "Untagged" && ImGui::MenuItem(("Loeschen: " + t).c_str()))
                        tagToDelete = t;
                    ImGui::EndPopup();
                }
                if (sel) ImGui::SetItemDefaultFocus();
            }
            if (!tagToDelete.empty())
            {
                auto& tags = ctx.state.knownTags;
                tags.erase(std::remove(tags.begin(), tags.end(), tagToDelete), tags.end());
            }

            // Neuen Tag hinzufügen
            ImGui::Separator();
            static char newTagBuf[64]{};
            ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - 26.f);
            ImGui::InputTextWithHint("##NewTagInput", "Neuer Tag...", newTagBuf, sizeof(newTagBuf));
            ImGui::SameLine();
            if (ImGui::SmallButton("+") && newTagBuf[0] != '\0')
            {
                const std::string newT = newTagBuf;
                if (std::find(ctx.state.knownTags.begin(), ctx.state.knownTags.end(), newT)
                    == ctx.state.knownTags.end())
                    ctx.state.knownTags.push_back(newT);
                BeginPendingSceneEdit(ctx, tagKey, "Tag geaendert");
                if (!ctx.world.Has<TagComponent>(selected))
                    ctx.world.Add<TagComponent>(selected, TagComponent{newT});
                else
                    ctx.world.Get<TagComponent>(selected)->tag = newT;
                CommitPendingSceneEdit(ctx, tagKey);
                newTagBuf[0] = '\0';
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndCombo();
        }

        ImGui::SameLine();

        // ── Layer: Dropdown mit editierbaren Namen ───────────────────────────
        value_ui::Text("Layer");
        if (mesh && value_ui::ScalarMenu("##layer_value", &mesh->layerMask, "layer-mask"))
            layerIdx = LayerIndexFromSingleMask(NormalizeSingleUserLayerMask(ctx, mesh->layerMask));
        value_ui::ForgetLabel();
        ImGui::SameLine();
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
        const std::string& layerPreview = ctx.state.layerNames[layerIdx];
        const std::string layerKey = MakeEditHistoryKey(selected, "layer");
        const bool noLayerSupport = (mesh == nullptr);
        if (noLayerSupport) ImGui::BeginDisabled();
        if (ImGui::BeginCombo("##LayerCombo", layerPreview.c_str()))
        {
            for (int b = 0; b < 32; ++b)
            {
                if (b == 1) continue; // EditorGizmo — niemals für User-Objekte
                const std::string& n = ctx.state.layerNames[b];
                if (n.empty() && b >= 8) continue;
                char label[48];
                std::snprintf(label, sizeof(label), "%d: %s", b, n.c_str());
                if (ImGui::Selectable(label, b == layerIdx))
                {
                    BeginPendingSceneEdit(ctx, layerKey, "Layer geaendert");
                    if (mesh) mesh->layerMask = (1u << b);
                    CommitPendingSceneEdit(ctx, layerKey);
                }
                if (b == layerIdx) ImGui::SetItemDefaultFocus();
            }
            ImGui::Separator();
            if (ImGui::Selectable("Edit Layers..."))
                ctx.state.layerNamesWindowOpen = true;
            ImGui::EndCombo();
        }
        if (noLayerSupport) ImGui::EndDisabled();
    }
    ImGui::Separator();

    if (deleteClicked)
    {
        ctx.state.deleteConfirmOpen = true;
        ImGui::PopID();
        ImGui::End();
        return;
    }

    // ── Active / Visible ─────────────────────────────────────────────────────
    {
        const bool isCamera = ctx.world.Has<CameraComponent>(selected);

        if (!isCamera)
        {
            auto* activeComp = ctx.world.Get<ActiveComponent>(selected);
            bool isActive = (activeComp == nullptr) || activeComp->active;
            if (value_ui::Checkbox("Active", &isActive))
            {
                BeginPendingSceneEdit(ctx, MakeEditHistoryKey(selected, "active"), "Active geaendert");
                if (activeComp)
                    activeComp->active = isActive;
                else
                    ctx.world.Add<ActiveComponent>(selected, ActiveComponent{isActive});
                CommitPendingSceneEdit(ctx, MakeEditHistoryKey(selected, "active"));
            }
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip(
                    "Entity komplett deaktivieren — wird aus\n"
                    "Rendering UND Systemen entfernt.\n"
                    "Entspricht Unity's SetActive().");

            ImGui::SameLine();

            auto* meshComp = ctx.world.Get<MeshComponent>(selected);
            bool isVisible = (meshComp == nullptr) || meshComp->visible;
            const bool hasNoMesh = (meshComp == nullptr);
            if (hasNoMesh) ImGui::BeginDisabled();
            if (value_ui::Checkbox("Visible", &isVisible))
            {
                BeginPendingSceneEdit(ctx, MakeEditHistoryKey(selected, "visible"), "Visible geaendert");
                if (meshComp)
                    meshComp->visible = isVisible;
                CommitPendingSceneEdit(ctx, MakeEditHistoryKey(selected, "visible"));
            }
            if (hasNoMesh) ImGui::EndDisabled();
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip(
                    "Nur Rendering an/aus — Entity laeuft weiter\n"
                    "durch Systeme und Transform.\n"
                    "Nur verfuegbar wenn Entity ein Mesh hat.");
            ImGui::Separator();
        }
    }

    // ── Persistent-Flag ──────────────────────────────────────────────────────
    {
        bool isPersistent = ctx.world.Has<EditorPersistentComponent>(selected);
        if (value_ui::Checkbox("Persistent", &isPersistent))
        {
            if (isPersistent)
                ctx.world.Add<EditorPersistentComponent>(selected, EditorPersistentComponent{});
            else
                ctx.world.Remove<EditorPersistentComponent>(selected);
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip(
                "Entity ueberlebt LoadScene() / UnloadScene().\n"
                "Gilt nur fuer diese Entity — nicht fuer Kinder.");
        ImGui::Separator();
    }

    // Alle Sektionen deaktivieren wenn Entity inaktiv ist (Kameras ausgenommen)
    {
        const bool isCamera = ctx.world.Has<CameraComponent>(selected);
        const auto* activeComp = ctx.world.Get<ActiveComponent>(selected);
        const bool isInactive = !isCamera && activeComp && !activeComp->active;
        if (isInactive) ImGui::BeginDisabled();

        using SectionDrawer = void (*)(EditorFrameContext&, EntityID);
        static constexpr std::array<SectionDrawer, 15> drawers = {
            DrawTransformSection, DrawCameraSection, DrawLightSection, DrawMeshSection,
            DrawMaterialSlotsSection, DrawRigidBodySection, DrawOBBSection,
            DrawSphereColliderSection, DrawCapsuleColliderSection, DrawCharacterControllerSection,
            DrawAudioSourceSection, DrawAudioListenerSection, DrawParticleEmitterSection,
            DrawMaterialSection, DrawScriptSection
        };
        static std::array<int, 15> order = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14};
        for (int section : order)
        {
            currentInspectorSection = section;
            drawers[section](ctx, selected);
        }
        currentInspectorSection = -1;
        // Erst nach dem Zeichnen umordnen, damit keine Sektion doppelt erscheint.
        if (movedInspectorSection >= 0 && targetInspectorSection >= 0 &&
            movedInspectorSection != targetInspectorSection)
        {
            const auto source = std::find(order.begin(), order.end(), movedInspectorSection);
            const auto target = std::find(order.begin(), order.end(), targetInspectorSection);
            if (source != order.end() && target != order.end())
            {
                const auto destination = target + (insertInspectorSectionAfter ? 1 : 0);
                if (source < destination)
                    std::rotate(source, source + 1, destination);
                else if (destination < source)
                    std::rotate(destination, source, source + 1);
            }
        }
        movedInspectorSection = targetInspectorSection = -1;

        if (isInactive) ImGui::EndDisabled();
    }

    // ── Drag-Drop-Zone für Material ───────────────────────────────────────────
    // Füllt den restlichen freien Raum oberhalb des "+ Komponente"-Buttons.
    {
        const float remainingH = ImGui::GetContentRegionAvail().y - 28.f;
        if (remainingH > 4.f)
        {
            ImGui::InvisibleButton("##InspectorFreeArea",
                ImVec2(std::max(1.f, ImGui::GetContentRegionAvail().x), remainingH));
            if (ImGui::BeginDragDropTarget())
            {
                if (const ImGuiPayload* payload =
                        ImGui::AcceptDragDropPayload("KROM_ASSET_MATERIAL"))
                {
                    const char* matPath = static_cast<const char*>(payload->Data);
                    if (matPath && matPath[0] != '\0')
                    {
                        ExecuteSceneMutation(ctx, "Material zugewiesen", [&]() {
                            SelectMaterialAsset(ctx, PathFromUtf8(matPath));
                            ApplyMaterialAssetToEntity(ctx, selected, PathFromUtf8(matPath));
                        });
                    }
                }
                ImGui::EndDragDropTarget();
            }
            // Rechtsklick auf freien Bereich → Popup öffnen
            if (ImGui::IsItemHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Right))
                ImGui::OpenPopup("AddComponentPopup##inspector");
        }
    }

    // "+" Button — am unteren Rand wenn Platz ist, aber niemals ueber Inhalt zeichnen.
    // Bei langen Inspector-Inhalten bleibt er im normalen Scroll-Flow unter den Controls.
    const float addButtonY = ImGui::GetWindowHeight() - 28.f;
    if (ImGui::GetCursorPosY() < addButtonY)
        ImGui::SetCursorPosY(addButtonY);
    ImGui::Separator();
    if (EditorActionButton("+ Komponente", ImVec2(-1.f, 36.f), EditorButtonTone::Primary))
        ImGui::OpenPopup("AddComponentPopup##inspector");

    // ── "Komponente hinzufügen"-Popup (Rechtsklick oder "+" Button) ───────────
    if (ImGui::BeginPopup("AddComponentPopup##inspector"))
    {
        value_ui::TextDisabled("Komponente hinzufuegen");
        ImGui::Separator();
        DrawPasteComponentAsNewItem(ctx, selected);
        DrawPasteScriptAsNewItem(ctx, selected);

        const bool hasMesh     = ctx.world.Has<MeshComponent>(selected);
        const bool hasOBB      = ctx.world.Has<OBBComponent>(selected);
        const bool hasMaterial = [&]() {
            auto* mc = ctx.world.Get<MaterialComponent>(selected);
            return mc && !mc->materialAssetPath.empty();
        }();

        // Kollision (OBB) — auch ohne Mesh, z.B. fuer unsichtbare Trigger-Zonen
        const bool obbDisabled = hasOBB;
        if (obbDisabled) ImGui::BeginDisabled();
        if (ImGui::MenuItem("Kollision (Box)"))
        {
            OBBComponent fresh;
            if (const auto* b = ctx.world.Get<BoundsComponent>(selected))
            {
                fresh.centerOffset = b->centerLocal;
                fresh.halfExtents  = b->extentsLocal;
            }
            ExecuteSceneMutation(ctx, "Komponente hinzugefuegt", [&]() { ctx.world.Add<OBBComponent>(selected, fresh); });
        }
        if (obbDisabled) ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        {
            if (hasOBB)       ImGui::SetTooltip("Kollision ist bereits vorhanden.");
            else if (hasMesh) ImGui::SetTooltip("Erstellt eine Box-Kollision aus der Mesh-AABB.");
            else              ImGui::SetTooltip("Erstellt eine 1x1x1-Box (z.B. als Trigger-Zone).");
        }

        // Kugel / Kapsel (Physics-Addon) — an die Mesh-Bounds angepasst.
        const auto* addBounds = ctx.world.Get<BoundsComponent>(selected);
        const bool hasSphere = ctx.world.Has<SphereColliderComponent>(selected);
        if (hasSphere) ImGui::BeginDisabled();
        if (ImGui::MenuItem("Kollision (Kugel)"))
        {
            SphereColliderComponent fresh;
            if (addBounds)
            {
                fresh.centerOffset = addBounds->centerLocal;
                fresh.radius = std::max(0.001f, std::max({addBounds->extentsLocal.x,
                                                          addBounds->extentsLocal.y,
                                                          addBounds->extentsLocal.z}));
            }
            fresh.showInEditor = true;
            ExecuteSceneMutation(ctx, "Komponente hinzugefuegt", [&]() { ctx.world.Add<SphereColliderComponent>(selected, fresh); });
        }
        if (hasSphere) ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip(hasSphere ? "Kugel-Kollision ist bereits vorhanden.\n"
                                          "Weitere Formen: \"Kollisions-Kind\"."
                                        : "Kugel um die Mesh-Bounds (Radius = groesste Halbachse).");

        const bool hasCapsule = ctx.world.Has<CapsuleColliderComponent>(selected);
        if (hasCapsule) ImGui::BeginDisabled();
        if (ImGui::MenuItem("Kollision (Kapsel)"))
        {
            CapsuleColliderComponent fresh;
            if (addBounds)
            {
                const math::Vec3 e = addBounds->extentsLocal;
                fresh.centerOffset = addBounds->centerLocal;
                fresh.axis   = CapsuleAxis::Y;
                fresh.radius = std::max(0.001f, std::max(e.x, e.z));
                fresh.height = std::max(fresh.radius * 2.f, e.y * 2.f);
            }
            fresh.showInEditor = true;
            ExecuteSceneMutation(ctx, "Komponente hinzugefuegt", [&]() { ctx.world.Add<CapsuleColliderComponent>(selected, fresh); });
        }
        if (hasCapsule) ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip(hasCapsule ? "Kapsel-Kollision ist bereits vorhanden."
                                         : "Kapsel entlang Y (z.B. fuer Figuren),\nan die Mesh-Bounds angepasst.");

        // Spielfigur — Kapsel um die Mesh-Bounds, Ursprung an den Fuessen.
        const bool hasCharacter = ctx.world.Has<CharacterControllerComponent>(selected);
        if (hasCharacter) ImGui::BeginDisabled();
        if (ImGui::MenuItem("Spielfigur (Character Controller)"))
        {
            CharacterControllerComponent fresh;
            if (addBounds)
            {
                const math::Vec3 e = addBounds->extentsLocal;
                fresh.radius = std::max(0.05f, std::max(e.x, e.z));
                fresh.height = std::max(fresh.radius * 2.f, e.y * 2.f);
            }
            ExecuteSceneMutation(ctx, "Komponente hinzugefuegt", [&]() { ctx.world.Add<CharacterControllerComponent>(selected, fresh); });
        }
        if (hasCharacter) ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip(hasCharacter ? "Spielfigur ist bereits vorhanden."
                                           : "Laeuft ueber Boden, Schraegen und Stufen.\n"
                                             "Gesteuert per Script (SetMoveVelocity, Jump).");

        // Audio
        ImGui::Separator();
        const bool hasAudioSource = ctx.world.Has<AudioSourceComponent>(selected);
        if (hasAudioSource) ImGui::BeginDisabled();
        if (ImGui::MenuItem("Audio-Quelle"))
            ExecuteSceneMutation(ctx, "Komponente hinzugefuegt", [&]() { ctx.world.Add<AudioSourceComponent>(selected, AudioSourceComponent{}); });
        if (hasAudioSource) ImGui::EndDisabled();
        const bool hasAudioListener = ctx.world.Has<AudioListenerComponent>(selected);
        if (hasAudioListener) ImGui::BeginDisabled();
        if (ImGui::MenuItem("Audio-Listener"))
            ExecuteSceneMutation(ctx, "Komponente hinzugefuegt", [&]() { ctx.world.Add<AudioListenerComponent>(selected, AudioListenerComponent{}); });
        if (hasAudioListener) ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("Das \"Ohr\" der Szene - meist an die Kamera haengen.");
        ImGui::Separator();

        // Effekte
        const bool hasEmitter = ctx.world.Has<ParticleEmitterComponent>(selected);
        if (hasEmitter) ImGui::BeginDisabled();
        if (ImGui::MenuItem("Partikel-Emitter"))
            ExecuteSceneMutation(ctx, "Komponente hinzugefuegt", [&]() { ctx.world.Add<ParticleEmitterComponent>(selected, ParticleEmitterComponent{}); });
        if (hasEmitter) ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip(hasEmitter ? "Partikel-Emitter ist bereits vorhanden."
                                         : "Rauch, Feuer, Funken - Vorschau laeuft direkt im Viewport.");
        ImGui::Separator();

        // Rigidbody — legt bei Bedarf gleich eine Box-Kollision aus der Mesh-AABB an.
        const bool hasRigidBody = ctx.world.Has<RigidBodyComponent>(selected);
        if (hasRigidBody) ImGui::BeginDisabled();
        if (ImGui::MenuItem("Rigidbody"))
        ExecuteSceneMutation(ctx, "Rigidbody hinzugefuegt", [&]()
        {
            uint32_t childColliders = 0u;
            CountChildColliders(ctx.world, selected, childColliders);
            if (!HasPhysicsCollider(ctx.world, selected) && childColliders == 0u)
            {
                if (auto* obb = ctx.world.Get<OBBComponent>(selected))
                {
                    obb->isTrigger = false;
                }
                else
                {
                    OBBComponent fresh;
                    if (const auto* b = ctx.world.Get<BoundsComponent>(selected))
                    {
                        fresh.centerOffset = b->centerLocal;
                        fresh.halfExtents  = b->extentsLocal;
                    }
                    ctx.world.Add<OBBComponent>(selected, fresh);
                }
            }
            ctx.world.Add<RigidBodyComponent>(selected, RigidBodyComponent{});
        });
        if (hasRigidBody) ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        {
            if (hasRigidBody) ImGui::SetTooltip("Rigidbody ist bereits vorhanden.");
            else              ImGui::SetTooltip("Dynamisch simuliert (faellt, rollt, wird geschoben).\n"
                                                "Ohne Kollision wird automatisch eine Box angelegt.");
        }

        // Zusammengesetzte Formen: weitere Collider als Kind-Objekte.
        ImGui::Separator();
        if (ImGui::MenuItem("Kollisions-Kind (Box)"))
            CreateColliderChildEntity(ctx, selected, ColliderChildShape::Box);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Leeres Kind-Objekt mit Box-Kollision.\n"
                              "Verschieben/drehen fuer zusammengesetzte Formen (z.B. L-Form).");
        if (ImGui::MenuItem("Kollisions-Kind (Kugel)"))
            CreateColliderChildEntity(ctx, selected, ColliderChildShape::Sphere);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Leeres Kind-Objekt mit Kugel-Kollision.");

        // Material
        if (hasMaterial) ImGui::BeginDisabled();
        if (ImGui::MenuItem("Material"))
            AssignDefaultMaterialToEntity(ctx, selected);
        if (hasMaterial) ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled) && hasMaterial)
            ImGui::SetTooltip("Material ist bereits zugewiesen.\nMaterial-Datei in den Inspector\noder auf die Entity ziehen um es zu wechseln.");

        ImGui::EndPopup();
    }

    ImGui::PopID();
    ctx.inspectorPanelWidth = ImGui::GetWindowWidth();
    ImGui::End();
}

} // namespace detail

} // namespace engine::renderer::addons::editor

#endif // KROM_EDITOR_HAS_IMGUI
