#pragma once
// =============================================================================
// KROM Engine - addons/editor/EditorPrefabWindow.hpp
// ResolvePrefabBindings (Live-Link der Prefab-Instanzen in der Szene).
// =============================================================================
#include <filesystem>
#include "core/Types.hpp"
#include <string>

namespace engine::renderer::addons::editor { struct EditorFrameContext; }

namespace engine::addons::editor {

// ---------------------------------------------------------------------------
// Live-Link: synchronisiert alle PrefabInstance-Entities in der Szene mit
// ihrem Quell-.prefab-Asset. Position/Rotation bleiben bei overrideTransform
// erhalten; Scale erbt den Prefab-Wert, sofern nicht lokal ueberschrieben.
// Aufrufen nach jedem Prefab-Save oder Szenen-Load.
bool ResetPrefabInstanceOverrides(engine::renderer::addons::editor::EditorFrameContext& ctx, EntityID entity);

void ResolvePrefabBindings(engine::renderer::addons::editor::EditorFrameContext& ctx,
                           const std::filesystem::path& changedPath = {});

} // namespace engine::addons::editor
