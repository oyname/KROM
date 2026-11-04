// =============================================================================
// KROM Engine - addons/editor/EditorPrefabWindow.cpp
// Prefab Live-Link (ResolvePrefabBindings). Bearbeitet werden Prefabs im Prefab-Modus (EnterPrefabMode).
// =============================================================================
#include "EditorPrefabWindow.hpp"
#include "addons/editor/EditorLocalization.hpp"
#include "core/PathUtf8.hpp"
#include "EditorFeature.hpp"
#include "EditorCommands.hpp"
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

using renderer::addons::editor::Translate;

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

bool ResetPrefabInstanceOverrides(EditorFrameContext& ctx, EntityID entity)
{
    for (size_t depth=0; entity.IsValid() && depth<=ctx.world.EntityCount(); ++depth)
    {
        if (const auto* link=ctx.world.Get<PrefabInstanceComponent>(entity))
        {
            engine::addons::prefab::PrefabAsset prefab; std::string error;
            if (!engine::addons::prefab::LoadPrefabFromFile(ResolveAbsPath(ctx,link->prefabAssetPath),ctx.registry,prefab,&error))
            { ctx.lastFileMessage=Translate("prefab.load_failed_prefix")+error; return false; }
            bool synchronized=false;
            const bool changed=engine::renderer::addons::editor::ExecuteSceneMutation(ctx,"history.reset_prefab",[&] {
                synchronized=engine::addons::prefab::SynchronizePrefabInstance(ctx.world,entity,prefab,ctx.registry,ctx.scriptRegistry,&error,false);
                if (!synchronized) return;
                if (ctx.assetPipeline)
                    engine::addons::mesh_renderer::ResolveMeshAssetBindings(*ctx.assetPipeline,ctx.registry,ctx.world);
                engine::renderer::addons::editor::ResolveMaterialAssetBindings(ctx);
            });
            if (!synchronized) ctx.lastFileMessage=Translate("prefab.reset_failed_prefix")+error;
            return changed && synchronized;
        }
        const auto* parent=ctx.world.Get<ParentComponent>(entity);
        entity=parent ? parent->parent : NULL_ENTITY;
    }
    return false;
}

void ResolvePrefabBindings(EditorFrameContext& ctx, const std::filesystem::path& changedPath)
{
    std::vector<EntityID> roots;
    ctx.world.ForEachAlive([&](EntityID id) {
        if (const auto* link = ctx.world.Get<PrefabInstanceComponent>(id); link && !link->prefabAssetPath.empty())
            roots.push_back(id);
    });
    // Outer instances first; refresh nested instances after their parent hierarchy.
    auto depth = [&](EntityID id) {
        size_t d = 0;
        while (const auto* parent = ctx.world.Get<ParentComponent>(id))
        {
            id = parent->parent;
            if (++d > ctx.world.EntityCount()) break;
        }
        return d;
    };
    std::stable_sort(roots.begin(), roots.end(), [&](EntityID a, EntityID b) { return depth(a) < depth(b); });
    for (size_t i = 0; i < roots.size(); ++i)
    {
        const auto root = roots[i];
        const auto* link = ctx.world.Get<PrefabInstanceComponent>(root);
        if (!link) continue;
        const auto path = ResolveAbsPath(ctx, link->prefabAssetPath);
        bool recursive = false;
        auto cursor = root;
        size_t ancestryDepth = 0;
        while (const auto* parent = ctx.world.Get<ParentComponent>(cursor))
        {
            cursor = parent->parent;
            if (++ancestryDepth > ctx.world.EntityCount()) { recursive = true; break; }
            if (const auto* ancestor = ctx.world.Get<PrefabInstanceComponent>(cursor))
            {
                std::error_code ec;
                if (std::filesystem::equivalent(path, ResolveAbsPath(ctx, ancestor->prefabAssetPath), ec))
                { recursive = true; break; }
            }
        }
        if (recursive)
        {
            Debug::LogWarning("ResolvePrefabBindings: rekursive Prefab-Verknuepfung '%s'", PathToUtf8(path).c_str());
            continue;
        }
        if (!changedPath.empty())
        {
            std::error_code ec;
            if (!std::filesystem::equivalent(path, changedPath, ec)) continue;
        }
        engine::addons::prefab::PrefabAsset prefab;
        std::string error;
        const auto* sceneTag = ctx.world.Get<renderer::addons::editor::EditorSceneTagComponent>(root);
        const std::string sceneName = sceneTag ? sceneTag->sceneName : std::string{};
        if (!engine::addons::prefab::LoadPrefabFromFile(path, ctx.registry, prefab, &error) ||
            !engine::addons::prefab::SynchronizePrefabInstance(ctx.world, root, prefab, ctx.registry, ctx.scriptRegistry, &error))
            Debug::LogWarning("ResolvePrefabBindings: '%s': %s", PathToUtf8(path).c_str(), error.c_str());
        // Keep newly inherited nodes in the same editor scene as their root.
        if (!sceneName.empty())
        {
            std::vector<EntityID> stack{root};
            while (!stack.empty())
            {
                const auto entity = stack.back(); stack.pop_back();
                if (!ctx.world.Has<renderer::addons::editor::EditorSceneTagComponent>(entity))
                    ctx.world.Add<renderer::addons::editor::EditorSceneTagComponent>(entity,
                        renderer::addons::editor::EditorSceneTagComponent{sceneName});
                if (const auto* children = ctx.world.Get<ChildrenComponent>(entity))
                    stack.insert(stack.end(), children->children.begin(), children->children.end());
            }
        }
        // A source update may have introduced nested instances.
        ctx.world.ForEachAlive([&](EntityID id) {
            if (ctx.world.Has<PrefabInstanceComponent>(id) && std::find(roots.begin(), roots.end(), id) == roots.end())
                roots.push_back(id);
        });
    }
    if (ctx.assetPipeline)
        engine::addons::mesh_renderer::ResolveMeshAssetBindings(*ctx.assetPipeline, ctx.registry, ctx.world);
    ResolveMaterialAssetBindings(ctx);
}

} // namespace engine::addons::editor
