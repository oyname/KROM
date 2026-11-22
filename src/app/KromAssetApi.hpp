#pragma once
#include "krom.h"
#include "KromRuntimeMaterials.hpp"

#include "addons/script/ScriptRegistry.hpp"
#include "assets/AssetPipeline.hpp"
#include "assets/AssetRegistry.hpp"
#include "ecs/World.hpp"
#include "scene/BoundsSystem.hpp"
#include "scene/TransformSystem.hpp"

// Asset functions of the krom.h wrapper: loading meshes, textures and
// materials, materials made through krom.h, assigning meshes and materials to
// entities, procedural meshes, and the model import (CreateEntityFromAsset,
// PrintAssetTree). The module owns no state. The loaders and PrintAssetTree
// only need the asset pipeline; everything else takes an AssetContext.
namespace Krom::internal {

// References into the wrapper State; built per call, never stored.
struct AssetContext
{
    bool initialized = false;                                 // State::initialized
    engine::ecs::World* world = nullptr;                      // null before Graphics()
    engine::assets::AssetPipeline* assetPipeline = nullptr;   // null before Graphics()
    engine::assets::AssetRegistry& assetRegistry;
    engine::TransformSystem& transformSystem;
    engine::BoundsSystem& boundsSystem;
    engine::script::ScriptRegistry& scriptAddonRegistry;      // scripts of imported prefabs
    // Creates the animation and skinning systems of the wrapper (they belong to
    // the State and are also used by Run()); called for models with skeletons
    // or animations.
    void (*ensureAnimationSystems)() = nullptr;
    RuntimeMaterialContext materials;
};

// Entity with mesh and a matt code material (CreateMeshEntity, shader warm-up);
// null if mesh is invalid.
[[nodiscard]] LPENTITY CreateProceduralEntity(const AssetContext& ctx, LPMESH mesh, const char* name);

// --- krom.h functions --------------------------------------------------------
// Same behaviour as the krom.h functions of the same name; assetPipeline and
// ctx.world may be null.

[[nodiscard]] LPENTITY CreateEntityFromAsset(const AssetContext& ctx, const char* path, const char* name);
bool PrintAssetTree(engine::assets::AssetPipeline* assetPipeline, const char* path);

[[nodiscard]] LPMESH LoadMesh(engine::assets::AssetPipeline* assetPipeline, const char* path);
// Remembers the path of the texture in texturePaths (CreateTextureMaterial(LPTEXTURE)).
[[nodiscard]] LPTEXTURE LoadTexture(engine::assets::AssetPipeline* assetPipeline,
                                    TexturePathMap& texturePaths,
                                    const char* path);
[[nodiscard]] LPMATERIAL LoadMaterial(engine::assets::AssetPipeline* assetPipeline, const char* path);

[[nodiscard]] LPMATERIAL CreateTextureMaterial(const AssetContext& ctx,
                                               const char* albedoTexturePath,
                                               int materialModel,
                                               engine::math::Vec4 baseColorFactor,
                                               float roughness,
                                               float metallic);
[[nodiscard]] LPMATERIAL CreateTextureMaterial(const AssetContext& ctx,
                                               LPTEXTURE texture,
                                               int materialModel,
                                               engine::math::Vec4 baseColorFactor,
                                               float roughness,
                                               float metallic);
[[nodiscard]] LPMATERIAL CreateLitMaterial(const AssetContext& ctx,
                                           const char* albedoTexturePath,
                                           engine::math::Vec4 baseColorFactor,
                                           float roughness,
                                           float metallic);

bool SetMesh(const AssetContext& ctx, LPENTITY entity, LPMESH mesh);
bool SetMaterial(const AssetContext& ctx, LPENTITY entity, LPMATERIAL material);
bool SetMaterialRecursive(const AssetContext& ctx, LPENTITY rootEntity, LPMATERIAL material);
bool SetTexture(const AssetContext& ctx, LPENTITY entity, const char* albedoTexturePath, int materialModel, bool recursive);
bool SetTexture(const AssetContext& ctx, LPENTITY entity, LPTEXTURE texture, int materialModel, bool recursive);
bool SetPBR(const AssetContext& ctx, LPENTITY entity, const PBR& materialDesc, bool recursive);
bool SetMeshFromAsset(const AssetContext& ctx, LPENTITY entity, const char* path);
bool SetMaterialFromAsset(const AssetContext& ctx, LPENTITY entity, const char* path);

[[nodiscard]] LPMESH CreateMeshAsset(const AssetContext& ctx, const MeshData& meshData);
[[nodiscard]] LPENTITY CreateMeshEntity(const AssetContext& ctx, const MeshData& meshData);
[[nodiscard]] LPENTITY CreateMesh(const AssetContext& ctx, const char* name);

} // namespace Krom::internal
