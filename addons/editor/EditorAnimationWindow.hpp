#pragma once
#include "addons/editor/EditorFeature.hpp"

#include <functional>
#include <string>
#include <vector>

struct ImVec2;
namespace engine::addons::debug_draw { class DebugDrawRenderer; }

namespace engine::renderer::addons::editor {

inline constexpr float kAnimationPreviewFovDeg = 35.f;
inline constexpr float kAnimationPreviewNear = 0.02f;

// Keyframe editor for .anim clips, docked like the material editor.
void DrawAnimationWindow(EditorFrameContext& ctx);

// Inspector entry point: opens the entity's animation in the editor, or starts
// a new one if it has none that can be edited.
void OpenAnimationEditor(EditorFrameContext& ctx, EntityID entity);

[[nodiscard]] bool AnimationFileExists(EditorFrameContext& ctx, const std::string& relativePath);
// Asset browser entry point; the path is relative to Assets.
void OpenAnimationFile(EditorFrameContext& ctx, const std::string& relativePath);
// Opens a model clip ("<model>#clip/<id>") with the settings of all clips of
// that model, on the object playing it or the selected animator.
void OpenModelClip(EditorFrameContext& ctx, const std::string& clipAssetPath);
// Clip names stored in a .anim file; cached until the file changes.
[[nodiscard]] std::vector<std::string> AnimationClipNames(EditorFrameContext& ctx, const std::string& relativePath);

// Starts a new, unsaved animation for the entity.
bool CreateAnimationClip(EditorFrameContext& ctx, EntityID entity);
// Writes the open animation and assigns it to its object.
bool SaveOpenAnimation(EditorFrameContext& ctx);
// Writes the open animation only if it has unsaved changes (scene save, editor exit).
bool SaveOpenAnimationIfChanged(EditorFrameContext& ctx);
// Clickable axis widget in the top right corner of the scene viewport.
void DrawViewportViewGizmo(EditorFrameContext& ctx, const ImVec2& origin, float width);
// True while the shared gizmos work in the animation preview instead of the viewport.
[[nodiscard]] bool AnimationPreviewOwnsGizmos();
void DrawAnimationPreviewGizmos(EditorFrameContext& ctx);
// Runs the editor's own gizmo code against the preview: its camera, its rectangle
// and the selected bone or object of the copy.
void RunInAnimationPreview(EditorFrameContext& ctx, const std::function<void()>& gizmoWork);
// The editor's ground grid, as seen from the preview camera.
void DrawAnimationPreviewGrid(EditorFrameContext& ctx, engine::addons::debug_draw::DebugDrawRenderer& debugDraw);
// Hidden copy that the window poses and shows.
[[nodiscard]] EntityID GetAnimationPreviewRoot();
void SetAnimationPreviewTime(float seconds);
// Inspector card with the values of the keyframe selected in the timeline;
// false when no keyframe is selected.
bool DrawAnimationKeyCard(EditorFrameContext& ctx);
// Stores the selected object's current pose as keys at the playhead.
void KeyAnimationSelection(EditorFrameContext& ctx);

} // namespace engine::renderer::addons::editor
