#pragma once
// =============================================================================
// KROM Engine - addons/editor/EditorControllerGraph.hpp
// Graph editor for animation controllers (*.animctrl), a dockable window like
// the material editor. States are nodes, transitions arrows; the details of
// the selected state or transition are a card in the inspector. Every edit is
// one step in the editor's undo history.
//
// The edit functions below are what the window itself calls; tests use them too.
// =============================================================================
#include "addons/animation/AnimationController.hpp"
#include "addons/editor/EditorFeature.hpp"

#include <cstdint>
#include <functional>
#include <string>

namespace engine::renderer::addons::editor {

void DrawControllerGraphWindow(EditorFrameContext& ctx);
// Opens the controller (path relative to Assets) in the window; a previous one
// with unsaved changes is saved first.
bool OpenControllerGraph(EditorFrameContext& ctx, const std::string& assetPath);
// Inspector card for the selected state or transition; false when nothing is selected.
bool DrawControllerGraphCard(EditorFrameContext& ctx);
bool SaveOpenControllerIfChanged(EditorFrameContext& ctx);

// --- State of the open controller (tests, inspector) --------------------------
[[nodiscard]] const std::string& ControllerGraphPath();
[[nodiscard]] const engine::addons::animation::AnimationControllerAsset* ControllerGraphAsset();
[[nodiscard]] bool ControllerGraphDirty();
// Bumped by every change of the working copy (the preview rebinds on it).
[[nodiscard]] uint64_t ControllerGraphVersion();

// --- Edits (one undo step each) ---------------------------------------------
std::string ControllerGraphAddState(EditorFrameContext& ctx, const std::string& clipAssetPath, float x, float y);
std::string ControllerGraphAddTransition(EditorFrameContext& ctx, const std::string& fromId, const std::string& toId);
std::string ControllerGraphAddParameter(EditorFrameContext& ctx, engine::addons::animation::AnimationParameterType type);
bool ControllerGraphSetEntry(EditorFrameContext& ctx, const std::string& stateId);
// Any other change of the working copy as one undo step (tests).
bool ControllerGraphEdit(EditorFrameContext& ctx, const std::function<void(engine::addons::animation::AnimationControllerAsset&)>& edit);
// Selects a state, transition or parameter by id ("" clears).
void ControllerGraphSelect(const std::string& id);
[[nodiscard]] const std::string& ControllerGraphSelection();
bool ControllerGraphDeleteSelection(EditorFrameContext& ctx);
// Where a state (centre) or transition (arrow middle) was drawn last frame, in
// screen pixels; false if it was not drawn (tests drive the mouse with it).
// "#canvasMin" / "#canvasMax" give the visible graph area.
bool ControllerGraphScreenPosition(const std::string& id, float& x, float& y);

} // namespace engine::renderer::addons::editor
