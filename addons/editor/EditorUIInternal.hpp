#pragma once
// =============================================================================
// KROM Engine - addons/editor/EditorUIInternal.hpp
// Interne Helfer, die zwischen den EditorUI*-Dateien geteilt werden.
// Nicht Teil der oeffentlichen Editor-API (die steht in EditorUI.hpp).
// =============================================================================
#include "addons/editor/EditorUI.hpp"
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

inline constexpr ImGuiWindowFlags kAlignedPanelFlags = ImGuiWindowFlags_NoMove;

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

// ── EditorUIHierarchy.cpp ─────────────────────────────────────────────────
void DrawEntityList(EditorFrameContext& ctx);

// ── EditorUIInspector.cpp ─────────────────────────────────────────────────
EntityID FindRigidBodyAncestor(const ecs::World& world, EntityID entity);
void ApplyEnvironmentFromState(EditorFrameContext& ctx);
void DrawInspector(EditorFrameContext& ctx);

// ── EditorUIProject.cpp ───────────────────────────────────────────────────
bool SaveEntityAsPrefab(EditorFrameContext& ctx, EntityID entity);
std::filesystem::path ResolveSelectedPrefabPath(const EditorFrameContext& ctx);
bool SpawnPrefabAsset(EditorFrameContext& ctx,
                      const std::filesystem::path& path,
                      const math::Vec3* worldPosition = nullptr);
void DrawProjectSettingsDialog(EditorFrameContext& ctx);
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

} // namespace engine::renderer::addons::editor::detail

#endif // KROM_EDITOR_HAS_IMGUI
