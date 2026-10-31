#pragma once

#include "addons/editor/EditorFeature.hpp"
#include "addons/editor/EditorComponents.hpp"
#include "addons/lighting/LightingComponents.hpp"
#include <filesystem>
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

// Legt unter parent ein leeres Kind "Collider" mit Box- oder Kugel-Kollision an
// (lokale Pose = Parent-Ursprung, Groesse aus den Parent-Bounds). Damit lassen
// sich zusammengesetzte Formen (z.B. L-Form) bauen. Selektiert das neue Kind.
enum class ColliderChildShape { Box, Sphere };
EntityID CreateColliderChildEntity(EditorFrameContext& ctx, EntityID parent, ColliderChildShape shape);
bool DestroyEntityHierarchy(EditorFrameContext& ctx, EntityID entity);

// Prefab-Modus wie in Unity: Enter legt Szene, Editor-Kamera und
// Rueckgaengig-Liste beiseite und zeigt nur das Prefab - bearbeitet wird mit
// Hierarchie, Inspector und Viewport. Save schreibt das (einzige) oberste
// Objekt in die .prefab-Datei. Exit speichert automatisch, stellt die Szene
// wieder her und aktualisiert alle Instanzen (false = Speichern fehlgeschlagen,
// der Modus bleibt aktiv).
[[nodiscard]] bool IsInPrefabMode(const EditorState& state);
bool EnterPrefabMode(EditorFrameContext& ctx, const std::filesystem::path& prefabPath);
bool SavePrefabMode(EditorFrameContext& ctx);
bool ExitPrefabMode(EditorFrameContext& ctx);

// Multi-Selektion
bool IsEntityInMultiSelection(const EditorState& state, EntityID id);
void AddToMultiSelection(EditorFrameContext& ctx, EntityID id);
void ClearMultiSelection(EditorFrameContext& ctx);

} // namespace engine::renderer::addons::editor
