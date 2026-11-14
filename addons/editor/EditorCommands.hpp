#pragma once

#include "addons/editor/EditorFeature.hpp"
#include "addons/editor/EditorComponents.hpp"
#include "addons/lighting/LightingComponents.hpp"
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace engine::addons::prefab { struct PrefabAsset; }

namespace engine::renderer::addons::editor {

void ClearEditorHistory(EditorState& state);
bool UndoEditorHistory(EditorFrameContext& ctx);
bool RedoEditorHistory(EditorFrameContext& ctx);
// For property edits in external assets, whose values are not part of a scene snapshot.
void RecordEditorValueChange(EditorFrameContext& ctx, const char* label,
                            std::function<void()> undo, std::function<void()> redo);
bool CanUndoEditorHistory(EditorState& state);
bool CanRedoEditorHistory(EditorState& state);
std::string GetUndoEditorHistoryLabel(EditorState& state);
std::string GetRedoEditorHistoryLabel(EditorState& state);

bool ExecuteSceneMutation(EditorFrameContext& ctx,
                          const char*         label,
                          const std::function<void()>& mutation);
bool AssignModelMesh(EditorFrameContext& ctx, EntityID entity, const std::filesystem::path& model);
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
// Folder for organizing the hierarchy: FolderComponent, name and active flag,
// no transform (no position, no gizmo); children keep their world placement.
// Saved like any other entity (.json and .kscene). Folders only sit at the
// root or inside other folders.
EntityID CreateFolderEntity(EditorFrameContext& ctx, const char* name = "Folder");
EntityID CreateCameraEntity(EditorFrameContext& ctx);
EntityID CreateLightEntity(EditorFrameContext& ctx, LightType type);
EntityID DuplicateEntity(EditorFrameContext& ctx, EntityID source);
bool ReparentEntity(EditorFrameContext& ctx, EntityID child, EntityID newParent);
// Moves entity under newParent (NULL_ENTITY = scene root) and places it directly
// before or after the sibling anchor. An invalid anchor appends at the end.
// Keeps the world pose. One undo step.
bool MoveEntityInHierarchy(EditorFrameContext& ctx, EntityID entity, EntityID newParent,
                           EntityID anchor, bool insertAfter);
// Outermost prefab instance that contains entity (entity itself or an ancestor),
// NULL_ENTITY if it is not part of a prefab instance.
EntityID FindOutermostPrefabInstanceRoot(EditorFrameContext& ctx, EntityID entity);
// Detaches that instance from its prefab asset: the objects stay in the scene as
// plain entities, nested prefab instances inside stay linked. One undo step.
bool UnpackPrefabInstance(EditorFrameContext& ctx, EntityID entity);

// Legt unter parent ein leeres Kind "Collider" mit Box- oder Kugel-Kollision an
// (lokale Pose = Parent-Ursprung, Groesse aus den Parent-Bounds). Damit lassen
// sich zusammengesetzte Formen (z.B. L-Form) bauen. Selektiert das neue Kind.
enum class ColliderChildShape { Box, Sphere };
EntityID CreateColliderChildEntity(EditorFrameContext& ctx, EntityID parent, ColliderChildShape shape);
bool DestroySelectedEntityHierarchies(EditorFrameContext& ctx);

// Prefab-Modus wie in Unity: Enter legt Szene, Editor-Kamera und
// Rueckgaengig-Liste beiseite und zeigt nur das Prefab - bearbeitet wird mit
// Hierarchie, Inspector und Viewport. Save schreibt das (einzige) oberste
// Objekt in die .prefab-Datei. Exit speichert automatisch, stellt die Szene
// wieder her und aktualisiert alle Instanzen (false = Speichern fehlgeschlagen,
// der Modus bleibt aktiv).
[[nodiscard]] bool IsInPrefabMode(const EditorState& state);
bool EnterPrefabMode(EditorFrameContext& ctx, const std::filesystem::path& prefabPath);
// Modell-Ansicht (Doppelklick auf .glb/.gltf): wie der Prefab-Modus, aber nur
// zum Anschauen - Verlassen speichert nichts, Speichern ist gesperrt. Mit
// "Als Prefab speichern" entsteht daraus ein bearbeitbares Prefab.
bool EnterModelView(EditorFrameContext& ctx, const std::filesystem::path& modelPath,
                    const engine::addons::prefab::PrefabAsset& modelPrefab);
// Einziges oberstes Objekt im Prefab-Modus/der Modell-Ansicht, sonst NULL_ENTITY.
[[nodiscard]] EntityID PrefabModeRootEntity(EditorFrameContext& ctx);
// Prefab-Modus: ein Prefab hat genau eine Wurzel. Weitere oberste Objekte
// (hineingezogen, eingefuegt, neu angelegt, nach oben gezogen) haengt diese
// Funktion unter die Wurzel; ihre Weltpose bleibt. Wurzel ist das Objekt, mit
// dem der Modus geoeffnet wurde, sonst das oberste in der Hierarchie.
// Laeuft in jeder Szenen-Aenderung und als Absicherung jeden Frame.
// true = es wurde etwas umgehaengt. Modell-Ansicht: unveraendert.
bool EnforcePrefabModeSingleRoot(EditorFrameContext& ctx);
bool SavePrefabMode(EditorFrameContext& ctx);
bool ExitPrefabMode(EditorFrameContext& ctx);

// Runs an action that leaves the current scene (switch, new scene). With unsaved
// changes a dialog asks first: save, don't save (the action runs without saving)
// or cancel.
void RunAfterSceneSaveCheck(EditorFrameContext& ctx, const std::string& actionLabel, std::function<void()> action);
void DrawSceneSavePrompt(EditorFrameContext& ctx);

// Play im Editor: Szene und Rueckgaengig-Liste beiseitelegen und bei Stop
// unveraendert wiederherstellen. Was waehrend Play passiert (Spiel, Inspector),
// geht bei Stop verloren; die Rueckgaengig-Liste ist waehrend Play leer.
bool BeginPlaySession(EditorFrameContext& ctx);
bool EndPlaySession(EditorFrameContext& ctx);
[[nodiscard]] bool IsPlaySessionActive(const EditorState& state);

// Multi-Selektion
std::vector<EntityID> GetSelectedEntities(const EditorFrameContext& ctx);
bool IsEntityInMultiSelection(const EditorState& state, EntityID id);
void AddToMultiSelection(EditorFrameContext& ctx, EntityID id);
void ClearMultiSelection(EditorFrameContext& ctx);

} // namespace engine::renderer::addons::editor
