#include "krom.h"
#include "KromAssetApi.hpp"
#include "KromEntityApi.hpp"
#include "KromRenderSettings.hpp"
#include "KromRuntimeMaterials.hpp"
#include "KromSceneManager.hpp"
#include "KromSurfaces.hpp"
#include "core/AtomicTextFile.hpp"
#include "core/PathUtf8.hpp"

#include "addons/script/ScriptList.hpp"
#include "addons/script/ScriptSerialization.hpp"
#include "addons/script/ScriptTriggers.hpp"
#include "collision/SceneQueries.hpp"
#include "addons/animation/AnimationComponents.hpp"
#include "addons/animation/AnimationSystem.hpp"
#include "addons/animation/SkinningSystem.hpp"
#include "addons/camera/CameraComponents.hpp"
#include "addons/camera/CameraSerialization.hpp"
#include "addons/camera/CameraViewBuilder.hpp"
#include "addons/forward/ForwardFeature.hpp"
#include "addons/forward/ForwardTonemapJson.hpp"
#include "addons/script/ScriptColorGrading.hpp"
#include "addons/gtao/GtaoFeature.hpp"
#include "addons/contact_shadow/ContactShadowFeature.hpp"
#include "addons/ssr/SsrFeature.hpp"
#include "addons/probes/ProbeBake.hpp"
#include "addons/probes/ProbeComponents.hpp"
#include "addons/probes/ProbeFeature.hpp"
#include "addons/probes/ProbeSerialization.hpp"
#include "addons/postprocess/PostProcessBlend.hpp"
#include "addons/postprocess/PostProcessSerialization.hpp"
#include "addons/lighting/LightingComponents.hpp"
#include "addons/lighting/LightingFeature.hpp"
#include "addons/lighting/LightingSerialization.hpp"
#include "addons/mesh_renderer/MeshAssetSceneBindings.hpp"
#include "addons/mesh_renderer/MeshBounds.hpp"
#include "addons/mesh_renderer/MeshRendererComponents.hpp"
#include "addons/mesh_renderer/MeshRendererFeature.hpp"
#include "addons/mesh_renderer/MeshRendererSerialization.hpp"
#include "addons/lit/LitMaterial.hpp"
#include "renderer/MaterialGraphBinding.hpp"
#include "addons/pbr/PbrInstanceBuilder.hpp"
#include "addons/pbr/PbrMasterMaterial.hpp"
#include "addons/prefab/Prefab.hpp"
#include "addons/prefab/PrefabSerialization.hpp"
#include "addons/shadow/ShadowFeature.hpp"
#include "addons/unlit/UnlitMaterial.hpp"
#include "assets/AssetPipeline.hpp"
#include "assets/AssetRegistry.hpp"
#include "assets/MeshTangents.hpp"
#include "assets/VertexLayoutBridge.hpp"
#include "core/Debug.hpp"
#include "core/FixedTimestep.hpp"
#include "ecs/Components.hpp"
#include "ecs/World.hpp"
#include "events/EventBus.hpp"
#include "platform/StdTiming.hpp"
#include "renderer/MaterialDomain.hpp"
#include "renderer/MaterialSystem.hpp"
#include "renderer/ShaderCompiler.hpp"
#include "renderer/ShaderBindingModel.hpp"
#include "renderer/ShaderConstantBufferReflection.hpp"
#include "renderer/PlatformRenderLoop.hpp"
#include "jobs/JobSystem.hpp"
#include "renderer/runtime/MaterialRuntimeBridge.hpp"
#include "scene/BoundsSystem.hpp"
#include "scene/TransformSystem.hpp"

#ifdef KROM_HAS_GAME_SCRIPTS
extern "C" void KromRegisterGameScripts(engine::script::ScriptRegistry& registry);
#endif

#if defined(KROM_APP_USE_WIN32_PLATFORM)
#include "platform/Win32Platform.hpp"
#elif defined(KROM_APP_USE_GLFW_PLATFORM)
#include "platform/GLFWPlatform.hpp"
#else
#error Krom wrapper requires KROM_APP_USE_WIN32_PLATFORM or KROM_APP_USE_GLFW_PLATFORM.
#endif


#include "serialization/SceneSerializer.hpp"

// KROM_WRAPPER_HAS_GLTF steuert nur noch die Importer-Registrierung,
// nicht mehr die Lade-Logik. CreateEntityFromAsset ist format-agnostisch.
#ifndef KROM_WRAPPER_HAS_GLTF
#define KROM_WRAPPER_HAS_GLTF 0
#endif

#if KROM_WRAPPER_HAS_GLTF
#include "addons/gltf/GltfImporter.hpp"
#endif

// Physik-Komponenten/-Serialisierung (engine_physics_data) sind immer da —
// Szenen und Prefabs koennen sie enthalten. Nur die Simulation (Jolt) haengt
// an KROM_WITH_PHYSICS.
#include "addons/physics/PhysicsComponents.hpp"
#include "addons/physics/PhysicsSerialization.hpp"
#include "addons/audio/AudioComponents.hpp"
#include "addons/audio/AudioSerialization.hpp"

#ifndef KROM_WRAPPER_HAS_PHYSICS
#define KROM_WRAPPER_HAS_PHYSICS 0
#endif

// Bildschirmtext (engine_text) ist immer dabei; ohne Schrift bleibt er stumm.
#include "addons/text/TextRenderer.hpp"
#include "addons/script/ITextService.hpp"

// Particles: components, serialization and CPU simulation are always present
// (engine_particles_data). Drawing them needs engine_particles (KROM_WITH_PARTICLES).
#include "addons/particles/ParticleComponents.hpp"
#include "addons/particles/ParticleScriptService.hpp"
#include "addons/particles/ParticleSerialization.hpp"
#include "addons/particles/ParticleSystem.hpp"
#ifdef KROM_WRAPPER_HAS_PARTICLES
#include "addons/particles/ParticleView.hpp"
#endif

// Audio-Wiedergabe (miniaudio) — nur wenn mit KROM_WITH_AUDIO gebaut.
#ifndef KROM_WRAPPER_HAS_AUDIO
#define KROM_WRAPPER_HAS_AUDIO 0
#endif

#if KROM_WRAPPER_HAS_AUDIO
#include "addons/audio/AudioSystem.hpp"
#endif
#if KROM_WRAPPER_HAS_PHYSICS
#include "addons/physics/PhysicsWorld.hpp"
#endif
#include "addons/gameplay/GameSimulation.hpp"

#include <cstdlib>
#include <cfloat>
#include <algorithm>
#include <cmath>
#include <chrono>
#include <fstream>
#include <future>
#include <string_view>
#include <thread>
#include <memory>
#include <optional>
#include <span>
#include <unordered_map>
#include <unordered_set>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

using engine::PathFromUtf8;
using engine::PathToUtf8;
using engine::PathToGenericUtf8;

namespace Krom {

namespace {

using internal::SurfaceRecord;
using internal::GenerateMissingNormals;

// =============================================================================

using internal::kRuntimeBloomIntensity;

// Asset counts of the registry stores a parsed prefab resolves its handles from.
struct PrefabRegistryCounts
{
    size_t meshes = 0u, materials = 0u, skeletons = 0u, animations = 0u;
    bool operator==(const PrefabRegistryCounts&) const = default;
};

// Parsed prefab file of a script spawn (InstantiatePrefabForScript). Reused while
// the file's write time and size stay the same.
struct PrefabCacheEntry
{
    // Shared: an instantiation in progress keeps its prefab when the entry is replaced.
    std::shared_ptr<const engine::addons::prefab::PrefabAsset> prefab;
    std::filesystem::file_time_type     writeTime{};
    std::uintmax_t                      fileSize = 0u;
    // Parsing looks the handles up in the registry by path. A path without a
    // registered asset (model not imported yet) is resolved again by parsing
    // anew once the registry has gained or lost assets since.
    PrefabRegistryCounts                registryCounts{};
};

struct State
{
    GraphicsConfig config{};
    engine::ProjectSettings projectSettings;
    std::filesystem::path assetRoot;
    std::filesystem::path engineAssetRoot; // builtin Engine-Shader/Assets neben der EXE

    engine::renderer::DeviceFactory::Registry deviceFactoryRegistry;
    engine::events::EventBus eventBus;
    std::unique_ptr<engine::platform::IPlatform> platform;
    // Worker pool of the engine. Declared before everything that uses it (renderer,
    // asset pipeline, game simulation) so that it is destroyed after them.
    engine::jobs::JobSystem jobSystem;
    engine::renderer::PlatformRenderLoop renderLoop;
    // Render options and the add-on settings they drive (KromRenderSettings.hpp).
    internal::RenderSettingsState render;
    engine::assets::AssetRegistry assetRegistry;
    std::unique_ptr<engine::assets::AssetPipeline> assetPipeline;
    engine::renderer::MaterialSystem materialSystem;
    engine::ecs::ComponentMetaRegistry componentRegistry;
    std::unique_ptr<engine::ecs::World> world;
    engine::TransformSystem transformSystem;
    engine::BoundsSystem boundsSystem;
    engine::platform::StdTiming timing;
    engine::addons::camera::CameraBuildOptions cameraOptions{};
    engine::renderer::EnvironmentHandle activeEnvironment = engine::renderer::EnvironmentHandle::Invalid();
    std::unique_ptr<engine::addons::animation::AnimationSystem> animationSystem;
    std::unique_ptr<engine::addons::animation::SkinningSystem> skinningSystem;

    InitFn initCallback;
    TickFn tickCallback;

    bool componentsRegistered = false;
    bool featuresRegistered = false;
    bool initialized = false;
    bool running = false;
    bool forwardPlusActive = false;

    // Scenes, persistent entities and the project scene order (KromSceneManager.hpp).
    internal::SceneManagerState               sceneManager;
    std::unordered_map<uint32_t, std::string> texturePaths;
    std::unordered_map<uint64_t, LPMATERIAL>  runtimeMaterialCache;
    std::unordered_map<uint32_t, SurfaceRecord> surfaces;
    uint32_t                                  nextSurfaceId = 1u;
    // Script prefab spawns, keyed by the resolved file path (UTF-8). Holds registry
    // handles: cleared with the registry contents (Shutdown) and on SetAssetRoot.
    std::unordered_map<std::string, PrefabCacheEntry> prefabCache;
    std::filesystem::path                     projectRoot;
    std::filesystem::path                     runtimeStatePath;
    float                                     runtimeStateWriteTimer = 0.0f;
    bool                                      editorLiveStateEnabled = false;

    // Ebene 3 — Systeme und Scripts
    std::vector<std::function<void(engine::ecs::World&, float)>> userSystems;
    engine::script::ScriptRegistry                               scriptAddonRegistry;
    // Spielschleife ohne Rendering (Scripts, Events, Trigger, Physik, Audio,
    // Partikel, Spielzeit, Bildschirmtext) — dieselbe wie bei Play im Editor.
    engine::gameplay::GameSimulation                             game;
    bool                                                         textConfigured = false;
#ifdef KROM_WRAPPER_HAS_PARTICLES
    engine::particles::ParticleView                              particleView;
    bool                                                         particleLoaderConfigured = false;
#endif
};

State& Get()
{
    static State state;
    return state;
}

// Engine shader root: "shaders" next to the running EXE, otherwise "shaders"
// relative to the current working directory.
static std::filesystem::path ResolveEngineAssetRootOnDisk()
{
#if defined(_WIN32)
    wchar_t exeBuf[260]{};
    if (::GetModuleFileNameW(nullptr, exeBuf, 260) > 0)
    {
        const auto exeDir = std::filesystem::path(exeBuf).parent_path();
        if (auto p = exeDir / "shaders"; std::filesystem::is_directory(p))
            return p;
    }
#endif
    if (auto p = std::filesystem::current_path() / "shaders"; std::filesystem::is_directory(p))
        return p;
    return {};
}

engine::renderer::DeviceFactory::BackendType ToBackend(int backend) noexcept
{
    using BackendType = engine::renderer::DeviceFactory::BackendType;
    switch (backend)
    {
    case Renderer::DX11: return BackendType::DirectX11;
    case Renderer::DX12: return BackendType::DirectX12;
    case Renderer::Vulkan: return BackendType::Vulkan;
    default: return BackendType::Vulkan;
    }
}

const char* BackendName(engine::renderer::DeviceFactory::BackendType backend) noexcept
{
    using BackendType = engine::renderer::DeviceFactory::BackendType;
    switch (backend)
    {
    case BackendType::DirectX11: return "DirectX11";
    case BackendType::DirectX12: return "DirectX12";
    case BackendType::Vulkan: return "Vulkan";
    default: return "Unknown";
    }
}

float ComputeImportedModelRadius(const engine::assets::ImportedAssetBundle& bundle,
                                 const engine::math::Vec3& center) noexcept
{
    using namespace engine;
    float radiusSq = 0.0f;
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

    const auto include = [&](const math::Vec3& p) {
        hasPosition = true;
        radiusSq = std::max(radiusSq, (p - center).LengthSq());
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

    return hasPosition ? std::max(std::sqrt(radiusSq), 0.1f) : 1.0f;
}

internal::RuntimeMaterialContext MakeMaterialContext(State& state)
{
    return internal::RuntimeMaterialContext{
        state.assetRegistry, state.assetPipeline.get(), state.materialSystem,
        state.renderLoop.GetRenderSystem(), state.world.get(), state.assetRoot, state.engineAssetRoot,
        state.runtimeMaterialCache, state.texturePaths,
        state.config.enableForwardPlus};
}

internal::SurfaceContext MakeSurfaceContext(State& state)
{
    return internal::SurfaceContext{state.surfaces, state.nextSurfaceId, state.assetRegistry, state.world.get()};
}

internal::SceneContext MakeSceneContext(State& state)
{
    return internal::SceneContext{
        state.sceneManager, state.world.get(), state.assetPipeline.get(), state.assetRegistry,
        state.transformSystem, state.boundsSystem, state.eventBus, state.scriptAddonRegistry, state.game,
        state.assetRoot, state.projectSettings, state.editorLiveStateEnabled, state.render,
        state.cameraOptions, state.activeEnvironment, state.renderLoop.GetRenderSystem(),
        MakeMaterialContext(state)};
}

internal::EntityContext MakeEntityContext(State& state)
{
    return internal::EntityContext{state.world.get(), state.transformSystem, state.boundsSystem};
}

static PrefabRegistryCounts CountPrefabRegistryAssets(const engine::assets::AssetRegistry& registry) noexcept
{
    return {registry.meshes.Count(), registry.materials.Count(),
            registry.skeletons.Count(), registry.animations.Count()};
}

// A handle that parsing found must still name the asset registered under the
// record's path; a path without handle counts as unresolved.
template<typename Store, typename HandleT>
static bool PrefabReferenceStillValid(const Store& store, HandleT handle, const std::string& path,
                                      bool& unresolved) noexcept
{
    if (!handle.IsValid())
    {
        unresolved = unresolved || !path.empty();
        return true;
    }
    const auto* asset = store.Get(handle);
    return asset && asset->path == path;
}

// False when a registry handle of the parsed prefab no longer matches the registry.
static bool PrefabReferencesStillValid(const engine::addons::prefab::PrefabAsset& prefab,
                                       const engine::assets::AssetRegistry& registry,
                                       bool& outUnresolved) noexcept
{
    outUnresolved = false;
    for (const engine::addons::prefab::PrefabEntityRecord& record : prefab.records)
    {
        if (!PrefabReferenceStillValid(registry.meshes, record.mesh, record.meshAssetPath, outUnresolved) ||
            !PrefabReferenceStillValid(registry.materials, record.material, record.materialAssetPath, outUnresolved) ||
            !PrefabReferenceStillValid(registry.skeletons, record.skeleton, record.skeletonAssetPath, outUnresolved) ||
            !PrefabReferenceStillValid(registry.animations, record.animation, record.animationAssetPath, outUnresolved))
            return false;
        for (const auto& slot : record.slotOverrides)
            if (!PrefabReferenceStillValid(registry.materials, slot.material, slot.materialAssetPath, outUnresolved))
                return false;
    }
    return true;
}

// The parsed prefab file, from the cache while the file (write time, size) and the
// registry handles it holds are unchanged, otherwise parsed anew. Null on failure
// (not cached). A prefab file reads no other files: nested prefabs are embedded
// in its records.
static std::shared_ptr<const engine::addons::prefab::PrefabAsset> AcquireScriptPrefab(
    State& state, const std::filesystem::path& path)
{
    const std::string key = PathToUtf8(path);
    std::error_code ec;
    const std::filesystem::file_time_type writeTime = std::filesystem::last_write_time(path, ec);
    const std::uintmax_t fileSize = ec ? 0u : std::filesystem::file_size(path, ec);
    if (ec)
    {
        state.prefabCache.erase(key);
        engine::Debug::LogError("krom.h: Prefab konnte nicht geladen werden '%s': %s",
            key.c_str(), ec.message().c_str());
        return nullptr;
    }

    if (const auto it = state.prefabCache.find(key);
        it != state.prefabCache.end() && it->second.writeTime == writeTime && it->second.fileSize == fileSize)
    {
        PrefabCacheEntry& entry = it->second;
        bool unresolved = false;
        if (PrefabReferencesStillValid(*entry.prefab, state.assetRegistry, unresolved) &&
            (!unresolved || entry.registryCounts == CountPrefabRegistryAssets(state.assetRegistry)))
            return entry.prefab;
    }

    auto prefab = std::make_shared<engine::addons::prefab::PrefabAsset>();
    std::string error;
    if (!engine::addons::prefab::LoadPrefabFromFile(path, state.assetRegistry, *prefab, &error))
    {
        state.prefabCache.erase(key);
        engine::Debug::LogError("krom.h: Prefab konnte nicht geladen werden '%s': %s",
            key.c_str(), error.c_str());
        return nullptr;
    }
    PrefabCacheEntry entry;
    entry.prefab = prefab;
    entry.writeTime = writeTime;
    entry.fileSize = fileSize;
    // Counted after parsing: parsing itself may import skeletons and animations.
    entry.registryCounts = CountPrefabRegistryAssets(state.assetRegistry);
    state.prefabCache[key] = std::move(entry);
    return prefab;
}

// World transform and bounds of freshly instantiated entities, computed per
// entity instead of a whole-world update. The transforms stay dirty, so the
// frame's TransformSystem::Update still processes them and bumps their versions
// (which makes BoundsSystem::Update recompute the bounds afterwards).
static void UpdateSpawnedTransforms(engine::ecs::World& world, std::span<const LPENTITY> entities)
{
    for (const LPENTITY entity : entities)
    {
        if (!world.IsAlive(entity) || !world.Has<engine::TransformComponent>(entity))
            continue;
        if (auto* worldTransform = world.Get<engine::WorldTransformComponent>(entity))
            *worldTransform = engine::TransformSystem::ComputeCurrentWorldTransform(world, entity);
        if (world.Has<engine::BoundsComponent>(entity))
            engine::BoundsSystem::ComputeBoundsForEntity(world, entity);
    }
}

static LPENTITY InstantiatePrefabForScript(State& state,
                                           std::string_view prefabAssetPath,
                                           const engine::math::Vec3& position,
                                           const engine::math::Quat& rotation,
                                           const engine::math::Vec3& scale,
                                           bool attachScripts = true)
{
    if (!state.world || prefabAssetPath.empty())
        return NULL_LPENTITY;

    std::filesystem::path path = PathFromUtf8(prefabAssetPath);
    if (path.is_relative() && !state.assetRoot.empty())
        path = state.assetRoot / path;

    const std::shared_ptr<const engine::addons::prefab::PrefabAsset> prefab = AcquireScriptPrefab(state, path);
    if (!prefab)
        return NULL_LPENTITY;

    engine::addons::prefab::PrefabInstantiateOptions options{};
    options.position = position;
    options.rotation = rotation;
    options.scale = scale;
    options.prefabAssetPath = std::string(prefabAssetPath);
    options.scriptRegistry = attachScripts ? &state.scriptAddonRegistry : nullptr;

    const engine::addons::prefab::PrefabInstance instance =
        engine::addons::prefab::InstantiatePrefab(*state.world, *prefab, options);
    if (!instance.IsValid())
    {
        engine::Debug::LogError("krom.h: Prefab konnte nicht instanziiert werden '%s'",
            PathToUtf8(path).c_str());
        return NULL_LPENTITY;
    }

    // Only the new entities (a valid instance is never empty; an empty span would mean the whole world).
    if (state.assetPipeline)
    {
        engine::addons::mesh_renderer::ResolveMeshAssetBindings(
            *state.assetPipeline,
            state.assetRegistry,
            *state.world,
            std::span<const LPENTITY>(instance.entities));
    }

    for (const LPENTITY entity : instance.entities)
    {
        if (!entity.IsValid() || !state.world->IsAlive(entity))
            continue;
        if (!state.world->Has<engine::WorldTransformComponent>(entity))
            state.world->Add<engine::WorldTransformComponent>(entity);
        if (auto* transform = state.world->Get<engine::TransformComponent>(entity))
        {
            transform->dirty = true;
            ++transform->localVersion;
        }
        if (state.world->Has<engine::MeshComponent>(entity))
            engine::mesh_renderer::UpdateLocalBoundsForEntity(*state.world, entity, state.assetRegistry);
    }

    // Usable right away (stored world transform and bounds), without a whole-world update.
    UpdateSpawnedTransforms(*state.world, instance.entities);
    return instance.root;
}

static const char* ScriptFieldTypeName(engine::script::ScriptFieldType type) noexcept
{
    switch (type)
    {
    case engine::script::ScriptFieldType::Float:  return "Float";
    case engine::script::ScriptFieldType::Int:    return "Int";
    case engine::script::ScriptFieldType::Bool:   return "Bool";
    case engine::script::ScriptFieldType::Vec3:   return "Vec3";
    case engine::script::ScriptFieldType::String: return "String";
    case engine::script::ScriptFieldType::Prefab: return "Prefab";
    case engine::script::ScriptFieldType::Entity: return "Entity";
    }
    return "Unknown";
}

static void WriteRuntimeScriptFieldValue(engine::serialization::JsonWriter& w,
                                         const char* key,
                                         const engine::script::ScriptFieldValue& value)
{
    switch (value.type)
    {
    case engine::script::ScriptFieldType::Float:  w.WriteFloat(key, value.floatValue); break;
    case engine::script::ScriptFieldType::Int:    w.WriteInt(key, value.intValue); break;
    case engine::script::ScriptFieldType::Bool:   w.WriteBool(key, value.boolValue); break;
    case engine::script::ScriptFieldType::Vec3:   w.WriteVec3(key, value.vec3Value); break;
    case engine::script::ScriptFieldType::Entity:
    case engine::script::ScriptFieldType::String:
    case engine::script::ScriptFieldType::Prefab:
        w.WriteString(key, value.stringValue);
        break;
    }
}

static void WriteRuntimeStateSnapshot(State& state)
{
    if (!state.editorLiveStateEnabled || !state.world || state.runtimeStatePath.empty())
        return;

    engine::serialization::JsonWriter w;
    w.BeginObject();
    w.WriteUint("version", 1u);
    w.BeginArray("fields");

    state.world->ForEachAlive([&](LPENTITY entity)
    {
        const auto* guid = state.world->Get<engine::GuidComponent>(entity);
        if (!guid || guid->guid.empty())
            return;

        auto* scripts = state.world->Get<engine::script::ScriptList>(entity);
        if (!scripts)
            return;

        const auto& instances = scripts->Instances();
        for (size_t scriptIndex = 0u; scriptIndex < instances.size(); ++scriptIndex)
        {
            const engine::script::ScriptInstance& inst = instances[scriptIndex];
            if (!inst.script)
                continue;

            const auto* fields = state.scriptAddonRegistry.GetFields(inst.className);
            if (!fields)
                continue;

            for (const engine::script::ScriptFieldMeta& field : *fields)
            {
                engine::script::ScriptFieldValue value{};
                if (!state.scriptAddonRegistry.ReadField(*inst.script, inst.className, field.name, value))
                    continue;
                if (value.type == engine::script::ScriptFieldType::Entity &&
                    value.entityValue.IsValid())
                {
                    if (const auto* targetGuid = state.world->Get<engine::GuidComponent>(value.entityValue))
                        value.stringValue = targetGuid->guid;
                }

                w.BeginObject();
                w.WriteString("entityGuid", guid->guid);
                w.WriteUint("scriptIndex", static_cast<uint32_t>(scriptIndex));
                w.WriteString("script", inst.className);
                w.WriteString("field", field.name);
                w.WriteString("type", ScriptFieldTypeName(value.type));
                WriteRuntimeScriptFieldValue(w, "value", value);
                w.EndObject();
            }
        }
    });

    w.EndArray();
    w.EndObject();

    std::error_code ec;
    std::filesystem::create_directories(state.runtimeStatePath.parent_path(), ec);
    if (ec)
        return;

    std::filesystem::path tmpPath = state.runtimeStatePath;
    tmpPath += L".tmp";
    {
        std::ofstream out(tmpPath, std::ios::binary | std::ios::trunc);
        if (!out)
            return;
        out << w.GetString();
        if (!out.good())
            return;
    }
    std::filesystem::rename(tmpPath, state.runtimeStatePath, ec);
    if (ec)
    {
        std::filesystem::remove(state.runtimeStatePath, ec);
        ec.clear();
        std::filesystem::rename(tmpPath, state.runtimeStatePath, ec);
    }
}

// IScriptContext::GetRenderSettings(): the same values as the Set*/Get* functions of krom.h.
class WrapperRenderSettingsService final : public engine::script::IRenderSettingsService
{
public:
    explicit WrapperRenderSettingsService(State& state) noexcept : m_state(state) {}

    engine::script::RenderSettings Get() const override { return internal::GetScriptRenderSettings(m_state.render); }
    void Set(const engine::script::RenderSettings& in) override { internal::SetScriptRenderSettings(m_state.render, in); }

    // Also remembered on this computer, like Krom::SetQualityLevel.
    void SetQualityLevel(engine::script::QualityLevel level) override { Krom::SetQualityLevel(level); }

private:
    State& m_state;
};

static std::string NormalizeProjectScenePath(std::string scenePath,
                                             const std::string& assetRootName)
{
    std::replace(scenePath.begin(), scenePath.end(), '\\', '/');
    if (scenePath.empty())
        return {};

    std::string assetPrefix = assetRootName;
    std::replace(assetPrefix.begin(), assetPrefix.end(), '\\', '/');
    if (!assetPrefix.empty() && assetPrefix.back() != '/')
        assetPrefix.push_back('/');
    if (!assetPrefix.empty() &&
        scenePath.size() > assetPrefix.size() &&
        scenePath.compare(0u, assetPrefix.size(), assetPrefix) == 0)
    {
        scenePath.erase(0u, assetPrefix.size());
    }

    const std::filesystem::path path = PathFromUtf8(scenePath);
    if (scenePath.find('/') == std::string::npos &&
        scenePath.find('\\') == std::string::npos)
    {
        return PathToGenericUtf8((std::filesystem::path("Scenes") / path)
            .replace_extension(".json"));
    }

    if (!path.has_extension())
        return PathToGenericUtf8(PathFromUtf8(scenePath).replace_extension(".json"));
    return PathToGenericUtf8(PathFromUtf8(scenePath));
}

static bool ReadProjectFile(State& state, const char* projectFilePath)
{
    if (!projectFilePath || projectFilePath[0] == '\0')
        return false;

    std::filesystem::path projectPath = PathFromUtf8(projectFilePath);
    if (std::filesystem::is_directory(projectPath))
        projectPath /= "krom-project.json";

    std::ifstream file(projectPath);
    if (!file)
    {
        engine::Debug::LogError("krom.h: project file not found: %s",
            PathToUtf8(projectPath).c_str());
        return false;
    }

    const std::string json(std::istreambuf_iterator<char>(file),
                           std::istreambuf_iterator<char>{});
    std::string err;
    const engine::serialization::JsonValue root =
        engine::serialization::JsonParser::Parse(json, err);
    if (!err.empty() || !root.IsObject())
    {
        engine::Debug::LogError("krom.h: project JSON error in '%s': %s",
            PathToUtf8(projectPath).c_str(),
            err.empty() ? "root is not an object" : err.c_str());
        return false;
    }

    state.projectRoot = std::filesystem::weakly_canonical(projectPath.parent_path());
    state.projectSettings = engine::ReadProjectSettings(root);
    internal::LoadUserGameSettings(state.render, state.projectSettings.windowTitle);   // needs the window title
    state.game.SetProjectSettings(state.projectSettings);
    if (state.projectSettings.renderingStored)
        internal::ApplyProjectRenderSettings(state.render, state.projectSettings.rendering, state.editorLiveStateEnabled);
    state.runtimeStatePath = state.projectRoot / "editor" / "runtime-state.json";
    state.runtimeStateWriteTimer = 0.0f;
    if (state.editorLiveStateEnabled)
    {
        std::error_code runtimeStateEc;
        std::filesystem::create_directories(state.runtimeStatePath.parent_path(), runtimeStateEc);
        std::filesystem::remove(state.runtimeStatePath, runtimeStateEc);
    }

    std::string assetRootName = "Assets";
    if (const auto* assetRoot = root.Get("assetRoot"); assetRoot && assetRoot->IsString())
        assetRootName = assetRoot->AsString();
    SetAssetRoot(state.projectRoot / PathFromUtf8(assetRootName));

    if (const auto* shaderCache = root.Get("shaderCache"); shaderCache && shaderCache->IsString())
        SetShaderCacheDir(state.projectRoot / PathFromUtf8(shaderCache->AsString()));

    state.sceneManager.projectScenePaths.clear();
    std::unordered_set<std::string> excludedScenes;
    if (const auto* excluded = root.Get("buildExclude"); excluded && excluded->IsArray())
        for (const auto& item : excluded->arrayVal)
            if (item.IsString())
                excludedScenes.insert(NormalizeProjectScenePath(item.AsString(), assetRootName));
    bool hasSceneOrder = false;
    if (const auto* order = root.Get("sceneOrder"); order && order->IsArray())
    {
        for (const engine::serialization::JsonValue& item : order->arrayVal)
        {
            if (!item.IsString())
                continue;
            std::string normalized = NormalizeProjectScenePath(item.AsString(), assetRootName);
            if (!normalized.empty())
            {
                hasSceneOrder = true;
                if (!excludedScenes.count(normalized))
                    state.sceneManager.projectScenePaths.push_back(std::move(normalized));
            }
        }
    }

    if (!hasSceneOrder)
    {
        if (const auto* editorScene = root.Get("editorScene"); editorScene && editorScene->IsString())
        {
            std::string normalized = NormalizeProjectScenePath(editorScene->AsString(), assetRootName);
            if (!normalized.empty() && !excludedScenes.count(normalized))
                state.sceneManager.projectScenePaths.push_back(std::move(normalized));
        }
    }

    engine::Debug::Log("krom.h: project loaded '%s' assetRoot='%s' scenes=%zu",
        PathToUtf8(projectPath).c_str(),
        PathToUtf8(state.assetRoot).c_str(),
        state.sceneManager.projectScenePaths.size());
    return true;
}

// =============================================================================

void EnsureAnimationSystems(State& state)
{
    if (!state.animationSystem)
        state.animationSystem = std::make_unique<engine::addons::animation::AnimationSystem>(state.assetRegistry);
    if (state.assetPipeline &&
        state.animationSystem->Controllers().AssetRoot() != state.assetPipeline->GetAssetRoot())
        state.animationSystem->SetAssetSources(state.assetPipeline->GetAssetRoot(), [&state](const std::string& clipPath) {
            return engine::addons::mesh_renderer::ImportModelAnimationAssets(*state.assetPipeline, state.assetRegistry, clipPath);
        });
    if (!state.skinningSystem)
    {
        if (engine::renderer::IDevice* device = state.renderLoop.GetRenderSystem().GetDevice())
            state.skinningSystem = std::make_unique<engine::addons::animation::SkinningSystem>(
                *device, &state.assetRegistry);
    }
}

internal::AssetContext MakeAssetContext(State& state)
{
    return internal::AssetContext{
        state.initialized, state.world.get(), state.assetPipeline.get(), state.assetRegistry,
        state.transformSystem, state.boundsSystem, state.scriptAddonRegistry,
        [] { EnsureAnimationSystems(Get()); },
        MakeMaterialContext(state)};
}

void RegisterEngineComponents(State& state)
{
    if (state.componentsRegistered)
        return;

    engine::RegisterCoreComponents(state.componentRegistry);
    engine::RegisterAnimationComponents(state.componentRegistry);
    engine::RegisterCameraComponents(state.componentRegistry);
    engine::RegisterMeshRendererComponents(state.componentRegistry);
    engine::RegisterLightingComponents(state.componentRegistry);
    engine::ecs::RegisterComponent<engine::script::ScriptList>(state.componentRegistry, "ScriptList");
    engine::RegisterPhysicsComponents(state.componentRegistry);
    engine::RegisterAudioComponents(state.componentRegistry);
    engine::RegisterParticleComponents(state.componentRegistry);
    engine::RegisterProbeComponents(state.componentRegistry);
    engine::RegisterPostProcessComponents(state.componentRegistry);
    engine::RegisterPrefabInstanceComponent(state.componentRegistry);
    state.componentsRegistered = true;
}

bool RegisterEngineFeatures(State& state)
{
    if (state.featuresRegistered)
        return true;

    using namespace engine;

    state.forwardPlusActive = state.config.enableForwardPlus;

    renderer::addons::forward::ForwardFeatureConfig forwardConfig{};
    forwardConfig.clearColorValue = state.config.clearColor;
    forwardConfig.enableEnvironmentBackground = true;
    forwardConfig.enableBloom = true;
    forwardConfig.bloomThreshold = 1.0f;
    forwardConfig.bloomIntensity = kRuntimeBloomIntensity;
    forwardConfig.bloomBlurRadius = 1.0f;
    forwardConfig.tonemapSettings = state.render.effectiveTonemapSettings;
    forwardConfig.autoExposureStatus = state.render.autoExposureStatus;
    forwardConfig.msaaSettings = state.render.msaaSettings;
    forwardConfig.postAASettings = state.render.postAASettings;
    forwardConfig.shadingSettings = state.render.shadingSettings;
    forwardConfig.mode = state.forwardPlusActive
        ? renderer::addons::forward::ForwardRendererMode::ForwardPlus
        : renderer::addons::forward::ForwardRendererMode::Forward;

    addons::shadow::ShadowFeatureConfig shadowConfig{};
    shadowConfig.maxShadowLights = state.config.maxShadowLights;
    shadowConfig.maxShadowViews = state.config.maxShadowViews;
    shadowConfig.quality = state.render.shadowQuality;

    renderer::RenderSystem& renderSystem = state.renderLoop.GetRenderSystem();
    if (!renderSystem.RegisterFeature(addons::mesh_renderer::CreateMeshRendererFeature()) ||
        !renderSystem.RegisterFeature(addons::lighting::CreateLightingFeature()) ||
        !renderSystem.RegisterFeature(addons::shadow::CreateShadowFeature(shadowConfig)) ||
        !renderSystem.RegisterFeature(renderer::addons::forward::CreateForwardFeature(forwardConfig)))
    {
        Debug::LogError("krom.h: failed to register core render features");
        return false;
    }

    if (state.config.enableGtao)
    {
        if (!renderSystem.RegisterFeature(renderer::addons::gtao::CreateGtaoFeature(&state.render.gtaoResources)))
        {
            Debug::LogError("krom.h: failed to register GTAO feature");
            return false;
        }
    }

    if (state.config.enableContactShadows &&
        !renderSystem.RegisterFeature(renderer::addons::contact_shadow::CreateContactShadowFeature(
            &state.render.contactShadowResources)))
    {
        Debug::LogError("krom.h: failed to register contact shadow feature");
        return false;
    }

    // SSR after GTAO: it adds to the scene composite GTAO creates.
    if (!renderSystem.RegisterFeature(renderer::addons::ssr::CreateSsrFeature(&state.render.ssrResources)))
    {
        Debug::LogError("krom.h: failed to register SSR feature");
        return false;
    }

    if (!renderSystem.RegisterFeature(engine::addons::probes::CreateProbeFeature(&state.render.probeRuntime)))
    {
        Debug::LogError("krom.h: failed to register probe feature");
        return false;
    }
    if (state.render.probeRuntime && !state.assetRoot.empty())
        state.render.probeRuntime->SetAssetRoot(PathToUtf8(state.assetRoot));
    // GTAO and the probes exist only now; LoadProject may have run before.
    if (state.projectSettings.renderingStored)
        internal::ApplyProjectRenderSettings(state.render, state.projectSettings.rendering, state.editorLiveStateEnabled);
    Debug::Log("krom.h: render quality '%s' (MSAA %u, post-AA %u, GTAO %d, SSR %d, contact shadows %d)%s",
               engine::RenderQualityLevelName(GetQualityLevel()), GetMsaa(), static_cast<unsigned>(GetPostAA()),
               GetGtaoEnabled() ? 1 : 0, GetSsrEnabled() ? 1 : 0, GetContactShadowsEnabled() ? 1 : 0,
               state.render.userQualityLevel && !state.editorLiveStateEnabled ? ", player's level" : "");

    state.featuresRegistered = true;
    return true;
}

bool InitializePlatform(State& state)
{
#if defined(KROM_APP_USE_WIN32_PLATFORM)
    state.platform = std::make_unique<engine::platform::win32::Win32Platform>();
#elif defined(KROM_APP_USE_GLFW_PLATFORM)
    state.platform = std::make_unique<engine::platform::GLFWPlatform>();
#endif
    return state.platform && state.platform->Initialize();
}

bool InitializeRenderLoop(State& state)
{
    using BackendType = engine::renderer::DeviceFactory::BackendType;
    const BackendType backend = ToBackend(state.config.backend);

    if (!state.deviceFactoryRegistry.IsRegistered(backend))
    {
        engine::Debug::LogError("krom.h: backend '%s' is not registered", BackendName(backend));
        return false;
    }

    const auto adapters = state.deviceFactoryRegistry.EnumerateAdapters(backend);
    if (adapters.empty())
    {
        engine::Debug::LogError("krom.h: backend '%s' reported no adapters", BackendName(backend));
        return false;
    }

    engine::platform::WindowDesc windowDesc{};
    windowDesc.title = state.config.title;
    windowDesc.width = state.config.width;
    windowDesc.height = state.config.height;
    // Open on the monitor the game is started on (under the mouse cursor). With
    // the system resolution the window takes that monitor's size; a framed
    // window is fitted into its work area by the platform.
    engine::platform::MonitorRect startupMonitor{};
    if (state.platform->GetStartupMonitor(startupMonitor))
    {
        windowDesc.monitor = startupMonitor;
        if (state.config.useSystemResolution)
        {
            windowDesc.width = startupMonitor.width;
            windowDesc.height = startupMonitor.height;
        }
    }
    windowDesc.windowMode = state.config.windowMode;
    windowDesc.resizable = state.config.resizable;
    windowDesc.visible = state.config.visible;
    windowDesc.vsync = state.config.vsync;

    // Diagnostics without code changes: KROM_DEBUG_LAYER=1 turns on the backend
    // validation (Vulkan layers, DX11 debug device); KROM_MSAA=<n> sets the start MSAA.
    if (const char* debugLayer = std::getenv("KROM_DEBUG_LAYER"); debugLayer && debugLayer[0] == '1')
        state.config.enableDebugLayer = true;
    internal::ApplyRenderEnvironmentOverrides(state.render);

    engine::renderer::IDevice::DeviceDesc deviceDesc{};
    deviceDesc.enableDebugLayer = state.config.enableDebugLayer;
    deviceDesc.adapterIndex = engine::renderer::DeviceFactory::FindBestAdapter(adapters);
    deviceDesc.appName = state.config.title;

    return state.renderLoop.Initialize(backend, *state.platform, windowDesc, &state.eventBus, deviceDesc);
}

bool InitializeAssetPipeline(State& state)
{
    // Asset-Root ist optional — wird erst benoetigt wenn ein Asset geladen wird.
    // Fehlt sie beim ersten Ladeversuch, gibt LoadMesh/LoadTexture usw. einen
    // Fehler aus. Hier kein hartes Abbrechen mehr.

    if (state.engineAssetRoot.empty())
        state.engineAssetRoot = ResolveEngineAssetRootOnDisk();

    state.renderLoop.GetRenderSystem().SetAssetRegistry(&state.assetRegistry);
    state.assetPipeline = std::make_unique<engine::assets::AssetPipeline>(
        state.assetRegistry,
        state.renderLoop.GetRenderSystem().GetDevice());
    state.assetPipeline->SetJobSystem(&state.jobSystem);
    engine::mesh_renderer::ConfigureAssetPipeline(*state.assetPipeline);
#if KROM_WRAPPER_HAS_GLTF
    state.assetPipeline->RegisterMeshImporter(
        std::make_unique<engine::addons::gltf::GltfImporter>());
#endif
    state.assetPipeline->SetAssetRoot(state.assetRoot);
    // Prefab loading resolves model skeletons/animations and relative paths with it.
    engine::addons::prefab::SetPrefabAssetPipeline(state.assetPipeline.get());
    return true;
}

bool InitializeTonemapMaterial(State& state)
{
    // Kein Asset-Root gesetzt — Tonemap-Pass ueberspringen.
    // Das Fenster oeffnet sich trotzdem; Rendering ohne Post-Processing ist moeglich.
    // SetAssetRoot() muss vor Graphics() gesetzt werden wenn Rendering gewuenscht ist.
    if (state.assetRoot.empty())
        return true;

    const char* tonemapVsPath = "fullscreen.vs.hlsl";
    const char* tonemapPsPath = "passthrough.ps.hlsl";

    const engine::ShaderHandle tonemapVs =
        internal::LoadEngineShader(MakeMaterialContext(state), tonemapVsPath, engine::assets::ShaderStage::Vertex);
    const engine::ShaderHandle tonemapPs =
        internal::LoadEngineShader(MakeMaterialContext(state), tonemapPsPath, engine::assets::ShaderStage::Fragment);
    if (!tonemapVs.IsValid() || !tonemapPs.IsValid())
    {
        engine::Debug::LogError("krom.h: failed to load tonemap shaders from asset root '%s'",
            PathToUtf8(state.assetRoot).c_str());
        return false;
    }

    engine::renderer::MaterialParam tonemapSampler{};
    tonemapSampler.name = "linearclamp";
    tonemapSampler.type = engine::renderer::MaterialParam::Type::Sampler;
    tonemapSampler.samplerIdx = 0u;

    const engine::renderer::ISwapchain* swapchain = state.renderLoop.GetRenderSystem().GetSwapchain();
    const engine::renderer::Format backbufferFormat = swapchain
        ? swapchain->GetBackbufferFormat()
        : engine::renderer::Format::BGRA8_UNORM_SRGB;

    engine::renderer::MaterialDesc tonemapDesc{};
    engine::renderer::MaterialRuntimeDesc tonemapRuntime{};
    tonemapDesc.name = "KromTonemap";
    tonemapDesc.domain = engine::renderer::MaterialDomain::Postprocess;
    tonemapDesc.renderPolicy.depth.test = false;
    tonemapDesc.renderPolicy.depth.write = false;
    tonemapDesc.renderPolicy.cull.mode = engine::renderer::MaterialCullMode::None;
    tonemapDesc.renderPolicy.castShadows = false;
    tonemapDesc.renderPolicy.receiveShadows = false;
    tonemapDesc.parameters.push_back(tonemapSampler);

    tonemapRuntime.renderPass = engine::renderer::StandardRenderPasses::Postprocess();
    tonemapRuntime.vertexShader = tonemapVs;
    tonemapRuntime.fragmentShader = tonemapPs;
    tonemapRuntime.colorFormat = backbufferFormat;
    tonemapRuntime.depthFormat = engine::renderer::Format::Unknown;

    const engine::MaterialHandle material =
        engine::renderer::MaterialRuntimeBridge::RegisterMaterial(
            state.materialSystem,
            std::move(tonemapDesc),
            tonemapRuntime);
    state.renderLoop.GetRenderSystem().SetDefaultTonemapMaterial(material, state.materialSystem);
    return material.IsValid();
}

bool EnsureInitialized()
{
    State& state = Get();
    if (state.initialized)
        return true;

    RegisterEngineComponents(state);
    state.world = std::make_unique<engine::ecs::World>(state.componentRegistry);
    state.cameraOptions.ambientColor = state.config.ambientColor;
    state.cameraOptions.ambientIntensity = state.config.ambientIntensity;
#ifdef KROM_HAS_GAME_SCRIPTS
    KromRegisterGameScripts(state.scriptAddonRegistry);
    engine::Debug::Log("krom.h: Projekt-Scripts registriert.");
#endif

    if (!InitializePlatform(state))
    {
        engine::Debug::LogError("krom.h: platform initialization failed");
        return false;
    }
    if (!RegisterEngineFeatures(state))
        return false;
    // The pool starts before its first user and is handed to the renderer.
    state.jobSystem.Initialize();
    state.renderLoop.GetRenderSystem().SetJobSystem(&state.jobSystem);
    if (!InitializeRenderLoop(state))
    {
        engine::Debug::LogError("krom.h: render loop initialization failed");
        return false;
    }
    if (!InitializeAssetPipeline(state))
        return false;
    if (!InitializeTonemapMaterial(state))
        return false;

    state.initialized = true;
    state.running = true;
    return true;
}

} // namespace

engine::assets::AssetRegistry& internal::GetWrapperAssetRegistry()
{
    return Get().assetRegistry;
}

size_t internal::GetWrapperSceneRootCount(LPSCENE scene)
{
    const auto& scenes = Get().sceneManager.scenes;
    const auto it = scenes.find(scene.id);
    return it != scenes.end() ? it->second.rootEntities.size() : 0u;
}

size_t internal::GetWrapperPersistentEntityCount()
{
    return Get().sceneManager.persistentEntityIds.size();
}

// =========================================================================
// Ebene 2 + 3 — Component-Zugriff, Systeme, Scripts
// =========================================================================

engine::ecs::ComponentMetaRegistry& GetComponentRegistry()
{
    return Get().componentRegistry;
}

engine::ecs::World& GetWorld()
{
    State& state = Get();
    assert(state.world && "Krom::GetWorld() erfordert vorherigen Graphics3D()-Aufruf");
    return *state.world;
}

engine::jobs::JobSystem& GetJobSystem()
{
    return Get().jobSystem;
}

void RegisterSystem(SystemFn fn)
{
    Get().userSystems.push_back(std::move(fn));
}

void SetTimeScale(float scale)
{
    Get().game.Clock().timeScale = (scale > 0.f) ? std::min(scale, 100.f) : 0.f;   // auch NaN → 0
}

float GetTimeScale()
{
    return Get().game.Clock().timeScale;
}

void SetPhysicsInterpolation(bool enabled)
{
    Get().game.Clock().physicsInterpolation = enabled;
}

bool GetPhysicsInterpolation()
{
    return Get().game.Clock().physicsInterpolation;
}

engine::script::ScriptRegistry& GetScriptRegistry()
{
    return Get().scriptAddonRegistry;
}

// =========================================================================

void SetShaderCacheDir(const std::filesystem::path& path)
{
    engine::renderer::ShaderCompiler::SetCacheDirectory(path);
}

void SetAssetRoot(const std::filesystem::path& path)
{
    State& state = Get();
    state.assetRoot = path;
    state.prefabCache.clear();   // relative asset paths of the cached prefabs refer to the old root
    state.game.SetAssetRoot(path);
    if (state.render.probeRuntime)
        state.render.probeRuntime->SetAssetRoot(PathToUtf8(path));
    if (state.assetPipeline)
        state.assetPipeline->SetAssetRoot(path);

    // Cache-Verzeichnis neben das Asset-Root legen, damit Editor und
    // Wrapper mit demselben Asset-Verzeichnis denselben Cache teilen.
    const auto abs = std::filesystem::weakly_canonical(std::filesystem::absolute(path));
    engine::renderer::ShaderCompiler::SetCacheDirectory(abs.parent_path() / "shader_artifacts");
}

const std::filesystem::path& GetAssetRoot()
{
    return Get().assetRoot;
}

bool LoadProject(const char* projectFilePath)
{
    return ReadProjectFile(Get(), projectFilePath);
}

void ApplyProjectGraphicsSettings(GraphicsConfig& config)
{
    const auto& settings = Get().projectSettings;
    if (!settings.windowTitle.empty())
        config.title = settings.windowTitle;
    config.width = static_cast<uint32_t>(settings.width);
    config.height = static_cast<uint32_t>(settings.height);
    config.useSystemResolution = settings.useSystemResolution;
    config.windowMode = settings.windowMode;
    config.vsync = settings.vsync;
}

void SetEditorLiveStateEnabled(bool enabled)
{
    State& state = Get();
    const bool userLevelChanges = state.render.userQualityLevel && state.editorLiveStateEnabled != enabled;
    state.editorLiveStateEnabled = enabled;
    state.runtimeStateWriteTimer = 0.0f;
    // Runs started from the editor show the project's options, not a player's level.
    if (userLevelChanges && state.projectSettings.renderingStored)
        internal::ApplyProjectRenderSettings(state.render, state.projectSettings.rendering, state.editorLiveStateEnabled);

    if (!enabled || state.projectRoot.empty())
        return;

    state.runtimeStatePath = state.projectRoot / "editor" / "runtime-state.json";
    std::error_code ec;
    std::filesystem::create_directories(state.runtimeStatePath.parent_path(), ec);
    std::filesystem::remove(state.runtimeStatePath, ec);
}

bool EditorLiveStateEnabled()
{
    return Get().editorLiveStateEnabled;
}

int DefaultRenderer() noexcept
{
#if defined(KROM_APP_BACKEND_DX11)
    return Renderer::DX11;
#elif defined(KROM_APP_BACKEND_VULKAN)
    return Renderer::Vulkan;
#else
    return Renderer::Vulkan;
#endif
}

const char* RendererName(int backend) noexcept
{
    switch (backend)
    {
    case Renderer::DX11: return "DX11";
    case Renderer::DX12: return "DX12";
    case Renderer::Vulkan: return "Vulkan";
    default: return "Unknown";
    }
}

bool IsRendererAvailable(int backend)
{
    return Get().deviceFactoryRegistry.IsAvailable(ToBackend(backend));
}

std::vector<AdapterInfo> EnumerateAdapters(int backend)
{
    const auto raw = Get().deviceFactoryRegistry.EnumerateAdapters(ToBackend(backend));
    std::vector<AdapterInfo> result;
    result.reserve(raw.size());
    for (const auto& a : raw)
        result.push_back({a.index, a.name, a.dedicatedVRAM, a.isDiscrete, a.featureLevel});
    return result;
}

void SetConfig(const GraphicsConfig& config)
{
    Get().config = config;
}

bool Graphics(const GraphicsConfig& config)
{
    SetConfig(config);
    return EnsureInitialized();
}

bool Graphics(int backend,
              int width,
              int height,
              const char* title,
              float clearR,
              float clearG,
              float clearB,
              bool resizable)
{
    GraphicsConfig config{};
    config.backend = backend;
    config.width = static_cast<uint32_t>(width);
    config.height = static_cast<uint32_t>(height);
    config.title = title ? title : "KROM";
    config.clearColor = {clearR, clearG, clearB, 1.0f};
    config.resizable = resizable;
    return Graphics(config);
}

bool Graphics(int width,
              int height,
              const char* title,
              float clearR,
              float clearG,
              float clearB,
              bool resizable)
{
    return Graphics(DefaultRenderer(), width, height, title, clearR, clearG, clearB, resizable);
}

void OnInit(InitFn fn)
{
    Get().initCallback = std::move(fn);
}

void OnUpdate(TickFn fn)
{
    Get().tickCallback = std::move(fn);
}

bool KeyDown(Key key)
{
    if (engine::platform::IInput* input = Get().renderLoop.GetInput())
        return input->KeyDown(key);
    return false;
}

bool KeyHit(Key key)
{
    if (engine::platform::IInput* input = Get().renderLoop.GetInput())
        return input->KeyHit(key);
    return false;
}

bool MouseButtonDown(MouseButton button)
{
    if (engine::platform::IInput* input = Get().renderLoop.GetInput())
        return input->MouseButtonDown(button);
    return false;
}

int MouseDeltaX()
{
    if (engine::platform::IInput* input = Get().renderLoop.GetInput())
        return input->MouseDeltaX();
    return 0;
}

int MouseDeltaY()
{
    if (engine::platform::IInput* input = Get().renderLoop.GetInput())
        return input->MouseDeltaY();
    return 0;
}

float MouseScrollDelta()
{
    if (engine::platform::IInput* input = Get().renderLoop.GetInput())
        return input->MouseScrollDelta();
    return 0.0f;
}

void Quit()
{
    State& state = Get();
    state.running = false;
    if (engine::platform::IWindow* window = state.renderLoop.GetWindow())
        window->RequestClose();
}

bool AppRunning()
{
    State& state = Get();
    return state.running && !state.renderLoop.ShouldExit();
}

// =============================================================================
// Entities (KromEntityApi.cpp)
// =============================================================================

LPENTITY CreateEntity(const char* name)
{
    return internal::CreateEntity(Get().world.get(), name);
}

bool IsAlive(LPENTITY entity)
{
    return internal::IsAlive(Get().world.get(), entity);
}

// Stays here: it also clears scene-manager and surface state of the entity.
bool DestroyEntity(LPENTITY entity)
{
    State& state = Get();
    if (!state.world || !state.world->IsAlive(entity))
        return false;

    // No transform/bounds update needed: dead ids are skipped, and the next regular
    // update rebuilds its sorted list through the world's structure version.
    internal::ForgetDestroyedEntities(state.sceneManager, *state.world, entity);
    std::erase_if(state.surfaces, [&](const auto& item) {
        return item.second.entity == entity;
    });
    internal::DestroyEntityRecursive(*state.world, entity);
    return true;
}

bool SetName(LPENTITY entity, const char* name)
{
    return internal::SetName(Get().world.get(), entity, name);
}

const char* GetName(LPENTITY entity)
{
    return internal::GetName(Get().world.get(), entity);
}

bool SetLayer(LPENTITY entity, uint32_t layerMask)
{
    return internal::SetLayer(Get().world.get(), entity, layerMask);
}

bool SetLayerRecursive(LPENTITY rootEntity, uint32_t layerMask)
{
    return internal::SetLayerRecursive(Get().world.get(), rootEntity, layerMask);
}

uint32_t GetLayer(LPENTITY entity)
{
    return internal::GetLayer(Get().world.get(), entity);
}

bool SetCameraLayers(LPENTITY camera, uint32_t cullingMask)
{
    return internal::SetCameraLayers(Get().world.get(), camera, cullingMask);
}

uint32_t GetCameraLayers(LPENTITY camera)
{
    return internal::GetCameraLayers(Get().world.get(), camera);
}

bool SetTag(LPENTITY entity, const char* tag)
{
    return internal::SetTag(Get().world.get(), entity, tag);
}

const char* GetTag(LPENTITY entity)
{
    return internal::GetTag(Get().world.get(), entity);
}

LPENTITY FindEntityByTag(const char* tag)
{
    return internal::FindEntityByTag(Get().world.get(), tag);
}

LPENTITY FindChild(LPENTITY rootEntity, const char* name)
{
    return internal::FindChild(Get().world.get(), rootEntity, name);
}

int CountChildren(LPENTITY entity)
{
    return internal::CountChildren(Get().world.get(), entity);
}

LPENTITY GetChild(LPENTITY entity, int index)
{
    return internal::GetChild(Get().world.get(), entity, index);
}

bool SetVisible(LPENTITY entity, bool visible, bool recursive)
{
    return internal::SetVisible(Get().world.get(), entity, visible, recursive);
}

bool SetActive(LPENTITY entity, bool active, bool recursive)
{
    return internal::SetActive(Get().world.get(), entity, active, recursive);
}

bool SetParent(LPENTITY child, LPENTITY parent, bool keepWorldTransform)
{
    return internal::SetParent(MakeEntityContext(Get()), child, parent, keepWorldTransform);
}

bool Detach(LPENTITY child, bool keepWorldTransform)
{
    return internal::Detach(MakeEntityContext(Get()), child, keepWorldTransform);
}

bool SetPosition(LPENTITY entity, const engine::math::Vec3& position)
{
    return internal::SetPosition(Get().world.get(), entity, position);
}

bool SetPosition(LPENTITY entity, float x, float y, float z)
{
    return SetPosition(entity, engine::math::Vec3{x, y, z});
}

bool SetRotationEulerDeg(LPENTITY entity, float pitch, float yaw, float roll)
{
    return internal::SetRotationEulerDeg(Get().world.get(), entity, pitch, yaw, roll);
}

engine::math::Vec3 GetRotationEulerDeg(LPENTITY entity)
{
    return internal::GetRotationEulerDeg(Get().world.get(), entity);
}

bool SetScale(LPENTITY entity, const engine::math::Vec3& scale)
{
    return internal::SetScale(Get().world.get(), entity, scale);
}

bool SetScale(LPENTITY entity, float x, float y, float z)
{
    return SetScale(entity, engine::math::Vec3{x, y, z});
}

bool SetScale(LPENTITY entity, float uniformScale)
{
    return SetScale(entity, engine::math::Vec3{uniformScale, uniformScale, uniformScale});
}

bool MoveEntity(LPENTITY entity, float x, float y, float z)
{
    return internal::MoveEntity(Get().world.get(), entity, x, y, z);
}

bool TurnEntity(LPENTITY entity, float pitch, float yaw, float roll)
{
    return internal::TurnEntity(Get().world.get(), entity, pitch, yaw, roll);
}

bool GetWorldBounds(LPENTITY entity,
                    engine::math::Vec3& outCenter,
                    engine::math::Vec3& outExtents,
                    float& outBoundingSphere)
{
    return internal::GetWorldBounds(Get().world.get(), entity, outCenter, outExtents, outBoundingSphere);
}

bool LookAt(LPENTITY entity, const engine::math::Vec3& target, const engine::math::Vec3& up)
{
    return internal::LookAt(Get().world.get(), entity, target, up);
}

bool LookAt(LPENTITY entity, LPENTITY targetEntity, const engine::math::Vec3& up)
{
    return internal::LookAt(Get().world.get(), entity, targetEntity, up);
}

LPENTITY CreateCamera(const char* name,
                      const engine::math::Vec3& position,
                      float fovYDeg,
                      bool isMainCamera)
{
    return internal::CreateCamera(Get().world.get(), name, position, fovYDeg, isMainCamera);
}

LPENTITY CreateDirectionalLight(const char* name,
                                const engine::math::Vec3& position,
                                const engine::math::Vec3& eulerDeg,
                                const engine::math::Vec3& color,
                                float intensity)
{
    return internal::CreateDirectionalLight(Get().world.get(), name, position, eulerDeg, color, intensity);
}

LPENTITY CreatePointLight(const char* name,
                          const engine::math::Vec3& position,
                          const engine::math::Vec3& color,
                          float intensity,
                          float range)
{
    return internal::CreatePointLight(Get().world.get(), name, position, color, intensity, range);
}

LPENTITY CreateSpotLight(const char* name,
                         const engine::math::Vec3& position,
                         const engine::math::Vec3& eulerDeg,
                         const engine::math::Vec3& color,
                         float intensity,
                         float range,
                         float innerDeg,
                         float outerDeg)
{
    return internal::CreateSpotLight(Get().world.get(), name, position, eulerDeg, color, intensity, range, innerDeg, outerDeg);
}

// =============================================================================
// Assets: model import, loading, materials, assignment, procedural meshes
// (KromAssetApi.cpp)
// =============================================================================

LPENTITY CreateEntityFromAsset(const char* path, const char* name)
{
    return internal::CreateEntityFromAsset(MakeAssetContext(Get()), path, name);
}

bool PrintAssetTree(const char* path)
{
    return internal::PrintAssetTree(Get().assetPipeline.get(), path);
}

LPMESH LoadMesh(const char* path)
{
    State& state = Get();
    return internal::LoadMesh(state.assetPipeline.get(), state.assetRegistry, path);
}

LPTEXTURE LoadTexture(const char* path)
{
    State& state = Get();
    return internal::LoadTexture(state.assetPipeline.get(), state.texturePaths, path);
}

LPMATERIAL CreateTextureMaterial(const char* albedoTexturePath,
                                 int materialModel,
                                 engine::math::Vec4 baseColorFactor,
                                 float roughness,
                                 float metallic)
{
    return internal::CreateTextureMaterial(MakeAssetContext(Get()),
        albedoTexturePath, materialModel, baseColorFactor, roughness, metallic);
}

LPMATERIAL CreateTextureMaterial(LPTEXTURE texture,
                                 int materialModel,
                                 engine::math::Vec4 baseColorFactor,
                                 float roughness,
                                 float metallic)
{
    return internal::CreateTextureMaterial(MakeAssetContext(Get()),
        texture, materialModel, baseColorFactor, roughness, metallic);
}

LPMATERIAL LoadMaterial(const char* path)
{
    State& state = Get();
    return internal::LoadMaterial(state.assetPipeline.get(), state.assetRegistry, path);
}

bool SetMesh(LPENTITY entity, LPMESH mesh)
{
    return internal::SetMesh(MakeAssetContext(Get()), entity, mesh);
}

bool SetMaterial(LPENTITY entity, LPMATERIAL material)
{
    return internal::SetMaterial(MakeAssetContext(Get()), entity, material);
}

bool SetMaterialRecursive(LPENTITY rootEntity, LPMATERIAL material)
{
    return internal::SetMaterialRecursive(MakeAssetContext(Get()), rootEntity, material);
}

bool SetTexture(LPENTITY entity, const char* albedoTexturePath, int materialModel, bool recursive)
{
    return internal::SetTexture(MakeAssetContext(Get()), entity, albedoTexturePath, materialModel, recursive);
}

bool SetTexture(LPENTITY entity, LPTEXTURE texture, int materialModel, bool recursive)
{
    return internal::SetTexture(MakeAssetContext(Get()), entity, texture, materialModel, recursive);
}

bool SetPBR(LPENTITY entity, const PBR& materialDesc, bool recursive)
{
    return internal::SetPBR(MakeAssetContext(Get()), entity, materialDesc, recursive);
}

bool SetMeshFromAsset(LPENTITY entity, const char* path)
{
    return internal::SetMeshFromAsset(MakeAssetContext(Get()), entity, path);
}

bool SetMaterialFromAsset(LPENTITY entity, const char* path)
{
    return internal::SetMaterialFromAsset(MakeAssetContext(Get()), entity, path);
}

LPMESH CreateMeshAsset(const MeshData& meshData)
{
    return internal::CreateMeshAsset(MakeAssetContext(Get()), meshData);
}

LPENTITY CreateMeshEntity(const MeshData& meshData)
{
    return internal::CreateMeshEntity(MakeAssetContext(Get()), meshData);
}

LPENTITY CreateMesh(const char* name)
{
    return internal::CreateMesh(MakeAssetContext(Get()), name);
}

LPSURFACE CreateSurface(LPENTITY meshEntity)
{
    State& state = Get();
    if (!state.initialized)
        return NULL_SURFACE;
    return internal::CreateSurface(MakeSurfaceContext(state), meshEntity);
}

int AddVertex(LPSURFACE surface, float x, float y, float z, float u, float v)
{
    State& state = Get();
    return internal::AddVertex(MakeSurfaceContext(state), state.runtimeMaterialCache, surface, x, y, z, u, v);
}

int AddVertex(LPSURFACE surface, float x, float y, float z, float u, float v, float nx, float ny, float nz)
{
    const int index = Krom::AddVertex(surface, x, y, z, u, v);
    if (index >= 0)
        (void)Krom::VertexNormal(surface, index, nx, ny, nz);
    return index;
}

bool AddTriangle(LPSURFACE surface, int v0, int v1, int v2)
{
    State& state = Get();
    return internal::AddTriangle(MakeSurfaceContext(state), state.runtimeMaterialCache, surface, v0, v1, v2);
}

bool VertexNormal(LPSURFACE surface, int index, float nx, float ny, float nz)
{
    State& state = Get();
    return internal::VertexNormal(MakeSurfaceContext(state), state.runtimeMaterialCache, surface, index, nx, ny, nz);
}

bool VertexTexCoords(LPSURFACE surface, int index, float u, float v)
{
    State& state = Get();
    return internal::VertexTexCoords(MakeSurfaceContext(state), state.runtimeMaterialCache, surface, index, u, v);
}

int CountVertices(LPSURFACE surface)
{
    State& state = Get();
    return internal::CountVertices(MakeSurfaceContext(state), surface);
}

int CountTriangles(LPSURFACE surface)
{
    State& state = Get();
    return internal::CountTriangles(MakeSurfaceContext(state), surface);
}

LPMATERIAL CreateLitMaterial(const char* albedoTexturePath,
                              engine::math::Vec4 baseColorFactor,
                              float roughness,
                              float metallic)
{
    return internal::CreateLitMaterial(MakeAssetContext(Get()), albedoTexturePath, baseColorFactor, roughness, metallic);
}

engine::renderer::EnvironmentHandle CreateEnvironmentFromHDR(const char* path,
                                                             float intensity,
                                                             bool enableIBL)
{
    if (!path || !Get().assetPipeline)
        return engine::renderer::EnvironmentHandle::Invalid();

    const LPTEXTURE hdrTexture = LoadTexture(path);
    if (!hdrTexture.IsValid())
        return engine::renderer::EnvironmentHandle::Invalid();

    Get().assetPipeline->UploadPendingGpuAssets();

    engine::renderer::EnvironmentDesc desc{};
    desc.mode = engine::renderer::EnvironmentMode::Texture;
    desc.sourceTexture = hdrTexture;
    desc.intensity = intensity;
    desc.enableIBL = enableIBL;
    return Get().renderLoop.GetRenderSystem().CreateEnvironment(desc);
}

void SetTonemapping(engine::renderer::addons::forward::TonemapOperator op, float exposureEv)
{
    internal::SetTonemapping(Get().render, op, exposureEv);
}

bool BakeReflectionProbe(LPENTITY entity, const char* sceneName)
{
    State& state = Get();
    if (!state.world || !state.world->IsAlive(entity) || !state.render.probeRuntime)
    {
        engine::Debug::LogError("krom.h: BakeReflectionProbe needs a live entity and the probe feature");
        return false;
    }
    engine::ReflectionProbeComponent* probe = state.world->Get<engine::ReflectionProbeComponent>(entity);
    if (!probe)
        probe = &state.world->Add<engine::ReflectionProbeComponent>(entity, engine::ReflectionProbeComponent{});
    if (probe->probeId == 0u)
        probe->probeId = engine::addons::probes::GenerateProbeId();
    probe->bakedDataPath = engine::addons::probes::MakeProbeDataPath(sceneName ? sceneName : "", probe->probeId);

    state.transformSystem.Update(*state.world);
    const auto* xf = state.world->Get<engine::WorldTransformComponent>(entity);
    engine::renderer::RenderSystem& renderSystem = state.renderLoop.GetRenderSystem();
    const engine::renderer::ISwapchain* swapchain = renderSystem.GetSwapchain();
    engine::renderer::RenderView view{};
    if (!xf || !swapchain || !engine::addons::camera::BuildPrimaryRenderView(
            *state.world, swapchain->GetWidth(), swapchain->GetHeight(), view, state.cameraOptions))
    {
        engine::Debug::LogError("krom.h: BakeReflectionProbe needs a main camera");
        return false;
    }

    engine::addons::probes::ProbeBakeRequest request{};
    request.position = xf->position;
    request.extents = probe->extents;
    request.resolution = probe->resolution;
    request.relativePath = probe->bakedDataPath;
    request.inputHash = engine::addons::probes::ComputeProbeInputHash(*state.world, entity);
    // Pass 1: global IBL only; pass 2 (if set): lit by the probes of pass 1.
    const uint32_t passes = state.render.probeRuntime->GetSettings().bakePasses >= 2u ? 2u : 1u;
    for (uint32_t pass = 1u; pass <= passes; ++pass)
    {
        request.lightWithProbes = pass == 2u;
        std::string error;
        if (!engine::addons::probes::BakeProbeBlocking(renderSystem, *state.world, state.materialSystem,
                                                        state.timing, view, *state.render.probeRuntime, request,
                                                        &state.jobSystem, &error))
        {
            engine::Debug::LogError("krom.h: BakeReflectionProbe failed: %s", error.c_str());
            return false;
        }
    }
    return true;
}

bool SetPostProcessWeight(LPENTITY entity, float weight)
{
    State& state = Get();
    auto* volume = (state.world && state.world->IsAlive(entity))
        ? state.world->Get<engine::PostProcessVolumeComponent>(entity) : nullptr;
    if (!volume)
        return false;
    volume->weight = std::isfinite(weight) ? std::clamp(weight, 0.0f, 1.0f) : 0.0f;
    return true;
}

float GetPostProcessWeight(LPENTITY entity)
{
    State& state = Get();
    const auto* volume = (state.world && state.world->IsAlive(entity))
        ? state.world->Get<engine::PostProcessVolumeComponent>(entity) : nullptr;
    return volume ? volume->weight : 0.0f;
}

void SetReflectionProbesEnabled(bool enabled) { internal::SetReflectionProbesEnabled(Get().render, enabled); }
void SetProbeDebugView(uint32_t view) { internal::SetProbeDebugView(Get().render, view); }
void SetSpecularOcclusion(bool enabled) { internal::SetSpecularOcclusion(Get().render, enabled); }
bool GetSpecularOcclusion() { return internal::GetSpecularOcclusion(Get().render); }

void SetGtaoEnabled(bool enabled) { internal::SetGtaoEnabled(Get().render, enabled); }
bool GetGtaoEnabled() { return internal::GetGtaoEnabled(Get().render); }
void SetSsrEnabled(bool enabled) { internal::SetSsrEnabled(Get().render, enabled); }
bool GetSsrEnabled() { return internal::GetSsrEnabled(Get().render); }
void SetContactShadowsEnabled(bool enabled) { internal::SetContactShadowsEnabled(Get().render, enabled); }
bool GetContactShadowsEnabled() { return internal::GetContactShadowsEnabled(Get().render); }

void SetShadowQuality(uint32_t sunResolution, uint32_t localResolution, uint8_t filter)
{
    internal::SetShadowQuality(Get().render, sunResolution, localResolution, filter);
}

void GetShadowQuality(uint32_t& sunResolution, uint32_t& localResolution, uint8_t& filter)
{
    internal::GetShadowQuality(Get().render, sunResolution, localResolution, filter);
}

void SetPostAA(engine::RenderPostAA mode) { internal::SetPostAA(Get().render, mode); }
engine::RenderPostAA GetPostAA() { return internal::GetPostAA(Get().render); }
void SetMsaa(uint32_t sampleCount) { internal::SetMsaa(Get().render, sampleCount); }
uint32_t GetMsaa() { return internal::GetMsaa(Get().render); }

void SetQualityLevel(engine::RenderQualityLevel level)
{
    State& state = Get();
    internal::SetQualityLevel(state.render, level, state.editorLiveStateEnabled, state.projectSettings.windowTitle);
}

engine::RenderQualityLevel GetQualityLevel() { return internal::GetQualityLevel(Get().render); }

void SetShading(const engine::renderer::addons::forward::ShadingSettings& settings)
{
    internal::SetShading(Get().render, settings);
}

engine::renderer::addons::forward::ShadingSettings GetShading() { return internal::GetShading(Get().render); }

engine::renderer::addons::forward::TonemapSettings GetTonemapping() { return internal::GetTonemapping(Get().render); }

void SetColorGrading(const engine::renderer::addons::forward::ColorGradingSettings& grading)
{
    internal::SetColorGrading(Get().render, grading);
}

engine::renderer::addons::forward::ColorGradingSettings GetColorGrading()
{
    return internal::GetColorGrading(Get().render);
}

void SetActiveEnvironment(engine::renderer::EnvironmentHandle environment)
{
    if (environment.IsValid())
    {
        Get().activeEnvironment = environment;
        Get().renderLoop.GetRenderSystem().SetActiveEnvironment(environment);
    }
}

void WarmUpShaders()
{
    State& state = Get();
    if (!state.initialized || !state.assetPipeline)
        return;

    // Parallel-kompiliert Shader-Caches (Job-System) und lädt Texturen hoch.
    // BuildPendingShaderCaches() füllt compiledArtifacts in den ShaderAssets —
    // danach braucht PrepareShaderAsset() nur noch CreateShaderFromBytecode().
    state.assetPipeline->UploadPendingGpuAssets();

    engine::renderer::ShaderRuntime& shaderRuntime =
        state.renderLoop.GetRenderSystem().GetShaderRuntime();
    const engine::renderer::MaterialSystemShaderMaterialSource materialSource(state.materialSystem);

    // Nur Shader hochladen, die die aktuelle Szene tatsächlich benötigt.
    std::vector<engine::ShaderHandle> shaderRequests;
    (void)shaderRuntime.CollectShaderRequests(materialSource, shaderRequests);
    (void)shaderRuntime.CommitShaderRequests(shaderRequests);

    (void)shaderRuntime.PrepareAllMaterials(materialSource);
}

void SetParallelShaderCompiles(bool enabled)
{
    State& state = Get();
    if (!state.initialized)
        return;
    state.renderLoop.GetRenderSystem().GetShaderRuntime().SetAsyncShaderCompileJobs(
        enabled ? &state.jobSystem : nullptr);
}

bool ShaderCompilesPending()
{
    State& state = Get();
    return state.initialized &&
        state.renderLoop.GetRenderSystem().GetShaderRuntime().HasPendingShaderCompiles();
}

// Unit cube with its own normals and UVs per face (24 vertices), so the
// mesh layout matches imported models (normals + generated tangents).
static LPMESH CreateWarmupCubeMesh()
{
    using engine::math::Vec2;
    using engine::math::Vec3;
    struct Face { Vec3 normal, u, v; };
    const Face faces[6] = {
        {{ 1, 0, 0}, { 0, 0,-1}, {0, 1, 0}},
        {{-1, 0, 0}, { 0, 0, 1}, {0, 1, 0}},
        {{ 0, 1, 0}, { 1, 0, 0}, {0, 0,-1}},
        {{ 0,-1, 0}, { 1, 0, 0}, {0, 0, 1}},
        {{ 0, 0, 1}, { 1, 0, 0}, {0, 1, 0}},
        {{ 0, 0,-1}, {-1, 0, 0}, {0, 1, 0}},
    };
    std::vector<MeshVertex> vertices;
    std::vector<uint32_t> indices;
    vertices.reserve(24u);
    indices.reserve(36u);
    for (const Face& face : faces)
    {
        const uint32_t base = static_cast<uint32_t>(vertices.size());
        const float corners[4][2] = {{-1, -1}, {1, -1}, {1, 1}, {-1, 1}};
        for (const auto& c : corners)
        {
            MeshVertex vertex{};
            vertex.position = (face.normal + face.u * c[0] + face.v * c[1]) * 0.5f;
            vertex.normal = face.normal;
            vertex.uv = Vec2{(c[0] + 1.0f) * 0.5f, 1.0f - (c[1] + 1.0f) * 0.5f};
            vertices.push_back(vertex);
        }
        for (const uint32_t i : {0u, 1u, 2u, 0u, 2u, 3u})
            indices.push_back(base + i);
    }

    MeshData data{};
    data.name = "__WarmupCube";
    data.vertices = vertices.data();
    data.vertexCount = static_cast<uint32_t>(vertices.size());
    data.indices = indices.data();
    data.indexCount = static_cast<uint32_t>(indices.size());
    data.generateNormals = false;
    data.generateTangents = true;
    return CreateMeshAsset(data);
}

int SpawnShaderWarmupAssets()
{
    State& state = Get();
    if (!state.initialized || !state.world || !state.assetPipeline || state.assetRoot.empty())
        return 0;

    std::vector<std::string> prefabs;
    std::vector<std::string> materials;
    std::error_code ec;
    for (std::filesystem::recursive_directory_iterator it(state.assetRoot, ec), end; !ec && it != end; it.increment(ec))
    {
        if (!it->is_regular_file(ec))
            continue;
        const std::string ext = PathToUtf8(it->path().extension());
        if (ext != ".prefab" && ext != ".mat")
            continue;
        const std::string relative = PathToGenericUtf8(it->path().lexically_relative(state.assetRoot));
        (ext == ".prefab" ? prefabs : materials).push_back(relative);
    }
    std::sort(prefabs.begin(), prefabs.end());
    std::sort(materials.begin(), materials.end());

    // Everything goes a few units in front of the main camera: inside the view
    // frustum and the first shadow cascade. Overlap does not matter, every
    // object is still drawn once per pass.
    engine::math::Vec3 origin{0.0f, 0.0f, 0.0f};
    const LPENTITY camera = GetMainCamera();
    if (camera.IsValid())
    {
        state.transformSystem.Update(*state.world);
        if (const auto* world = state.world->Get<engine::WorldTransformComponent>(camera))
            origin = world->position + world->rotation.Rotate(engine::math::Vec3::Forward()) * 4.0f;
    }

    int spawned = 0;
    for (const std::string& prefab : prefabs)
    {
        // Scripts stay off: the warm-up must not run game logic (scene loads,
        // spawns) of objects that are not in the scene.
        const LPENTITY root = InstantiatePrefabForScript(state, prefab, origin,
            engine::math::Quat::Identity(), engine::math::Vec3{1.0f, 1.0f, 1.0f}, false);
        if (!root.IsValid())
            continue;
        ++spawned;

        // A camera inside a prefab must not take over the view.
        std::vector<LPENTITY> stack{root};
        while (!stack.empty())
        {
            const LPENTITY entity = stack.back();
            stack.pop_back();
            if (auto* cameraComponent = state.world->Get<engine::CameraComponent>(entity))
                cameraComponent->isMainCamera = false;
            for (int i = 0, n = CountChildren(entity); i < n; ++i)
                stack.push_back(GetChild(entity, i));
        }
    }

    if (!materials.empty())
    {
        const LPMESH cube = CreateWarmupCubeMesh();
        for (const std::string& path : materials)
        {
            const LPMATERIAL material = LoadMaterial(path.c_str());
            if (!material.IsValid())
                continue;
            const LPENTITY entity = internal::CreateProceduralEntity(MakeAssetContext(state), cube, "__WarmupMaterial");
            if (!entity.IsValid())
                continue;
            (void)SetMaterial(entity, material);
            (void)SetPosition(entity, origin);
            ++spawned;
        }
    }

    // Set back main camera, in case a prefab camera was the first one found.
    if (camera.IsValid())
    {
        if (auto* cameraComponent = state.world->Get<engine::CameraComponent>(camera))
            cameraComponent->isMainCamera = true;
    }

    state.transformSystem.Update(*state.world);
    state.boundsSystem.Update(*state.world);
    engine::Debug::Log("krom.h: shader warm-up spawned %d prefab/material objects (%zu prefabs, %zu materials)",
        spawned, prefabs.size(), materials.size());
    return spawned;
}

// Schriftquellen fuer den Bildschirmtext: Projekt, Engine, dann System.
static std::vector<std::filesystem::path> TextFontCandidates(const State& state)
{
    std::vector<std::filesystem::path> candidates;
    if (!state.assetRoot.empty())
        candidates.push_back(state.assetRoot / "fonts" / "default.ttf");
    if (!state.engineAssetRoot.empty())
        candidates.push_back(state.engineAssetRoot / "fonts" / "default.ttf");
#if defined(_WIN32)
    std::filesystem::path fontDir = "C:/Windows/Fonts";
    if (const wchar_t* windir = _wgetenv(L"WINDIR"); windir && *windir)
        fontDir = std::filesystem::path(windir) / "Fonts";
    candidates.push_back(fontDir / "segoeui.ttf");
    candidates.push_back(fontDir / "arial.ttf");
#else
    candidates.push_back("/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf");
#endif
    return candidates;
}

int Run()
{
    State& state = Get();
    if (!EnsureInitialized())
        return -1;

    if (state.initCallback && !state.initCallback())
        return -2;

    state.transformSystem.Update(*state.world);
    state.boundsSystem.Update(*state.world);

    // Keine Kamera in der Welt — Standard-Kamera automatisch anlegen damit
    // der Render-Pass nicht fehlschlaegt (z.B. bei reinen Fenster-Beispielen).
    {
        bool hasMainCamera = false;
        state.world->View<engine::CameraComponent>([&](LPENTITY, engine::CameraComponent& c)
        {
            if (c.isMainCamera) hasMainCamera = true;
        });
        if (!hasMainCamera)
            CreateCamera("__DefaultCamera");
    }

    // Rigs can arrive from scene files or script prefab spawns, not only from
    // CreateEntityFromAsset: keep both systems available from the first frame
    // (they cost nothing without SkinComponent/AnimationPlayerComponent).
    EnsureAnimationSystems(state);

    while (!state.renderLoop.ShouldExit())
    {
        engine::platform::IWindow* window = state.renderLoop.GetWindow();
        engine::platform::IInput* input = state.renderLoop.GetInput();
        if (!state.platform || !window || !input)
        {
            engine::Debug::LogError("krom.h: runtime frame failed - platform/window/input missing");
            return -4;
        }

        state.timing.BeginFrame();
        state.platform->PumpEvents();
        const engine::platform::WindowEventState windowState = window->PumpEvents(*input);

        if (windowState.resized)
        {
            const uint32_t resizeWidth = windowState.framebufferWidth > 0u ? windowState.framebufferWidth : windowState.width;
            const uint32_t resizeHeight = windowState.framebufferHeight > 0u ? windowState.framebufferHeight : windowState.height;
            state.renderLoop.GetRenderSystem().HandleResize(resizeWidth, resizeHeight);
        }

        if (windowState.quitRequested || window->ShouldClose())
        {
            state.timing.EndFrame();
            break;
        }

        // Spielzeit: alles Gameplay (Tick, Systeme, Scripts, Physik, Animation)
        // laeuft mit dem skalierten Delta; TimeScale aus dem letzten Frame.
        // BeginFrame leert auch den Bildschirmtext des letzten Frames.
        engine::gameplay::GameSimulation& game = state.game;
        game.SetAudioDevice(state.config.audioDevice);
        const float unscaledDeltaSeconds = state.timing.GetDeltaSecondsF();

        // Bildschirmtext auf den aktuellen Backbuffer einstellen.
        {
            engine::addons::text::TextRenderer& text = game.Text();
            if (!state.textConfigured)
            {
                text.SetFontCandidates(TextFontCandidates(state));
                state.textConfigured = true;
            }
            engine::renderer::RenderSystem& renderSystem = state.renderLoop.GetRenderSystem();
            const engine::renderer::ISwapchain* textSwapchain = renderSystem.GetSwapchain();
            text.SetViewport(
                (textSwapchain && textSwapchain->GetWidth() > 0u) ? textSwapchain->GetWidth() : state.config.width,
                (textSwapchain && textSwapchain->GetHeight() > 0u) ? textSwapchain->GetHeight() : state.config.height);
            if (textSwapchain)
                text.SetTargetFormat(textSwapchain->GetBackbufferFormat());
        }

        const engine::script::PrefabInstantiateFn instantiatePrefab =
            [&state](std::string_view prefabAssetPath,
                     const engine::math::Vec3& position,
                     const engine::math::Quat& rotation,
                     const engine::math::Vec3& scale) -> LPENTITY
            {
                return InstantiatePrefabForScript(state, prefabAssetPath, position, rotation, scale);
            };
        const std::function<void(LPENTITY, LPENTITY)> prefabInstantiated =
            [&state](LPENTITY owner, LPENTITY root) { internal::RegisterScriptSpawn(MakeSceneContext(state), owner, root); };
        engine::gameplay::GameSimulationHooks hooks{};
        hooks.instantiatePrefab  = &instantiatePrefab;
        hooks.prefabInstantiated = &prefabInstantiated;
        hooks.materials = &state.materialSystem;
        hooks.jobs = &state.jobSystem;
        WrapperRenderSettingsService renderSettings(state);
        hooks.renderSettings = &renderSettings;
        hooks.destroyEntity = [&state](LPENTITY id)
        {
            internal::ForgetDestroyedEntities(state.sceneManager, *state.world, id);
            internal::DestroyEntityRecursive(*state.world, id);
        };
        hooks.loadScene = [](const engine::script::SceneLoadRequest& request)
        {
            if (!request.path.empty())
            {
                if (request.additive) (void)LoadSceneAdditive(request.path.c_str());
                else                  (void)LoadScene(request.path.c_str());
            }
            else if (request.index > 0)
            {
                if (request.additive) (void)LoadSceneAdditive(request.index);
                else                  (void)LoadScene(request.index);
            }
        };

        // Frame order lives in GameSimulation::RunFrame (shared with editor play).
        engine::gameplay::GameFrameSteps steps{};
        steps.transforms = &state.transformSystem;
        steps.bounds     = &state.boundsSystem;
        steps.animation  = state.animationSystem.get();
        steps.beforeScripts = [&state](float deltaSeconds)
        {
            if (state.tickCallback)
                state.tickCallback(deltaSeconds);
            for (auto& sys : state.userSystems)
                sys(*state.world, deltaSeconds);
        };
        steps.beforeTransforms = [&state]()
        {
            engine::mesh_renderer::UpdateLocalBoundsFromMeshes(*state.world, state.assetRegistry);
        };
        // Frame time follows the display (vsync on the window's monitor).
        if (const engine::platform::IWindow* window = state.renderLoop.GetWindow())
            game.SetDisplay(window->GetRefreshRateHz(), state.renderLoop.GetRenderSystem().IsPresentVsync());
        game.RunFrame(*state.world, unscaledDeltaSeconds, input, hooks, steps);

        // GPU upload of the bone palettes written by the AnimationSystem.
        if (state.skinningSystem)
            state.skinningSystem->Upload(*state.world);

        if (state.editorLiveStateEnabled)
        {
            state.runtimeStateWriteTimer += unscaledDeltaSeconds;
            if (state.runtimeStateWriteTimer >= 0.1f)
            {
                state.runtimeStateWriteTimer = 0.0f;
                WriteRuntimeStateSnapshot(state);
            }
        }

        const engine::renderer::ISwapchain* swapchain = state.renderLoop.GetRenderSystem().GetSwapchain();
        const uint32_t viewportWidth = (swapchain && swapchain->GetWidth() > 0u)
            ? swapchain->GetWidth()
            : state.config.width;
        const uint32_t viewportHeight = (swapchain && swapchain->GetHeight() > 0u)
            ? swapchain->GetHeight()
            : state.config.height;

        engine::renderer::RenderView view{};
        if (!engine::addons::camera::BuildPrimaryRenderView(
                *state.world,
                viewportWidth,
                viewportHeight,
                view,
                state.cameraOptions))
        {
            engine::Debug::LogError("krom.h: failed to build primary render view - create a main camera first");
            state.timing.EndFrame();
            return -3;
        }

        // The prepass only runs when an add-on (GTAO) asks for an opaque input.
        view.enableDepthPrepass = true;

        // Post-process volumes at the camera over the scene and project values.
        internal::ApplyPostProcessVolumes(state.render, *state.world, view.cameraPosition);

        // Text-Overlay im UI-Pass; Atlas/Pipeline vorher ausserhalb des Passes anlegen.
        engine::renderer::FramePipelineCallbacks frameCallbacks;
        if (engine::renderer::IDevice* device = state.renderLoop.GetRenderSystem().GetDevice())
            game.Text().Prepare(*device);
        engine::addons::text::AddTextCallback(frameCallbacks, game.Text());

#ifdef KROM_WRAPPER_HAS_PARTICLES
        // Particle pass only when there is something to draw.
        if (!state.particleLoaderConfigured)
        {
            state.particleView.UseAssetTextures(state.assetPipeline, state.assetRegistry);
            state.particleLoaderConfigured = true;
        }
        if (engine::renderer::IDevice* particleDevice = state.renderLoop.GetRenderSystem().GetDevice())
            state.particleView.Submit(game.Particles(), view, particleDevice, frameCallbacks);
#endif

        // Diagnostics without the editor: KROM_GPU_PASS_TIMING_LOG=1 measures the GPU
        // time per pass and logs the smoothed values about every two seconds.
        {
            static const bool logGpuPasses = [] {
                const char* v = std::getenv("KROM_GPU_PASS_TIMING_LOG");
                return v && v[0] == '1';
            }();
            if (logGpuPasses)
            {
                engine::renderer::RenderSystem& rs = state.renderLoop.GetRenderSystem();
                rs.SetGpuPassTimingEnabled(true);
                static double nextLogSeconds = 0.0;
                const double now = state.timing.GetRawTimestampSeconds();
                if (nextLogSeconds == 0.0)
                    nextLogSeconds = now + 2.0;
                if (now >= nextLogSeconds)
                {
                    nextLogSeconds = now + 2.0;
                    const engine::renderer::GpuPassTimingReport report = rs.GetGpuPassTimings();
                    std::string line;
                    for (const engine::renderer::GpuPassTiming& pass : report.passes)
                    {
                        char entry[160];
                        std::snprintf(entry, sizeof(entry), " %s=%.3f", pass.name.c_str(), pass.ms);
                        line += entry;
                    }
                    engine::Debug::LogWarning("KromWrapper.cpp: GPU pass ms (%s, %ux%u): total=%.3f%s",
                        rs.GetDevice() ? rs.GetDevice()->GetBackendName() : "?",
                        rs.GetSwapchain() ? rs.GetSwapchain()->GetWidth() : 0u,
                        rs.GetSwapchain() ? rs.GetSwapchain()->GetHeight() : 0u,
                        report.totalMs, report.supported ? line.c_str() : " (no GPU timestamps)");
                }
            }
        }

        if (!state.renderLoop.GetRenderSystem().RenderFrame(*state.world, state.materialSystem, view, state.timing, frameCallbacks))
        {
            state.timing.EndFrame();
            if (state.renderLoop.ShouldExit())
                break;
            return -4;
        }

        state.timing.EndFrame();

        static uint32_t s_debugFrameCounter = 0u;
        if (s_debugFrameCounter < 5u)
        {
            const auto& stats = state.renderLoop.GetRenderSystem().GetStats();
            engine::Debug::Log(
                "krom.h: frame=%u proxies=%u visible=%u opaque=%u transparent=%u shadow=%u instancedGroups=%u instancedObjects=%u savedDraws=%u uploadedBytes=%llu",
                s_debugFrameCounter,
                stats.totalProxyCount,
                stats.visibleProxyCount,
                stats.opaqueDraws,
                stats.transparentDraws,
                stats.shadowDraws,
                stats.instancedDraws,
                stats.instancedObjects,
                stats.instancingSavedDraws,
                static_cast<unsigned long long>(stats.uploadedBytes));
            ++s_debugFrameCounter;
        }
    }

    state.running = false;
    return 0;
}

int Run(TickFn fn)
{
    OnUpdate(std::move(fn));
    return Run();
}

void Shutdown()
{
    State& state = Get();
    // Let jobs started by game code finish while everything they may use still exists.
    state.jobSystem.WaitIdle();
    state.running = false;
    state.initialized = false;
    state.featuresRegistered = false;
    state.tickCallback = {};
    state.initCallback = {};
    internal::ClearScenes(state.sceneManager);
    state.prefabCache.clear();
    state.texturePaths.clear();
    state.runtimeMaterialCache.clear();
    state.surfaces.clear();
    state.nextSurfaceId = 1u;
    state.sceneManager.nextSceneId = 1u;
    state.projectRoot.clear();
    state.sceneManager.projectScenePaths.clear();
    if (state.activeEnvironment.IsValid())
    {
        state.renderLoop.GetRenderSystem().DestroyEnvironment(state.activeEnvironment);
        state.activeEnvironment = engine::renderer::EnvironmentHandle::Invalid();
    }
    engine::addons::prefab::SetPrefabAssetPipeline(nullptr);
    state.assetPipeline.reset();
    state.skinningSystem.reset();
    state.animationSystem.reset();
    state.game.Shutdown();
    if (engine::renderer::IDevice* device = state.renderLoop.GetRenderSystem().GetDevice())
    {
        device->WaitIdle();
        state.game.Text().OnDeviceShutdown();
#ifdef KROM_WRAPPER_HAS_PARTICLES
        state.particleView.OnDeviceShutdown();
#endif
    }
    state.textConfigured = false;
    state.world.reset();
    state.renderLoop.Shutdown();
    // Last: the pool stops after everything that used it.
    state.jobSystem.Shutdown();
    state.platform.reset();
}

// =============================================================================
// Scene-Management (KromSceneManager.cpp)
// =============================================================================

LPSCENE LoadScene(const char* path)
{
    State& state = Get();
    if (!EnsureInitialized() || !path)
        return NULL_SCENE;
    return internal::LoadScene(MakeSceneContext(state), path);
}

LPSCENE LoadScene(int sceneIndex)
{
    return internal::LoadScene(Get().sceneManager, sceneIndex);
}

int GetSceneCount()
{
    return internal::GetSceneCount(Get().sceneManager);
}

LPSCENE LoadSceneAdditive(const char* path)
{
    State& state = Get();
    if (!EnsureInitialized() || !path)
        return NULL_SCENE;
    return internal::LoadSceneAdditive(MakeSceneContext(state), path);
}

LPSCENE LoadSceneAdditive(int sceneIndex)
{
    return internal::LoadSceneAdditive(Get().sceneManager, sceneIndex);
}

LPSCENE LoadSceneAsync(const char* path)
{
    State& state = Get();
    if (!EnsureInitialized() || !path)
        return NULL_SCENE;
    return internal::LoadSceneAsync(MakeSceneContext(state), path);
}

bool IsSceneReady(LPSCENE scene)
{
    return internal::IsSceneReady(Get().sceneManager, scene);
}

bool ActivateScene(LPSCENE scene)
{
    return internal::ActivateScene(MakeSceneContext(Get()), scene);
}

void UnloadScene(LPSCENE scene)
{
    internal::UnloadScene(MakeSceneContext(Get()), scene);
}

void UnloadAll()
{
    internal::UnloadAll(MakeSceneContext(Get()));
}

void SetPersistent(LPENTITY entity)
{
    internal::SetPersistent(Get().sceneManager, entity);
}

void SetScenePersistent(LPSCENE scene)
{
    internal::SetScenePersistent(MakeSceneContext(Get()), scene);
}

LPENTITY FindEntity(const char* name)
{
    return internal::FindEntity(Get().world.get(), name);
}

LPENTITY GetMainCamera()
{
    return internal::GetMainCamera(Get().world.get());
}

LPENTITY FindEntityInScene(LPSCENE scene, const char* name)
{
    return internal::FindEntityInScene(MakeSceneContext(Get()), scene, name);
}

} // namespace Krom
