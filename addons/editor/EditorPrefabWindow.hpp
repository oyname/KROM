#pragma once
// =============================================================================
// KROM Engine - addons/editor/EditorPrefabWindow.hpp
// ResolvePrefabBindings (Live-Link der Prefab-Instanzen in der Szene).
// =============================================================================
#include <filesystem>
#include <string>

namespace engine::renderer::addons::editor { struct EditorFrameContext; }

namespace engine::addons::editor {

// ---------------------------------------------------------------------------
// Live-Link: synchronisiert alle PrefabInstance-Entities in der Szene mit
// ihrem Quell-.prefab-Asset. Transform bleibt erhalten wenn overrideTransform.
// Aufrufen nach jedem Prefab-Save oder Szenen-Load.
void ResolvePrefabBindings(engine::renderer::addons::editor::EditorFrameContext& ctx);

} // namespace engine::addons::editor
