#pragma once

#include "addons/editor/EditorFeature.hpp"
#include "addons/editor/EditorComponents.hpp"
#include "addons/lighting/LightingComponents.hpp"
#include <functional>
#include <string>

namespace engine::renderer::addons::editor {

void ClearEditorHistory(EditorState& state);
bool UndoEditorHistory(EditorFrameContext& ctx);
bool RedoEditorHistory(EditorFrameContext& ctx);
bool CanUndoEditorHistory(EditorState& state);
bool CanRedoEditorHistory(EditorState& state);
std::string GetUndoEditorHistoryLabel(EditorState& state);
std::string GetRedoEditorHistoryLabel(EditorState& state);

bool ExecuteSceneMutation(EditorFrameContext& ctx,
                          const char*         label,
                          const std::function<void()>& mutation);
void BeginPendingSceneEdit(EditorFrameContext& ctx,
                           const std::string&  key,
                           const char*         label);
void CommitPendingSceneEdit(EditorFrameContext& ctx, const std::string& key);
std::string MakeEditHistoryKey(EntityID entity, std::string_view field);

uint32_t GetEntityHierarchyOrder(EditorFrameContext& ctx, EntityID entity);
EntityID GetEntityParent(EditorFrameContext& ctx, EntityID entity);
bool MoveEntityInSiblingOrder(EditorFrameContext& ctx, EntityID entity, int direction);
void SetExclusiveMainCamera(EditorFrameContext& ctx, EntityID entity);

EntityID CreateEmptyEntity(EditorFrameContext& ctx, const char* name = "Entity");
EntityID CreateCameraEntity(EditorFrameContext& ctx);
EntityID CreateLightEntity(EditorFrameContext& ctx, LightType type);
EntityID DuplicateEntity(EditorFrameContext& ctx, EntityID source);
bool ReparentEntity(EditorFrameContext& ctx, EntityID child, EntityID newParent);
bool DestroyEntityHierarchy(EditorFrameContext& ctx, EntityID entity);

// Multi-Selektion
bool IsEntityInMultiSelection(const EditorState& state, EntityID id);
void AddToMultiSelection(EditorFrameContext& ctx, EntityID id);
void ClearMultiSelection(EditorFrameContext& ctx);

} // namespace engine::renderer::addons::editor
