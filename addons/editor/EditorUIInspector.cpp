// =============================================================================
// KROM Engine - addons/editor/EditorUIInspector.cpp
// Inspector: Komponenten-Sektionen (Transform, Kamera, Licht, Mesh,
// Material, Script, Physik/Kollision) und Environment.
// =============================================================================
#include "addons/editor/EditorUIInternal.hpp"
#include "core/PathUtf8.hpp"
#include "addons/editor/EditorValueClipboard.hpp"
#include "addons/particles/ParticleComponents.hpp"
#include "addons/particles/ParticleRenderer.hpp"
#include <unordered_map>
#include <bit>

#ifdef KROM_EDITOR_HAS_IMGUI

#include "addons/editor/EditorComponentClipboard.hpp"

#include <cstring>
#include <functional>
#include <array>
#include <limits>
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
// Set by ComponentHeader for the card drawn around the current inspector section.
static bool  sectionHeaderDrawn = false;
static float sectionHeaderTop = 0.f;
// Set by a click on the header's "..." button, consumed by BeginComponentContextMenu.
static bool headerMenuRequested = false;

static bool ComponentHeader(const char* label, ComponentIcon icon, ImGuiTreeNodeFlags flags = 0)
{
    headerMenuRequested = false;
    if (currentInspectorSection >= 0)
    {
        // Inside the section loop each component is drawn as a card; the gap
        // between cards replaces the separator line.
        ImGui::Dummy(ImVec2(0, 4.f));
    }
    else
    {
        ImGui::Dummy(ImVec2(0, 9.f));
        ImGui::PushStyleColor(ImGuiCol_Separator, theme::kBorder);
        ImGui::Separator();
        ImGui::PopStyleColor();
        ImGui::Dummy(ImVec2(0, 3.f));
    }
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
    if (currentInspectorSection >= 0)
    {
        sectionHeaderDrawn = true;
        sectionHeaderTop = ImGui::GetItemRectMin().y;
    }
    if (!ImGui::IsItemVisible())
        return open;
    const ImVec2 min = ImGui::GetItemRectMin();
    const ImVec2 max = ImGui::GetItemRectMax();
    const float mid = (min.y + max.y) * 0.5f;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->PushClipRect(min, max, true);
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
        // The whole header is the handle for reordering.
        if (ImGui::BeginDragDropSource())
        {
            ImGui::SetDragDropPayload("KROM_INSPECTOR_SECTION", &currentInspectorSection, sizeof(int));
            ImGui::TextUnformatted(title.c_str());
            ImGui::EndDragDropSource();
        }
        // The script section has no header menu, so it gets no "..." button.
        if (icon != ComponentIcon::Script)
        {
            const ImVec2 cursor = ImGui::GetCursorScreenPos();
            ImGui::SetCursorScreenPos(ImVec2(max.x - 28.f, min.y));
            ImGui::PushID(label);
            if (ImGui::InvisibleButton("##header_menu", ImVec2(28.f, max.y - min.y)))
                headerMenuRequested = true;
            const ImU32 dotColor = ImGui::IsItemHovered() ? ImGui::GetColorU32(ImGuiCol_ButtonHovered) : textColor;
            for (int dot = 0; dot < 3; ++dot)
                dl->AddCircleFilled(ImVec2(max.x - 20.f + dot * 6.f, mid), 1.5f, dotColor);
            ImGui::PopID();
            ImGui::SetCursorScreenPos(cursor);
        }
        ImGui::GetCurrentContext()->LastItemData = headerItem;
    }
    ImGui::GetCurrentContext()->LastItemData = headerItem;
    return open;
}

// Reorder handles overlap the header. Explicitly allow this overlap when
// requesting its clipboard popup; keep the standard keyboard context action.
static bool BeginComponentContextMenu(const char* popupId)
{
    if (headerMenuRequested)
    {
        headerMenuRequested = false;
        ImGui::OpenPopup(popupId);
    }
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
        if (m_commitOnExit || !ImGui::IsAnyItemActive())
            CommitPendingSceneEdit(m_ctx, m_key);
    }

    // A button may disappear after a keyboard activation, keeping ActiveId alive
    // for another frame. Discrete edits still need their own undo command now.
    void CommitOnExit() noexcept { m_commitOnExit = true; }

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
    bool                m_commitOnExit = false;
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
    value_ui::RowLabel(fieldName);
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
        ImGui::SetTooltip(Translate("script.live_value"));
    ImGui::SetNextItemWidth(-FLT_MIN);
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
            ImGui::SetTooltip(Translate("script.relative_asset"));
        else if (ImGui::IsItemHovered() && !ImGui::IsItemActive())
            ImGui::SetTooltip(Translate("script.drop_asset"));
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
            ImGui::SetTooltip(Translate("inspector.reveal_entity"));
        ImGui::SameLine();
        ImGui::BeginDisabled(hasLiveValue);
        if (ImGui::BeginCombo("##value", nullptr, ImGuiComboFlags_NoPreview))
        {
            const bool noneSelected = !value.entityValue.IsValid();
            if (ImGui::Selectable(TranslateLabel("inspector.none", "(None)").c_str(), noneSelected))
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
            ImGui::SetTooltip(Translate("script.clear_entity"));
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

static void RemoveEntityMaterial(EditorFrameContext& ctx, EntityID entity)
{
    ExecuteSceneMutation(ctx, "history.remove_material", [&]() {
        if (auto* material = ctx.world.Get<MaterialComponent>(entity))
        {
            material->material = MaterialHandle{};
            material->materialAssetPath.clear();
            material->materialOverrideSource.clear();
        }
    });
}

struct MaterialRowResult
{
    std::string droppedPath;
    bool open = false;
    bool makeCopy = false;
};

// One grid row for an assigned material: name (drop target, opens the editor),
// "Open" and "own copy". The inspector never edits material values itself.
static MaterialRowResult DrawMaterialRow(const char* label, const std::string& name, const std::string& path,
                                         const std::function<void()>& contextMenu = {})
{
    MaterialRowResult result;
    value_ui::RowLabel(label);
    const bool canOpen = CanOpenMaterialAssetPath(path);
    const ImGuiStyle& style = ImGui::GetStyle();
    const std::string openLabel = TranslateLabel("common.open", "Open");
    const float openWidth = ImGui::CalcTextSize(openLabel.c_str(), nullptr, true).x + style.FramePadding.x * 2.f;
    const float copyWidth = ImGui::GetFrameHeight();
    const float nameWidth = std::max(1.f, ImGui::GetContentRegionAvail().x - openWidth - copyWidth - style.ItemSpacing.x * 2.f);

    ImGui::PushStyleVar(ImGuiStyleVar_ButtonTextAlign, ImVec2(0.f, 0.5f));
    ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_FrameBg));
    result.open = ImGui::Button((name + "###material_name").c_str(), ImVec2(nameWidth, 0.f)) && canOpen;
    ImGui::PopStyleColor();
    ImGui::PopStyleVar();
    if (ImGui::IsItemHovered() && !path.empty())
        ImGui::SetTooltip("%s", path.c_str());
    if (contextMenu && ImGui::BeginPopupContextItem("##material_row_ctx"))
    {
        contextMenu();
        ImGui::EndPopup();
    }
    if (ImGui::BeginDragDropTarget())
    {
        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("KROM_ASSET_MATERIAL"))
            if (const char* dropped = static_cast<const char*>(payload->Data); dropped && dropped[0] != '\0')
                result.droppedPath = dropped;
        ImGui::EndDragDropTarget();
    }

    ImGui::SameLine();
    ImGui::BeginDisabled(!canOpen);
    result.open |= ImGui::Button((openLabel + "###material_open").c_str(), ImVec2(openWidth, 0.f));
    ImGui::SameLine();
    result.makeCopy = ImGui::Button(ICON_LC_COPY "###material_copy", ImVec2(copyWidth, 0.f));
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("%s", Translate("material.make_copy_tip"));
    return result;
}

static const char* PrimaryEntityType(EditorFrameContext& ctx, EntityID entity)
{
    if (ctx.world.Get<CameraComponent>(entity))
        return Translate("component.camera");
    if (ctx.world.Get<LightComponent>(entity))
        return Translate("component.light");
    if (ctx.world.Get<MeshComponent>(entity))
        return "Mesh";
    return Translate("component.entity");
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
        append(Translate("component.transform"));
    if (ctx.world.Get<CameraComponent>(entity))
        append(Translate("component.camera"));
    if (ctx.world.Get<LightComponent>(entity))
        append(Translate("component.light"));
    if (ctx.world.Get<MeshComponent>(entity))
        append("Mesh");
    if (ctx.world.Get<MaterialComponent>(entity))
        append("Material");
    if (ctx.world.Get<ParentComponent>(entity))
        append(Translate("component.parent"));
    if (ctx.world.Get<ChildrenComponent>(entity))
        append(Translate("component.children"));

    return summary.empty() ? Translate("common.none") : summary;
}

// ─── Script-Section ──────────────────────────────────────────────────────────

static void RevealScriptAsset(EditorFrameContext& ctx, const std::string& className)
{
    if (!ctx.state.assetBrowser)
        return;
    const auto path = FindCppScriptAssetPath(ctx, className);
    if (path.empty())
    {
        ctx.lastFileMessage = Translate("script.asset_missing") + className;
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

    const bool open = ComponentHeader(TranslateLabel("component.scripts", "Scripts##scripts_hdr").c_str(), ComponentIcon::Script,
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
                ImGui::SetTooltip(Translate("script.reveal_toggle"));
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
                value_ui::TextDisabled(Translate("script.class"));
                ImGui::SameLine();
                if (ImGui::SmallButton(inst.className.c_str()))
                    RevealScriptAsset(ctx, inst.className);
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip(Translate("script.reveal"));

                // Platzhalter: Klasse weder registriert noch in den Projektquellen.
                // Eintrag und gespeicherte Werte bleiben beim Speichern erhalten.
                if (!inst.script &&
                    !std::binary_search(scriptClassNames.begin(), scriptClassNames.end(), inst.className))
                {
                    ImGui::PushStyleColor(ImGuiCol_Text, theme::kWarning);
                    ImGui::TextWrapped(Translate("script.class_missing"),
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
        if (EditorActionButton(TranslateLabel("script.add", "+ Script hinzufuegen").c_str(), ImVec2(-1.f, 34.f)))
            ImGui::OpenPopup("AddScriptPopup");

        if (ImGui::BeginPopup("AddScriptPopup"))
        {
            value_ui::TextDisabled(Translate("script.select_class"));
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
                                TranslateWithValue("script.added_build_needed", name);
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

    const bool transformOpen = ComponentHeader(TranslateLabel("component.transform", "Transform").c_str(), ComponentIcon::Transform, ImGuiTreeNodeFlags_DefaultOpen);
    if (BeginComponentContextMenu("##transform_hdr_ctx"))
    {
        DrawTransformClipboardMenuItems(ctx, entity);
        ImGui::EndPopup();
    }
    if (!transformOpen)
        return;

    int space = ctx.state.transformSpace == EditorTransformSpace::World ? 1 : 0;
    const std::string localLabel = ICON_LC_LOCATE_FIXED "  " + TranslateLabel("transform.local", "Local");
    if (ImGui::RadioButton(localLabel.c_str(), space == 0))
        space = 0;
    if (ImGui::IsItemHovered())
        ImGui::SetItemTooltip(Translate("transform.local_tip"));
    ImGui::SameLine();
    const std::string worldLabel = ICON_LC_EARTH "  " + TranslateLabel("transform.world", "World");
    if (ImGui::RadioButton(worldLabel.c_str(), space == 1))
        space = 1;
    if (ImGui::IsItemHovered())
        ImGui::SetItemTooltip(Translate("transform.world_tip"));
    ctx.state.transformSpace = space == 1 ? EditorTransformSpace::World : EditorTransformSpace::Local;

    // Hilfsfunktion: Label links (klickbar für Gizmo-Modus), DragFloat3 rechts
    // Gibt {valueChanged, labelClicked} zurück.
    // labelColor: wenn gesetzt, wird das Label in dieser Farbe gezeichnet (Modus-Anzeige).
    struct DragResult { bool changed; bool labelClicked; };
    auto LabeledDragFloat3 = [&](const char* label, const char* id,
                                 float* v, float speed,
                                 const ImVec4* labelColor = nullptr,
                                 float vmin = 0.f, float vmax = 0.f) -> DragResult
    {
        if (labelColor) ImGui::PushStyleColor(ImGuiCol_Text, *labelColor);
        value_ui::RowLabel(label);
        if (labelColor) ImGui::PopStyleColor();
        const auto clipboardPart = std::strcmp(id, "##pos") == 0 ? TransformClipboardPart::Position
            : (std::strcmp(id, "##rot") == 0 ? TransformClipboardPart::Rotation : TransformClipboardPart::Scale);
        DrawTransformValueClipboardMenu(ctx, entity, clipboardPart);
        const bool labelClicked = ImGui::IsItemClicked();
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip(Translate("transform.change_tool"));
        ImGui::SetNextItemWidth(-FLT_MIN);
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
            BeginPendingSceneEdit(ctx, localPositionKey, "history.position");
            t->dirty = true;
        }
        if (ImGui::IsItemDeactivatedAfterEdit())
            CommitPendingSceneEdit(ctx, localPositionKey);

        math::Quat localRotation = t->localRotation;
        math::Vec3 euler = PrepareRotationEditEuler(
            ctx, entity, EditorTransformSpace::Local, localRotation);
        const std::string localRotationKey = MakeEditHistoryKey(entity, "transform.local.rotation");
        auto rotResult = LabeledDragFloat3(Translate("transform.rotation"), "##rot", &euler.x, 1.0f,
                                           rotMode ? &kRotActive : nullptr, 0.f, 360.f);
        if (rotResult.labelClicked) ctx.state.gizmoMode = GizmoMode::Rotation;
        if (rotResult.changed)
        {
            BeginPendingSceneEdit(ctx, localRotationKey, "history.rotation");
            localRotation = math::Quat::FromEulerDeg(euler.x, euler.y, euler.z);
            t->localRotation = localRotation;
            t->dirty = true;
        }
        if (ImGui::IsItemDeactivatedAfterEdit())
            CommitPendingSceneEdit(ctx, localRotationKey);
        CommitRotationEditState(ctx, entity, EditorTransformSpace::Local, euler, localRotation);

        const std::string localScaleKey = MakeEditHistoryKey(entity, "transform.local.scale");
        auto sclResult = LabeledDragFloat3(Translate("transform.scale"), "##scl", &t->localScale.x, 0.01f,
                                           sclMode ? &kSclActive : nullptr, 0.0001f, 1000.f);
        if (sclResult.labelClicked) ctx.state.gizmoMode = GizmoMode::Scale;
        if (sclResult.changed)
        {
            BeginPendingSceneEdit(ctx, localScaleKey, "history.scale");
            t->dirty = true;
        }
        if (ImGui::IsItemDeactivatedAfterEdit())
            CommitPendingSceneEdit(ctx, localScaleKey);

        // Checkbox "Inherit Parent Scale" — nur sichtbar wenn Entity einen Parent hat
        if (ctx.world.Has<ParentComponent>(entity))
        {
            bool inherit = t->inheritParentScale;
            if (value_ui::Checkbox(TranslateLabel("transform.inherit_scale", "Scale erben").c_str(), &inherit))
            {
                const std::string key = MakeEditHistoryKey(entity, "transform.inheritParentScale");
                BeginPendingSceneEdit(ctx, key, "history.scale_inheritance");
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
            BeginPendingSceneEdit(ctx, worldPositionKey, "history.position");
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
        auto rotResult = LabeledDragFloat3(Translate("transform.rotation"), "##rot", &euler.x, 1.0f,
                                           rotMode ? &kRotActive : nullptr, 0.f, 360.f);
        if (rotResult.labelClicked) ctx.state.gizmoMode = GizmoMode::Rotation;
        if (rotResult.changed && !worldRotationBlocked)
        {
            BeginPendingSceneEdit(ctx, worldRotationKey, "history.rotation");
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
                ImGui::SetItemTooltip(Translate("transform.world_rotation_disabled"));
        }

        // World-scale edits must be converted to local scale, including roots
        // without meshes whose transform controls an entire prefab hierarchy.
        math::Vec3 scale = WorldScale(ctx, entity);
        {
            const std::string worldScaleKey = MakeEditHistoryKey(entity, "transform.world.scale");
            auto sclResult = LabeledDragFloat3(Translate("transform.scale"), "##scl", &scale.x, 0.01f,
                                               sclMode ? &kSclActive : nullptr, 0.0001f, 1000.f);
            if (sclResult.labelClicked) ctx.state.gizmoMode = GizmoMode::Scale;
            if (sclResult.changed)
            {
                BeginPendingSceneEdit(ctx, worldScaleKey, "history.scale");
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
    std::string rowId;
    if (ImGui::BeginCombo(value_ui::RowId(label, rowId), preview.c_str()))
    {
        if (ImGui::Selectable(TranslateLabel("layer.everything", std::string("Everything##") + id).c_str(),
                              (nextMask & visibleMask) == visibleMask))
        {
            nextMask = visibleMask;
            changed = true;
        }
        if (ImGui::Selectable(TranslateLabel("layer.nothing", std::string("Nothing##") + id).c_str(),
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

    const bool camOpen = ComponentHeader(TranslateLabel("component.camera", "Camera").c_str(), ComponentIcon::Camera, ImGuiTreeNodeFlags_DefaultOpen);
    if (BeginComponentContextMenu("##cam_hdr_ctx"))
    {
        DrawComponentClipboardMenuItems<CameraComponent>(ctx, entity, "component.camera");
        ImGui::Separator();
        if (ImGui::MenuItem(TranslateLabel("component.remove_camera", "Camera entfernen").c_str()))
            ExecuteSceneMutation(ctx, "history.remove_component", [&]() { ctx.world.Remove<CameraComponent>(entity); });
        ImGui::EndPopup();
    }
    if (!camOpen)
        return;

    int projection = camera->projection == ProjectionType::Orthographic ? 1 : 0;
    const std::string projectionKey = MakeEditHistoryKey(entity, "camera.projection");
    const char* projectionNames[] = { Translate("camera.perspective"), Translate("camera.orthographic") };
    if (value_ui::Combo(TranslateLabel("camera.projection", "Projection").c_str(), &projection, projectionNames, IM_ARRAYSIZE(projectionNames)))
    {
        BeginPendingSceneEdit(ctx, projectionKey, "history.camera_projection");
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
            BeginPendingSceneEdit(ctx, primaryValueKey, "history.camera_fov");
        if (ImGui::IsItemDeactivatedAfterEdit())
            CommitPendingSceneEdit(ctx, primaryValueKey);
    }
    else
    {
        if (value_ui::DragFloat(TranslateLabel("camera.ortho_size", "Ortho Size").c_str(), &camera->orthoSize, 0.05f, 0.01f, 10000.f))
            BeginPendingSceneEdit(ctx, primaryValueKey, "history.camera_ortho_size");
        if (ImGui::IsItemDeactivatedAfterEdit())
            CommitPendingSceneEdit(ctx, primaryValueKey);
    }

    const std::string nearKey = MakeEditHistoryKey(entity, "camera.near");
    if (value_ui::DragFloat(TranslateLabel("camera.near", "Near").c_str(), &camera->nearPlane, 0.01f, 0.001f, camera->farPlane - 0.001f))
        BeginPendingSceneEdit(ctx, nearKey, "history.camera_near");
    if (ImGui::IsItemDeactivatedAfterEdit())
        CommitPendingSceneEdit(ctx, nearKey);

    const std::string farKey = MakeEditHistoryKey(entity, "camera.far");
    if (value_ui::DragFloat(TranslateLabel("camera.far", "Far").c_str(), &camera->farPlane, 1.0f, camera->nearPlane + 0.001f, 100000.f))
        BeginPendingSceneEdit(ctx, farKey, "history.camera_far");
    if (ImGui::IsItemDeactivatedAfterEdit())
        CommitPendingSceneEdit(ctx, farKey);


    bool activeCamera = camera->isMainCamera;
    const std::string mainKey = MakeEditHistoryKey(entity, "camera.main");
    if (value_ui::Checkbox(TranslateLabel("camera.active", "Active Camera").c_str(), &activeCamera))
    {
        BeginPendingSceneEdit(ctx, mainKey, "history.active_camera");
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
        if (DrawLayerMaskCombo(ctx, TranslateLabel("camera.culling_mask", "Culling Mask").c_str(), "cameraCulling", cm))
        {
            BeginPendingSceneEdit(ctx, cmKey, "history.culling_mask");
            camera->cullingMask = cm;
            CommitPendingSceneEdit(ctx, cmKey);
        }
    }

    ImGui::Separator();
    value_ui::RowLabel(Translate("settings.background"));

    {
        int bgMode = static_cast<int>(camera->backgroundMode);
        const std::string bgModeKey = MakeEditHistoryKey(entity, "camera.backgroundMode");
        const char* backgroundModes[] = { Translate("camera.solid"), "Skybox" };
        if (value_ui::Combo("##bgMode", &bgMode, backgroundModes, IM_ARRAYSIZE(backgroundModes)))
        {
            BeginPendingSceneEdit(ctx, bgModeKey, "history.background_mode");
            camera->backgroundMode = static_cast<BackgroundMode>(bgMode);
            CommitPendingSceneEdit(ctx, bgModeKey);
        }
    }

    if (camera->backgroundMode == BackgroundMode::ClearColor)
    {
        const std::string colorKey = MakeEditHistoryKey(entity, "camera.clearColor");
        if (value_ui::ColorEdit4(TranslateLabel("field.color", "Farbe##bgColor").c_str(), camera->clearColor.data()))
            BeginPendingSceneEdit(ctx, colorKey, "history.background_color");
        if (ImGui::IsItemDeactivatedAfterEdit())
            CommitPendingSceneEdit(ctx, colorKey);
    }

}

static void DrawLightSection(EditorFrameContext& ctx, EntityID entity)
{
    auto* light = ctx.world.Get<LightComponent>(entity);
    if (!light)
        return;

    const bool lightOpen = ComponentHeader(TranslateLabel("component.light", "Light").c_str(), ComponentIcon::Light, ImGuiTreeNodeFlags_DefaultOpen);
    if (BeginComponentContextMenu("##light_hdr_ctx"))
    {
        DrawComponentClipboardMenuItems<LightComponent>(ctx, entity, "component.light");
        ImGui::Separator();
        if (ImGui::MenuItem(TranslateLabel("component.remove_light", "Light entfernen").c_str()))
            ExecuteSceneMutation(ctx, "history.remove_component", [&]() { ctx.world.Remove<LightComponent>(entity); });
        ImGui::EndPopup();
    }
    if (!lightOpen)
        return;

    int type = static_cast<int>(light->type);
    const std::string typeKey = MakeEditHistoryKey(entity, "light.type");
    const char* lightTypes[] = { Translate("light.directional"), Translate("light.point"), "Spot" };
    if (value_ui::Combo(TranslateLabel("field.type", "Type").c_str(), &type, lightTypes, IM_ARRAYSIZE(lightTypes)))
    {
        BeginPendingSceneEdit(ctx, typeKey, "history.light_type");
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
    if (value_ui::ColorEdit3(TranslateLabel("field.color", "Color").c_str(), &light->color.x))
        BeginPendingSceneEdit(ctx, colorKey, "history.light_color");
    if (ImGui::IsItemDeactivatedAfterEdit())
        CommitPendingSceneEdit(ctx, colorKey);

    const std::string intensityKey = MakeEditHistoryKey(entity, "light.intensity");
    if (value_ui::DragFloat(TranslateLabel("field.intensity", "Intensity").c_str(), &light->intensity, 0.05f, 0.f, 100000.f))
        BeginPendingSceneEdit(ctx, intensityKey, "history.light_intensity");
    if (ImGui::IsItemDeactivatedAfterEdit())
        CommitPendingSceneEdit(ctx, intensityKey);

    if (light->type != LightType::Directional)
    {
        const float oldRange = light->range;
        const std::string rangeKey = MakeEditHistoryKey(entity, "light.range");
        if (value_ui::DragFloat(TranslateLabel("light.range", "Range").c_str(), &light->range, 0.1f, 0.01f, 100000.f))
        {
            const bool shadowDistanceFollowedRange =
                std::fabs(light->shadowSettings.maxDistance - oldRange) <= 0.01f ||
                std::fabs(light->shadowSettings.maxDistance - 100.f) <= 0.01f;
            if (shadowDistanceFollowedRange)
                light->shadowSettings.maxDistance = light->range;
            BeginPendingSceneEdit(ctx, rangeKey, "history.light_range");
        }
        if (ImGui::IsItemDeactivatedAfterEdit())
            CommitPendingSceneEdit(ctx, rangeKey);
    }

    if (light->type == LightType::Spot)
    {
        const std::string innerKey = MakeEditHistoryKey(entity, "light.innerAngle");
        if (value_ui::DragFloat(TranslateLabel("light.inner_angle", "Inner Angle").c_str(), &light->spotInnerDeg, 0.25f, 0.f, 179.f))
            BeginPendingSceneEdit(ctx, innerKey, "history.inner_angle");
        if (ImGui::IsItemDeactivatedAfterEdit())
            CommitPendingSceneEdit(ctx, innerKey);

        const std::string outerKey = MakeEditHistoryKey(entity, "light.outerAngle");
        if (value_ui::DragFloat(TranslateLabel("light.outer_angle", "Outer Angle").c_str(), &light->spotOuterDeg, 0.25f, light->spotInnerDeg, 179.f))
            BeginPendingSceneEdit(ctx, outerKey, "history.outer_angle");
        if (ImGui::IsItemDeactivatedAfterEdit())
            CommitPendingSceneEdit(ctx, outerKey);
    }

    bool shadows = light->castShadows || light->shadowSettings.enabled;
    const std::string castShadowsKey = MakeEditHistoryKey(entity, "light.castShadows");
    if (value_ui::Checkbox(TranslateLabel("light.cast_shadows", "Cast Shadows").c_str(), &shadows))
    {
        BeginPendingSceneEdit(ctx, castShadowsKey, "history.shadow_option");
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
        if (value_ui::DragInt(TranslateLabel("light.shadow_resolution", "Shadow Resolution").c_str(), &resolution, 16.f, 128, 8192))
        {
            BeginPendingSceneEdit(ctx, resolutionKey, "history.shadow_resolution");
            light->shadowSettings.resolution = static_cast<uint32_t>(std::max(resolution, 128));
        }
        if (ImGui::IsItemDeactivatedAfterEdit())
            CommitPendingSceneEdit(ctx, resolutionKey);

        const float minShadowBias = light->type == LightType::Directional ? 0.0005f : 0.0001f;
        const float minNormalBias = light->type == LightType::Directional ? 0.0010f : 0.0010f;

        const std::string biasKey = MakeEditHistoryKey(entity, "light.shadowBias");
        if (value_ui::DragFloat(TranslateLabel("light.shadow_bias", "Shadow Bias").c_str(), &light->shadowSettings.bias, 0.0001f, minShadowBias, 1.f, "%.5f"))
        {
            light->shadowSettings.bias = std::max(light->shadowSettings.bias, minShadowBias);
            BeginPendingSceneEdit(ctx, biasKey, "history.shadow_bias");
        }
        if (ImGui::IsItemDeactivatedAfterEdit())
            CommitPendingSceneEdit(ctx, biasKey);

        const std::string normalBiasKey = MakeEditHistoryKey(entity, "light.normalBias");
        if (value_ui::DragFloat(TranslateLabel("light.normal_bias", "Normal Bias").c_str(), &light->shadowSettings.normalBias, 0.0001f, minNormalBias, 1.f, "%.5f"))
        {
            light->shadowSettings.normalBias = std::max(light->shadowSettings.normalBias, minNormalBias);
            BeginPendingSceneEdit(ctx, normalBiasKey, "history.normal_bias");
        }
        if (ImGui::IsItemDeactivatedAfterEdit())
            CommitPendingSceneEdit(ctx, normalBiasKey);

        const std::string strengthKey = MakeEditHistoryKey(entity, "light.shadowStrength");
        if (value_ui::DragFloat(TranslateLabel("light.shadow_strength", "Shadow Strength").c_str(), &light->shadowSettings.strength, 0.01f, 0.f, 1.f))
            BeginPendingSceneEdit(ctx, strengthKey, "history.shadow_strength");
        if (ImGui::IsItemDeactivatedAfterEdit())
            CommitPendingSceneEdit(ctx, strengthKey);

        const std::string distanceKey = MakeEditHistoryKey(entity, "light.shadowDistance");
        if (value_ui::DragFloat(TranslateLabel("light.shadow_distance", "Shadow Distanz").c_str(), &light->shadowSettings.maxDistance, 0.5f, 1.f, 10000.f, "%.1f"))
            BeginPendingSceneEdit(ctx, distanceKey, "history.shadow_distance");
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
            if (value_ui::SliderInt(TranslateLabel("light.cascades", "Cascades").c_str(), &cascades, 1, 4))
            {
                BeginPendingSceneEdit(ctx, cascadeKey, "history.cascade_count");
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
                if (value_ui::SliderFloat(TranslateLabel("light.cascade_split", "Cascade-Verteilung").c_str(), &light->shadowSettings.cascadeLambda, 0.f, 1.f, "%.2f"))
                    BeginPendingSceneEdit(ctx, lambdaKey, "history.cascade_split");
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

    const auto* mesh = ctx.registry.meshes.Get(meshComp->mesh);
    ImGui::BeginGroup();

    if (mesh)
    {
        value_ui::Text("Name: %s", mesh->debugName.c_str());
        value_ui::Text(Translate("mesh.submeshes"), static_cast<uint32_t>(mesh->submeshes.size()));
    }
    else if (!meshComp->meshAssetPath.empty())
    {
        ImGui::PushStyleColor(ImGuiCol_Text, theme::kWarning);
        ImGui::TextWrapped(Translate("mesh.unbound"));
        ImGui::PopStyleColor();
    }
    else
        value_ui::TextDisabled(Translate("mesh.none"));
    if (!meshComp->meshAssetPath.empty())
        value_ui::TextDisabled(Translate("field.path_format"), meshComp->meshAssetPath.c_str());

    const bool canAssign = ctx.assetPipeline && !ctx.state.prefabModeReadOnly;
    if (canAssign)
        ImGui::TextWrapped("%s", Translate("mesh.drop_model"));
    const bool retry = !mesh && !meshComp->meshAssetPath.empty() && ctx.syncSceneState &&
        ImGui::Button(TranslateLabel("mesh.rebind", "Erneut binden##mesh").c_str());
    ImGui::EndGroup();

    if (canAssign)
    {
        if (const auto path = AcceptModelAssetDrop(ctx); !path.empty())
        {
            const bool assigned = AssignModelMesh(ctx, entity, path);
            ctx.lastFileMessage = TranslateWithValue(assigned ? "mesh.assigned" : "mesh.assign_failed",
                                                    PathToUtf8(path.filename()));
        }
    }
    if (retry)
        ctx.syncSceneState();
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

    const bool materialsOpen = ComponentHeader(TranslateLabel("component.materials", "Materials").c_str(), ComponentIcon::Material, ImGuiTreeNodeFlags_DefaultOpen);
    if (BeginComponentContextMenu("##materials_hdr_ctx"))
    {
        if (ctx.world.Has<MaterialComponent>(entity))
            DrawComponentClipboardMenuItems<MaterialComponent>(ctx, entity, "component.materials");
        else if (ClipboardHolds<MaterialComponent>())
            DrawPasteComponentAsNewItem(ctx, entity);
        else
            value_ui::TextDisabled(Translate("material.no_overrides"));
        if (const auto* assigned = ctx.world.Get<MaterialComponent>(entity); assigned && !assigned->materialAssetPath.empty())
        {
            ImGui::Separator();
            if (ImGui::MenuItem(TranslateLabel("component.remove_material", "Material entfernen").c_str()))
                RemoveEntityMaterial(ctx, entity);
        }
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
        value_ui::TextDisabled(Translate("material.no_slots"));
        return;
    }

    // Materials are edited in the material editor only; this list just assigns them.
    // Actions run after the loop because they can reallocate the slot overrides.
    std::function<void()> deferredAction;
    const auto queueAssign = [&](int scope, const std::string& path) {
        deferredAction = [&ctx, entity, scope, path] {
            ExecuteSceneMutation(ctx, scope >= 0 ? "history.assign_material_slot" : "history.assign_material", [&] {
                if (scope >= 0)
                    ApplyMaterialAssetToEntitySlot(ctx, entity, static_cast<uint32_t>(scope), PathFromUtf8(path));
                else
                    ApplyMaterialAssetToEntity(ctx, entity, PathFromUtf8(path));
            });
        };
    };
    const auto queueCopy = [&](int scope, const std::string& path) {
        deferredAction = [&ctx, entity, scope, path] { CreateEntityMaterialCopy(ctx, entity, scope, PathFromUtf8(path)); };
    };

    if (submeshCount > 1u)
    {
        const std::string entityPath = overrideMaterial ? overrideMaterial->materialAssetPath : std::string{};
        const std::string entityName = entityPath.empty()
            ? std::string(Translate("material.drop")) : PathToUtf8(PathFromUtf8(entityPath).filename());
        const MaterialRowResult row = DrawMaterialRow(Translate("material.all_slots"), entityName, entityPath);
        if (!row.droppedPath.empty()) queueAssign(-1, row.droppedPath);
        if (row.open) OpenMaterialAsset(ctx, PathFromUtf8(entityPath));
        if (row.makeCopy) queueCopy(-1, entityPath);
    }

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
        if (!hasSlotOverride && !hasEntityOverride && materialIndex >= mesh->materialHandles.size())
            visibleName = Translate("asset.missing");

        // Local values from older scenes: still rendered, but no longer editable here.
        const bool slotLocal = hasSlotOverride && !slotOverride->materialOverrideSource.empty();
        const bool entityLocal = !hasSlotOverride && hasEntityOverride && !overrideMaterial->materialOverrideSource.empty();
        const int scope = entityLocal ? -1 : static_cast<int>(si);

        ImGui::PushID(static_cast<int>(si));
        const std::string rowLabel = "Element " + std::to_string(si);
        const MaterialRowResult row = DrawMaterialRow(rowLabel.c_str(), visibleName, visiblePath, [&] {
            if (ImGui::MenuItem(TranslateLabel("clipboard.copy", "Wert kopieren").c_str())) value_ui::CopyText("asset-material", visiblePath);
            const bool canPasteMaterial = value_ui::Clipboard().type == "asset-material" &&
                CanOpenMaterialAssetPath(value_ui::Clipboard().text);
            if (ImGui::MenuItem(TranslateLabel("clipboard.paste", "Wert einfuegen").c_str(), nullptr, false, canPasteMaterial))
                queueAssign(static_cast<int>(si), value_ui::Clipboard().text);
            if (hasSlotOverride)
            {
                ImGui::Separator();
                if (ImGui::MenuItem(TranslateLabel("material.remove_override", "Remove Slot Override").c_str()))
                    deferredAction = [&ctx, entity, si] {
                        ExecuteSceneMutation(ctx, "history.remove_material_slot", [&] {
                            if (MaterialComponent* material = ctx.world.Get<MaterialComponent>(entity))
                                material->slotOverrides.erase(
                                    std::remove_if(material->slotOverrides.begin(), material->slotOverrides.end(),
                                                   [&](const MaterialComponent::SlotOverride& slot) { return slot.submeshIndex == si; }),
                                    material->slotOverrides.end());
                        });
                    };
            }
        });
        if (!row.droppedPath.empty()) queueAssign(static_cast<int>(si), row.droppedPath);
        if (row.open) OpenMaterialAsset(ctx, PathFromUtf8(visiblePath));
        if (row.makeCopy) queueCopy(scope, visiblePath);

        if (slotLocal || entityLocal)
        {
            value_ui::RowLabel(Translate("material.local_values"));
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("%s", Translate("material.local_values_tip"));
            const float width = std::max(1.f, (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x) * 0.5f);
            if (ImGui::Button(TranslateLabel("material.reset_local", "Reset").c_str(), ImVec2(width, 0.f)))
                deferredAction = [&ctx, entity, scope, visiblePath] { ResetEntityMaterialOverride(ctx, entity, scope, PathFromUtf8(visiblePath)); };
            ImGui::SameLine();
            if (ImGui::Button(TranslateLabel("material.save_local_copy", "Save as Copy").c_str(), ImVec2(width, 0.f)))
                queueCopy(scope, visiblePath);
        }
        ImGui::PopID();
    }

    if (deferredAction)
        deferredAction();
}

static void DrawMaterialSection(EditorFrameContext& ctx, EntityID entity)
{
    const auto* meshComponent = ctx.world.Get<MeshComponent>(entity);
    const bool hasMesh = meshComponent != nullptr;
    // A mesh with slots is fully covered by the slot list above.
    if (const assets::MeshAsset* meshAsset = meshComponent ? ctx.registry.meshes.Get(meshComponent->mesh) : nullptr;
        meshAsset && !meshAsset->submeshes.empty())
        return;

    MaterialComponent* materialComp = ctx.world.Get<MaterialComponent>(entity);
    const MaterialHandle matHandle = GetEntityMaterial(ctx, entity);
    // Fuer Entities OHNE Mesh den Material-Bereich nur anzeigen wenn ein Material
    // zugewiesen ist. Fuer Mesh-Entities immer anzeigen (Drag-Drop-Ziel muss sichtbar
    // bleiben, auch nachdem das Material entfernt wurde).
    if (!hasMesh && !matHandle.IsValid() &&
        (!materialComp || materialComp->materialAssetPath.empty()))
        return;

    const bool matOpen = ComponentHeader("Material", ComponentIcon::Material, ImGuiTreeNodeFlags_DefaultOpen);
    if (BeginComponentContextMenu("##mat_hdr_ctx"))
    {
        DrawComponentClipboardMenuItems<MaterialComponent>(ctx, entity, "Material");
        ImGui::Separator();
        if (materialComp && !materialComp->materialAssetPath.empty())
        {
            if (ImGui::MenuItem(TranslateLabel("material.open_window", "In eigenem Fenster oeffnen").c_str()))
                OpenMaterialAsset(ctx, PathFromUtf8(materialComp->materialAssetPath));
            ImGui::Separator();
            if (ImGui::MenuItem(TranslateLabel("component.remove_material", "Material entfernen").c_str()))
                RemoveEntityMaterial(ctx, entity);
        }
        ImGui::EndPopup();
    }
    if (!matOpen)
        return;

    const std::string assignedPath = materialComp ? materialComp->materialAssetPath : std::string{};
    const std::string assignedName = assignedPath.empty()
        ? std::string(Translate("material.drop")) : PathToUtf8(PathFromUtf8(assignedPath).filename());
    const MaterialRowResult row = DrawMaterialRow("Material", assignedName, assignedPath);
    if (row.open)
        OpenMaterialAsset(ctx, PathFromUtf8(assignedPath));
    if (row.makeCopy)
    {
        CreateEntityMaterialCopy(ctx, entity, -1, PathFromUtf8(assignedPath));
    }
    else if (!row.droppedPath.empty())
    {
        ExecuteSceneMutation(ctx, "history.assign_material", [&]() {
            ApplyMaterialAssetToEntity(ctx, entity, PathFromUtf8(row.droppedPath));
        });
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

    const bool open = ComponentHeader(TranslateLabel("component.rigidbody", "Rigidbody##rb_hdr").c_str(), ComponentIcon::Physics);
    if (BeginComponentContextMenu("##rb_hdr_ctx"))
    {
        DrawComponentClipboardMenuItems<RigidBodyComponent>(ctx, entity, "component.rigidbody");
        ImGui::Separator();
        if (ImGui::MenuItem(TranslateLabel("component.remove_rigidbody", "Rigidbody entfernen").c_str()))
        {
            ExecuteSceneMutation(ctx, "history.remove_component", [&]() { ctx.world.Remove<RigidBodyComponent>(entity); });
            ImGui::EndPopup();
            return;
        }
        ImGui::EndPopup();
    }
    if (!open)
        return;
    ComponentUndoScope<RigidBodyComponent> undo(ctx, entity, "rigidbody", "history.edit_rigidbody");

    value_ui::Checkbox(TranslateLabel("field.enabled", "Aktiv##rb_enabled").c_str(), &rb->enabled);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip(Translate("physics.enabled_tip"));

    const char* typeNames[] = { Translate("physics.static"), Translate("physics.dynamic"), Translate("physics.kinematic") };
    int type = static_cast<int>(rb->type);
    if (value_ui::Combo(TranslateLabel("field.type", "Typ##rb_type").c_str(), &type, typeNames, IM_ARRAYSIZE(typeNames)))
        rb->type = static_cast<RigidBodyType>(type);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip(Translate("physics.type_tip"));

    // Kollisionen dieses Koerpers (eigene + Kinder) und Hinweise.
    uint32_t childColliders = 0u;
    CountChildColliders(ctx.world, entity, childColliders);
    const bool ownCollider = HasPhysicsCollider(ctx.world, entity);
    const uint32_t total = childColliders + (ownCollider ? 1u : 0u);
    value_ui::TextDisabled(Translate("physics.collider_counts"),
                        total, ownCollider ? 1u : 0u, childColliders);
    const ImVec4 warnColor(1.f, 0.7f, 0.2f, 1.f);
    if (total == 0u)
        ImGui::TextColored(warnColor, Translate("physics.no_collider"));
    if (const auto* obb = ctx.world.Get<OBBComponent>(entity); obb && obb->isTrigger)
        ImGui::TextColored(warnColor, Translate("physics.trigger_not_simulated"));
    const EntityID ancestor = FindRigidBodyAncestor(ctx.world, entity);
    if (ancestor.IsValid())
    {
        ImGui::TextColored(warnColor, Translate("physics.ancestor_rigidbody"), EntityLabel(ctx.world, ancestor).c_str());
    }
    else if (ctx.world.Has<ParentComponent>(entity) && rb->type == RigidBodyType::Dynamic)
    {
        value_ui::TextDisabled(Translate("physics.parent_position"));
    }
    if (ImGui::SmallButton(TranslateLabel("physics.add_box_child", "+ Kollisions-Kind (Box)##rb_child_box").c_str()))
    {
        CreateColliderChildEntity(ctx, entity, ColliderChildShape::Box);
        return;   // Selektion wechselt auf das neue Kind
    }
    ImGui::SameLine();
    if (ImGui::SmallButton(TranslateLabel("physics.add_sphere_child", "+ Kollisions-Kind (Kugel)##rb_child_sphere").c_str()))
    {
        CreateColliderChildEntity(ctx, entity, ColliderChildShape::Sphere);
        return;
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip(Translate("physics.compound_tip"));
    ImGui::Separator();

    value_ui::DragFloat(TranslateLabel("physics.friction", "Reibung##rb_friction").c_str(), &rb->friction, 0.01f, 0.f, 10.f, "%.2f");
    value_ui::SliderFloat(TranslateLabel("physics.restitution", "Elastizitaet##rb_restitution").c_str(), &rb->restitution, 0.f, 1.f, "%.2f");
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip(Translate("physics.restitution_tip"));

    if (rb->type != RigidBodyType::Dynamic)
        return;

    value_ui::DragFloat(TranslateLabel("physics.mass", "Masse (kg)##rb_mass").c_str(), &rb->mass, 0.05f, 0.001f, 100000.f, "%.3f");
    rb->mass = std::max(rb->mass, 0.001f);
    value_ui::DragFloat(TranslateLabel("physics.gravity_scale", "Schwerkraft-Faktor##rb_gravity").c_str(), &rb->gravityScale, 0.01f, -10.f, 10.f, "%.2f");
    value_ui::DragFloat(TranslateLabel("physics.linear_damping", "Daempfung linear##rb_lin").c_str(), &rb->linearDamping, 0.01f, 0.f, 10.f, "%.2f");
    value_ui::DragFloat(TranslateLabel("physics.angular_damping", "Daempfung Drehung##rb_ang").c_str(), &rb->angularDamping, 0.01f, 0.f, 10.f, "%.2f");
    value_ui::RowLabel(Translate("physics.lock_rotation"));
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
        value_ui::TextDisabled(Translate("physics.rigidbody_owner"), EntityLabel(ctx.world, owner).c_str());
        const std::string label = TranslateLabel("physics.jump_to_owner", std::string("Zum Rigidbody springen##goto_rb_") + idSuffix);
        if (ImGui::SmallButton(label.c_str()))
            ctx.state.selectedEntity = owner;
    }
    else if (!ctx.world.Has<RigidBodyComponent>(entity))
    {
        value_ui::TextDisabled(Translate("physics.static_obstacle"));
    }
}

static void DrawSphereColliderSection(EditorFrameContext& ctx, EntityID entity)
{
    auto* sphere = ctx.world.Get<SphereColliderComponent>(entity);
    if (!sphere)
        return;

    const bool open = ComponentHeader(TranslateLabel("physics.sphere", "Kollision (Kugel)##sphere_hdr").c_str(), ComponentIcon::Collider);
    if (BeginComponentContextMenu("##sphere_hdr_ctx"))
    {
        DrawComponentClipboardMenuItems<SphereColliderComponent>(ctx, entity, "physics.sphere");
        ImGui::Separator();
        if (ImGui::MenuItem(TranslateLabel("physics.remove_sphere", "Kugel-Kollision entfernen").c_str()))
        {
            ExecuteSceneMutation(ctx, "history.remove_component", [&]() { ctx.world.Remove<SphereColliderComponent>(entity); });
            ImGui::EndPopup();
            return;
        }
        ImGui::EndPopup();
    }
    if (!open)
        return;
    ComponentUndoScope<SphereColliderComponent> undo(ctx, entity, "sphere", "history.edit_sphere");

    value_ui::Checkbox(TranslateLabel("physics.sphere_visible", "Sichtbar (Kugel anzeigen)##sphere_vis").c_str(), &sphere->showInEditor);
    DrawColliderOwnerInfo(ctx, entity, "sphere");
    ImGui::Separator();

    value_ui::DragFloat(TranslateLabel("field.radius", "Radius##sphere_radius").c_str(), &sphere->radius, 0.01f, 0.001f, 10000.f, "%.3f");
    sphere->radius = std::max(sphere->radius, 0.001f);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip(Translate("physics.sphere_scale_tip"));
    value_ui::RowLabel(Translate("physics.center_offset"));
    value_ui::DragFloat3("##sphere_center", &sphere->centerOffset.x, 0.01f, -10000.f, 10000.f, "%.3f");

    ImGui::Separator();
    if (ImGui::Button(TranslateLabel("physics.fit_mesh", "Aus Mesh berechnen##sphere_auto").c_str()))
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

    const bool open = ComponentHeader(TranslateLabel("component.character", "Spielfigur (Character Controller)##cc_hdr").c_str(), ComponentIcon::Character);
    if (BeginComponentContextMenu("##cc_hdr_ctx"))
    {
        DrawComponentClipboardMenuItems<CharacterControllerComponent>(ctx, entity, "component.character");
        ImGui::Separator();
        if (ImGui::MenuItem(TranslateLabel("component.remove_character", "Spielfigur entfernen").c_str()))
        {
            ExecuteSceneMutation(ctx, "history.remove_component", [&]() { ctx.world.Remove<CharacterControllerComponent>(entity); });
            ImGui::EndPopup();
            return;
        }
        ImGui::EndPopup();
    }
    if (!open)
        return;
    ComponentUndoScope<CharacterControllerComponent> undo(ctx, entity, "character", "history.edit_character");

    value_ui::Checkbox(TranslateLabel("field.enabled", "Aktiv##cc_enabled").c_str(), &cc->enabled);
    value_ui::Checkbox(TranslateLabel("field.visible", "Sichtbar##cc_vis").c_str(), &cc->showInEditor);
    value_ui::TextDisabled(Translate("physics.character_origin"));
    const ImVec4 warnColor(1.f, 0.7f, 0.2f, 1.f);
    if (ctx.world.Has<RigidBodyComponent>(entity))
        ImGui::TextColored(warnColor, Translate("physics.character_rigidbody"));
    ImGui::Separator();

    value_ui::DragFloat(TranslateLabel("field.radius", "Radius##cc_radius").c_str(), &cc->radius, 0.01f, 0.05f, 10.f, "%.2f");
    value_ui::DragFloat(TranslateLabel("field.height", "Hoehe##cc_height").c_str(), &cc->height, 0.01f, 0.1f, 20.f, "%.2f");
    cc->radius = std::max(cc->radius, 0.05f);
    cc->height = std::max(cc->height, cc->radius * 2.f);
    value_ui::DragFloat(TranslateLabel("physics.max_slope", "Max. Steigung (Grad)##cc_slope").c_str(), &cc->maxSlopeDeg, 0.5f, 0.f, 89.f, "%.0f");
    value_ui::DragFloat(TranslateLabel("physics.step_height", "Stufenhoehe##cc_step").c_str(), &cc->stepHeight, 0.01f, 0.f, 2.f, "%.2f");
    value_ui::DragFloat(TranslateLabel("physics.mass", "Masse (kg)##cc_mass").c_str(), &cc->mass, 0.5f, 1.f, 10000.f, "%.0f");
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip(Translate("physics.character_push"));
    value_ui::DragFloat(TranslateLabel("physics.gravity_scale", "Schwerkraft-Faktor##cc_gravity").c_str(), &cc->gravityScale, 0.01f, -10.f, 10.f, "%.2f");
}

static void DrawAudioSourceSection(EditorFrameContext& ctx, EntityID entity)
{
    auto* source = ctx.world.Get<AudioSourceComponent>(entity);
    if (!source)
        return;

    const bool open = ComponentHeader(TranslateLabel("component.audio_source", "Audio-Quelle##audio_src_hdr").c_str(), ComponentIcon::Audio);
    if (BeginComponentContextMenu("##audio_src_hdr_ctx"))
    {
        DrawComponentClipboardMenuItems<AudioSourceComponent>(ctx, entity, "component.audio_source");
        ImGui::Separator();
        if (ImGui::MenuItem(TranslateLabel("component.remove_audio_source", "Audio-Quelle entfernen").c_str()))
        {
            ExecuteSceneMutation(ctx, "history.remove_component", [&]() { ctx.world.Remove<AudioSourceComponent>(entity); });
            ImGui::EndPopup();
            return;
        }
        ImGui::EndPopup();
    }
    if (!open)
        return;
    ComponentUndoScope<AudioSourceComponent> undo(ctx, entity, "audio_source", "history.edit_audio_source");

    // Datei: Textfeld + Drop-Ziel fuer Audiodateien aus dem Asset-Browser.
    char buffer[512];
    std::snprintf(buffer, sizeof(buffer), "%s", source->clipPath.c_str());
    value_ui::RowLabel(Translate("audio.file"));
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
        ImGui::TextColored(ImVec4(1.f, 0.7f, 0.2f, 1.f), Translate("audio.drop"));
    ImGui::Separator();

    value_ui::SliderFloat(TranslateLabel("audio.volume", "Lautstaerke##audio_vol").c_str(), &source->volume, 0.f, 2.f, "%.2f");
    value_ui::DragFloat(TranslateLabel("audio.pitch", "Tonhoehe##audio_pitch").c_str(), &source->pitch, 0.01f, 0.1f, 4.f, "%.2f");
    value_ui::Checkbox(TranslateLabel("audio.play_on_start", "Beim Start abspielen##audio_play").c_str(), &source->playOnStart);
    value_ui::Checkbox(TranslateLabel("field.loop", "Schleife##audio_loop").c_str(), &source->loop);
    value_ui::Checkbox(TranslateLabel("audio.spatial", "Raeumlich (3D)##audio_spatial").c_str(), &source->spatial);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip(Translate("audio.spatial_tip"));
    if (source->spatial)
    {
        value_ui::DragFloat(TranslateLabel("audio.min_distance", "Min. Abstand##audio_min").c_str(), &source->minDistance, 0.1f, 0.01f, 1000.f, "%.1f");
        value_ui::DragFloat(TranslateLabel("audio.max_distance", "Max. Abstand##audio_max").c_str(), &source->maxDistance, 0.1f, 0.01f, 10000.f, "%.1f");
        source->maxDistance = std::max(source->maxDistance, source->minDistance);
    }
    value_ui::TextDisabled(Translate("audio.play_mode"));
}

static void DrawAudioListenerSection(EditorFrameContext& ctx, EntityID entity)
{
    auto* listener = ctx.world.Get<AudioListenerComponent>(entity);
    if (!listener)
        return;

    const bool open = ComponentHeader(TranslateLabel("component.audio_listener", "Audio-Listener##audio_lst_hdr").c_str(), ComponentIcon::Audio);
    if (BeginComponentContextMenu("##audio_lst_hdr_ctx"))
    {
        DrawComponentClipboardMenuItems<AudioListenerComponent>(ctx, entity, "component.audio_listener");
        ImGui::Separator();
        if (ImGui::MenuItem(TranslateLabel("component.remove_audio_listener", "Audio-Listener entfernen").c_str()))
        {
            ExecuteSceneMutation(ctx, "history.remove_component", [&]() { ctx.world.Remove<AudioListenerComponent>(entity); });
            ImGui::EndPopup();
            return;
        }
        ImGui::EndPopup();
    }
    if (!open)
        return;
    ComponentUndoScope<AudioListenerComponent> undo(ctx, entity, "audio_listener", "history.edit_audio_listener");
    value_ui::SliderFloat(TranslateLabel("audio.master_volume", "Gesamtlautstaerke##audio_lst_vol").c_str(), &listener->volume, 0.f, 2.f, "%.2f");
    value_ui::TextDisabled(Translate("audio.listener_help"));
}

static ParticleFrame AtlasFrame(const ParticleEmitterComponent& emitter, uint32_t index,
                                const AssetBrowserState::TextureThumbnail& texture)
{
    if (!emitter.frames.empty()) return emitter.frames[index];
    const uint32_t columns = std::max(emitter.flipbookColumns, 1u);
    const uint32_t rows = std::max(emitter.flipbookRows, 1u);
    const uint32_t col = index % columns, row = index / columns;
    const uint32_t x = static_cast<uint32_t>(uint64_t(col) * texture.width / columns);
    const uint32_t y = static_cast<uint32_t>(uint64_t(row) * texture.height / rows);
    const uint32_t endX = static_cast<uint32_t>(uint64_t(col + 1u) * texture.width / columns);
    const uint32_t endY = static_cast<uint32_t>(uint64_t(row + 1u) * texture.height / rows);
    return {x, y, endX - x, endY - y, 0.5f, 0.5f};
}

static bool MakeExplicitAtlas(ParticleEmitterComponent& emitter, const AssetBrowserState::TextureThumbnail& texture)
{
    if (!emitter.frames.empty()) return true;
    const uint64_t count = uint64_t(std::max(emitter.flipbookColumns, 1u)) * std::max(emitter.flipbookRows, 1u);
    if (count > 4096u || emitter.flipbookColumns > texture.width || emitter.flipbookRows > texture.height)
        return false;
    std::vector<ParticleFrame> frames;
    frames.reserve(static_cast<size_t>(count));
    for (uint32_t index = 0u; index < count; ++index)
        frames.push_back(AtlasFrame(emitter, index, texture));
    emitter.frames = std::move(frames);
    return true;
}

static uint32_t ParticleAtlasFrameCount(const ParticleEmitterComponent& emitter)
{
    if (!emitter.frames.empty()) return static_cast<uint32_t>(emitter.frames.size());
    return static_cast<uint32_t>(std::min(uint64_t(std::max(emitter.flipbookColumns, 1u)) *
        std::max(emitter.flipbookRows, 1u), uint64_t(UINT32_MAX)));
}

static void DrawParticleRange(uint32_t& first, uint32_t& last, uint32_t count)
{
    const uint32_t min = 0u, max = UINT32_MAX;
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * .55f);
    ImGui::DragScalar(TranslateLabel("particles.frame_first", "First Frame##first").c_str(),
        ImGuiDataType_U32, &first, .2f, &min, &max, "%u", ImGuiSliderFlags_AlwaysClamp);
    bool toLast = last == kLastParticleFrame;
    if (value_ui::Checkbox(TranslateLabel("particles.to_last_frame", "To Last Frame##to_last").c_str(), &toLast))
        last = toLast ? kLastParticleFrame : count - 1u;
    if (!toLast)
    {
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * .55f);
        ImGui::DragScalar(TranslateLabel("particles.frame_last", "Last Frame##last").c_str(),
            ImGuiDataType_U32, &last, .2f, &min, &max, "%u", ImGuiSliderFlags_AlwaysClamp);
    }
    if ((first >= count && first != kLastParticleFrame) || (last >= count && last != kLastParticleFrame))
    {
        ImGui::PushStyleColor(ImGuiCol_Text, theme::kWarning);
        ImGui::TextWrapped("%s", Translate("particles.frame_range_warning"));
        ImGui::PopStyleColor();
    }
}

static void DrawParticleRate(float& fps, bool& loop)
{
    value_ui::DragFloat(TranslateLabel("particles.frame_fps", "FPS##fps").c_str(), &fps,
        .1f, .01f, 10000.f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
    if (!std::isfinite(fps) || fps <= 0.f) fps = 12.f;
    value_ui::Checkbox(TranslateLabel("particles.frame_loop", "Loop Frames##frame_loop").c_str(), &loop);
}

static void DrawEmitterFrameAnimation(ParticleEmitterComponent& emitter)
{
    ImGui::SeparatorText(Translate("particles.frame_animation"));
    ImGui::PushID("EmitterFrameAnimation");
    const char* modes[] = {Translate("particles.animation_lifetime"), Translate("particles.animation_rate"),
        Translate("particles.animation_fixed"), Translate("particles.animation_random")};
    int mode = static_cast<int>(emitter.frameAnimation);
    if (value_ui::Combo(TranslateLabel("particles.animation_mode", "Mode##mode").c_str(), &mode, modes, IM_ARRAYSIZE(modes)))
        emitter.frameAnimation = static_cast<ParticleFrameAnimation>(mode);
    DrawParticleRange(emitter.frameFirst, emitter.frameLast, ParticleAtlasFrameCount(emitter));
    if (emitter.frameAnimation == ParticleFrameAnimation::Rate)
    {
        DrawParticleRate(emitter.frameFps, emitter.frameLoop);
        value_ui::Checkbox(TranslateLabel("particles.random_start", "Random Start##random_start").c_str(), &emitter.frameRandomStart);
    }
    ImGui::PopID();
}

static std::string UniqueParticleClipName(const ParticleEmitterComponent& emitter)
{
    std::string name = "Clip";
    uint32_t number = 1u;
    while (std::any_of(emitter.clips.begin(), emitter.clips.end(), [&](const ParticleClip& clip) { return clip.name == name; }))
        name = "Clip " + std::to_string(++number);
    return name;
}

static bool DrawParticleClips(ParticleEmitterComponent& emitter, uint32_t count, int& source)
{
    if (!ImGui::TreeNode(TranslateLabel("particles.clips", "Clips##pe_clips").c_str())) return false;
    bool discreteEdit = false;
    if (ImGui::SmallButton(TranslateLabel("particles.add_clip", "Add Clip##add").c_str()))
    {
        ParticleClip clip;
        clip.name = UniqueParticleClipName(emitter);
        emitter.clips.push_back(std::move(clip));
        source = static_cast<int>(emitter.clips.size()) - 1;
        discreteEdit = true;
    }
    std::unordered_map<std::string, size_t> names;
    for (const auto& clip : emitter.clips) ++names[clip.name];
    int duplicate = -1, remove = -1, move = 0, moveIndex = -1;
    for (size_t i = 0; i < emitter.clips.size(); ++i)
    {
        ImGui::PushID(static_cast<int>(i));
        auto& clip = emitter.clips[i];
        const bool validName = IsParticleClipNameValid(clip.name) && names[clip.name] == 1u;
        if (!validName) ImGui::PushStyleColor(ImGuiCol_Text, theme::kDanger);
        if (ImGui::Selectable(clip.name.empty() ? Translate("particles.unnamed_clip") : clip.name.c_str(), source == static_cast<int>(i)))
            source = static_cast<int>(i);
        if (!validName) ImGui::PopStyleColor();
        if (source == static_cast<int>(i))
        {
            char buffer[257];
            std::snprintf(buffer, sizeof(buffer), "%s", clip.name.c_str());
            if (value_ui::InputText(TranslateLabel("particles.clip_name", "Name##clip_name").c_str(), buffer, sizeof(buffer)))
                clip.name = buffer;
            if (!validName)
            {
                ImGui::PushStyleColor(ImGuiCol_Text, theme::kDanger);
                ImGui::TextWrapped("%s", Translate("particles.clip_name_warning"));
                ImGui::PopStyleColor();
            }
            DrawParticleRange(clip.first, clip.last, count);
            const char* timings[] = {Translate("particles.animation_rate"), Translate("particles.animation_lifetime")};
            int timing = static_cast<int>(clip.timing);
            if (value_ui::Combo(TranslateLabel("particles.clip_timing", "Timing##timing").c_str(), &timing, timings, IM_ARRAYSIZE(timings)))
                clip.timing = static_cast<ParticleClipTiming>(timing);
            if (clip.timing == ParticleClipTiming::Rate) DrawParticleRate(clip.fps, clip.loop);
            ImGui::BeginDisabled(i == 0u);
            if (ImGui::SmallButton(TranslateLabel("particles.clip_up", "Move Up##up").c_str())) { moveIndex = static_cast<int>(i); move = -1; }
            ImGui::EndDisabled();
            ImGui::SameLine();
            ImGui::BeginDisabled(i + 1u == emitter.clips.size());
            if (ImGui::SmallButton(TranslateLabel("particles.clip_down", "Move Down##down").c_str())) { moveIndex = static_cast<int>(i); move = 1; }
            ImGui::EndDisabled();
            if (ImGui::SmallButton(TranslateLabel("particles.duplicate_clip", "Duplicate##duplicate").c_str())) duplicate = static_cast<int>(i);
            ImGui::SameLine();
            if (ImGui::SmallButton(TranslateLabel("particles.delete_clip", "Delete##delete").c_str())) remove = static_cast<int>(i);
        }
        ImGui::PopID();
    }
    if (moveIndex >= 0)
    {
        std::swap(emitter.clips[moveIndex], emitter.clips[moveIndex + move]);
        source = moveIndex + move;
        discreteEdit = true;
    }
    else if (duplicate >= 0)
    {
        ParticleClip clip = emitter.clips[duplicate];
        clip.name = UniqueParticleClipName(emitter);
        emitter.clips.push_back(std::move(clip));
        source = static_cast<int>(emitter.clips.size()) - 1;
        discreteEdit = true;
    }
    else if (remove >= 0)
    {
        emitter.clips.erase(emitter.clips.begin() + remove);
        source = -1;
        discreteEdit = true;
    }
    ImGui::TreePop();
    return discreteEdit;
}

static ParticleFrameSettings ParticlePreviewSettings(const ParticleEmitterComponent& emitter, int source)
{
    if (source < 0 || source >= static_cast<int>(emitter.clips.size())) return emitter.FrameSettings();
    const auto& clip = emitter.clips[source];
    return {clip.timing == ParticleClipTiming::Rate ? ParticleFrameAnimation::Rate : ParticleFrameAnimation::OverLifetime,
        clip.first, clip.last, clip.fps, clip.loop, false};
}

static void DrawParticleFramePreview(const ParticleEmitterComponent& emitter,
    const AssetBrowserState::TextureThumbnail& texture, int& source)
{
    using namespace engine::particles;
    auto* storage = ImGui::GetStateStorage();
    const ImGuiID stateID = ImGui::GetID("##particle_preview_state");
    ImGui::SeparatorText(Translate("particles.frame_preview"));
    const char* sourceName = source < 0 ? Translate("particles.preview_emitter") : emitter.clips[source].name.c_str();
    std::string sourceRowId;
    if (ImGui::BeginCombo(value_ui::RowId(TranslateLabel("particles.preview_source", "Source##source").c_str(), sourceRowId), sourceName))
    {
        if (ImGui::Selectable(Translate("particles.preview_emitter"), source < 0)) source = -1;
        for (size_t i = 0; i < emitter.clips.size(); ++i)
        {
            ImGui::PushID(static_cast<int>(i));
            if (ImGui::Selectable(emitter.clips[i].name.empty() ? Translate("particles.unnamed_clip") : emitter.clips[i].name.c_str(), source == static_cast<int>(i)))
                source = static_cast<int>(i);
            ImGui::PopID();
        }
        ImGui::EndCombo();
    }
    const auto settings = ParticlePreviewSettings(emitter, source);
    const uint32_t count = ParticleAtlasFrameCount(emitter);
    const auto range = ResolveParticleFrameRange(settings.first, settings.last, count);
    const double lifetime = std::isfinite(emitter.particleLifetime) && emitter.particleLifetime > 0.f ? emitter.particleLifetime : .01;
    const double fps = std::isfinite(settings.fps) && settings.fps > 0.f ? settings.fps : 12.;
    const double duration = settings.mode == ParticleFrameAnimation::Rate ? range.Count() / fps :
        settings.mode == ParticleFrameAnimation::Random ? double(range.Count()) : lifetime;
    const uint64_t savedTime = uint64_t(uint32_t(storage->GetInt(stateID, 0))) |
        (uint64_t(uint32_t(storage->GetInt(stateID + 1u, 0))) << 32u);
    double time = std::bit_cast<double>(savedTime);
    bool playing = storage->GetBool(stateID + 2u, true);
    bool manual = storage->GetBool(stateID + 3u, false);
    if (storage->GetInt(stateID + 4u, -2) != source)
    {
        time = 0.; playing = true; manual = false;
        storage->SetInt(stateID + 4u, source);
    }
    if (ImGui::SmallButton(TranslateLabel(playing ? "particles.preview_pause" : "particles.preview_play", "Play / Pause##play_pause").c_str()))
    {
        playing = !playing;
        manual = false;
    }
    ImGui::SameLine();
    if (ImGui::SmallButton(TranslateLabel("particles.preview_restart", "Restart##restart").c_str()))
    {
        time = 0.; playing = true; manual = false;
    }
    if (playing)
    {
        time += std::clamp(double(ImGui::GetIO().DeltaTime), 0., .1);
        const bool loops = (settings.mode == ParticleFrameAnimation::Rate && settings.loop) || settings.mode == ParticleFrameAnimation::Random;
        if (loops) time = std::fmod(time, duration);
        else if (time >= duration) { time = duration; playing = false; }
    }
    float progress = static_cast<float>(std::clamp(time / duration, 0., 1.));
    std::string progressRowId;
    if (ImGui::SliderFloat(value_ui::RowId(TranslateLabel("particles.preview_progress", "Progress##progress").c_str(), progressRowId), &progress, 0.f, 1.f, "%.2f"))
    {
        time = progress * duration;
        playing = false;
        manual = true;
    }
    const uint64_t timeBits = std::bit_cast<uint64_t>(time);
    storage->SetInt(stateID, static_cast<int>(uint32_t(timeBits)));
    storage->SetInt(stateID + 1u, static_cast<int>(uint32_t(timeBits >> 32u)));
    storage->SetBool(stateID + 2u, playing);
    storage->SetBool(stateID + 3u, manual);
    uint32_t random = static_cast<uint32_t>(std::fmod(std::floor(time), double(UINT32_MAX) + 1.)) * 747796405u + stateID;
    random = (random ^ (random >> 16u)) * 2246822519u;
    double animationTime = time;
    if (settings.mode == ParticleFrameAnimation::Rate && settings.randomStart)
        animationTime += double(stateID) / (double(UINT32_MAX) + 1.) * duration;
    const uint32_t frameIndex = manual
        ? ResolveParticleFrame(ParticleFrameAnimation::OverLifetime, range.first, range.last, 0., false, progress, 1., count)
        : ResolveParticleFrame(settings.mode, range.first, range.last, fps, settings.loop, animationTime, lifetime, count, random);
    ImGui::Text("%s %u", Translate("particles.frame_index"), frameIndex);

    // Measure original frame bounds around the common particle pivot.
    const auto reference = AtlasFrame(emitter, 0u, texture);
    const double referenceHeight = emitter.frames.empty() ? double(texture.height) / std::max(emitter.flipbookRows, 1u) : reference.height;
    const auto sizeOf = [&](uint32_t index) {
        if (emitter.frames.empty()) return math::Vec2{float((double(texture.width) / std::max(emitter.flipbookColumns, 1u)) / referenceHeight), 1.f};
        const auto& frame = emitter.frames[index];
        return math::Vec2{float(frame.width / referenceHeight), float(frame.height / referenceHeight)};
    };
    const auto pivotOf = [&](uint32_t index) {
        if (emitter.frames.empty()) return math::Vec2{.5f, .5f};
        const auto& frame = emitter.frames[index];
        return math::Vec2{frame.pivotX, frame.pivotY};
    };
    const auto referenceSize = sizeOf(0u), referencePivot = pivotOf(0u);
    float minX = -referenceSize.x * referencePivot.x, maxX = referenceSize.x * (1.f - referencePivot.x);
    float minY = -referenceSize.y * referencePivot.y, maxY = referenceSize.y * (1.f - referencePivot.y);
    if (!emitter.frames.empty())
        for (uint32_t i = std::min(range.first, range.last);; ++i)
        {
            const auto size = sizeOf(i), pivot = pivotOf(i);
            minX = std::min(minX, -size.x * pivot.x); maxX = std::max(maxX, size.x * (1.f - pivot.x));
            minY = std::min(minY, -size.y * pivot.y); maxY = std::max(maxY, size.y * (1.f - pivot.y));
            if (i == std::max(range.first, range.last)) break;
        }
    minX = std::min(minX, 0.f); maxX = std::max(maxX, 0.f);
    minY = std::min(minY, 0.f); maxY = std::max(maxY, 0.f);
    const float side = std::clamp(ImGui::GetContentRegionAvail().x, 32.f, 128.f);
    const float scale = (side - 16.f) / std::max({maxX - minX, maxY - minY, .001f});
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    ImGui::Dummy(ImVec2(side, side));
    const ImVec2 end(origin.x + side, origin.y + side);
    const ImVec2 anchor(origin.x + (side - (maxX - minX) * scale) * .5f - minX * scale,
                        origin.y + (side - (maxY - minY) * scale) * .5f - minY * scale);
    auto* draw = ImGui::GetWindowDrawList();
    draw->PushClipRect(origin, end, true);
    ImGui::RenderColorRectWithAlphaCheckerboard(draw, origin, end, IM_COL32(30, 34, 40, 155), 8.f, ImVec2(0.f, 0.f));
    ParticleBillboard billboard;
    billboard.size = sizeOf(frameIndex);
    billboard.pivot = pivotOf(frameIndex);
    const auto frame = AtlasFrame(emitter, frameIndex, texture);
    if (emitter.frames.empty())
    {
        const float w = 1.f / std::max(emitter.flipbookColumns, 1u), h = 1.f / std::max(emitter.flipbookRows, 1u);
        const float x = (frameIndex % std::max(emitter.flipbookColumns, 1u)) * w;
        const float y = (frameIndex / std::max(emitter.flipbookColumns, 1u)) * h;
        billboard.uvRect = {x, y, x + w, y + h};
    }
    std::array<ParticleVertex, 6> vertices;
    if (BuildParticleQuad(billboard, texture.width, texture.height, {1.f, 0.f, 0.f}, {0.f, 1.f, 0.f},
        vertices, emitter.frames.empty() ? nullptr : &frame))
        draw->AddImage(reinterpret_cast<ImTextureID>(texture.imguiId),
            ImVec2(anchor.x + vertices[5].position.x * scale, anchor.y - vertices[5].position.y * scale),
            ImVec2(anchor.x + vertices[1].position.x * scale, anchor.y - vertices[1].position.y * scale),
            ImVec2(vertices[5].u, vertices[5].v), ImVec2(vertices[1].u, vertices[1].v));
    draw->AddRect(ImVec2(anchor.x - referencePivot.x * referenceSize.x * scale, anchor.y - referencePivot.y * referenceSize.y * scale),
        ImVec2(anchor.x + (1.f - referencePivot.x) * referenceSize.x * scale, anchor.y + (1.f - referencePivot.y) * referenceSize.y * scale),
        ImGui::GetColorU32(ImGuiCol_TextDisabled));
    const ImU32 accent = ImGui::GetColorU32(ImGuiCol_PlotHistogram);
    draw->AddLine(ImVec2(anchor.x - 4.f, anchor.y), ImVec2(anchor.x + 4.f, anchor.y), accent);
    draw->AddLine(ImVec2(anchor.x, anchor.y - 4.f), ImVec2(anchor.x, anchor.y + 4.f), accent);
    const float floorY = anchor.y + billboard.size.y * (1.f - billboard.pivot.y) * scale;
    draw->AddLine(ImVec2(origin.x + 4.f, floorY), ImVec2(end.x - 4.f, floorY), accent);
    draw->PopClipRect();
    ImGui::TextWrapped("%s", Translate("particles.preview_legend"));
}

static void DrawParticleFrameEditor(ParticleEmitterComponent& emitter,
    const AssetBrowserState::TextureThumbnail& texture, bool open, bool& discreteEdit)
{
    if (open) ImGui::SetNextItemOpen(true);
    if (!ImGui::TreeNode(TranslateLabel("particles.frames", "Atlas Frames##pe_frames").c_str())) return;
    const uint64_t count = emitter.frames.empty()
        ? uint64_t(std::max(emitter.flipbookColumns, 1u)) * std::max(emitter.flipbookRows, 1u) : emitter.frames.size();
    auto* storage = ImGui::GetStateStorage();
    const ImGuiID selectionID = ImGui::GetID("##frame_selection");
    int selected = std::clamp(storage->GetInt(selectionID, 0), 0,
        static_cast<int>(std::min(count - 1u, uint64_t(std::numeric_limits<int>::max()))));
    value_ui::DragInt(TranslateLabel("particles.frame_index", "Frame Index##pe_frame_index").c_str(),
        &selected, 0.2f, 0, static_cast<int>(std::min(count - 1u, uint64_t(std::numeric_limits<int>::max()))));
    storage->SetInt(selectionID, selected);
    const ImGuiID sourceID = ImGui::GetID("##preview_source");
    int source = storage->GetInt(sourceID, -1);
    discreteEdit |= DrawParticleClips(emitter, static_cast<uint32_t>(count), source);
    if (source < -1 || source >= static_cast<int>(emitter.clips.size())) source = -1;

    if (!emitter.frames.empty() && ImGui::SmallButton(TranslateLabel("particles.use_grid", "Use Grid##pe_grid").c_str()))
    {
        emitter.frames.clear();
        discreteEdit = true;
        ImGui::TreePop();
        return;
    }
    if (texture.width == 0u || texture.height == 0u)
    {
        ImGui::TreePop();
        return;
    }
    const float scale = std::max(0.000001f, std::min({ImGui::GetContentRegionAvail().x / texture.width,
        256.f / texture.height, 1.f}));
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const ImVec2 size(texture.width * scale, texture.height * scale);
    ImGui::InvisibleButton("##atlas_canvas", size);
    const bool hovered = ImGui::IsItemHovered();
    const bool active = ImGui::IsItemActive();
    const bool clicked = ImGui::IsItemClicked();
    const auto mouse = ImGui::GetIO().MousePos;
    const ImGuiID dragID = ImGui::GetID("##atlas_drag");
    auto frame = AtlasFrame(emitter, static_cast<uint32_t>(selected), texture);
    if (clicked)
    {
        const float px = (mouse.x - origin.x) / scale, py = (mouse.y - origin.y) / scale;
        const ImVec2 corner(origin.x + float((double(frame.x) + frame.width) * scale),
                            origin.y + float((double(frame.y) + frame.height) * scale));
        const ImVec2 pivot(origin.x + float((double(frame.x) + frame.width * frame.pivotX) * scale),
                           origin.y + float((double(frame.y) + frame.height * frame.pivotY) * scale));
        int mode = 0;
        if (std::abs(mouse.x - corner.x) <= 8.f && std::abs(mouse.y - corner.y) <= 8.f) mode = 1;
        else if (std::abs(mouse.x - pivot.x) <= 8.f && std::abs(mouse.y - pivot.y) <= 8.f) mode = 2;
        else
        {
            for (uint32_t i = 0u; i < std::min(count, uint64_t(4096u)); ++i)
            {
                const auto candidate = AtlasFrame(emitter, i, texture);
                if (px >= candidate.x && py >= candidate.y && px < double(candidate.x) + candidate.width && py < double(candidate.y) + candidate.height)
                {
                    selected = static_cast<int>(i);
                    frame = candidate;
                    storage->SetInt(selectionID, selected);
                    break;
                }
            }
        }
        storage->SetInt(dragID, mode);
        storage->SetFloat(dragID + 1u, mouse.x);
        storage->SetFloat(dragID + 2u, mouse.y);
        storage->SetInt(dragID + 3u, static_cast<int>(frame.x));
        storage->SetInt(dragID + 4u, static_cast<int>(frame.y));
        storage->SetInt(dragID + 5u, static_cast<int>(frame.width));
        storage->SetInt(dragID + 6u, static_cast<int>(frame.height));
        storage->SetFloat(dragID + 7u, frame.pivotX);
        storage->SetFloat(dragID + 8u, frame.pivotY);
    }
    if (active && ImGui::IsMouseDragging(ImGuiMouseButton_Left) && MakeExplicitAtlas(emitter, texture))
    {
        const double dx = std::round((mouse.x - storage->GetFloat(dragID + 1u)) / scale);
        const double dy = std::round((mouse.y - storage->GetFloat(dragID + 2u)) / scale);
        frame = {static_cast<uint32_t>(storage->GetInt(dragID + 3u)), static_cast<uint32_t>(storage->GetInt(dragID + 4u)),
                 static_cast<uint32_t>(storage->GetInt(dragID + 5u)), static_cast<uint32_t>(storage->GetInt(dragID + 6u)),
                 storage->GetFloat(dragID + 7u), storage->GetFloat(dragID + 8u)};
        const int mode = storage->GetInt(dragID);
        const double max = std::numeric_limits<uint32_t>::max();
        if (mode == 1)
        {
            frame.width = static_cast<uint32_t>(std::clamp(double(frame.width) + dx, 1., max - frame.x));
            frame.height = static_cast<uint32_t>(std::clamp(double(frame.height) + dy, 1., max - frame.y));
        }
        else if (mode == 2)
        {
            frame.pivotX += static_cast<float>(dx / frame.width);
            frame.pivotY += static_cast<float>(dy / frame.height);
        }
        else
        {
            frame.x = static_cast<uint32_t>(std::clamp(double(frame.x) + dx, 0., max - frame.width));
            frame.y = static_cast<uint32_t>(std::clamp(double(frame.y) + dy, 0., max - frame.height));
        }
        if (frame.IsValid()) emitter.frames[selected] = frame;
    }
    auto* draw = ImGui::GetWindowDrawList();
    const ImVec2 end(origin.x + size.x, origin.y + size.y);
    draw->PushClipRect(origin, end, true);
    ImGui::RenderColorRectWithAlphaCheckerboard(draw, origin, end, IM_COL32(30, 34, 40, 155), 8.f, ImVec2(0.f, 0.f));
    draw->AddImage(reinterpret_cast<ImTextureID>(texture.imguiId), origin, end);
    const auto preview = ParticlePreviewSettings(emitter, source);
    const auto outlined = engine::particles::ResolveParticleFrameRange(preview.first, preview.last, static_cast<uint32_t>(count));
    for (uint32_t i = 0u; i < std::min(count, uint64_t(4096u)); ++i)
    {
        const auto rectangle = AtlasFrame(emitter, i, texture);
        const bool inRange = i >= std::min(outlined.first, outlined.last) && i <= std::max(outlined.first, outlined.last);
        const ImU32 color = ImGui::GetColorU32(i == static_cast<uint32_t>(selected) ? ImGuiCol_PlotHistogram :
            inRange ? ImGuiCol_PlotLines : ImGuiCol_Border);
        draw->AddRect(ImVec2(origin.x + rectangle.x * scale, origin.y + rectangle.y * scale),
            ImVec2(origin.x + float((double(rectangle.x) + rectangle.width) * scale),
                   origin.y + float((double(rectangle.y) + rectangle.height) * scale)), color, 0.f, 0, i == static_cast<uint32_t>(selected) ? 2.f : 1.f);
    }
    frame = AtlasFrame(emitter, static_cast<uint32_t>(selected), texture);
    const ImVec2 corner(origin.x + float((double(frame.x) + frame.width) * scale), origin.y + float((double(frame.y) + frame.height) * scale));
    const ImVec2 pivot(origin.x + float((double(frame.x) + frame.width * frame.pivotX) * scale),
                       origin.y + float((double(frame.y) + frame.height * frame.pivotY) * scale));
    draw->AddRectFilled(ImVec2(corner.x - 5.f, corner.y - 5.f), corner, ImGui::GetColorU32(ImGuiCol_PlotHistogram));
    draw->AddCircle(pivot, 5.f, ImGui::GetColorU32(ImGuiCol_PlotHistogram), 12, 2.f);
    draw->PopClipRect();
    if (hovered) ImGui::SetTooltip(Translate("particles.frame_drag_tip"));
    ImGui::TextWrapped("%s", Translate("particles.frame_drag_tip"));
    if (count > 4096u && emitter.frames.empty())
        value_ui::TextDisabled(Translate("particles.grid_edit_limit"));
    else
    {
        uint32_t rect[] = {frame.x, frame.y, frame.width, frame.height};
        const uint32_t min = 0u, max = std::numeric_limits<uint32_t>::max();
        std::string rectRowId;
        bool changed = ImGui::DragScalarN(value_ui::RowId(TranslateLabel("particles.frame_rect", "X / Y / W / H##pe_rect").c_str(), rectRowId),
            ImGuiDataType_U32, rect, 4, 1.f, &min, &max, "%u", ImGuiSliderFlags_AlwaysClamp);
        frame.x = rect[0]; frame.y = rect[1]; frame.width = rect[2]; frame.height = rect[3];
        float pivots[] = {frame.pivotX, frame.pivotY};
        changed |= value_ui::DragFloat2(TranslateLabel("particles.frame_pivot", "Pivot X / Y##pe_pivot").c_str(), pivots, 0.01f);
        frame.pivotX = pivots[0]; frame.pivotY = pivots[1];
        if (changed && frame.IsValid() && MakeExplicitAtlas(emitter, texture))
            emitter.frames[selected] = frame;
    }
    DrawParticleFramePreview(emitter, texture, source);
    storage->SetInt(sourceID, source);
    ImGui::TreePop();
}

static void DrawParticleEmitterSection(EditorFrameContext& ctx, EntityID entity)
{
    auto* emitter = ctx.world.Get<ParticleEmitterComponent>(entity);
    if (!emitter)
        return;

    const bool open = ComponentHeader(TranslateLabel("component.particles", "Partikel-Emitter##particles_hdr").c_str(), ComponentIcon::Particles);
    if (BeginComponentContextMenu("##particles_hdr_ctx"))
    {
        DrawComponentClipboardMenuItems<ParticleEmitterComponent>(ctx, entity, "component.particles");
        ImGui::Separator();
        if (ImGui::MenuItem(TranslateLabel("component.remove_particles", "Partikel-Emitter entfernen").c_str()))
        {
            ExecuteSceneMutation(ctx, "history.remove_component", [&]() { ctx.world.Remove<ParticleEmitterComponent>(entity); });
            ImGui::EndPopup();
            return;
        }
        ImGui::EndPopup();
    }
    if (!open)
        return;
    ComponentUndoScope<ParticleEmitterComponent> undo(ctx, entity, "particle_emitter", "history.edit_particles");
    bool discreteEdit = false;

    // Wiedergabe
    value_ui::Checkbox(TranslateLabel("field.enabled", "Aktiv##pe_playing").c_str(), &emitter->playing);
    value_ui::Checkbox(TranslateLabel("field.loop", "Schleife##pe_loop").c_str(), &emitter->looping);
    ImGui::SameLine();
    const std::string restartLabel = std::string(ICON_LC_REFRESH_CW "  ") + Translate("common.restart") + "##pe_restart";
    if (ImGui::SmallButton(restartLabel.c_str()) && ctx.restartParticleEmitter)
        ctx.restartParticleEmitter(entity);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip(Translate("particles.restart_tip"));
    ImGui::BeginDisabled(!emitter->looping);
    value_ui::Checkbox(TranslateLabel("particles.prewarm", "Prewarm##pe_prewarm").c_str(), &emitter->prewarm);
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip(Translate("particles.prewarm_tip"));
    ImGui::EndDisabled();
    value_ui::DragFloat(TranslateLabel("particles.duration", "Dauer (s)##pe_duration").c_str(), &emitter->duration, 0.05f, 0.f, 600.f, "%.2f");
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip(emitter->looping
            ? Translate("particles.loop_duration_tip")
            : Translate("particles.duration_tip"));

    // Emission
    ImGui::SeparatorText(Translate("particles.emission"));
    value_ui::DragFloat(TranslateLabel("particles.rate", "Rate (pro s)##pe_rate").c_str(), &emitter->emitRate, 0.5f, 0.f, 10000.f, "%.1f");
    int burst = static_cast<int>(emitter->burstCount);
    if (value_ui::DragInt(TranslateLabel("particles.initial_burst", "Burst beim Start##pe_burst").c_str(), &burst, 1.f, 0, 100000))
        emitter->burstCount = static_cast<uint32_t>(std::max(burst, 0));
    int maxParticles = static_cast<int>(emitter->maxParticles);
    if (value_ui::DragInt(TranslateLabel("particles.max_count", "Max. Partikel##pe_max").c_str(), &maxParticles, 10.f, 1, 100000))
        emitter->maxParticles = static_cast<uint32_t>(std::max(maxParticles, 1));
    value_ui::DragFloat(TranslateLabel("particles.lifetime", "Lebensdauer (s)##pe_life").c_str(), &emitter->particleLifetime, 0.01f, 0.01f, 60.f, "%.2f");
    value_ui::SliderFloat(TranslateLabel("particles.lifetime_random", "Lebensdauer-Zufall##pe_liferand").c_str(), &emitter->lifetimeRandom, 0.f, 1.f, "%.2f");
    int spawnShape = static_cast<int>(emitter->spawnShape);
    const char* shapes[] = {Translate("particles.shape_sphere"), Translate("particles.shape_box"),
                            Translate("particles.shape_torus"), Translate("particles.shape_plane")};
    if (value_ui::Combo(TranslateLabel("particles.spawn_shape", "Spawn Shape##pe_shape").c_str(),
                        &spawnShape, shapes, IM_ARRAYSIZE(shapes)))
        emitter->spawnShape = static_cast<ParticleSpawnShape>(spawnShape);
    switch (emitter->spawnShape)
    {
    case ParticleSpawnShape::Sphere:
        value_ui::DragFloat(TranslateLabel("field.radius", "Radius##pe_radius").c_str(), &emitter->emitterRadius, 0.01f, 0.f, 100.f, "%.2f");
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip(Translate("particles.radius_tip"));
        break;
    case ParticleSpawnShape::Box:
        value_ui::DragFloat3(TranslateLabel("particles.box_size", "Size X / Y / Z##pe_box").c_str(), &emitter->boxSize.x, 0.01f, 0.f, 200.f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
        break;
    case ParticleSpawnShape::Torus:
    {
        const bool ringChanged = value_ui::DragFloat(TranslateLabel("particles.torus_radius", "Ring Radius##pe_ring").c_str(), &emitter->torusRadius, 0.01f, 0.f, 100.f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
        const bool tubeChanged = value_ui::DragFloat(TranslateLabel("particles.torus_tube_radius", "Tube Radius##pe_tube").c_str(), &emitter->torusTubeRadius, 0.01f, 0.f, std::max(emitter->torusRadius, 0.f), "%.2f", ImGuiSliderFlags_AlwaysClamp);
        if (ringChanged || tubeChanged)
            emitter->torusTubeRadius = std::clamp(emitter->torusTubeRadius, 0.f, std::max(emitter->torusRadius, 0.f));
        break;
    }
    case ParticleSpawnShape::Plane:
        value_ui::DragFloat(TranslateLabel("particles.plane_width", "Width (X)##pe_plane_width").c_str(), &emitter->planeWidth, 0.01f, 0.f, 200.f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
        value_ui::DragFloat(TranslateLabel("particles.plane_depth", "Depth (Z)##pe_plane_depth").c_str(), &emitter->planeDepth, 0.01f, 0.f, 200.f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
        break;
    }

    // Bewegung
    ImGui::SeparatorText(Translate("particles.motion"));
    value_ui::DragFloat3(TranslateLabel("particles.velocity", "Startgeschwindigkeit##pe_vel").c_str(), &emitter->initialVelocity.x, 0.05f, -1000.f, 1000.f, "%.2f");
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip(Translate("particles.local_velocity_tip"));
    value_ui::DragFloat3(TranslateLabel("particles.velocity_random", "Geschw.-Zufall (+/-)##pe_velrand").c_str(), &emitter->velocityRandom.x, 0.05f, 0.f, 1000.f, "%.2f");
    value_ui::DragFloat3(TranslateLabel("particles.gravity", "Schwerkraft##pe_gravity").c_str(), &emitter->gravity.x, 0.05f, -1000.f, 1000.f, "%.2f");
    value_ui::DragFloat(TranslateLabel("particles.drag", "Luftwiderstand##pe_drag").c_str(), &emitter->drag, 0.01f, 0.f, 50.f, "%.2f");

    // Boden
    ImGui::SeparatorText(Translate("particles.floor"));
    value_ui::Checkbox(TranslateLabel("particles.floor_collision", "Bodenkollision##pe_floor").c_str(), &emitter->collideWithFloor);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip(Translate("particles.floor_tip"));
    if (emitter->collideWithFloor)
    {
        value_ui::Checkbox(TranslateLabel("particles.relative_floor", "Relativ zum Emitter##pe_floor_rel").c_str(), &emitter->floorRelative);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip(Translate("particles.relative_floor_tip"));
        value_ui::DragFloat(emitter->floorRelative ? TranslateLabel("particles.relative_height", "Hoehe (relativ)##pe_floor_h").c_str() : TranslateLabel("particles.world_height", "Hoehe (Welt-Y)##pe_floor_h").c_str(),
                         &emitter->floorHeight, 0.01f, -1000.f, 1000.f, "%.2f m");
        value_ui::Checkbox(TranslateLabel("particles.die_on_floor", "Beim Aufprall verschwinden##pe_floor_die").c_str(), &emitter->dieOnFloor);
        if (!emitter->dieOnFloor)
        {
            value_ui::SliderFloat(TranslateLabel("particles.bounce", "Abprallen##pe_bounce").c_str(), &emitter->bounce, 0.f, 1.f, "%.2f");
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip(Translate("particles.bounce_tip"));
            value_ui::SliderFloat(TranslateLabel("physics.friction", "Reibung##pe_floor_friction").c_str(), &emitter->floorFriction, 0.f, 1.f, "%.2f");
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip(Translate("particles.floor_friction_tip"));
        }
    }

    // Aussehen
    ImGui::SeparatorText(Translate("particles.appearance"));
    int blend = emitter->blendMode == ParticleBlendMode::Additive ? 1 : 0;
    const char* blendModes[] = { Translate("particles.alpha"), Translate("particles.additive") };
    if (value_ui::Combo(TranslateLabel("particles.blending", "Mischung##pe_blend").c_str(), &blend, blendModes, IM_ARRAYSIZE(blendModes)))
        emitter->blendMode = blend == 1 ? ParticleBlendMode::Additive : ParticleBlendMode::Alpha;
    value_ui::DragFloat(TranslateLabel("particles.start_size", "Startgroesse##pe_size0").c_str(), &emitter->startSize, 0.005f, 0.f, 100.f, "%.3f");
    value_ui::DragFloat(TranslateLabel("particles.end_size", "Endgroesse##pe_size1").c_str(), &emitter->endSize, 0.005f, 0.f, 100.f, "%.3f");
    value_ui::ColorEdit4(TranslateLabel("particles.start_color", "Startfarbe##pe_col0").c_str(), &emitter->startColor.x);
    value_ui::ColorEdit4(TranslateLabel("particles.end_color", "Endfarbe##pe_col1").c_str(), &emitter->endColor.x);
    value_ui::DragFloat(TranslateLabel("field.intensity", "Intensitaet##pe_intensity").c_str(), &emitter->intensity, 0.05f, 0.f, 100.f, "%.2f");
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip(Translate("particles.bloom_tip"));

    // Textur: Textfeld + Drop-Ziel fuer Bilder aus dem Asset-Browser.
    // Custom frames are pixel rectangles of the old texture, so a new texture
    // starts again with the regular grid (one undo step restores them).
    const std::string previousTexture = emitter->texturePath;
    ImGui::SeparatorText(Translate("field.texture"));
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
                discreteEdit = true;
            }
        }
        ImGui::EndDragDropTarget();
    }
    if (emitter->texturePath.empty())
        value_ui::TextDisabled(Translate("particles.default_texture"));
    else
    {
        if (const auto* thumbnail = GetAssetTextureThumbnail(ctx, emitter->texturePath))
        {
            const float limit = std::clamp(ImGui::GetContentRegionAvail().x, 1.f, 88.f);
            const float aspect = std::max(thumbnail->aspect, 0.01f);
            const ImVec2 size(limit * std::min(aspect, 1.f), limit / std::max(aspect, 1.f));
            const ImVec2 min = ImGui::GetCursorScreenPos();
            ImGui::RenderColorRectWithAlphaCheckerboard(ImGui::GetWindowDrawList(), min,
                ImVec2(min.x + size.x, min.y + size.y), IM_COL32(30, 34, 40, 155), 6.f, ImVec2(0.f, 0.f));
            ImGui::Image(reinterpret_cast<ImTextureID>(thumbnail->imguiId), size);
            const bool openFrames = ImGui::IsItemClicked();
            DrawParticleFrameEditor(*emitter, *thumbnail, openFrames, discreteEdit);
        }
        if (ImGui::SmallButton(TranslateLabel("particles.remove_texture", "Textur entfernen##pe_tex_clear").c_str()))
        {
            emitter->texturePath.clear();
            discreteEdit = true;
        }
    }

    if (emitter->texturePath != previousTexture && !emitter->frames.empty())
        emitter->frames.clear();

    // Always editable: changing the grid while custom frames exist switches
    // back to the grid, like "Use Grid" in the frame editor.
    int columns = static_cast<int>(emitter->flipbookColumns);
    int rows = static_cast<int>(emitter->flipbookRows);
    const bool customFrames = !emitter->frames.empty();
    bool gridChanged = false;
    std::string flipbookRowsId;
    const char* flipbookRowsLabel = value_ui::RowId(TranslateLabel("particles.flipbook", "Flipbook (Spalten x Zeilen)##pe_fb_rows").c_str(), flipbookRowsId);
    const float flipbookFieldWidth = std::max(1.f, (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x) * 0.5f);
    ImGui::SetNextItemWidth(flipbookFieldWidth);
    if (value_ui::DragInt("##pe_fb_cols", &columns, 0.1f, 1, 64))
    {
        emitter->flipbookColumns = static_cast<uint32_t>(std::clamp(columns, 1, 64));
        gridChanged = true;
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip(Translate(customFrames ? "particles.flipbook_custom_tip" : "particles.flipbook_tip"));
    ImGui::SameLine();
    ImGui::SetNextItemWidth(flipbookFieldWidth);
    if (value_ui::DragInt(flipbookRowsLabel, &rows, 0.1f, 1, 64))
    {
        emitter->flipbookRows = static_cast<uint32_t>(std::clamp(rows, 1, 64));
        gridChanged = true;
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip(Translate(customFrames ? "particles.flipbook_custom_tip" : "particles.flipbook_tip"));
    if (gridChanged && customFrames)
        emitter->frames.clear();
    if (customFrames && !gridChanged)
        value_ui::TextDisabled(Translate("particles.flipbook_custom_active"));
    DrawEmitterFrameAnimation(*emitter);
    if (discreteEdit) undo.CommitOnExit();
}

static void DrawCapsuleColliderSection(EditorFrameContext& ctx, EntityID entity)
{
    auto* capsule = ctx.world.Get<CapsuleColliderComponent>(entity);
    if (!capsule)
        return;

    const bool open = ComponentHeader(TranslateLabel("physics.capsule", "Kollision (Kapsel)##capsule_hdr").c_str(), ComponentIcon::Collider);
    if (BeginComponentContextMenu("##capsule_hdr_ctx"))
    {
        DrawComponentClipboardMenuItems<CapsuleColliderComponent>(ctx, entity, "physics.capsule");
        ImGui::Separator();
        if (ImGui::MenuItem(TranslateLabel("physics.remove_capsule", "Kapsel-Kollision entfernen").c_str()))
        {
            ExecuteSceneMutation(ctx, "history.remove_component", [&]() { ctx.world.Remove<CapsuleColliderComponent>(entity); });
            ImGui::EndPopup();
            return;
        }
        ImGui::EndPopup();
    }
    if (!open)
        return;
    ComponentUndoScope<CapsuleColliderComponent> undo(ctx, entity, "capsule", "history.edit_capsule");

    value_ui::Checkbox(TranslateLabel("physics.capsule_visible", "Sichtbar (Kapsel anzeigen)##capsule_vis").c_str(), &capsule->showInEditor);
    DrawColliderOwnerInfo(ctx, entity, "capsule");
    ImGui::Separator();

    static const char* kAxisNames[] = { "X", "Y", "Z" };
    int axis = static_cast<int>(capsule->axis);
    if (value_ui::Combo(TranslateLabel("physics.axis", "Achse##capsule_axis").c_str(), &axis, kAxisNames, IM_ARRAYSIZE(kAxisNames)))
        capsule->axis = static_cast<CapsuleAxis>(axis);
    value_ui::DragFloat(TranslateLabel("field.radius", "Radius##capsule_radius").c_str(), &capsule->radius, 0.01f, 0.001f, 10000.f, "%.3f");
    capsule->radius = std::max(capsule->radius, 0.001f);
    value_ui::DragFloat(TranslateLabel("field.height", "Hoehe##capsule_height").c_str(), &capsule->height, 0.01f, 0.002f, 10000.f, "%.3f");
    capsule->height = std::max(capsule->height, 0.002f);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip(Translate("physics.capsule_height_tip"));
    value_ui::RowLabel(Translate("physics.center_offset"));
    value_ui::DragFloat3("##capsule_center", &capsule->centerOffset.x, 0.01f, -10000.f, 10000.f, "%.3f");
}

static void DrawOBBSection(EditorFrameContext& ctx, EntityID entity)
{
    // Sektion nur anzeigen wenn eine OBB-Komponente vorhanden ist.
    // Hinzufügen läuft über Rechtsklick im Inspector → "+ Komponente".
    auto* obb = ctx.world.Get<OBBComponent>(entity);
    if (!obb)
        return;

    const bool obbOpen = ComponentHeader(TranslateLabel("physics.box", "Kollision##obb_hdr").c_str(), ComponentIcon::Collider);
    if (BeginComponentContextMenu("##obb_hdr_ctx"))
    {
        DrawComponentClipboardMenuItems<OBBComponent>(ctx, entity, "physics.box");
        ImGui::Separator();
        if (ImGui::MenuItem(TranslateLabel("physics.remove_box", "Kollision entfernen").c_str()))
        {
            ExecuteSceneMutation(ctx, "history.remove_component", [&]() { ctx.world.Remove<OBBComponent>(entity); });
            ctx.state.obbDragFaceIdx = -1;
        }
        ImGui::EndPopup();
    }
    if (!obbOpen)
        return;
    ComponentUndoScope<OBBComponent, ObbEqual> undo(ctx, entity, "obb", "history.edit_box");

    value_ui::Checkbox(TranslateLabel("physics.box_visible", "Sichtbar (OBB anzeigen)##obb_vis").c_str(), &obb->showInEditor);
    value_ui::Checkbox("Trigger##obb_trigger", &obb->isTrigger);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip(Translate("physics.trigger_tip"));

    if (!obb->isTrigger)
        DrawColliderOwnerInfo(ctx, entity, "obb");
    ImGui::Separator();

    // Vollgroesse anzeigen, intern als halfExtents speichern
    math::Vec3 size = obb->halfExtents * 2.f;
    value_ui::RowLabel(Translate("physics.box_size"));
    if (value_ui::DragFloat3("##obb_size", &size.x, 0.01f, 0.001f, 10000.f, "%.3f"))
    {
        obb->halfExtents.x = std::max(0.001f, size.x * 0.5f);
        obb->halfExtents.y = std::max(0.001f, size.y * 0.5f);
        obb->halfExtents.z = std::max(0.001f, size.z * 0.5f);
    }

    value_ui::RowLabel(Translate("physics.center_offset"));
    value_ui::DragFloat3("##obb_center", &obb->centerOffset.x, 0.01f, -10000.f, 10000.f, "%.3f");

    ImGui::Separator();
    if (ImGui::Button(TranslateLabel("physics.fit_mesh", "Aus Mesh berechnen##obb_auto").c_str()))
    {
        if (const auto* b = ctx.world.Get<BoundsComponent>(entity))
        {
            obb->centerOffset = b->centerLocal;
            obb->halfExtents  = b->extentsLocal;
            obb->orientation  = math::Quat::Identity();
        }
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip(Translate("physics.fit_box_tip"));
}

struct InspectorDragResult { bool changed; bool labelClicked; };
static InspectorDragResult InspectorDragFloat3(const char* label, const char* id,
                                      float* v, float speed,
                                      const ImVec4* labelColor = nullptr,
                                      float vmin = 0.f, float vmax = 0.f)
{
    value_ui::InlineLabelScope rows;
    if (labelColor) ImGui::PushStyleColor(ImGuiCol_Text, *labelColor);
    value_ui::RowLabel(label);
    if (labelColor) ImGui::PopStyleColor();
    const bool labelClicked = ImGui::IsItemClicked();
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip(Translate("transform.change_tool"));
    ImGui::SetNextItemWidth(-FLT_MIN);
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
        ? std::string(Translate("environment.no_hdr"))
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
    const char* dropText = Translate("environment.drop_hdr");
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
        if (ImGui::SmallButton(TranslateLabel("common.remove", "Entfernen##skybox").c_str()))
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
    if (ComponentHeader(TranslateLabel("environment.title", "Environment").c_str(), ComponentIcon::Environment, ImGuiTreeNodeFlags_DefaultOpen))
    {
        DrawSkyboxDropZone(ctx);
    }

    // ── Environment Lighting ──────────────────────────────────────────────────
    if (ComponentHeader(TranslateLabel("environment.lighting", "Environment Lighting").c_str(), ComponentIcon::Environment, ImGuiTreeNodeFlags_DefaultOpen))
    {
        value_ui::InlineLabelScope rows;
        int src = static_cast<int>(ctx.state.ambientSource);
        const char* sources[] = { "Skybox", Translate("environment.gradient"), Translate("field.color") };
        if (value_ui::Combo(TranslateLabel("environment.source", "Source").c_str(), &src, sources, IM_ARRAYSIZE(sources)))
        {
            ctx.state.ambientSource        = static_cast<AmbientSource>(src);
            ctx.state.environmentEnableIBL = (ctx.state.ambientSource == AmbientSource::Skybox);
            ApplyEnvironmentFromState(ctx);
        }

        // IBL Intensity – nur wenn Skybox-Quelle aktiv; Rebake nur beim Loslassen der Maus
        if (ctx.state.ambientSource == AmbientSource::Skybox)
        {
            float envIntensity = ctx.state.environmentIntensity;
            value_ui::RowLabel(Translate("environment.ibl_intensity"));
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
            value_ui::RowLabel(Translate("environment.sky"));
            ambientChanged |= value_ui::ColorEdit3("##ambSky", &ctx.state.ambientColor.x,
                                                ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_NoLabel);
            value_ui::RowLabel(Translate("environment.ground"));
            ambientChanged |= value_ui::ColorEdit3("##ambGnd", &ctx.state.ambientColorGround.x,
                                                ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_NoLabel);

            float ambientIntensity = ctx.state.ambientIntensity;
            value_ui::RowLabel(Translate("environment.brightness"));
            if (value_ui::DragFloat("##ambientIntensity", &ambientIntensity, 0.02f, 0.0f, 10.0f, "%.2f",
                                 ImGuiSliderFlags_AlwaysClamp))
            {
                ctx.state.ambientIntensity = ambientIntensity;
                ambientChanged = true;
            }
        }
        else if (ctx.state.ambientSource == AmbientSource::Color)
        {
            value_ui::RowLabel(Translate("field.color"));
            ambientChanged |= value_ui::ColorEdit3("##ambColor", &ctx.state.ambientColor.x,
                                                ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_NoLabel);

            float ambientIntensity = ctx.state.ambientIntensity;
            value_ui::RowLabel(Translate("environment.brightness"));
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
        ctx.lastFileMessage = Translate("environment.updated");
    }
    else
    {
        ctx.lastFileMessage = Translate("environment.load_failed");
    }
}

void DrawSceneSettingsPanel(EditorFrameContext& ctx)
{
    const auto inspectorLabel = TranslateLabel("panel.inspector", "Inspector##editor");
    const auto* settings = ImGui::FindWindowSettingsByID(ImHashStr(inspectorLabel.c_str()));
    // Layouts saved before this panel existed can use the Inspector's dock.
    if (settings && settings->DockId && ImGui::DockBuilderGetNode(settings->DockId))
        ImGui::SetNextWindowDockID(settings->DockId,
            ctx.dockSceneSettingsWithInspector ? ImGuiCond_Always : ImGuiCond_FirstUseEver);
    ctx.dockSceneSettingsWithInspector = false;

    SolidEditorPanelStyleScope solidPanelStyle;
    if (ctx.focusSceneSettingsPanel)
    {
        ImGui::SetNextWindowFocus();
        ctx.focusSceneSettingsPanel = false;
    }
    const std::string sceneTitle = TranslateLabel("panel.scene", "Scene##editor");
    KeepNextWindowInMainViewport(sceneTitle);
    if (ImGui::Begin(sceneTitle.c_str(), &ctx.sceneSettingsPanelVisible,
        kAlignedPanelFlags | ImGuiWindowFlags_NoFocusOnAppearing))
    {
        if (!ctx.inspectorPanelVisible)
            ctx.inspectorPanelWidth = ImGui::GetWindowWidth();
        const std::string pasteKey = MakeEditHistoryKey(NULL_ENTITY, "property.paste");
        value_ui::Scope valueScope(
            [&] { BeginPendingSceneEdit(ctx, pasteKey, "history.paste_value"); },
            [&] { CommitPendingSceneEdit(ctx, pasteKey); });
        DrawEnvironmentInspector(ctx);
    }
    ImGui::End();
}

void DrawInspector(EditorFrameContext& ctx)
{
    SolidEditorPanelStyleScope solidPanelStyle;
    const std::string inspectorTitle = TranslateLabel("panel.inspector", "Inspector##editor");
    KeepNextWindowInMainViewport(inspectorTitle);
    if (!ImGui::Begin(inspectorTitle.c_str(), &ctx.inspectorPanelVisible, kAlignedPanelFlags))
    {
        ImGui::End();
        return;
    }
    ctx.inspectorPanelWidth = ImGui::GetWindowWidth();

    const std::string pasteKey = MakeEditHistoryKey(ctx.state.selectedEntity, "property.paste");
    value_ui::Scope valueScope(
        [&] { BeginPendingSceneEdit(ctx, pasteKey, "history.paste_value"); },
        [&] { CommitPendingSceneEdit(ctx, pasteKey); });

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
                value_ui::Text(Translate("prefab.entity_count"), prefab.records.size());
                value_ui::Text(Translate("prefab.root_index"), prefab.rootIndex);
                ImGui::Separator();
                if (ImGui::Button(TranslateLabel("assets.load_into_scene", "In Szene laden").c_str(), ImVec2(-1.f, 0.f)))
                    SpawnPrefabAsset(ctx, prefabPath);
            }
            else
            {
                ImGui::TextWrapped("%s", error.empty() ? Translate("prefab.read_failed") : error.c_str());
            }

            ImGui::End();
            return;
        }

        value_ui::TextDisabled(Translate("inspector.no_selection"));
        ImGui::End();
        return;
    }

    const EntityID selected = ctx.state.selectedEntity;

    // Kamera-Gizmo-Entities können nicht ausgewählt werden (Entity-Liste filtert sie aus),
    // aber sicherheitshalber auch hier abfangen.
    if (ctx.world.Get<EditorCameraGizmoComponent>(selected) != nullptr)
    {
        value_ui::TextDisabled(Translate("inspector.camera_gizmo"));
        ImGui::End();
        return;
    }
    if (ctx.world.Get<EditorRuntimeGizmoTag>(selected) != nullptr)
    {
        value_ui::TextDisabled(Translate("inspector.gizmo"));
        ImGui::End();
        return;
    }
    if (ctx.world.Get<EditorMaterialPreviewComponent>(selected) != nullptr)
    {
        value_ui::TextDisabled(Translate("inspector.material_preview"));
        ImGui::End();
        return;
    }
    if (ctx.world.Get<EditorAssetThumbnailComponent>(selected) != nullptr)
    {
        value_ui::TextDisabled(Translate("inspector.asset_preview"));
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
                // Reset/unpack live in the hierarchy context menu.
                value_ui::TextDisabled("Prefab: %s",PathToUtf8(PathFromUtf8(link->prefabAssetPath).filename()).c_str());
                ImGui::Separator(); break;
            }
            const auto* parent=ctx.world.Get<ParentComponent>(prefabRoot);
            prefabRoot=parent ? parent->parent : NULL_ENTITY;
        }
    }

    // Multi-Selektion: Inspector zeigt Gruppeninfo + editierbare Transform
    if (ctx.state.multiSelection.size() >= 2)
    {
        value_ui::TextDisabled(Translate("inspector.group"), ctx.state.multiSelection.size());
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
            auto mSclResult = InspectorDragFloat3(Translate("transform.scale"), "##mscl", &displayScale.x, 0.01f,
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
            if (value_ui::Checkbox(TranslateLabel("field.active", "Active").c_str(), &activeVal))
            {
                BeginPendingSceneEdit(ctx, "multisel.active", "history.active");
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
            if (value_ui::Checkbox(TranslateLabel("field.visible", "Visible").c_str(), &visibleVal))
            {
                BeginPendingSceneEdit(ctx, "multisel.visible", "history.visible");
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
            if (value_ui::Checkbox(TranslateLabel("field.persistent", "Persistent").c_str(), &persistVal))
            {
                BeginPendingSceneEdit(ctx, "multisel.persistent", "history.persistent");
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
                if (value_ui::Checkbox(TranslateLabel("transform.inherit_scale", "Scale erben").c_str(), &inheritVal))
                {
                    BeginPendingSceneEdit(ctx, "multisel.inheritScale", "history.scale_inheritance");
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
        value_ui::TextDisabled(Translate("inspector.group_tip"));
        ImGui::End();
        return;
    }
    const auto* prefabSource = IsInPrefabMode(ctx.state) ? ctx.world.Get<PrefabSourceComponent>(selected) : nullptr;
    if (prefabSource && !prefabSource->recordId.empty())
    {
        // Beim erneuten Oeffnen werden die Laufzeit-IDs neu vergeben.
        ImGui::PushOverrideID(ImHashStr(prefabSource->recordId.c_str(), 0,
            ImGui::GetID(ctx.state.prefabModePath.c_str())));
    }
    else
    {
        ImGui::PushID(static_cast<int>(GetStableEntityId(ctx.state, selected)));
    }

    // Compact head: entity icon (type and components as tooltip), name, active toggle.
    const bool headIsCamera = ctx.world.Has<CameraComponent>(selected);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(8, 7));
    {
        const float size = ImGui::GetFrameHeight();
        const ImVec2 iconMin = ImGui::GetCursorScreenPos();
        ImGui::Dummy(ImVec2(size, size));
        const ComponentIcon headIcon = headIsCamera ? ComponentIcon::Camera
            : ctx.world.Has<LightComponent>(selected) ? ComponentIcon::Light
            : ctx.world.Has<CharacterControllerComponent>(selected) ? ComponentIcon::Character
            : ctx.world.Has<MeshComponent>(selected) ? ComponentIcon::Mesh
            : ComponentIcon::Transform;
        ImDrawList* headDrawList = ImGui::GetWindowDrawList();
        headDrawList->AddRectFilled(iconMin, ImVec2(iconMin.x + size, iconMin.y + size),
                                    ImGui::GetColorU32(theme::kPanel), ImGui::GetStyle().FrameRounding);
        DrawComponentIcon(*headDrawList, headIcon, ImVec2(iconMin.x + size * 0.5f, iconMin.y + size * 0.5f),
                          ComponentColor(headIcon));
        if (ImGui::BeginItemTooltip())
        {
            ImGui::Text(Translate("inspector.type"), PrimaryEntityType(ctx, selected));
            ImGui::Text(Translate("inspector.components"), ComponentSummary(ctx, selected).c_str());
            ImGui::EndTooltip();
        }
        ImGui::SameLine();
    }
    const float headActiveWidth = headIsCamera ? 0.f : ImGui::GetFrameHeight() + ImGui::GetStyle().ItemSpacing.x;
    const float nameWidth = std::max(1.f, ImGui::GetContentRegionAvail().x - headActiveWidth);

    ImGui::SetNextItemWidth(nameWidth);
    if (auto* nc = ctx.world.Get<NameComponent>(selected))
    {
        char nameBuffer[128]{};
        std::snprintf(nameBuffer, sizeof(nameBuffer), "%s", nc->name.c_str());
        const std::string nameKey = MakeEditHistoryKey(selected, "name");
        if (value_ui::InputText("##EntityName", nameBuffer, sizeof(nameBuffer)))
        {
            BeginPendingSceneEdit(ctx, nameKey, "history.rename_entity");
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
            BeginPendingSceneEdit(ctx, nameKey, "history.rename_entity");
            ctx.world.Add<NameComponent>(selected, NameComponent{nameBuffer});
        }
        if (ImGui::IsItemDeactivatedAfterEdit())
            CommitPendingSceneEdit(ctx, nameKey);
    }
    if (!headIsCamera)
    {
        ImGui::SameLine();
        auto* activeComp = ctx.world.Get<ActiveComponent>(selected);
        bool isActive = (activeComp == nullptr) || activeComp->active;
        if (value_ui::Checkbox("##EntityActive", &isActive))
        {
            BeginPendingSceneEdit(ctx, MakeEditHistoryKey(selected, "active"), "history.active");
            if (activeComp)
                activeComp->active = isActive;
            else
                ctx.world.Add<ActiveComponent>(selected, ActiveComponent{isActive});
            CommitPendingSceneEdit(ctx, MakeEditHistoryKey(selected, "active"));
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip(Translate("inspector.active_tip"));
    }
    ImGui::PopStyleVar();

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
        ImGui::AlignTextToFramePadding();
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
                    BeginPendingSceneEdit(ctx, tagKey, "history.tag");
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
            ImGui::InputTextWithHint("##NewTagInput", Translate("inspector.new_tag"), newTagBuf, sizeof(newTagBuf));
            ImGui::SameLine();
            if (ImGui::SmallButton("+") && newTagBuf[0] != '\0')
            {
                const std::string newT = newTagBuf;
                if (std::find(ctx.state.knownTags.begin(), ctx.state.knownTags.end(), newT)
                    == ctx.state.knownTags.end())
                    ctx.state.knownTags.push_back(newT);
                BeginPendingSceneEdit(ctx, tagKey, "history.tag");
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
                    BeginPendingSceneEdit(ctx, layerKey, "history.layer");
                    if (mesh) mesh->layerMask = (1u << b);
                    CommitPendingSceneEdit(ctx, layerKey);
                }
                if (b == layerIdx) ImGui::SetItemDefaultFocus();
            }
            ImGui::Separator();
            if (ImGui::Selectable(TranslateLabel("layer.edit", "Edit Layers...").c_str()))
                ctx.state.layerNamesWindowOpen = true;
            ImGui::EndCombo();
        }
        if (noLayerSupport) ImGui::EndDisabled();
    }

    // ── Visible / Persistent ─────────────────────────────────────────────────
    {
        const bool isCamera = ctx.world.Has<CameraComponent>(selected);

        if (!isCamera)
        {
            auto* meshComp = ctx.world.Get<MeshComponent>(selected);
            bool isVisible = (meshComp == nullptr) || meshComp->visible;
            const bool hasNoMesh = (meshComp == nullptr);
            if (hasNoMesh) ImGui::BeginDisabled();
            if (value_ui::Checkbox(TranslateLabel("field.visible", "Visible").c_str(), &isVisible))
            {
                BeginPendingSceneEdit(ctx, MakeEditHistoryKey(selected, "visible"), "history.visible");
                if (meshComp)
                    meshComp->visible = isVisible;
                CommitPendingSceneEdit(ctx, MakeEditHistoryKey(selected, "visible"));
            }
            if (hasNoMesh) ImGui::EndDisabled();
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip(
                    Translate("inspector.visible_tip"));
            ImGui::SameLine();
        }
    }

    // ── Persistent-Flag ──────────────────────────────────────────────────────
    {
        bool isPersistent = ctx.world.Has<EditorPersistentComponent>(selected);
        if (value_ui::Checkbox(TranslateLabel("field.persistent", "Persistent").c_str(), &isPersistent))
        {
            if (isPersistent)
                ctx.world.Add<EditorPersistentComponent>(selected, EditorPersistentComponent{});
            else
                ctx.world.Remove<EditorPersistentComponent>(selected);
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip(
                Translate("inspector.persistent_tip"));
        // No separator here: every component header draws its own line above.
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
        // Each component as a card: subtle background, border and its type colour
        // as a stripe down the whole section. The background goes into a lower
        // draw channel because the section height is known only afterwards.
        ImDrawList* cardDrawList = ImGui::GetWindowDrawList();
        constexpr float kCardInset = 7.f;
        value_ui::InlineLabelScope rows;
        for (int section : order)
        {
            currentInspectorSection = section;
            sectionHeaderDrawn = false;
            const float cardLeft = ImGui::GetCursorScreenPos().x - 2.f;
            const float cardRight = ImGui::GetCursorScreenPos().x + ImGui::GetContentRegionAvail().x + 3.f;
            ImDrawListSplitter splitter;
            splitter.Split(cardDrawList, 2);
            splitter.SetCurrentChannel(cardDrawList, 1);
            ImGui::Indent(kCardInset);
            drawers[section](ctx, selected);
            ImGui::Unindent(kCardInset);
            splitter.SetCurrentChannel(cardDrawList, 0);
            if (sectionHeaderDrawn)
            {
                ImGui::Dummy(ImVec2(0.f, 2.f));
                const float bottom = ImGui::GetCursorScreenPos().y - ImGui::GetStyle().ItemSpacing.y + 2.f;
                const ImVec2 cardMin(cardLeft, sectionHeaderTop - kCardInset * 0.5f);
                const ImVec2 cardMax(cardRight, bottom);
                ImVec4 fill = ImGui::GetStyleColorVec4(ImGuiCol_FrameBg);
                fill.w *= 0.45f;
                cardDrawList->AddRectFilled(cardMin, cardMax, ImGui::GetColorU32(fill), 5.f);
                cardDrawList->AddRect(cardMin, cardMax, ImGui::GetColorU32(theme::kBorder), 5.f);
            }
            splitter.Merge(cardDrawList);
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
                        ExecuteSceneMutation(ctx, "history.assign_material", [&]() {
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
    if (EditorActionButton(TranslateLabel("inspector.add_component", "+ Komponente").c_str(), ImVec2(-1.f, 36.f), EditorButtonTone::Primary))
        ImGui::OpenPopup("AddComponentPopup##inspector");

    // ── "Komponente hinzufügen"-Popup (Rechtsklick oder "+" Button) ───────────
    if (ImGui::BeginPopup("AddComponentPopup##inspector"))
    {
        value_ui::TextDisabled(Translate("inspector.add_component_title"));
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
        if (ImGui::MenuItem(TranslateLabel("physics.box", "Kollision (Box)").c_str()))
        {
            OBBComponent fresh;
            if (const auto* b = ctx.world.Get<BoundsComponent>(selected))
            {
                fresh.centerOffset = b->centerLocal;
                fresh.halfExtents  = b->extentsLocal;
            }
            ExecuteSceneMutation(ctx, "history.add_component", [&]() { ctx.world.Add<OBBComponent>(selected, fresh); });
        }
        if (obbDisabled) ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        {
            if (hasOBB)       ImGui::SetTooltip(Translate("physics.collider_exists"));
            else if (hasMesh) ImGui::SetTooltip(Translate("physics.box_from_mesh"));
            else              ImGui::SetTooltip(Translate("physics.default_box"));
        }

        // Kugel / Kapsel (Physics-Addon) — an die Mesh-Bounds angepasst.
        const auto* addBounds = ctx.world.Get<BoundsComponent>(selected);
        const bool hasSphere = ctx.world.Has<SphereColliderComponent>(selected);
        if (hasSphere) ImGui::BeginDisabled();
        if (ImGui::MenuItem(TranslateLabel("physics.sphere", "Kollision (Kugel)").c_str()))
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
            ExecuteSceneMutation(ctx, "history.add_component", [&]() { ctx.world.Add<SphereColliderComponent>(selected, fresh); });
        }
        if (hasSphere) ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip(hasSphere ? Translate("physics.sphere_exists")
                                        : Translate("physics.sphere_from_mesh"));

        const bool hasCapsule = ctx.world.Has<CapsuleColliderComponent>(selected);
        if (hasCapsule) ImGui::BeginDisabled();
        if (ImGui::MenuItem(TranslateLabel("physics.capsule", "Kollision (Kapsel)").c_str()))
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
            ExecuteSceneMutation(ctx, "history.add_component", [&]() { ctx.world.Add<CapsuleColliderComponent>(selected, fresh); });
        }
        if (hasCapsule) ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip(hasCapsule ? Translate("physics.capsule_exists")
                                         : Translate("physics.capsule_from_mesh"));

        // Spielfigur — Kapsel um die Mesh-Bounds, Ursprung an den Fuessen.
        const bool hasCharacter = ctx.world.Has<CharacterControllerComponent>(selected);
        if (hasCharacter) ImGui::BeginDisabled();
        if (ImGui::MenuItem(TranslateLabel("component.character", "Spielfigur (Character Controller)").c_str()))
        {
            CharacterControllerComponent fresh;
            if (addBounds)
            {
                const math::Vec3 e = addBounds->extentsLocal;
                fresh.radius = std::max(0.05f, std::max(e.x, e.z));
                fresh.height = std::max(fresh.radius * 2.f, e.y * 2.f);
            }
            ExecuteSceneMutation(ctx, "history.add_component", [&]() { ctx.world.Add<CharacterControllerComponent>(selected, fresh); });
        }
        if (hasCharacter) ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip(hasCharacter ? Translate("physics.character_exists")
                                           : Translate("physics.character_help"));

        // Audio
        ImGui::Separator();
        const bool hasAudioSource = ctx.world.Has<AudioSourceComponent>(selected);
        if (hasAudioSource) ImGui::BeginDisabled();
        if (ImGui::MenuItem(TranslateLabel("component.audio_source", "Audio-Quelle").c_str()))
            ExecuteSceneMutation(ctx, "history.add_component", [&]() { ctx.world.Add<AudioSourceComponent>(selected, AudioSourceComponent{}); });
        if (hasAudioSource) ImGui::EndDisabled();
        const bool hasAudioListener = ctx.world.Has<AudioListenerComponent>(selected);
        if (hasAudioListener) ImGui::BeginDisabled();
        if (ImGui::MenuItem(TranslateLabel("component.audio_listener", "Audio-Listener").c_str()))
            ExecuteSceneMutation(ctx, "history.add_component", [&]() { ctx.world.Add<AudioListenerComponent>(selected, AudioListenerComponent{}); });
        if (hasAudioListener) ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip(Translate("audio.listener_tip"));
        ImGui::Separator();

        // Effekte
        const bool hasEmitter = ctx.world.Has<ParticleEmitterComponent>(selected);
        if (hasEmitter) ImGui::BeginDisabled();
        if (ImGui::MenuItem(TranslateLabel("component.particles", "Partikel-Emitter").c_str()))
            ExecuteSceneMutation(ctx, "history.add_component", [&]() { ctx.world.Add<ParticleEmitterComponent>(selected, ParticleEmitterComponent{}); });
        if (hasEmitter) ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip(hasEmitter ? Translate("particles.exists")
                                         : Translate("particles.help"));
        ImGui::Separator();

        // Rigidbody — legt bei Bedarf gleich eine Box-Kollision aus der Mesh-AABB an.
        const bool hasRigidBody = ctx.world.Has<RigidBodyComponent>(selected);
        if (hasRigidBody) ImGui::BeginDisabled();
        if (ImGui::MenuItem(TranslateLabel("component.rigidbody", "Rigidbody").c_str()))
        ExecuteSceneMutation(ctx, "history.add_rigidbody", [&]()
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
            if (hasRigidBody) ImGui::SetTooltip(Translate("physics.rigidbody_exists"));
            else              ImGui::SetTooltip(Translate("physics.dynamic_help"));
        }

        // Zusammengesetzte Formen: weitere Collider als Kind-Objekte.
        ImGui::Separator();
        if (ImGui::MenuItem(TranslateLabel("physics.box_child", "Kollisions-Kind (Box)").c_str()))
            CreateColliderChildEntity(ctx, selected, ColliderChildShape::Box);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip(Translate("physics.box_child_tip"));
        if (ImGui::MenuItem(TranslateLabel("physics.sphere_child", "Kollisions-Kind (Kugel)").c_str()))
            CreateColliderChildEntity(ctx, selected, ColliderChildShape::Sphere);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip(Translate("physics.sphere_child_tip"));

        // Material
        if (hasMaterial) ImGui::BeginDisabled();
        if (ImGui::MenuItem("Material"))
            AssignDefaultMaterialToEntity(ctx, selected);
        if (hasMaterial) ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled) && hasMaterial)
            ImGui::SetTooltip(Translate("material.assigned_tip"));

        ImGui::EndPopup();
    }

    ImGui::PopID();
    ImGui::End();
}

} // namespace detail

} // namespace engine::renderer::addons::editor

#endif // KROM_EDITOR_HAS_IMGUI
