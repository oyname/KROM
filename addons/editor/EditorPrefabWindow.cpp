// =============================================================================
// KROM Engine - addons/editor/EditorPrefabWindow.cpp
// Prefab Live-Link (ResolvePrefabBindings). Bearbeitet werden Prefabs im Prefab-Modus (EnterPrefabMode).
// =============================================================================
#include "EditorPrefabWindow.hpp"
#include "core/PathUtf8.hpp"
#include "EditorFeature.hpp"
#include "EditorComponents.hpp"
#include "EditorMaterialLibrary.hpp"

#include "addons/prefab/Prefab.hpp"
#include "addons/prefab/PrefabInstanceComponent.hpp"
#include "addons/mesh_renderer/MeshRendererComponents.hpp"
#include "addons/mesh_renderer/MeshRendererSerialization.hpp"
#include "assets/AssetRegistry.hpp"
#include "assets/AssetPipeline.hpp"
#include "ecs/World.hpp"
#include "core/Logger.hpp"
#include "renderer/RenderLayers.hpp"

#ifdef KROM_EDITOR_HAS_IMGUI
#include "imgui.h"
#endif

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <filesystem>
#include <string>
#include <vector>

namespace engine::addons::editor {

using EditorFrameContext = engine::renderer::addons::editor::EditorFrameContext;

// ============================================================================
//  Interne Hilfsfunktionen
// ============================================================================

namespace {

// Loest einen relativen Asset-Pfad zu einem absoluten Pfad auf.
std::filesystem::path ResolveAbsPath(const EditorFrameContext& ctx,
                                     const std::string& relPath)
{
    std::filesystem::path p = PathFromUtf8(relPath);
    if (p.is_absolute()) return p;
    if (!ctx.currentProjectRoot.empty())
        return PathFromUtf8(ctx.currentProjectRoot) / "Assets" / p;
    return p;
}

} // namespace

// ============================================================================
//  ResolvePrefabBindings — Live-Link Kern
// ============================================================================

void ResolvePrefabBindings(EditorFrameContext& ctx)
{
    if (!ctx.assetPipeline)
        return;

    // Alle Instanz-Roots sammeln (nicht waehrend ForEach modifizieren)
    struct InstanceInfo
    {
        EntityID    root;
        std::string prefabAssetPath;
        math::Vec3  position;
        math::Quat  rotation;
        math::Vec3  scale;
        bool        overrideTransform;
        EntityID    parent = NULL_ENTITY;      // Instanz haengt unter einem Objekt
        std::string name;
        bool        hasOrder = false;
        uint32_t    order = 0u;
    };
    std::vector<InstanceInfo> instances;

    ctx.world.ForEachAlive([&](EntityID id)
        {
            PrefabInstanceComponent* pic = ctx.world.Get<PrefabInstanceComponent>(id);
            if (!pic || pic->prefabAssetPath.empty()) return;
            InstanceInfo info;
            info.root             = id;
            info.prefabAssetPath  = pic->prefabAssetPath;
            info.overrideTransform = pic->overrideTransform;
            if (const auto* parent = ctx.world.Get<ParentComponent>(id))
                info.parent = parent->parent;
            if (const auto* name = ctx.world.Get<NameComponent>(id))
                info.name = name->name;
            if (const auto* hierarchy = ctx.world.Get<renderer::addons::editor::EditorHierarchyComponent>(id))
            {
                info.hasOrder = true;
                info.order = hierarchy->order;
            }
            if (const auto* t = ctx.world.Get<TransformComponent>(id))
            {
                info.position = t->localPosition;
                info.rotation = t->localRotation;
                info.scale    = t->localScale;
            }
            else
            {
                info.position = {0.f, 0.f, 0.f};
                info.rotation = math::Quat::Identity();
                info.scale    = {1.f, 1.f, 1.f};
            }
            instances.push_back(info);
        });

    for (const InstanceInfo& info : instances)
    {
        const std::filesystem::path absPath = ResolveAbsPath(ctx, info.prefabAssetPath);

        engine::addons::prefab::PrefabAsset prefab;
        std::string error;
        if (!engine::addons::prefab::LoadPrefabFromFile(absPath, ctx.registry, prefab, &error))
        {
            Debug::LogWarning("ResolvePrefabBindings: Prefab nicht geladen '%s': %s",
                              PathToUtf8(absPath).c_str(),
                              error.c_str());
            continue;
        }

        // Alte Instanz zerstoeren
        engine::addons::prefab::DestroyPrefabInstanceFromRoot(ctx.world, info.root);

        // Neue Instanz spawnen
        engine::addons::prefab::PrefabInstantiateOptions opts;
        opts.scriptRegistry = ctx.scriptRegistry;
        if (info.overrideTransform)
        {
            opts.position = info.position;
            opts.rotation = info.rotation;
            opts.scale    = info.scale;
        }

        const engine::addons::prefab::PrefabInstance instance =
            engine::addons::prefab::InstantiatePrefab(ctx.world, prefab, opts);

        if (!instance.IsValid())
        {
            Debug::LogWarning("ResolvePrefabBindings: Instanziierung fehlgeschlagen '%s'",
                              info.prefabAssetPath.c_str());
            continue;
        }

        // PrefabInstanceComponent auf neue Root-Entity setzen
        ctx.world.Add<PrefabInstanceComponent>(instance.root,
            PrefabInstanceComponent{ info.prefabAssetPath, info.overrideTransform });

        // Wieder dort einhaengen, wo die alte Instanz war (Parent, Reihenfolge, Name).
        if (info.parent.IsValid() && ctx.world.IsAlive(info.parent))
        {
            if (auto* children = ctx.world.Get<ChildrenComponent>(info.parent))
            {
                children->Remove(info.root);
                children->Add(instance.root);
            }
            else
            {
                ctx.world.Add<ChildrenComponent>(info.parent).Add(instance.root);
            }
            ctx.world.Add<ParentComponent>(instance.root, ParentComponent{info.parent});
        }
        if (!info.name.empty())
        {
            if (auto* name = ctx.world.Get<NameComponent>(instance.root))
                name->name = info.name;
        }
        if (info.hasOrder)
        {
            ctx.world.Add<renderer::addons::editor::EditorHierarchyComponent>(
                instance.root, renderer::addons::editor::EditorHierarchyComponent{info.order});
        }
    }

    // Einmal alle Meshes + Materialien aufloesen
    engine::addons::mesh_renderer::ResolveMeshAssetBindings(
        *ctx.assetPipeline, ctx.registry, ctx.world);
    ResolveMaterialAssetBindings(ctx);
}

} // namespace engine::addons::editor
