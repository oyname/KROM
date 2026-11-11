#pragma once
#include "addons/editor/EditorFeature.hpp"
#include <filesystem>
#include "renderer/MaterialTypes.hpp"
#include <cstdint>
#include <string>
#include <span>

namespace engine::renderer::addons::editor {

bool SetEntityMaterialOverride(EditorFrameContext& ctx, EntityID entity, int slot, const assets::MaterialAsset& asset);

void DrawMaterialLibrary(EditorFrameContext& ctx);
// Drops local material values kept from older scenes (slot < 0: entity level)
// and binds the entity to the material asset again. One undo step.
bool ResetEntityMaterialOverride(EditorFrameContext& ctx, EntityID entity, int slot, const std::filesystem::path& materialPath);
// Writes the material the entity shows (including local values) to a new .mat,
// assigns it and opens it in the material editor.
bool CreateEntityMaterialCopy(EditorFrameContext& ctx, EntityID entity, int slot, const std::filesystem::path& materialPath);
void SelectMaterialAsset(EditorFrameContext& ctx, const std::filesystem::path& path);
void OpenMaterialAsset(EditorFrameContext& ctx, const std::filesystem::path& path);
void QueueOpenMaterialAsset(EditorFrameContext& ctx, const std::filesystem::path& path);
void FlushPendingMaterialOpen(EditorFrameContext& ctx);
bool CreateMaterialAsset(EditorFrameContext& ctx, const std::filesystem::path& directory);
void ResolveMaterialAssetBindings(EditorFrameContext& ctx, std::span<const EntityID> entities = {});
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
// Writes the open material only if it has unsaved changes (scene save, editor exit).
bool SaveSelectedMaterialAssetIfChanged(EditorFrameContext& ctx);
bool ApplyMaterialAssetToEntity(EditorFrameContext& ctx,
                                EntityID entity,
                                const std::filesystem::path& materialPath);
bool ApplyMaterialAssetToEntitySlot(EditorFrameContext& ctx,
                                    EntityID entity,
                                    uint32_t submeshIndex,
                                    const std::filesystem::path& materialPath);

} // namespace engine::renderer::addons::editor
