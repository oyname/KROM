#pragma once
// =============================================================================
// KROM Engine - addons/editor/EditorUIInternal.hpp
// Interne Helfer, die zwischen den EditorUI*-Dateien geteilt werden.
// Nicht Teil der oeffentlichen Editor-API (die steht in EditorUI.hpp).
// =============================================================================
#include "addons/editor/EditorUI.hpp"
#include "addons/editor/EditorLocalization.hpp"
#include "core/Debug.hpp"
#include "addons/editor/EditorCommands.hpp"
#include "addons/editor/EditorFeature.hpp"
#include "addons/editor/EditorAssetBrowser.hpp"
#include "addons/editor/EditorFileNaming.hpp"
#include "addons/editor/EditorScriptAssets.hpp"
#include "addons/editor/EditorComponents.hpp"
#include "addons/editor/EditorMaterialLibrary.hpp"
#include "addons/debug_draw/DebugDraw.hpp"
#include "addons/lighting/LightingFrameData.hpp"
#include "addons/mesh_renderer/MeshSceneQueries.hpp"
#include "addons/mesh_renderer/MeshRendererSerialization.hpp"
#include "addons/prefab/Prefab.hpp"
#include "addons/prefab/PrefabInstanceComponent.hpp"
#include "addons/shadow/ShadowFrameData.hpp"
#include "EditorPrefabWindow.hpp"
#include "collision/SceneQueries.hpp"


#ifdef KROM_EDITOR_HAS_IMGUI

#include "addons/camera/CameraComponents.hpp"
#include "addons/editor/EditorIcons.hpp"
#include "addons/editor/EditorTheme.hpp"
#include "addons/lighting/LightingComponents.hpp"
#include "addons/lit/LitMaterial.hpp"
#include "addons/mesh_renderer/MeshRendererComponents.hpp"
#include "addons/audio/AudioComponents.hpp"
#include "addons/physics/PhysicsComponents.hpp"
#include "addons/script/ScriptList.hpp"
#include "renderer/RenderLayers.hpp"
#include "assets/AssetPipeline.hpp"
#include "assets/MeshTangents.hpp"
#include "ecs/World.hpp"
#include "ecs/Components.hpp"
#include "core/Math.hpp"
#include "core/Types.hpp"

#include "imgui.h"
#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <filesystem>
#include <string>
#include <sstream>
#include <utility>
#include <unordered_set>
#include <vector>

namespace engine::renderer::addons::editor::detail {

inline uint32_t GetStableEntityId(const EditorState& state, EntityID entity)
{
    const auto it = state.stableEntityIds.find(entity.value);
    return it != state.stableEntityIds.end() ? it->second : entity.value;
}

enum class EditorButtonTone { Neutral, Primary, Danger };

inline bool EditorActionButton(const char* label, ImVec2 size,
                               EditorButtonTone tone = EditorButtonTone::Neutral)
{
    const bool primary = tone == EditorButtonTone::Primary;
    const bool danger = tone == EditorButtonTone::Danger;
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 5.f);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.f);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(12.f, 7.f));
    ImGui::PushStyleColor(ImGuiCol_Button, danger ? theme::kDanger :
        primary ? theme::kAccent : theme::kButton);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, danger ? theme::Hex(0xE2707A) :
        primary ? theme::kAccentHover : theme::kButtonHover);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, danger ? theme::Hex(0xB8424D) :
        primary ? theme::kButtonActive : theme::kButtonActive);
    ImGui::PushStyleColor(ImGuiCol_Border, danger ? theme::Hex(0xE2707A) :
        primary ? theme::kAccentHover : theme::kBorder);
    const bool clicked = ImGui::Button(label, size);
    ImGui::PopStyleColor(4);
    ImGui::PopStyleVar(3);
    return clicked;
}

void PushSolidEditorPanelStyle();
void PopSolidEditorPanelStyle();

struct EditorLayoutRects
{
    ImVec2 hierarchyPos{};
    ImVec2 hierarchySize{};
    ImVec2 inspectorPos{};
    ImVec2 inspectorSize{};
    ImVec2 assetsPos{};
    ImVec2 assetsSize{};
    ImVec2 renderPos{};
    ImVec2 renderSize{};
};

// Bildbereich der Szene in Bildschirmpixeln: das Bild im Viewport-Panel
// (renderView*), ohne Panel das ganze Fenster.
struct SceneScreenRect
{
    ImVec2 min{};
    ImVec2 size{1.f, 1.f};
};

inline SceneScreenRect GetSceneScreenRect(const EditorFrameContext& ctx)
{
    if (ctx.renderViewWidth > 1.f && ctx.renderViewHeight > 1.f)
        return { ImVec2(ctx.renderViewX, ctx.renderViewY), ImVec2(ctx.renderViewWidth, ctx.renderViewHeight) };
    const ImGuiIO& io = ImGui::GetIO();
    return { ImVec2(0.f, 0.f), ImVec2(std::max(io.DisplaySize.x, 1.f), std::max(io.DisplaySize.y, 1.f)) };
}

// Maus gehoert ImGui statt der Szene. Ueber dem Bild im Viewport-Panel meldet ImGui
// zwar WantCaptureMouse, die Eingabe ist dort aber fuer die Szene bestimmt.
inline bool SceneMouseBlocked(const EditorFrameContext& ctx)
{
    return ImGui::GetIO().WantCaptureMouse && !ctx.sceneViewHovered;
}

// Hauptpanels sind andockbar; Position und Groesse bestimmt der Dockspace.
inline constexpr ImGuiWindowFlags kAlignedPanelFlags = ImGuiWindowFlags_None;

struct SolidEditorPanelStyleScope
{
    SolidEditorPanelStyleScope()
    {
        PushSolidEditorPanelStyle();
    }

    ~SolidEditorPanelStyleScope()
    {
        PopSolidEditorPanelStyle();
    }

    SolidEditorPanelStyleScope(const SolidEditorPanelStyleScope&) = delete;
    SolidEditorPanelStyleScope& operator=(const SolidEditorPanelStyleScope&) = delete;
};

// ── EditorUI.cpp ──────────────────────────────────────────────────────────
EditorLayoutRects ComputeEditorLayout(const EditorFrameContext& ctx, const ImVec2& displaySize);
void SetFixedEditorWindow(const ImVec2& pos, const ImVec2& size);
// Multi-viewports: keeps the next window inside the editor's OS window. Only the
// material editor may become its own OS window; panels with absolute screen
// overlays (Viewport, Game) and the rest stay in the editor window. Docked
// windows follow their dock node and are not pinned.
void KeepNextWindowInMainViewport(const char* windowName);
inline void KeepNextWindowInMainViewport(const std::string& windowName)
{
    KeepNextWindowInMainViewport(windowName.c_str());
}
// Initial position for a floating window, relative to the editor window
// (ImGui positions are desktop coordinates once multi-viewports are on).
ImVec2 EditorWindowPos(float x, float y);
bool ShouldCancelSelectionChangeForActiveInput();
bool IsScenePickEntity(const ecs::World& world, EntityID entity);
math::Vec3 PrepareRotationEditEuler(EditorFrameContext& ctx,
                                    EntityID entity,
                                    EditorTransformSpace space,
                                    const math::Quat& rotation) noexcept;
void CommitRotationEditState(EditorFrameContext& ctx,
                             EntityID entity,
                             EditorTransformSpace space,
                             const math::Vec3& euler,
                             const math::Quat& rotation) noexcept;
bool HasNonUniformScaledParent(EditorFrameContext& ctx, EntityID entity) noexcept;
void SetWorldTransform(EditorFrameContext& ctx,
                       EntityID entity,
                       TransformComponent& transform,
                       const math::Mat4& desiredWorldMatrix);
math::Vec3 WorldPosition(EditorFrameContext& ctx, EntityID entity);
math::Quat WorldRotation(EditorFrameContext& ctx, EntityID entity);
math::Vec3 WorldScale(EditorFrameContext& ctx, EntityID entity);
void SetWorldPosition(EditorFrameContext& ctx, EntityID entity, TransformComponent& transform, const math::Vec3& worldPosition);
void SetWorldRotation(EditorFrameContext& ctx, EntityID entity, TransformComponent& transform, const math::Quat& worldRotation);
void SetWorldScale(EditorFrameContext& ctx, EntityID entity, TransformComponent& transform, const math::Vec3& worldScale);

// ── EditorUIHierarchy.cpp ─────────────────────────────────────────────────
void DrawEntityList(EditorFrameContext& ctx);

// ── EditorUIInspector.cpp ─────────────────────────────────────────────────
EntityID FindRigidBodyAncestor(const ecs::World& world, EntityID entity);
void ApplyEnvironmentFromState(EditorFrameContext& ctx);
void DrawInspector(EditorFrameContext& ctx);
void DrawSceneSettingsPanel(EditorFrameContext& ctx);

// ── EditorUIProject.cpp ───────────────────────────────────────────────────
// targetDir empty: Assets/Prefabs of the current project.
bool SaveEntityAsPrefab(EditorFrameContext& ctx, EntityID entity,
                        const std::filesystem::path& targetDir = {});
std::filesystem::path ResolveSelectedPrefabPath(const EditorFrameContext& ctx);
// parent gesetzt: als Kind am Ursprung des Parents (wie Unity beim Ziehen
// eines Prefabs auf ein Objekt in der Hierarchie).
bool SpawnPrefabAsset(EditorFrameContext& ctx,
                      const std::filesystem::path& path,
                      const math::Vec3* worldPosition = nullptr,
                      EntityID parent = NULL_ENTITY);
void DrawProjectSettingsDialog(EditorFrameContext& ctx);
// Hinweisfenster mit Fortschritt, solange ein Runtime-Build (Play/Export) laeuft.
void DrawRuntimeBuildStatus(EditorFrameContext& ctx);
void DrawSceneLoadStatus(EditorFrameContext& ctx);
void DrawDeleteConfirmDialog(EditorFrameContext& ctx);
void DrawNoProjectScreen(EditorFrameContext& ctx);
void DrawFileMenuBar(EditorFrameContext& ctx);

// ── EditorUIViewport.cpp ──────────────────────────────────────────────────
void UpdateSnapAxis(EditorFrameContext& ctx);
void SnapEntityToSurface(EditorFrameContext& ctx);
void DrawOBBDebugLines(EditorFrameContext& ctx);
void DrawPhysicsColliderLines(EditorFrameContext& ctx);
void UpdateOBBHandles(EditorFrameContext& ctx);
void DrawLightGizmos(EditorFrameContext& ctx);
void DrawScaleGizmo(EditorFrameContext& ctx);
void DrawTransformGizmo(EditorFrameContext& ctx);
void UpdateTransformGizmoInput(EditorFrameContext& ctx);
void DrawRotationGizmo(EditorFrameContext& ctx);
void UpdateRotationGizmoInput(EditorFrameContext& ctx);
void DrawSnapGrid(EditorFrameContext& ctx);
void DrawParticleEmitterGizmo(EditorFrameContext& ctx);
void DrawEditorIcons(EditorFrameContext& ctx);
void RenderPendingEditorIcons(EditorFrameContext& ctx, ImDrawList* dl);
EntityID PickEditorIcon(EditorFrameContext& ctx, const ImVec2& mouse, bool cycleFromSelection = false);

} // namespace engine::renderer::addons::editor::detail

#endif // KROM_EDITOR_HAS_IMGUI
