#pragma once
#include "krom.h"
#include "KromSurfaces.hpp"
#include "assets/AssetPipeline.hpp"
#include "assets/AssetRegistry.hpp"
#include "renderer/MaterialSystem.hpp"
#include "renderer/RenderSystem.hpp"

#include <cstdint>
#include <filesystem>
#include <string>
#include <unordered_map>
#include <unordered_set>

// Runtime materials of the krom.h wrapper: turns material assets (.mat, imported
// models, materials made in code) into MaterialSystem materials that fit the
// vertex layout of a mesh, and caches them per material and mesh.
// The functions only see the State fields they need, passed in as RuntimeMaterialContext.
namespace Krom::internal {

// Path a texture handle was loaded from (Krom::LoadTexture, runtime materials).
using TexturePathMap = std::unordered_map<uint32_t, std::string>;

// References into the wrapper State; built per call, never stored.
struct RuntimeMaterialContext
{
    engine::assets::AssetRegistry& assetRegistry;
    engine::assets::AssetPipeline* assetPipeline = nullptr;   // null before Graphics()
    engine::renderer::MaterialSystem& materialSystem;
    const engine::renderer::RenderSystem& renderSystem;       // device for custom shader reflection
    engine::ecs::World* world = nullptr;
    const std::filesystem::path& assetRoot;
    const std::filesystem::path& engineAssetRoot;             // built-in shaders next to the EXE
    RuntimeMaterialCache& runtimeMaterialCache;
    TexturePathMap& texturePaths;
    bool openGL = false;
    bool enableForwardPlus = false;                           // GraphicsConfig::enableForwardPlus
};

// Fills in normals that are missing or zero from the triangles (smooth per
// vertex) and normalises the others.
void GenerateMissingNormals(engine::assets::SubMeshData& submesh);
// The same for every submesh, but only if mesh.generateMissingNormals is set;
// otherwise the normals stay exactly as supplied. Use this whenever the mesh is known.
void GenerateMissingNormals(engine::assets::MeshAsset& mesh);

// Shader path below the project assets, then below the engine shaders; the
// path itself if neither has it.
[[nodiscard]] std::string ResolveEngineAssetPath(const RuntimeMaterialContext& ctx, const char* path);
[[nodiscard]] engine::ShaderHandle LoadEngineShader(const RuntimeMaterialContext& ctx,
                                                    const char* path,
                                                    engine::assets::ShaderStage stage);

// Lit material for the vertex layout of mesh (static or skinned); asset may be
// null (plain white). Not cached.
[[nodiscard]] engine::MaterialHandle CreateRuntimeLitMaterialFromAssets(const RuntimeMaterialContext& ctx,
                                                                        engine::assets::MeshAsset& mesh,
                                                                        const engine::assets::MaterialAsset* asset,
                                                                        const char* name);

// Returns a MaterialSystem handle: if material is an AssetRegistry handle (a
// MaterialAsset exists) and the entity has a mesh, the runtime material for
// its template (custom, unlit, pbr-lit, lit) is created and cached per material
// and mesh. Otherwise material is returned unchanged.
[[nodiscard]] LPMATERIAL RealizeMaterialForEntity(const RuntimeMaterialContext& ctx, LPENTITY entity, LPMATERIAL material);

// Caches of one scene load: every model file is imported once (failures too),
// every runtime material is built once per (mesh, model material), and every
// unresolvable "<model>#material/<n>" key is reported once.
struct ImportedMaterialCache
{
    std::unordered_map<std::string, engine::assets::ImportedAssetBundle> bundles;   // by model path
    std::unordered_map<std::string, LPMATERIAL> runtimeMaterials;                   // "<mesh key>|<material key>"
    std::unordered_set<std::string> reportedKeys;
};

// Entities whose mesh comes from an imported model ("<file>#mesh/<n>") and that
// have no material assigned get the model's own materials.
void ApplyImportedMeshMaterialsForEntity(const RuntimeMaterialContext& ctx,
                                         LPENTITY entity,
                                         ImportedMaterialCache& cache);

// True when materialAssetPath is a sub-asset key "<model file>#material/<n>"
// and no local override source replaces it. Such keys are no files: they are
// resolved by ApplyImportedMaterialKeysForEntity, not by LoadMaterial.
[[nodiscard]] bool IsImportedMaterialKey(const std::string& materialAssetPath,
                                         const std::string& materialOverrideSource);

// Resolves the "<model file>#material/<n>" keys of the entity's MaterialComponent
// (component path and slot paths) to runtime materials built from material n of
// that model's import, for the vertex layout of the entity's mesh. When the
// component key names the mesh's own model and the mesh has several submeshes,
// submeshes without their own slot path get the model material of their
// materialIndex. Unresolvable keys log one error per key and cache and keep the
// material that is set (the default material).
void ApplyImportedMaterialKeysForEntity(const RuntimeMaterialContext& ctx,
                                        LPENTITY entity,
                                        ImportedMaterialCache& cache);

// Gives a mesh entity without a material the default lit material (matt white).
// Does nothing if a valid material is set or the mesh asset brings its own
// material handles (e.g. glTF import), which the fallback chain of the mesh
// renderer must keep.
void EnsureDefaultMaterialForEntity(const RuntimeMaterialContext& ctx, LPENTITY entity);

// Materials made in code: a MaterialAsset in the registry; the returned handle
// is an AssetRegistry handle. RealizeMaterialForEntity turns it into a runtime
// material per entity when it is assigned.
[[nodiscard]] LPMATERIAL BuildCodeMaterial(const RuntimeMaterialContext& ctx,
                                           const char* albedoPath,
                                           const engine::math::Vec4& baseColorFactor,
                                           float roughness,
                                           float metallic);
[[nodiscard]] LPMATERIAL BuildPbrCodeMaterial(const RuntimeMaterialContext& ctx, const PBR& pbr);

} // namespace Krom::internal
