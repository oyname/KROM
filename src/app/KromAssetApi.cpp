#include "KromAssetApi.hpp"
#include "KromEntityApi.hpp"

#include "addons/mesh_renderer/MeshBounds.hpp"
#include "addons/mesh_renderer/MeshRendererComponents.hpp"
#include "addons/prefab/Prefab.hpp"
#include "assets/MeshTangents.hpp"
#include "core/Debug.hpp"
#include "core/PathUtf8.hpp"
#include "ecs/Components.hpp"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

using engine::PathFromUtf8;
using engine::PathToGenericUtf8;
using engine::PathToUtf8;

namespace Krom::internal {

// =============================================================================
// Helpers
// =============================================================================

// Centre of all mesh positions of the bundle in model space (node transforms
// applied); CreateEntityFromAsset moves the model so that it sits on its root.
static engine::math::Vec3 ComputeImportedModelCenter(const engine::assets::ImportedAssetBundle& bundle) noexcept
{
    using namespace engine;
    math::Vec3 boundsMin{FLT_MAX, FLT_MAX, FLT_MAX};
    math::Vec3 boundsMax{-FLT_MAX, -FLT_MAX, -FLT_MAX};
    bool hasPosition = false;

    std::vector<math::Mat4> nodeWorld(bundle.nodes.size(), math::Mat4::Identity());
    std::vector<uint8_t> nodeWorldReady(bundle.nodes.size(), 0u);
    auto resolveNodeWorld = [&](auto&& self, size_t i) -> const math::Mat4& {
        if (nodeWorldReady[i] != 0u)
            return nodeWorld[i];
        const assets::ImportedSceneNode& node = bundle.nodes[i];
        const math::Mat4 local = math::Mat4::TRS(node.translation, node.rotation, node.scale);
        if (node.parentIndex >= 0 && static_cast<size_t>(node.parentIndex) < nodeWorld.size())
            nodeWorld[i] = self(self, static_cast<size_t>(node.parentIndex)) * local;
        else
            nodeWorld[i] = local;
        nodeWorldReady[i] = 1u;
        return nodeWorld[i];
    };
    for (size_t i = 0; i < bundle.nodes.size(); ++i)
        resolveNodeWorld(resolveNodeWorld, i);

    auto include = [&](const math::Vec3& p) {
        boundsMin.x = std::min(boundsMin.x, p.x); boundsMax.x = std::max(boundsMax.x, p.x);
        boundsMin.y = std::min(boundsMin.y, p.y); boundsMax.y = std::max(boundsMax.y, p.y);
        boundsMin.z = std::min(boundsMin.z, p.z); boundsMax.z = std::max(boundsMax.z, p.z);
        hasPosition = true;
    };

    if (!bundle.nodes.empty())
    {
        for (size_t ni = 0; ni < bundle.nodes.size(); ++ni)
        {
            const assets::ImportedSceneNode& node = bundle.nodes[ni];
            if (node.meshIndex < 0 || static_cast<size_t>(node.meshIndex) >= bundle.meshes.size())
                continue;
            const assets::MeshAsset& mesh = bundle.meshes[static_cast<size_t>(node.meshIndex)];
            for (const assets::SubMeshData& sm : mesh.submeshes)
                for (size_t pi = 0; pi + 2 < sm.positions.size(); pi += 3)
                    include(nodeWorld[ni].TransformPoint({sm.positions[pi], sm.positions[pi + 1], sm.positions[pi + 2]}));
        }
    }
    else
    {
        for (const assets::MeshAsset& mesh : bundle.meshes)
            for (const assets::SubMeshData& sm : mesh.submeshes)
                for (size_t pi = 0; pi + 2 < sm.positions.size(); pi += 3)
                    include({sm.positions[pi], sm.positions[pi + 1], sm.positions[pi + 2]});
    }

    return hasPosition ? (boundsMin + boundsMax) * 0.5f : math::Vec3::Zero();
}

// Same rule as AssetPipeline::Resolve (private): absolute paths and an empty
// asset root stay as they are, everything else is relative to the asset root.
static std::filesystem::path ResolveAssetPath(const engine::assets::AssetPipeline& assetPipeline, const char* path)
{
    const std::filesystem::path requested = PathFromUtf8(path);
    if (requested.is_absolute() || assetPipeline.GetAssetRoot().empty())
        return requested;
    return assetPipeline.GetAssetRoot() / requested;
}

static bool IsRegularFile(const std::filesystem::path& path)
{
    std::error_code ec;
    return std::filesystem::is_regular_file(path, ec);
}

static void LogFileNotFound(const engine::assets::AssetPipeline& assetPipeline, const char* path, const char* caller)
{
    engine::Debug::LogError("krom.h: %s: file not found: '%s' (resolved: '%s')",
        caller, path, PathToGenericUtf8(ResolveAssetPath(assetPipeline, path)).c_str());
}

// True if the path names an existing file; otherwise logs one error naming the
// calling krom.h function and the path.
static bool AssetFileExists(const engine::assets::AssetPipeline& assetPipeline, const char* path, const char* caller)
{
    if (IsRegularFile(ResolveAssetPath(assetPipeline, path)))
        return true;
    LogFileNotFound(assetPipeline, path, caller);
    return false;
}

enum class AssetFileKind
{
    Missing,   // neither the file nor (for "file#...") the model file exists; error logged
    File,      // the path is an existing file
    SubAsset,  // "Models/a.glb#material/0": a part of an existing model file
};

// Sub-asset keys ("model.glb#mesh/2", "model.glb#material/0") come from the
// model import (prefabs, scenes). They are no files of their own, so they are
// looked up in the registry instead of being loaded through the pipeline.
static AssetFileKind CheckAssetFile(const engine::assets::AssetPipeline& assetPipeline, const char* path, const char* caller)
{
    if (IsRegularFile(ResolveAssetPath(assetPipeline, path)))
        return AssetFileKind::File;
    if (const char* hash = std::strchr(path, '#'); hash && hash != path)
    {
        const std::string modelFile(path, hash);
        if (IsRegularFile(ResolveAssetPath(assetPipeline, modelFile.c_str())))
            return AssetFileKind::SubAsset;
    }
    LogFileNotFound(assetPipeline, path, caller);
    return AssetFileKind::Missing;
}

static void AddVertex(engine::assets::SubMeshData& submesh, const MeshVertex& vertex)
{
    submesh.positions.insert(submesh.positions.end(), {vertex.position.x, vertex.position.y, vertex.position.z});
    submesh.normals.insert(submesh.normals.end(), {vertex.normal.x, vertex.normal.y, vertex.normal.z});
    submesh.uvs.insert(submesh.uvs.end(), {vertex.uv.x, vertex.uv.y});
    submesh.colors.insert(submesh.colors.end(), {vertex.color.x, vertex.color.y, vertex.color.z, vertex.color.w});
}

// generateNormals = false keeps the normals exactly as supplied, now and in
// every later material realisation (MeshAsset::generateMissingNormals).
static LPMESH RegisterProceduralMesh(const AssetContext& ctx,
                                     engine::assets::SubMeshData submesh,
                                     const char* name,
                                     bool generateNormals,
                                     bool generateTangents)
{
    if (submesh.positions.empty())
        return NULL_MESH;

    if (generateNormals)
        GenerateMissingNormals(submesh);
    if (submesh.uvs.empty())
        submesh.uvs.assign((submesh.positions.size() / 3u) * 2u, 0.0f);
    if (submesh.colors.empty())
        submesh.colors.assign((submesh.positions.size() / 3u) * 4u, 1.0f);
    if (generateTangents)
        (void)engine::assets::EnsureTangents(submesh);

    auto mesh = std::make_unique<engine::assets::MeshAsset>();
    mesh->debugName = (name && name[0] != '\0') ? name : "KromProceduralMesh";
    mesh->path = mesh->debugName;
    mesh->state = engine::assets::AssetState::Loaded;
    mesh->generateMissingNormals = generateNormals;
    mesh->submeshes.push_back(std::move(submesh));
    mesh->gpuStatus.dirty = true;
    return ctx.assetRegistry.meshes.Add(std::move(mesh));
}

static void ApplyMaterialRecursive(const AssetContext& ctx, engine::ecs::World& world, LPENTITY entity, LPMATERIAL material)
{
    if (!world.IsAlive(entity))
        return;

    if (world.Has<engine::MeshComponent>(entity))
    {
        const LPMATERIAL runtimeMat = internal::RealizeMaterialForEntity(ctx.materials, entity, material);
        if (!world.Has<engine::MaterialComponent>(entity))
            world.Add<engine::MaterialComponent>(entity, engine::MaterialComponent{runtimeMat});
        else
            world.Get<engine::MaterialComponent>(entity)->material = runtimeMat;
    }

    if (const auto* children = world.Get<engine::ChildrenComponent>(entity))
    {
        for (const LPENTITY child : children->children)
            ApplyMaterialRecursive(ctx, world, child, material);
    }
}

static bool BundleHasSkeletonsOrAnimations(const engine::assets::ImportedAssetBundle& bundle) noexcept
{
    if (!bundle.skeletons.empty() || !bundle.animations.empty())
        return true;
    for (const auto& mesh : bundle.meshes)
    {
        if (mesh.HasSkinning())
            return true;
    }
    return false;
}

// =============================================================================
// Model import
// =============================================================================

LPENTITY CreateEntityFromAsset(const AssetContext& ctx, const char* path, const char* name)
{
    if (!ctx.world || !ctx.assetPipeline || !path || path[0] == '\0')
        return NULL_LPENTITY;

    if (!AssetFileExists(*ctx.assetPipeline, path, "CreateEntityFromAsset"))
        return NULL_LPENTITY;

    // Extension without a registered importer: load the file as one simple mesh
    // (text mesh format of AssetPipeline::ReloadMesh, .kmesh).
    if (!ctx.assetPipeline->HasMeshImporterFor(path))
    {
        engine::Debug::Log("krom.h: CreateEntityFromAsset: no model importer for '%s', loading it as a simple mesh", path);
        const LPENTITY entity = CreateEntity(ctx.world, (name && name[0] != '\0') ? name : path);
        if (!entity.IsValid())
            return NULL_LPENTITY;
        if (!SetMeshFromAsset(ctx, entity, path))
        {
            DestroyEntityRecursive(*ctx.world, entity);
            return NULL_LPENTITY;
        }
        return entity;
    }

    // Format-agnostisch: AssetPipeline wählt den passenden registrierten Importer.
    // Ob GLB, FBX, OBJ oder ein anderes Format — der Wrapper muss es nicht wissen.
    engine::assets::ImportedAssetBundle bundle = ctx.assetPipeline->ImportBundle(path);

    if (!bundle.Ok())
    {
        // ImportBundle already logged the importer error (bundle.error).
        engine::Debug::LogError("krom.h: CreateEntityFromAsset: import of '%s' failed", path);
        return NULL_LPENTITY;
    }

    const std::filesystem::path logicalPath = PathFromUtf8(path);

    const engine::math::Vec3 modelCenter = ComputeImportedModelCenter(bundle);
    const bool needsAnimationSystems = BundleHasSkeletonsOrAnimations(bundle);
    if (needsAnimationSystems)
        ctx.ensureAnimationSystems();

    engine::assets::MeshAsset* sharedFallbackMesh =
        !bundle.meshes.empty() ? &bundle.meshes[0] : nullptr;
    const engine::assets::MaterialAsset* sharedFallbackAsset =
        !bundle.materials.empty() ? &bundle.materials[0] : nullptr;
    const std::string sharedFallbackName =
        ((name && name[0] != '\0') ? std::string(name) : PathToUtf8(logicalPath.stem())) + "_SharedRuntimeMaterial";
    const engine::MaterialHandle sharedFallbackMaterial =
        sharedFallbackMesh
            ? internal::CreateRuntimeLitMaterialFromAssets(
                ctx.materials, *sharedFallbackMesh, sharedFallbackAsset, sharedFallbackName.c_str())
            : engine::MaterialHandle::Invalid();

    engine::addons::prefab::PrefabBuildOptions buildOptions;
    buildOptions.name = (name && name[0] != '\0') ? name : PathToUtf8(logicalPath.stem());
    buildOptions.createSyntheticRoot = true;
    buildOptions.playFirstAnimation = true;
    buildOptions.loopAnimations = true;

    engine::addons::prefab::PrefabAsset prefab =
        engine::addons::prefab::BuildPrefabFromImportedBundle(
            std::move(bundle), ctx.assetRegistry, buildOptions);

    for (engine::addons::prefab::PrefabEntityRecord& record : prefab.records)
    {
        if (record.parentIndex == static_cast<int32_t>(prefab.rootIndex))
            record.localPosition -= modelCenter;

        if (record.mesh.IsValid())
        {
            auto* meshAsset = ctx.assetRegistry.meshes.Get(record.mesh);
            const auto* materialAsset = record.material.IsValid()
                ? ctx.assetRegistry.materials.Get(record.material)
                : nullptr;
            if (meshAsset)
            {
                const std::string runtimeName = !record.name.empty()
                    ? record.name + "_RuntimeMaterial"
                    : buildOptions.name + "_RuntimeMaterial";
                const engine::MaterialHandle runtimeMaterial =
                    internal::CreateRuntimeLitMaterialFromAssets(ctx.materials, *meshAsset, materialAsset, runtimeName.c_str());
                record.material = runtimeMaterial.IsValid() ? runtimeMaterial : sharedFallbackMaterial;
                if (!record.material.IsValid())
                    engine::Debug::LogError("krom.h: kein Material für Node '%s' in '%s'",
                        record.name.c_str(), path);
            }
        }
    }

    engine::addons::prefab::PrefabInstantiateOptions options{};
    options.scriptRegistry = &ctx.scriptAddonRegistry;
    const engine::addons::prefab::PrefabInstance instance =
        engine::addons::prefab::InstantiatePrefab(*ctx.world, prefab, options);
    if (!instance.IsValid())
    {
        engine::Debug::LogError("krom.h: Instantiierung fehlgeschlagen: '%s'", path);
        return NULL_LPENTITY;
    }
    for (const LPENTITY instantiatedEntity : instance.entities)
    {
        if (instantiatedEntity.IsValid() && ctx.world->Has<engine::MeshComponent>(instantiatedEntity))
            engine::mesh_renderer::UpdateLocalBoundsForEntity(*ctx.world, instantiatedEntity, ctx.assetRegistry);
    }
    ctx.transformSystem.Update(*ctx.world);
    ctx.boundsSystem.Update(*ctx.world);
    return instance.root;
}

bool PrintAssetTree(engine::assets::AssetPipeline* assetPipeline, const char* path)
{
    if (!assetPipeline || !path || path[0] == '\0')
    {
        engine::Debug::LogError("krom.h: PrintAssetTree requires Graphics3D/Graphics and a valid path");
        return false;
    }

    const engine::assets::ImportedAssetBundle bundle = assetPipeline->ImportBundle(path);
    if (!bundle.Ok())
    {
        engine::Debug::LogError("krom.h: PrintAssetTree import failed for '%s': %s",
            path, bundle.error.c_str());
        return false;
    }

    engine::Debug::Log("Asset tree: %s", path);
    engine::Debug::Log("  nodes=%zu meshes=%zu materials=%zu skeletons=%zu animations=%zu",
        bundle.nodes.size(),
        bundle.meshes.size(),
        bundle.materials.size(),
        bundle.skeletons.size(),
        bundle.animations.size());

    std::vector<std::vector<size_t>> children(bundle.nodes.size());
    std::vector<size_t> roots;
    for (size_t i = 0; i < bundle.nodes.size(); ++i)
    {
        const int32_t parent = bundle.nodes[i].parentIndex;
        if (parent >= 0 && static_cast<size_t>(parent) < bundle.nodes.size())
            children[static_cast<size_t>(parent)].push_back(i);
        else
            roots.push_back(i);
    }

    const auto printMesh = [&](int32_t meshIndex, const std::string& indent)
    {
        if (meshIndex < 0 || static_cast<size_t>(meshIndex) >= bundle.meshes.size())
            return;

        const engine::assets::MeshAsset& mesh = bundle.meshes[static_cast<size_t>(meshIndex)];
        engine::Debug::Log("%s  mesh[%d] submeshes=%zu materials=%zu",
            indent.c_str(), meshIndex, mesh.submeshes.size(), mesh.materialHandles.size());
        for (size_t si = 0; si < mesh.submeshes.size(); ++si)
        {
            const engine::assets::SubMeshData& submesh = mesh.submeshes[si];
            engine::Debug::Log("%s    submesh[%zu] vertices=%zu indices=%zu materialIndex=%u",
                indent.c_str(),
                si,
                submesh.positions.size() / 3u,
                submesh.indices.size(),
                submesh.materialIndex);
        }
    };

    std::function<void(size_t, std::string)> printNode =
        [&](size_t nodeIndex, std::string indent)
    {
        const engine::assets::ImportedSceneNode& node = bundle.nodes[nodeIndex];
        const char* nodeName = node.name.empty() ? "<unnamed>" : node.name.c_str();
        engine::Debug::Log("%snode[%zu] '%s' parent=%d mesh=%d skin=%d",
            indent.c_str(),
            nodeIndex,
            nodeName,
            node.parentIndex,
            node.meshIndex,
            node.skinIndex);
        printMesh(node.meshIndex, indent);
        for (const size_t child : children[nodeIndex])
            printNode(child, indent + "  ");
    };

    if (bundle.nodes.empty())
    {
        for (size_t meshIndex = 0; meshIndex < bundle.meshes.size(); ++meshIndex)
            printMesh(static_cast<int32_t>(meshIndex), "  ");
    }
    else
    {
        for (const size_t root : roots)
            printNode(root, "  ");
    }

    for (size_t i = 0; i < bundle.materials.size(); ++i)
    {
        const engine::assets::MaterialAsset& material = bundle.materials[i];
        engine::Debug::Log("  material[%zu] name='%s' template='%s' baseColor='%s' normal='%s' orm='%s'",
            i,
            material.debugName.empty() ? "<unnamed>" : material.debugName.c_str(),
            material.templateName.c_str(),
            material.baseColorTexture.path.c_str(),
            material.normalTexture.path.c_str(),
            material.metallicRoughnessTexture.path.c_str());
    }

    return true;
}

// =============================================================================
// Loading
// =============================================================================

LPMESH LoadMesh(engine::assets::AssetPipeline* assetPipeline,
                engine::assets::AssetRegistry& assetRegistry,
                const char* path,
                const char* caller)
{
    if (!assetPipeline || !path || path[0] == '\0')
        return NULL_MESH;
    const AssetFileKind kind = CheckAssetFile(*assetPipeline, path, caller);
    if (kind == AssetFileKind::Missing)
        return NULL_MESH;

    // A sub-asset key is no file: AssetPipeline::LoadMesh would mark the
    // imported entry Failed, so only take it from the registry.
    const LPMESH mesh = kind == AssetFileKind::SubAsset
        ? assetRegistry.FindMeshByPath(path)
        : assetPipeline->LoadMesh(path);
    const engine::assets::MeshAsset* asset = mesh.IsValid() ? assetRegistry.meshes.Get(mesh) : nullptr;
    if (!asset || asset->state != engine::assets::AssetState::Loaded)
    {
        engine::Debug::LogError("krom.h: %s: mesh '%s' could not be loaded", caller, path);
        return NULL_MESH;
    }
    return mesh;
}

LPTEXTURE LoadTexture(engine::assets::AssetPipeline* assetPipeline,
                      TexturePathMap& texturePaths,
                      const char* path)
{
    const LPTEXTURE texture = (assetPipeline && path) ? assetPipeline->LoadTexture(path) : NULL_TEXTURE;
    if (texture.IsValid() && path && path[0] != '\0')
        texturePaths[texture.value] = path;
    return texture;
}

LPMATERIAL LoadMaterial(engine::assets::AssetPipeline* assetPipeline,
                        engine::assets::AssetRegistry& assetRegistry,
                        const char* path,
                        const char* caller)
{
    if (!assetPipeline || !path || path[0] == '\0')
        return NULL_MATERIAL;
    const AssetFileKind kind = CheckAssetFile(*assetPipeline, path, caller);
    if (kind == AssetFileKind::Missing)
        return NULL_MATERIAL;

    // See LoadMesh: sub-asset keys only come from the registry.
    const LPMATERIAL material = kind == AssetFileKind::SubAsset
        ? assetRegistry.FindMaterialByPath(path)
        : assetPipeline->LoadMaterial(path);
    const engine::assets::MaterialAsset* asset =
        material.IsValid() ? assetRegistry.materials.Get(material) : nullptr;
    if (!asset || asset->state != engine::assets::AssetState::Loaded)
    {
        engine::Debug::LogError("krom.h: %s: material '%s' could not be loaded", caller, path);
        return NULL_MATERIAL;
    }
    return material;
}

// =============================================================================
// Materials made through krom.h
// =============================================================================

LPMATERIAL CreateTextureMaterial(const AssetContext& ctx,
                                 const char* albedoTexturePath,
                                 int materialModel,
                                 engine::math::Vec4 baseColorFactor,
                                 float roughness,
                                 float metallic)
{
    if (!ctx.initialized)
        return NULL_MATERIAL;

    if (materialModel != MaterialModel::Lit)
    {
        engine::Debug::LogWarning(
            "krom.h: CreateTextureMaterial currently supports Lit for EntityTexture; use .mat/PBR workflow for advanced materials");
    }

    return internal::BuildCodeMaterial(ctx.materials, albedoTexturePath, baseColorFactor, roughness, metallic);
}

LPMATERIAL CreateTextureMaterial(const AssetContext& ctx,
                                 LPTEXTURE texture,
                                 int materialModel,
                                 engine::math::Vec4 baseColorFactor,
                                 float roughness,
                                 float metallic)
{
    const auto it = ctx.materials.texturePaths.find(texture.value);
    if (it == ctx.materials.texturePaths.end())
    {
        engine::Debug::LogError("krom.h: texture handle has no known path; load it through Krom::LoadTexture/LoadTexture first");
        return NULL_MATERIAL;
    }
    return CreateTextureMaterial(ctx, it->second.c_str(), materialModel, baseColorFactor, roughness, metallic);
}

LPMATERIAL CreateLitMaterial(const AssetContext& ctx,
                             const char* albedoTexturePath,
                             engine::math::Vec4 baseColorFactor,
                             float roughness,
                             float metallic)
{
    if (!ctx.initialized)
        return NULL_MATERIAL;
    return internal::BuildCodeMaterial(ctx.materials, albedoTexturePath, baseColorFactor, roughness, metallic);
}

// =============================================================================
// Assignment
// =============================================================================

bool SetMesh(const AssetContext& ctx, LPENTITY entity, LPMESH mesh)
{
    if (!ctx.world || !ctx.world->IsAlive(entity) || !mesh.IsValid())
        return false;

    if (!ctx.world->Has<engine::MeshComponent>(entity))
        ctx.world->Add<engine::MeshComponent>(entity, engine::MeshComponent{mesh});
    else
        ctx.world->Get<engine::MeshComponent>(entity)->mesh = mesh;

    if (!ctx.world->Has<engine::BoundsComponent>(entity))
        ctx.world->Add<engine::BoundsComponent>(entity);
    engine::mesh_renderer::UpdateLocalBoundsForEntity(*ctx.world, entity, ctx.assetRegistry);

    // Objekt ohne Material → Default-Lit (matt, weiß) zuweisen, damit es
    // sichtbar gerendert wird statt im Renderer übersprungen zu werden.
    internal::EnsureDefaultMaterialForEntity(ctx.materials, entity);
    return true;
}

bool SetMaterial(const AssetContext& ctx, LPENTITY entity, LPMATERIAL material)
{
    if (!ctx.world || !ctx.world->IsAlive(entity) || !material.IsValid())
        return false;

    const LPMATERIAL runtimeMat = internal::RealizeMaterialForEntity(ctx.materials, entity, material);
    if (!ctx.world->Has<engine::MaterialComponent>(entity))
        ctx.world->Add<engine::MaterialComponent>(entity, engine::MaterialComponent{runtimeMat});
    else
        ctx.world->Get<engine::MaterialComponent>(entity)->material = runtimeMat;
    return true;
}

bool SetMaterialRecursive(const AssetContext& ctx, LPENTITY rootEntity, LPMATERIAL material)
{
    if (!ctx.world || !ctx.world->IsAlive(rootEntity) || !material.IsValid())
        return false;

    ApplyMaterialRecursive(ctx, *ctx.world, rootEntity, material);
    return true;
}

bool SetTexture(const AssetContext& ctx, LPENTITY entity, const char* albedoTexturePath, int materialModel, bool recursive)
{
    if (!albedoTexturePath || albedoTexturePath[0] == '\0')
        return false;

    const LPMATERIAL material = Krom::CreateTextureMaterial(albedoTexturePath, materialModel);
    if (!material.IsValid())
        return false;

    return recursive ? SetMaterialRecursive(ctx, entity, material) : SetMaterial(ctx, entity, material);
}

bool SetTexture(const AssetContext& ctx, LPENTITY entity, LPTEXTURE texture, int materialModel, bool recursive)
{
    const LPMATERIAL material = Krom::CreateTextureMaterial(texture, materialModel);
    if (!material.IsValid())
        return false;

    return recursive ? SetMaterialRecursive(ctx, entity, material) : SetMaterial(ctx, entity, material);
}

bool SetPBR(const AssetContext& ctx, LPENTITY entity, const PBR& materialDesc, bool recursive)
{
    if (!ctx.initialized || !ctx.world || !ctx.world->IsAlive(entity))
        return false;

    const LPMATERIAL material = internal::BuildPbrCodeMaterial(ctx.materials, materialDesc);
    if (!material.IsValid())
        return false;

    return recursive ? SetMaterialRecursive(ctx, entity, material) : SetMaterial(ctx, entity, material);
}

bool SetMeshFromAsset(const AssetContext& ctx, LPENTITY entity, const char* path)
{
    if (!ctx.world || !ctx.world->IsAlive(entity))
        return false;
    const LPMESH mesh = LoadMesh(ctx.assetPipeline, ctx.assetRegistry, path, "SetMeshFromAsset");
    return mesh.IsValid() && SetMesh(ctx, entity, mesh);
}

bool SetMaterialFromAsset(const AssetContext& ctx, LPENTITY entity, const char* path)
{
    if (!ctx.world || !ctx.world->IsAlive(entity))
        return false;
    const LPMATERIAL material = LoadMaterial(ctx.assetPipeline, ctx.assetRegistry, path, "SetMaterialFromAsset");
    if (!material.IsValid())
        return false;

    const bool ok = SetMaterial(ctx, entity, material);
    if (ok && ctx.world->Has<engine::MaterialComponent>(entity))
        ctx.world->Get<engine::MaterialComponent>(entity)->materialAssetPath = path ? path : "";
    return ok;
}

// =============================================================================
// Procedural meshes
// =============================================================================

LPENTITY CreateProceduralEntity(const AssetContext& ctx, LPMESH mesh, const char* name)
{
    if (!mesh.IsValid())
        return NULL_LPENTITY;

    LPENTITY entity = CreateEntity(ctx.world, (name && name[0] != '\0') ? name : "ProceduralEntity");
    if (!entity.IsValid() || !SetMesh(ctx, entity, mesh))
        return NULL_LPENTITY;

    const LPMATERIAL material = internal::BuildCodeMaterial(ctx.materials,
        nullptr,
        engine::math::Vec4{0.82f, 0.82f, 0.78f, 1.0f},
        0.8f,
        0.0f);
    if (material.IsValid())
        (void)SetMaterial(ctx, entity, material);
    return entity;
}

LPMESH CreateMeshAsset(const AssetContext& ctx, const MeshData& meshData)
{
    if (!ctx.initialized || !meshData.vertices || meshData.vertexCount == 0u)
        return NULL_MESH;

    engine::assets::SubMeshData submesh;
    submesh.positions.reserve(static_cast<size_t>(meshData.vertexCount) * 3u);
    submesh.normals.reserve(static_cast<size_t>(meshData.vertexCount) * 3u);
    submesh.uvs.reserve(static_cast<size_t>(meshData.vertexCount) * 2u);
    submesh.colors.reserve(static_cast<size_t>(meshData.vertexCount) * 4u);

    for (uint32_t i = 0u; i < meshData.vertexCount; ++i)
        AddVertex(submesh, meshData.vertices[i]);

    if (meshData.indices && meshData.indexCount > 0u)
        submesh.indices.assign(meshData.indices, meshData.indices + meshData.indexCount);
    else
    {
        submesh.indices.reserve(meshData.vertexCount);
        for (uint32_t i = 0u; i < meshData.vertexCount; ++i)
            submesh.indices.push_back(i);
    }

    return RegisterProceduralMesh(ctx, std::move(submesh), meshData.name,
                                  meshData.generateNormals, meshData.generateTangents);
}

LPENTITY CreateMeshEntity(const AssetContext& ctx, const MeshData& meshData)
{
    const LPMESH mesh = CreateMeshAsset(ctx, meshData);
    return CreateProceduralEntity(ctx, mesh, meshData.name);
}

LPENTITY CreateMesh(const AssetContext& ctx, const char* name)
{
    if (!ctx.initialized || !ctx.world)
        return NULL_LPENTITY;

    auto meshAsset = std::make_unique<engine::assets::MeshAsset>();
    meshAsset->debugName = (name && name[0] != '\0') ? name : "Mesh";
    meshAsset->path = meshAsset->debugName;
    meshAsset->state = engine::assets::AssetState::Loaded;
    meshAsset->gpuStatus.dirty = true;

    const LPMESH mesh = ctx.assetRegistry.meshes.Add(std::move(meshAsset));
    LPENTITY entity = CreateEntity(ctx.world, (name && name[0] != '\0') ? name : "Mesh");
    if (!entity.IsValid() || !SetMesh(ctx, entity, mesh))
        return NULL_LPENTITY;
    return entity;
}

} // namespace Krom::internal
