#pragma once
#include "addons/editor/EditorFeature.hpp"
#include <filesystem>
#include "renderer/MaterialTypes.hpp"
#include <cstdint>
#include <string>

namespace engine::renderer::addons::editor {

void DrawMaterialLibrary(EditorFrameContext& ctx);
void DrawSelectedMaterialInspector(EditorFrameContext& ctx);
void SelectMaterialAsset(EditorFrameContext& ctx, const std::filesystem::path& path);
void OpenMaterialAsset(EditorFrameContext& ctx, const std::filesystem::path& path);
void QueueOpenMaterialAsset(EditorFrameContext& ctx, const std::filesystem::path& path);
void FlushPendingMaterialOpen(EditorFrameContext& ctx);
bool CreateMaterialAsset(EditorFrameContext& ctx, const std::filesystem::path& directory);
void ResolveMaterialAssetBindings(EditorFrameContext& ctx);
// Prueft pro Frame ob nachlaufend geladene Texturen (Normal, ORM, …) jetzt
// verfuegbar sind und aktualisiert MaterialComponent.material-Handles.
// Muss jedes Frame aufgerufen werden.
void TickMaterialTextureSync(EditorFrameContext& ctx);
// Leert den projektübergreifenden SharedMaterialBindings-Cache.
// Muss vor dem Laden eines neuen Projekts aufgerufen werden, damit
// veraltete (materialPath, meshHandle)-Einträge aus der alten Session
// nicht wieder verwendet werden.
void ClearSharedMaterialBindings();
bool ApplySelectedMaterialToEntity(EditorFrameContext& ctx, EntityID entity);
bool AssignDefaultMaterialToEntity(EditorFrameContext& ctx, EntityID entity);
bool AssignThumbnailWhiteMaterialToEntity(EditorFrameContext& ctx, EntityID entity);

// Weist eine Textur dem selektierten Material zu.
// Fuer PBR-Materialien wird die Shader-Permutation automatisch gewechselt.
void AssignTextureToSelectedMaterial(EditorFrameContext& ctx,
                                      TextureHandle tex,
                                      const std::string& texParamName,
                                      const std::string& fileName);

// Speichert das aktuell im Editor geladene Material explizit (z.B. via Ctrl+S).
// Gibt false zurueck wenn kein Material geladen ist oder der Pfad leer ist.
bool SaveSelectedMaterialAsset(EditorFrameContext& ctx);
bool ApplyMaterialAssetToEntity(EditorFrameContext& ctx,
                                EntityID entity,
                                const std::filesystem::path& materialPath);
bool ApplyMaterialAssetToEntitySlot(EditorFrameContext& ctx,
                                    EntityID entity,
                                    uint32_t submeshIndex,
                                    const std::filesystem::path& materialPath);

} // namespace engine::renderer::addons::editor
