#include "KromSceneManager.hpp"
#include "KromEntityApi.hpp"

#include "addons/camera/CameraComponents.hpp"
#include "addons/camera/CameraSerialization.hpp"
#include "addons/forward/ForwardTonemapJson.hpp"
#include "addons/lighting/LightingSerialization.hpp"
#include "addons/mesh_renderer/MeshAssetSceneBindings.hpp"
#include "addons/mesh_renderer/MeshRendererComponents.hpp"
#include "addons/mesh_renderer/MeshRendererSerialization.hpp"
#include "addons/particles/ParticleSerialization.hpp"
#include "addons/physics/PhysicsSerialization.hpp"
#include "addons/audio/AudioSerialization.hpp"
#include "addons/postprocess/PostProcessSerialization.hpp"
#include "addons/prefab/PrefabSerialization.hpp"
#include "addons/probes/ProbeSerialization.hpp"
#include "addons/script/ScriptSerialization.hpp"
#include "core/Debug.hpp"
#include "core/PathUtf8.hpp"
#include "ecs/Components.hpp"
#include "serialization/SceneSerializer.hpp"

#include <algorithm>
#include <chrono>
#include <fstream>
#include <functional>
#include <iterator>

using engine::PathFromUtf8;
using engine::PathToUtf8;

namespace Krom::internal {

// =============================================================================
// .kscene Parser
// =============================================================================

// .kscene ist das SceneSerializer-Format (Entities mit "components"-Array)
// plus Root-Metadaten (scene, version, persistent, environment).
// Das alte, handgeschriebene Schema (model/light/camera direkt am Entity)
// wird nicht mehr unterstuetzt — solche Dateien im Editor neu bauen.
static bool HasLegacyKsceneEntities(const engine::serialization::JsonValue& root) noexcept
{
    const auto* arr = root.Get("entities");
    if (!arr || !arr->IsArray())
        return false;

    for (const auto& e : arr->arrayVal)
    {
        const auto* components = e.Get("components");
        if (!components || !components->IsArray())
            return true;
    }
    return false;
}

static KromSceneData ParseKscene(const std::string& json)
{
    KromSceneData data;
    std::string err;
    const engine::serialization::JsonValue root =
        engine::serialization::JsonParser::Parse(json, err);
    if (!err.empty())
    {
        engine::Debug::LogError("krom.h: kscene JSON-Fehler: %s", err.c_str());
        return data;
    }

    if (const auto* s = root.Get("scene"))     data.name       = s->AsString();
    if (const auto* p = root.Get("persistent")) data.persistent = p->AsBool();
    if (const auto* v = root.Get("version"))    data.version    = v->AsInt();
    if (const auto* env = root.Get("environment"))
    {
        if (env->IsObject())
        {
            if (const auto* path = env->Get("texturePath"); path && path->IsString())
            {
                data.environmentTexturePath = path->AsString();
                data.hasEnvironment = !data.environmentTexturePath.empty();
            }
            if (const auto* enable = env->Get("enableIBL"); enable && enable->IsBool())
                data.environmentEnableIBL = enable->AsBool();
            if (const auto* removeSun = env->Get("removeSunFromLighting"); removeSun && removeSun->IsBool())
                data.environmentRemoveSunFromLighting = removeSun->AsBool();
            if (const auto* intensity = env->Get("iblIntensity"); intensity && intensity->IsNumber())
                data.environmentIntensity = std::max(0.0f, intensity->AsFloat());
            if (const auto* bg = env->Get("backgroundColor"); bg && bg->IsArray() && bg->arrayVal.size() >= 3u)
            {
                data.backgroundColor[0] = bg->arrayVal[0].AsFloat();
                data.backgroundColor[1] = bg->arrayVal[1].AsFloat();
                data.backgroundColor[2] = bg->arrayVal[2].AsFloat();
                data.backgroundColor[3] = bg->arrayVal.size() >= 4u ? bg->arrayVal[3].AsFloat() : 1.f;
                data.hasBackgroundColor = true;
            }
            if (const auto* amb = env->Get("ambientColor"); amb && amb->IsArray() && amb->arrayVal.size() >= 3u)
            {
                data.ambientColor = {amb->arrayVal[0].AsFloat(),
                                     amb->arrayVal[1].AsFloat(),
                                     amb->arrayVal[2].AsFloat()};
                data.hasAmbientColor = true;
            }
            if (const auto* ambI = env->Get("ambientIntensity"); ambI && ambI->IsNumber())
            {
                data.ambientIntensity = std::max(0.0f, ambI->AsFloat());
                data.hasAmbientIntensity = true;
            }
            if (const auto* ev = env->Get("exposureEv"); ev && ev->IsNumber())
                data.exposureEv = std::clamp(ev->AsFloat(), engine::renderer::addons::forward::kMinExposureEv,
                                             engine::renderer::addons::forward::kMaxExposureEv);
            engine::renderer::addons::forward::ReadSceneExposureJson(*env, data.exposureMode, data.autoExposure);
            engine::renderer::addons::forward::ReadSceneColorGradingJson(*env, data.grading);
            data.legacyRendering = engine::ReadLegacySceneRenderSettings(root);
            if (const auto* passes = env->Get("probeBakePasses"); passes && passes->IsNumber())
                data.probeBakePasses = passes->AsInt() >= 2 ? 2u : 1u;
        }
    }

    if (HasLegacyKsceneEntities(root))
    {
        engine::Debug::LogError("krom.h: kscene '%s' nutzt das alte Format (version %d) — "
            "bitte im Editor neu bauen (Build)", data.name.c_str(), data.version);
        return data;
    }

    data.sourceJson = json;
    data.valid = true;
    return data;
}

// =============================================================================
// Scene Entity-Instantiierung
// =============================================================================

// =============================================================================
// Scene roots: SceneRecord::rootEntities plus the index SceneManagerState::rootScenes
// =============================================================================

static void RemoveSceneRoot(SceneManagerState& manager, LPENTITY entity)
{
    const auto it = manager.rootScenes.find(entity.value);
    if (it == manager.rootScenes.end())
        return;
    if (const auto sceneIt = manager.scenes.find(it->second); sceneIt != manager.scenes.end())
    {
        // erase, not swap-and-pop: FindEntityInScene depends on the root order.
        std::vector<LPENTITY>& roots = sceneIt->second.rootEntities;
        if (const auto pos = std::find(roots.begin(), roots.end(), entity); pos != roots.end())
            roots.erase(pos);
    }
    manager.rootScenes.erase(it);
}

static void AddSceneRoot(SceneManagerState& manager, uint32_t sceneId, LPENTITY entity)
{
    const auto sceneIt = manager.scenes.find(sceneId);
    if (sceneIt == manager.scenes.end())
        return;
    if (const auto it = manager.rootScenes.find(entity.value); it != manager.rootScenes.end())
    {
        if (it->second == sceneId)
            return;
        RemoveSceneRoot(manager, entity);   // a root belongs to one scene
    }
    sceneIt->second.rootEntities.push_back(entity);
    manager.rootScenes.emplace(entity.value, sceneId);
}

// Removes the scene record and the index entries of its roots.
static void ForgetScene(SceneManagerState& manager, uint32_t sceneId)
{
    const auto sceneIt = manager.scenes.find(sceneId);
    if (sceneIt == manager.scenes.end())
        return;
    for (const LPENTITY root : sceneIt->second.rootEntities)
        if (const auto it = manager.rootScenes.find(root.value); it != manager.rootScenes.end() && it->second == sceneId)
            manager.rootScenes.erase(it);
    manager.scenes.erase(sceneIt);
}

void ForgetDestroyedEntities(SceneManagerState& manager, const engine::ecs::World& world, LPENTITY subtreeRoot)
{
    if (!world.IsAlive(subtreeRoot) || (manager.rootScenes.empty() && manager.persistentEntityIds.empty()))
        return;
    // Same entities as DestroyEntityRecursive: the subtree through ChildrenComponent.
    std::vector<LPENTITY> stack{subtreeRoot};
    while (!stack.empty())
    {
        const LPENTITY entity = stack.back();
        stack.pop_back();
        if (!world.IsAlive(entity))
            continue;
        RemoveSceneRoot(manager, entity);
        manager.persistentEntityIds.erase(entity.value);
        if (const auto* children = world.Get<engine::ChildrenComponent>(entity))
            stack.insert(stack.end(), children->children.begin(), children->children.end());
    }
}

void ClearScenes(SceneManagerState& manager)
{
    manager.scenes.clear();
    manager.rootScenes.clear();
    manager.persistentEntityIds.clear();
}

static void MarkPersistentRecursive(const SceneContext& ctx, LPENTITY entity)
{
    if (!ctx.world || !ctx.world->IsAlive(entity))
        return;

    ctx.manager.persistentEntityIds.insert(entity.value);
    if (const auto* children = ctx.world->Get<engine::ChildrenComponent>(entity))
    {
        for (const LPENTITY child : children->children)
            MarkPersistentRecursive(ctx, child);
    }
}

static void PreservePersistentSubtrees(engine::ecs::World& world,
                                       LPENTITY entity,
                                       const std::unordered_set<uint32_t>& persistentEntityIds)
{
    if (!world.IsAlive(entity))
        return;

    if (const auto* children = world.Get<engine::ChildrenComponent>(entity))
    {
        const auto copy = children->children;
        for (const LPENTITY child : copy)
        {
            if (!world.IsAlive(child))
                continue;

            if (persistentEntityIds.count(child.value) != 0u)
            {
                DetachEntityFromParent(world, child);
                continue;
            }

            PreservePersistentSubtrees(world, child, persistentEntityIds);
        }
    }
}

// Spawns gehoeren zur Szene des aufrufenden Objekts, nicht einfach zur zuletzt
// geladenen Szene. Parent-Ketten decken auch Scripts auf Prefab-Kindern ab.
void RegisterScriptSpawn(const SceneContext& ctx, LPENTITY owner, LPENTITY root)
{
    if (!ctx.world || !ctx.world->IsAlive(root))
        return;
    // One index lookup per ancestor; the depth limit guards against parent cycles.
    LPENTITY current = owner;
    for (uint32_t depth = 0u; depth < 1024u && ctx.world->IsAlive(current); ++depth)
    {
        if (const auto it = ctx.manager.rootScenes.find(current.value); it != ctx.manager.rootScenes.end())
        {
            const uint32_t sceneId = it->second;
            if (const auto sceneIt = ctx.manager.scenes.find(sceneId); sceneIt != ctx.manager.scenes.end())
            {
                const bool persistent = sceneIt->second.persistent;
                AddSceneRoot(ctx.manager, sceneId, root);
                if (persistent)
                    MarkPersistentRecursive(ctx, root);
                return;
            }
        }
        const auto* parent = ctx.world->Get<engine::ParentComponent>(current);
        current = parent ? parent->parent : NULL_LPENTITY;
    }
    // Ein explizit persistentes Objekt kann seine Ursprungsszene bereits
    // ueberlebt haben. Seine Spawns erhalten dann denselben Lebenszyklus.
    if (ctx.manager.persistentEntityIds.count(owner.value))
        MarkPersistentRecursive(ctx, root);
    else
        engine::Debug::LogWarning("krom.h: Script-Spawn ohne besitzende Szene (owner=%u root=%u)",
            owner.value, root.value);
}

static void UnloadSceneById(const SceneContext& ctx, uint32_t sceneId)
{
    auto it = ctx.manager.scenes.find(sceneId);
    if (it == ctx.manager.scenes.end())
        return;
    SceneRecord& record = it->second;
    if (ctx.world)
    {
        // A copy: ForgetDestroyedEntities removes the destroyed roots from the record.
        const std::vector<LPENTITY> roots = record.rootEntities;
        for (const LPENTITY entity : roots)
        {
            if (!ctx.world->IsAlive(entity))
                continue;
            if (ctx.manager.persistentEntityIds.count(entity.value))
                continue;
            PreservePersistentSubtrees(*ctx.world, entity, ctx.manager.persistentEntityIds);
            // Also covers roots of other scenes reparented below this one.
            ForgetDestroyedEntities(ctx.manager, *ctx.world, entity);
            DestroyEntityRecursive(*ctx.world, entity);
        }
    }
    if (record.active && !record.name.empty())
        ctx.eventBus.Emit<engine::events::SceneUnloadedEvent>(record.name);
    ForgetScene(ctx.manager, sceneId);
}

static void RebuildChildrenComponentsForLoadedEntities(const SceneContext& ctx,
                                                       const std::vector<LPENTITY>& loadedEntities)
{
    if (!ctx.world)
        return;

    for (const LPENTITY entity : loadedEntities)
    {
        if (!ctx.world->IsAlive(entity))
            continue;

        const auto* parent = ctx.world->Get<engine::ParentComponent>(entity);
        if (!parent || !parent->parent.IsValid() || !ctx.world->IsAlive(parent->parent))
            continue;

        if (!ctx.world->Has<engine::ChildrenComponent>(parent->parent))
            ctx.world->Add<engine::ChildrenComponent>(parent->parent);
        ctx.world->Get<engine::ChildrenComponent>(parent->parent)->Add(entity);
    }
}

static void EnsureRuntimeTransformComponents(const SceneContext& ctx,
                                             const std::vector<LPENTITY>& loadedEntities)
{
    if (!ctx.world)
        return;

    for (const LPENTITY entity : loadedEntities)
    {
        if (!ctx.world->IsAlive(entity))
            continue;

        auto* transform = ctx.world->Get<engine::TransformComponent>(entity);
        if (!transform)
            continue;

        if (!ctx.world->Has<engine::WorldTransformComponent>(entity))
            ctx.world->Add<engine::WorldTransformComponent>(entity);
        transform->dirty = true;
    }
}

static void ResolveSerializedSceneAssetBindings(const SceneContext& ctx,
                                                const std::vector<LPENTITY>& loadedEntities)
{
    if (!ctx.world || !ctx.assetPipeline)
        return;

    engine::addons::mesh_renderer::ResolveMeshAssetBindings(
        *ctx.assetPipeline,
        ctx.assetRegistry,
        *ctx.world);

    internal::ImportedMaterialCache importedMaterialCache;

    for (const LPENTITY entity : loadedEntities)
    {
        if (!ctx.world->IsAlive(entity))
            continue;

        if (const auto* mesh = ctx.world->Get<engine::MeshComponent>(entity))
        {
            if (!mesh->mesh.IsValid() && mesh->meshAssetPath.empty())
            {
                const auto* name = ctx.world->Get<engine::NameComponent>(entity);
                engine::Debug::LogWarning(
                    "krom.h: Entity '%s' hat MeshComponent ohne meshAssetPath; "
                    "alte meshHandle-Werte koennen in der Runtime nicht neu geladen werden",
                    name ? name->name.c_str() : "<unnamed>");
            }
        }

        // Erst GLB-Import-Materialien in Runtime-Materialien uebersetzen.
        // DefaultLit greift nur, wenn danach immer noch kein Material existiert.
        internal::ApplyImportedMeshMaterialsForEntity(ctx.materials, entity, importedMaterialCache);
        internal::EnsureDefaultMaterialForEntity(ctx.materials, entity);
        // "<model>#material/<n>" keys are no files: built from the model import.
        internal::ApplyImportedMaterialKeysForEntity(ctx.materials, entity, importedMaterialCache);

        auto* material = ctx.world->Get<engine::MaterialComponent>(entity);
        if (!material)
            continue;

        if (!material->materialAssetPath.empty() &&
            !internal::IsImportedMaterialKey(material->materialAssetPath, material->materialOverrideSource))
        {
            const LPMATERIAL assetMaterial = material->materialOverrideSource.empty()
                ? Krom::LoadMaterial(material->materialAssetPath.c_str())
                : ctx.assetPipeline->LoadMaterialSource(material->materialOverrideSource);
            if (assetMaterial.IsValid())
                material->material = internal::RealizeMaterialForEntity(ctx.materials, entity, assetMaterial);
        }

        for (engine::MaterialComponent::SlotOverride& slot : material->slotOverrides)
        {
            if (slot.materialAssetPath.empty() ||
                internal::IsImportedMaterialKey(slot.materialAssetPath, slot.materialOverrideSource))
                continue;

            const LPMATERIAL assetMaterial = slot.materialOverrideSource.empty()
                ? Krom::LoadMaterial(slot.materialAssetPath.c_str())
                : ctx.assetPipeline->LoadMaterialSource(slot.materialOverrideSource);
            if (assetMaterial.IsValid())
                slot.material = internal::RealizeMaterialForEntity(ctx.materials, entity, assetMaterial);
        }
    }
}

static bool LoadComponentSerializedScene(const SceneContext& ctx,
                                         const KromSceneData& data,
                                         uint32_t sceneId)
{
    engine::serialization::SceneDeserializer deserializer(*ctx.world);
    deserializer.RegisterDefaultHandlers();
    engine::addons::camera::RegisterCameraDeserializationHandlers(deserializer);
    engine::addons::lighting::RegisterLightingDeserializationHandlers(deserializer);
    engine::addons::mesh_renderer::RegisterMeshRendererDeserializationHandlers(deserializer);
    engine::addons::prefab::RegisterPrefabDeserializationHandlers(deserializer);
    engine::script::RegisterScriptDeserializationHandlers(deserializer, &ctx.scriptAddonRegistry);
    engine::physics::RegisterPhysicsDeserializationHandlers(deserializer);
    engine::audio::RegisterAudioDeserializationHandlers(deserializer);
    engine::particles::RegisterParticleDeserializationHandlers(deserializer);
    engine::addons::probes::RegisterProbeDeserializationHandlers(deserializer);
    engine::addons::postprocess::RegisterPostProcessDeserializationHandlers(deserializer);

    engine::serialization::DeserializeResult result =
        deserializer.DeserializeFromJson(data.sourceJson);
    if (!result.success)
    {
        engine::Debug::LogError("krom.h: serialized scene '%s' konnte nicht geladen werden: %s",
            data.name.c_str(), result.error.c_str());
        return false;
    }

    std::vector<LPENTITY> loadedEntities;
    loadedEntities.reserve(result.entityRemap.size());
    for (const auto& [oldId, newId] : result.entityRemap)
    {
        (void)oldId;
        if (newId.IsValid() && ctx.world->IsAlive(newId))
            loadedEntities.push_back(newId);
    }

    RebuildChildrenComponentsForLoadedEntities(ctx, loadedEntities);
    EnsureRuntimeTransformComponents(ctx, loadedEntities);
    ResolveSerializedSceneAssetBindings(ctx, loadedEntities);
    engine::script::ResolveScriptEntityReferences(*ctx.world, ctx.scriptAddonRegistry);

    for (const LPENTITY entity : loadedEntities)
    {
        if (!ctx.world->IsAlive(entity))
            continue;
        if (!ctx.world->Has<engine::ParentComponent>(entity))
            AddSceneRoot(ctx.manager, sceneId, entity);
        if (data.persistent)
            MarkPersistentRecursive(ctx, entity);
    }

    engine::Debug::Log("krom.h: serialized scene '%s' geladen (%u entities, %u components)",
        data.name.c_str(), result.entitiesRead, result.componentsRead);
    return true;
}

static void ApplySceneEnvironment(const SceneContext& ctx, const KromSceneData& data)
{
    if (!ctx.projectSettings.renderingStored)
        internal::ApplyProjectRenderSettings(ctx.render, data.legacyRendering, ctx.editorLiveStateEnabled);
    internal::ApplySceneRenderSettings(ctx.render, data.exposureEv, data.exposureMode, data.autoExposure,
                                       data.grading, data.probeBakePasses);

    // ── Ambient ───────────────────────────────────────────────────────────────
    if (data.hasAmbientColor)    ctx.cameraOptions.ambientColor    = data.ambientColor;
    if (data.hasAmbientIntensity) ctx.cameraOptions.ambientIntensity = data.ambientIntensity;

    // ── IBL-Modus von der Game-Main-Kamera ableiten ───────────────────────────
    // Der Editor-IBL-Toggle ist unabhängig von der Game-Kamera.
    // Im Wrapper bestimmt ausschließlich backgroundMode der Main-Kamera ob IBL aktiv ist.
    bool gameCameraWantsSkybox = false;
    if (ctx.world)
    {
        ctx.world->ForEachAlive([&](engine::EntityID id)
        {
            const auto* cam = ctx.world->Get<engine::CameraComponent>(id);
            if (cam && cam->isMainCamera)
                gameCameraWantsSkybox = (cam->backgroundMode == engine::BackgroundMode::Skybox);
        });
    }

    // ── Environment-Textur laden ──────────────────────────────────────────────
    if (data.hasEnvironment && !data.environmentTexturePath.empty() && ctx.assetPipeline)
    {
        const LPTEXTURE sourceTexture = Krom::LoadTexture(data.environmentTexturePath.c_str());
        if (sourceTexture.IsValid())
        {
            ctx.assetPipeline->UploadPendingGpuAssets();

            engine::renderer::EnvironmentDesc envDesc{};
            envDesc.mode          = engine::renderer::EnvironmentMode::Texture;
            envDesc.sourceTexture = sourceTexture;
            envDesc.intensity     = data.environmentIntensity;
            envDesc.enableIBL     = data.environmentEnableIBL;
            envDesc.removeSunFromLighting = data.environmentRemoveSunFromLighting;

            engine::renderer::RenderSystem& rs = ctx.renderSystem;
            const engine::renderer::EnvironmentHandle next = rs.CreateEnvironment(envDesc);
            if (next.IsValid())
            {
                const engine::renderer::EnvironmentHandle prev = ctx.activeEnvironment;
                ctx.activeEnvironment = next;
                rs.SetActiveEnvironment(next);
                if (prev.IsValid()) rs.DestroyEnvironment(prev);

                engine::Debug::Log("krom.h: environment '%s' intensity=%.2f ibl=%s (von Game-Kamera)",
                    data.environmentTexturePath.c_str(), data.environmentIntensity,
                    gameCameraWantsSkybox ? "true" : "false");
            }
            else
            {
                engine::Debug::LogWarning("krom.h: environment konnte nicht erstellt werden: %s",
                    data.environmentTexturePath.c_str());
            }
        }
        else
        {
            engine::Debug::LogWarning("krom.h: environment-Textur nicht gefunden: %s",
                data.environmentTexturePath.c_str());
        }
    }

    // ── Kamera-Hintergrund ────────────────────────────────────────────────────
    // backgroundMode und clearColor kommen direkt aus der Entity-Deserialisierung —
    // kein Override nötig.
}

// Gemeinsamer Instantiierungspfad fuer LoadScene/LoadSceneAdditive und ActivateScene.
static bool InstantiateSceneData(const SceneContext& ctx, const KromSceneData& data,
                                 uint32_t sceneId, const SceneRecord& record)
{
    if (!LoadComponentSerializedScene(ctx, data, sceneId))
        return false;

    ctx.transformSystem.Update(*ctx.world);
    ctx.boundsSystem.Update(*ctx.world);
    ApplySceneEnvironment(ctx, data);
    if (!record.name.empty())
        ctx.eventBus.Emit<engine::events::SceneLoadedEvent>(record.name);
    return true;
}

static LPSCENE LoadSceneFromData(const SceneContext& ctx, KromSceneData data, bool additive,
                                 const char* filePath = nullptr)
{
    if (!data.valid || !ctx.world)
        return NULL_SCENE;

    if (!additive)
    {
        // Trigger, Events, Physikkoerper, Klaenge und Partikel der alten Szene verwerfen.
        ctx.game.ResetForSceneChange();
        // Alle nicht-persistenten Scenes entladen
        std::vector<uint32_t> toUnload;
        for (const auto& [id, scene] : ctx.manager.scenes)
            if (!scene.persistent)
                toUnload.push_back(id);
        for (const uint32_t id : toUnload)
            UnloadSceneById(ctx, id);
    }

    const uint32_t sceneId = ctx.manager.nextSceneId++;
    SceneRecord& record = ctx.manager.scenes[sceneId];
    record.name       = data.name;
    record.path       = (filePath && filePath[0] != '\0') ? filePath : data.name;
    record.persistent = data.persistent;
    record.active     = true;

    if (!InstantiateSceneData(ctx, data, sceneId, record))
    {
        ForgetScene(ctx.manager, sceneId);
        return NULL_SCENE;
    }

    engine::Debug::Log("krom.h: scene '%s' geladen (%zu entities, persistent=%s)",
        data.name.c_str(), record.rootEntities.size(),
        record.persistent ? "true" : "false");

    return LPSCENE{sceneId};
}

static KromSceneData LoadKsceneFile(const std::filesystem::path& assetRoot,
                                     const char* path)
{
    const std::filesystem::path resolved =
        std::filesystem::weakly_canonical(assetRoot / PathFromUtf8(path));
    std::ifstream file(resolved);
    if (!file)
    {
        engine::Debug::LogError("krom.h: kscene file not found: %s",
            PathToUtf8(resolved).c_str());
        return KromSceneData{};
    }
    const std::string json(std::istreambuf_iterator<char>(file),
                           std::istreambuf_iterator<char>{});
    return ParseKscene(json);
}

// =============================================================================
// Scene-Management (krom.h functions)
// =============================================================================

LPSCENE LoadScene(const SceneContext& ctx, const char* path)
{
    return LoadSceneFromData(ctx, LoadKsceneFile(ctx.assetRoot, path), false, path);
}

LPSCENE LoadScene(const SceneManagerState& manager, int sceneIndex)
{
    if (sceneIndex <= 0)
        return NULL_SCENE;

    const size_t index = static_cast<size_t>(sceneIndex - 1);
    if (index >= manager.projectScenePaths.size())
    {
        engine::Debug::LogError("krom.h: LoadScene(%d) out of range; project has %zu scenes",
            sceneIndex, manager.projectScenePaths.size());
        return NULL_SCENE;
    }
    return Krom::LoadScene(manager.projectScenePaths[index].c_str());
}

int GetSceneCount(const SceneManagerState& manager)
{
    return static_cast<int>(manager.projectScenePaths.size());
}

LPSCENE LoadSceneAdditive(const SceneContext& ctx, const char* path)
{
    return LoadSceneFromData(ctx, LoadKsceneFile(ctx.assetRoot, path), true, path);
}

LPSCENE LoadSceneAdditive(const SceneManagerState& manager, int sceneIndex)
{
    if (sceneIndex <= 0)
        return NULL_SCENE;

    const size_t index = static_cast<size_t>(sceneIndex - 1);
    if (index >= manager.projectScenePaths.size())
    {
        engine::Debug::LogError("krom.h: LoadSceneAdditive(%d) out of range; project has %zu scenes",
            sceneIndex, manager.projectScenePaths.size());
        return NULL_SCENE;
    }
    return Krom::LoadSceneAdditive(manager.projectScenePaths[index].c_str());
}

LPSCENE LoadSceneAsync(const SceneContext& ctx, const char* path)
{
    const uint32_t sceneId = ctx.manager.nextSceneId++;
    SceneRecord& record = ctx.manager.scenes[sceneId];
    record.name    = "";
    record.path    = path;
    record.staging = true;
    record.staged  = false;
    record.active  = false;

    // Absoluten Pfad vor dem Thread-Start berechnen (thread-safe)
    const std::filesystem::path resolved =
        std::filesystem::weakly_canonical(ctx.assetRoot / PathFromUtf8(path));

    // Hintergrund-Thread: nur Datei-I/O + JSON-Parsing — kein ECS-Zugriff
    const std::string resolvedStr = PathToUtf8(resolved);
    record.asyncFuture = std::async(std::launch::async, [resolved, resolvedStr]() -> KromSceneData
    {
        std::ifstream file(resolved);
        if (!file)
        {
            engine::Debug::LogError("krom.h: LoadSceneAsync: Datei nicht gefunden: %s",
                resolvedStr.c_str());
            return KromSceneData{};
        }
        const std::string json(std::istreambuf_iterator<char>(file),
                               std::istreambuf_iterator<char>{});
        return ParseKscene(json);
    }).share();

    engine::Debug::Log("krom.h: LoadSceneAsync gestartet: %s", path);
    return LPSCENE{sceneId};
}

bool IsSceneReady(SceneManagerState& manager, LPSCENE scene)
{
    auto it = manager.scenes.find(scene.id);
    if (it == manager.scenes.end())
        return false;
    SceneRecord& record = it->second;
    if (record.staged || record.active)
        return true;
    if (!record.staging)
        return false;

    if (record.asyncFuture.valid() &&
        record.asyncFuture.wait_for(std::chrono::seconds(0)) == std::future_status::ready)
    {
        record.stagedData = record.asyncFuture.get();
        record.staging    = false;
        record.staged     = true;
        return true;
    }
    return false;
}

bool ActivateScene(const SceneContext& ctx, LPSCENE scene)
{
    auto it = ctx.manager.scenes.find(scene.id);
    if (it == ctx.manager.scenes.end())
        return false;
    SceneRecord& record = it->second;

    if (record.active)
        return true;  // bereits aktiv

    // Falls async noch läuft: blockierend warten
    if (record.staging && record.asyncFuture.valid())
    {
        record.stagedData = record.asyncFuture.get();
        record.staging    = false;
        record.staged     = true;
    }

    if (!record.staged)
        return false;

    // Entities jetzt auf dem Main-Thread erstellen
    const KromSceneData& data = record.stagedData;
    if (!data.valid)
    {
        ForgetScene(ctx.manager, scene.id);
        return false;
    }

    record.name       = data.name;
    record.persistent = data.persistent;
    record.active     = true;
    record.staged     = false;

    if (!InstantiateSceneData(ctx, data, scene.id, record))
    {
        ForgetScene(ctx.manager, scene.id);
        return false;
    }

    engine::Debug::Log("krom.h: scene '%s' aktiviert (%zu entities)",
        data.name.c_str(), record.rootEntities.size());
    return true;
}

void UnloadScene(const SceneContext& ctx, LPSCENE scene)
{
    UnloadSceneById(ctx, scene.id);
}

void UnloadAll(const SceneContext& ctx)
{
    std::vector<uint32_t> toUnload;
    for (const auto& [id, scene] : ctx.manager.scenes)
    {
        if (!scene.persistent)
            toUnload.push_back(id);
    }
    for (const uint32_t id : toUnload)
        UnloadSceneById(ctx, id);
}

void SetPersistent(SceneManagerState& manager, LPENTITY entity)
{
    if (entity.IsValid())
        manager.persistentEntityIds.insert(entity.value);
}

void SetScenePersistent(const SceneContext& ctx, LPSCENE scene)
{
    auto it = ctx.manager.scenes.find(scene.id);
    if (it == ctx.manager.scenes.end())
        return;
    SceneRecord& record = it->second;
    record.persistent = true;
    for (const LPENTITY entity : record.rootEntities)
        MarkPersistentRecursive(ctx, entity);
}

LPENTITY FindEntityInScene(const SceneContext& ctx, LPSCENE scene, const char* name)
{
    if (!ctx.world || !name)
        return NULL_LPENTITY;

    auto it = ctx.manager.scenes.find(scene.id);
    if (it == ctx.manager.scenes.end())
        return NULL_LPENTITY;

    LPENTITY found = NULL_LPENTITY;
    std::function<void(LPENTITY)> search = [&](LPENTITY e)
    {
        if (found.IsValid() || !ctx.world->IsAlive(e)) return;
        if (const auto* nc = ctx.world->Get<engine::NameComponent>(e))
            if (nc->name == name) { found = e; return; }
        if (const auto* ch = ctx.world->Get<engine::ChildrenComponent>(e))
            for (const LPENTITY child : ch->children)
                search(child);
    };
    for (const LPENTITY root : it->second.rootEntities)
        search(root);
    return found;
}

} // namespace Krom::internal
