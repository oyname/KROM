#pragma once
#include "krom.h"
#include "KromRenderSettings.hpp"
#include "KromRuntimeMaterials.hpp"

#include "addons/camera/CameraViewBuilder.hpp"
#include "addons/forward/ForwardColorGrading.hpp"
#include "addons/forward/ForwardTonemap.hpp"
#include "addons/gameplay/GameSimulation.hpp"
#include "addons/script/ScriptRegistry.hpp"
#include "assets/AssetPipeline.hpp"
#include "assets/AssetRegistry.hpp"
#include "core/ProjectSettings.hpp"
#include "ecs/World.hpp"
#include "events/EventBus.hpp"
#include "renderer/RenderSystem.hpp"
#include "scene/BoundsSystem.hpp"
#include "scene/TransformSystem.hpp"

#include <array>
#include <cstdint>
#include <filesystem>
#include <future>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

// Scene management of the krom.h wrapper (LoadScene, LoadSceneAdditive,
// LoadSceneAsync, UnloadScene, SetPersistent, ...): the .kscene parser, putting
// a parsed scene into the world (entities, asset bindings, environment) and the
// lifetime of scenes and persistent entities.
// The module owns SceneManagerState; everything shared with the rest of the
// wrapper comes in per call as SceneContext.
namespace Krom::internal {

// =============================================================================
// Scene-Datenstrukturen
// =============================================================================

struct KromSceneData
{
    std::string name;
    bool persistent = false;
    int  version = 1;
    std::string sourceJson;
    std::string environmentTexturePath;
    bool hasEnvironment = false;
    bool environmentEnableIBL = false;
    bool environmentRemoveSunFromLighting = true;
    float environmentIntensity = 1.0f;
    std::array<float, 4> backgroundColor{0.f, 0.f, 0.f, 1.f};
    bool hasBackgroundColor = false;
    engine::math::Vec3 ambientColor{0.06f, 0.06f, 0.08f};
    bool hasAmbientColor = false;
    float ambientIntensity = 1.0f;
    bool hasAmbientIntensity = false;
    float exposureEv = 0.0f;
    engine::renderer::addons::forward::ExposureMode exposureMode = engine::renderer::addons::forward::ExposureMode::Fixed;
    engine::renderer::addons::forward::AutoExposureSettings autoExposure{};
    engine::renderer::addons::forward::ColorGradingSettings grading{};   // neutral when missing
    // Render options of scenes written before they moved to the project settings;
    // used only when the project file has no "rendering" block.
    engine::ProjectRenderSettings legacyRendering = engine::ReadLegacySceneRenderSettings({});
    uint32_t probeBakePasses = 2u;    // probe add-on
    bool valid = false;
};

struct SceneRecord
{
    std::string name;
    std::string path;
    bool persistent = false;
    bool active = false;
    // Root entities in load/spawn order (FindEntityInScene returns the first match in
    // this order). Changed only through the SceneManagerState helpers below, which
    // keep SceneManagerState::rootScenes in sync.
    std::vector<LPENTITY> rootEntities;

    // Async-Support
    std::shared_future<KromSceneData> asyncFuture;
    KromSceneData stagedData;
    bool staging = false;  // Hintergrund-Load läuft
    bool staged  = false;  // Parsing fertig, Entity-Erstellung aussteht
};

// Owned by the wrapper State (member sceneManager).
struct SceneManagerState
{
    std::unordered_map<uint32_t, SceneRecord> scenes;
    // EntityID::value of every entry of every SceneRecord::rootEntities -> scene id.
    std::unordered_map<uint32_t, uint32_t>    rootScenes;
    std::unordered_set<uint32_t>              persistentEntityIds;
    uint32_t                                  nextSceneId = 1u;
    // Scene order of the project (LoadScene(int), GetSceneCount); written by LoadProject.
    std::vector<std::string>                  projectScenePaths;
};

// References into the wrapper State; built per call, never stored.
struct SceneContext
{
    SceneManagerState& manager;
    engine::ecs::World* world = nullptr;                      // null before Graphics()
    engine::assets::AssetPipeline* assetPipeline = nullptr;   // null before Graphics()
    engine::assets::AssetRegistry& assetRegistry;
    engine::TransformSystem& transformSystem;
    engine::BoundsSystem& boundsSystem;
    engine::events::EventBus& eventBus;                       // SceneLoadedEvent / SceneUnloadedEvent
    engine::script::ScriptRegistry& scriptAddonRegistry;
    engine::gameplay::GameSimulation& game;                   // reset on exclusive scene changes
    const std::filesystem::path& assetRoot;
    const engine::ProjectSettings& projectSettings;
    bool editorLiveStateEnabled = false;
    RenderSettingsState& render;
    engine::addons::camera::CameraBuildOptions& cameraOptions;     // ambient of the scene
    engine::renderer::EnvironmentHandle& activeEnvironment;        // environment/IBL of the scene
    engine::renderer::RenderSystem& renderSystem;
    RuntimeMaterialContext materials;
};

// Script spawns belong to the scene of the calling object (found through its
// parent chain); spawns of a persistent object become persistent too.
void RegisterScriptSpawn(const SceneContext& ctx, LPENTITY owner, LPENTITY root);

// Call before destroying subtreeRoot with its children (ChildrenComponent): every
// entity of the subtree leaves the scene roots and the persistent ids, so a stale
// id cannot match a recycled entity later (EntityID has 8 generation bits).
// Entities destroyed behind the wrapper's back (World::DestroyEntity) stay listed.
void ForgetDestroyedEntities(SceneManagerState& manager, const engine::ecs::World& world, LPENTITY subtreeRoot);

// Forgets all scenes, scene roots and persistent ids (Shutdown).
void ClearScenes(SceneManagerState& manager);

// Diagnostics of the wrapper State for wrapper-internal tests (defined in
// KromWrapper.cpp); not part of krom.h. Unknown scene: 0.
[[nodiscard]] size_t GetWrapperSceneRootCount(LPSCENE scene);
[[nodiscard]] size_t GetWrapperPersistentEntityCount();

// --- krom.h functions --------------------------------------------------------

// The wrapper checks EnsureInitialized() and path before calling these.
[[nodiscard]] LPSCENE LoadScene(const SceneContext& ctx, const char* path);
[[nodiscard]] LPSCENE LoadSceneAdditive(const SceneContext& ctx, const char* path);
[[nodiscard]] LPSCENE LoadSceneAsync(const SceneContext& ctx, const char* path);
// Project scene order, 1-based; call Krom::LoadScene / Krom::LoadSceneAdditive with the path.
[[nodiscard]] LPSCENE LoadScene(const SceneManagerState& manager, int sceneIndex);
[[nodiscard]] LPSCENE LoadSceneAdditive(const SceneManagerState& manager, int sceneIndex);
[[nodiscard]] int GetSceneCount(const SceneManagerState& manager);

[[nodiscard]] bool IsSceneReady(SceneManagerState& manager, LPSCENE scene);
[[nodiscard]] bool ActivateScene(const SceneContext& ctx, LPSCENE scene);
void UnloadScene(const SceneContext& ctx, LPSCENE scene);
void UnloadAll(const SceneContext& ctx);
void SetPersistent(SceneManagerState& manager, LPENTITY entity);
void SetScenePersistent(const SceneContext& ctx, LPSCENE scene);
[[nodiscard]] LPENTITY FindEntityInScene(const SceneContext& ctx, LPSCENE scene, const char* name);

} // namespace Krom::internal
