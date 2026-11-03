#pragma once

#include "app/AppScene.hpp"
#include "addons/camera/CameraComponents.hpp"
#include "addons/camera/CameraViewBuilder.hpp"
#include "addons/debug_draw/DebugDraw.hpp"
#include "addons/particles/ParticleRenderer.hpp"
#include "addons/particles/ParticleSystem.hpp"
#include "addons/lighting/LightingComponents.hpp"
#include "addons/mesh_renderer/MeshBounds.hpp"
#include "addons/mesh_renderer/MeshRendererComponents.hpp"
#include "scene/BoundsSystem.hpp"
#include "events/EventBus.hpp"
#include "platform/StdTiming.hpp"
#include "renderer/IDevice.hpp"
#include "platform/IWindow.hpp"
#include <array>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_set>
#include <unordered_map>

#ifdef KROM_EDITOR_HAS_IMGUI
#include "addons/editor/EditorFeature.hpp"
#include "addons/editor/EditorAssetBrowser.hpp"
#include "addons/gameplay/GameSimulation.hpp"
#endif
#include "addons/script/ScriptRegistry.hpp"
#include "app/editor/ScriptModuleHost.hpp"

namespace engine::serialization { struct JsonValue; }

namespace engine::app {

struct KromEditorAppConfig
{
    // -- Window --
    std::string                    windowTitle = "KROM Editor";
    uint32_t                       width       = 1280u;
    uint32_t                       height      = 720u;
    platform::WindowMode           windowMode  = platform::WindowMode::Windowed;
    bool                           visible = true;

    // -- Rendering --
    renderer::DeviceFactory::BackendType backend = renderer::DeviceFactory::BackendType::Vulkan;
    bool                 enableDebugLayer = true;
    std::array<float, 4> clearColor       = { 0.3f, 0.3f, 0.3f, 1.f };
    bool                 enableForwardPlus = false;
    bool                 enableGtao        = false;
    uint32_t             maxShadowLights   = 16u;
    uint32_t             maxShadowViews    = 64u;

    // -- World --
    math::Vec3 ambientColor     { 0.06f, 0.06f, 0.08f };
    float      ambientIntensity = 1.0f;
};

// Laufender Runtime-Build (Play/Export) im Hintergrund-Thread; Definition in KromEditorApp.cpp.
struct RuntimeBuildJob;
struct SceneLoadJob;

class KromEditorApp
{
public:
    KromEditorApp() = default;
    ~KromEditorApp();

    KromEditorApp(const KromEditorApp&) = delete;
    KromEditorApp& operator=(const KromEditorApp&) = delete;

    [[nodiscard]] bool Initialize(const KromEditorAppConfig& config);
    [[nodiscard]] int Run(IAppScene& scene);
    void Shutdown();

private:
    friend struct EditorLoadingSmokeTest;
    bool m_maximizeAfterProjectLoad = false;
    [[nodiscard]] bool InitializePlatform();
    [[nodiscard]] bool InitializeRenderLoop();
    [[nodiscard]] bool InitializeAssetPipeline();
    [[nodiscard]] bool InitializeTonemapMaterial();
    [[nodiscard]] std::filesystem::path GetCurrentAssetRoot() const;
    [[nodiscard]] std::filesystem::path GetCurrentShaderCacheDir() const;
#ifdef KROM_EDITOR_HAS_IMGUI
    [[nodiscard]] std::filesystem::path GetCurrentEditorScenePath() const;
    // Pfad für einen beliebigen Szenennamen (ohne .json-Suffix).
    [[nodiscard]] std::filesystem::path GetEditorScenePath(const std::string& sceneName) const;
    [[nodiscard]] std::filesystem::path GetCurrentProjectFilePath() const;
    [[nodiscard]] bool SaveProjectFile();
    [[nodiscard]] bool LoadProjectFile(const std::string& projectPath);
    [[nodiscard]] bool CreateEditorProject(const std::string& projectName,
                                           const std::string& parentDirectory,
                                           renderer::DeviceFactory::BackendType backend);
    void ApplyEditorProjectPaths();
    void ProcessEditorFileDrops();
    [[nodiscard]] bool SaveEditorScene();
    // clearFirst=true (Default): löscht alle Entities vor dem Laden (normaler Reload).
    // clearFirst=false: behält bestehende Entities (genutzt von SwitchEditorScene).
    // sceneTag: wenn nicht leer, wird dieser Name zum Tagging neuer Entities verwendet
    //           (anstelle von m_currentSceneName) — für additives Laden fremder Szenen.
    [[nodiscard]] bool LoadEditorScene(bool clearFirst = true,
                                       const std::string& sceneTag = "",
                                       const serialization::JsonValue* preparedRoot = nullptr,
                                       bool deferSync = false);
    enum class SceneSyncStep { All, Hierarchy, Meshes, Materials, Prefabs, Finalize };
    void SyncEditorSceneState(SceneSyncStep step = SceneSyncStep::All);
    // Erstellt fehlende Kamera-Gizmo-Entities (camera.glb) für alle CameraComponent-Entities.
    // Wird am Ende von SyncEditorSceneState() aufgerufen — nach jedem Scene-Load.
    void EnsureCameraGizmos();
    // Synchronisiert Position/Rotation aller Gizmo-Entities mit ihrer verknüpften Kamera.
    // Wird jeden Frame in Run() aufgerufen, bevor m_transformSystem.Update() läuft.
    void SyncCameraGizmoTransforms();
    void EnsureMaterialPreviewEntity();
    void SyncMaterialPreviewEntity();
    [[nodiscard]] bool BuildMaterialPreviewRenderView(renderer::RenderView& outView) const noexcept;
    // Prefab-Modus verlassen (speichert das Prefab, stellt die Szene her), bevor
    // eine Szenen-Aktion laeuft; false, wenn das Prefab nicht gespeichert werden konnte.
    bool LeavePrefabMode();
    [[nodiscard]] bool BuildPrefabPreviewRenderView(renderer::RenderView& outView) const noexcept;
    void ProcessAssetThumbnailQueue();
    // Bild eines Panels (Viewport, Game-Reiter): eigenes Ziel in Panelgroesse.
    struct PanelTarget
    {
        RenderTargetHandle rt;
        uint32_t width = 0u;
        uint32_t height = 0u;
        void* textureId = nullptr;
        uint32_t pendingWidth = 0u;
        uint32_t pendingHeight = 0u;
        uint32_t stableFrames = 0u;
    };
    // Ziel an die vom Panel gewuenschte Groesse anpassen (0 = Panel verdeckt: behalten).
    void UpdatePanelTarget(PanelTarget& target, uint32_t width, uint32_t height, const char* debugName);
    void ReleasePanelTarget(PanelTarget& target);
    // Ohne ImGui (Shutdown): nur das Ziel freigeben.
    void DestroyPanelTargetNoImGui(PanelTarget& target);
    [[nodiscard]] bool BuildAssetThumbnailRenderView(renderer::RenderView& outView) const noexcept;
    // Wechselt zu einer anderen Szene (speichert aktuelle, entfernt nicht-persistente
    // Entities wenn !additive, lädt neue Szene).
    // additive=true: primäre Szene bleibt, neue Szene wird additiv geladen.
    bool SwitchEditorScene(const std::string& sceneName, bool additive, bool prepareOnly = false);
    void ProcessPendingSceneSwitch();
    void ShowSceneLoadProgress(float progress, const char* phase);
    std::string m_pendingSceneSwitch;
    bool m_pendingSceneAdditive = false;
    std::shared_ptr<SceneLoadJob> m_sceneLoadJob;
    // Entlädt alle Entities einer additiv geladenen Szene (via EditorSceneTagComponent).
    bool UnloadEditorScene(const std::string& sceneName);
    // Erstellt eine neue leere Szene und wechselt zu ihr.
    bool NewEditorScene(const std::string& sceneName);
    bool NewEditorSceneInDir(const std::string& sceneName, const std::string& targetDir);
    // Benennt eine Szene um: Datei + m_currentSceneName + m_sceneOrder + Tags auf Entities.
    [[nodiscard]] bool RenameEditorScene(const std::string& oldName, const std::string& newName);
    // Löscht eine Szene: Datei + alle internen Referenzen.
    [[nodiscard]] bool DeleteEditorScene(const std::string& sceneName);
    // Exportiert die aktuelle ECS-World als .kscene (schneller Pfad für die aktuelle Szene).
    [[nodiscard]] bool ExportAsKscene(const std::filesystem::path& outPath);
    // Liest eine editor-.json-Datei, instanziiert in einer temporären World und exportiert .kscene.
    // Berührt weder m_world noch Asset-Pipeline — safe für Batch-Build.
    [[nodiscard]] bool ExportSceneFileAsKscene(const std::filesystem::path& editorJsonPath,
                                               const std::filesystem::path& kscenePath,
                                               const std::string& sceneName);
    // Exportiert alle nicht-ausgeschlossenen Szenen des Projekts als .kscene (der eigentliche Build).
    [[nodiscard]] bool BuildAllScenes();
    [[nodiscard]] bool ExportProjectBuild();
    // Runtime-Build fuer Play/Export: laeuft im Hintergrund, der Editor bleibt bedienbar.
    // onFinished wird im Main-Thread (PollRuntimeBuild) mit dem Ergebnis aufgerufen.
    [[nodiscard]] bool StartRuntimeBuild(const std::filesystem::path& buildDir,
                                         std::function<void(bool ok)> onFinished);
    // Pro Frame: Build-Ausgabe in die Editor-Konsole, Status fuer die UI, Abschluss.
    void PollRuntimeBuild();
    [[nodiscard]] bool LaunchPlayRuntime(const std::filesystem::path& buildDir);
    [[nodiscard]] bool FinishProjectExport(const std::filesystem::path& buildDir);
    void RefreshBuildExcludeState();
    // Listet alle .json-Dateien in Assets/Scenes/ (ohne Suffix).
    [[nodiscard]] std::vector<std::string> ListEditorScenes() const;
    // Aktualisiert Scene-Management-Felder im EditorFrameCtx (verfügbare + geladene Szenen).
    void RefreshSceneList();
    // Synchronisiert m_loadedSceneNames → EditorFrameContext::loadedEditorScenes.
    void RefreshLoadedScenesState();
    // Synchronisiert m_sceneOrder → EditorFrameContext::sceneOrder.
    void RefreshSceneOrderState();
    // Setzt neue Szenenreihenfolge und speichert sofort in krom-project.json.
    void SetSceneOrder(const std::vector<std::string>& order);
    // Erstellt Standard-Entities (Kamera + Directional Light) in der leeren Welt.
    void CreateDefaultSceneEntities();

    // Play im Editor: dieselbe Spielschleife wie die Runtime (GameSimulation) auf
    // der Editor-Welt. Start legt Szene und Rueckgaengig-Liste beiseite, Stop
    // stellt beides wieder her.
    bool StartEditorPlay();
    bool BeginEditorPlay();
    bool ReloadProjectScriptModule(bool reportMissing = false);
    bool CheckScriptEngineLibraries(std::string& error) const;
    void ReportScriptBuildError(const std::string& error);
    void StopEditorPlay();
    void SetEditorPlayPaused(bool paused);
    // Pro Frame: Scripts/Physik vor dem TransformSystem, Audio/Partikel danach.
    void TickEditorPlayScripts(float unscaledDeltaSeconds);
    void TickEditorPlayLate();
    [[nodiscard]] engine::gameplay::GameSimulationHooks MakeEditorPlayHooks();
    EntityID InstantiatePrefabForPlay(std::string_view prefabAssetPath, const math::Vec3& position,
                                      const math::Quat& rotation, const math::Vec3& scale);
    void LoadSceneForPlay(const engine::script::SceneLoadRequest& request);
#endif
    KromEditorAppConfig m_config{};
    renderer::DeviceFactory::Registry m_deviceFactoryRegistry;
    events::EventBus m_eventBus;
    std::unique_ptr<platform::IPlatform> m_platform;
    renderer::PlatformRenderLoop m_renderLoop;
    assets::AssetRegistry m_assetRegistry;
    std::unique_ptr<assets::AssetPipeline> m_assetPipeline;
    renderer::MaterialSystem m_materialSystem;
    ecs::ComponentMetaRegistry m_componentRegistry;
    // Muss laenger leben als World und Registry; Shutdown loest beide vorher ab.
    ScriptModuleHost m_scriptModule;
    std::unique_ptr<ecs::World> m_world;
    TransformSystem m_transformSystem;
    BoundsSystem    m_boundsSystem;
    engine::addons::debug_draw::DebugDrawRenderer m_debugDraw;
    // Partikel-Vorschau im Viewport (laeuft auch ohne Play-Modus).
    engine::particles::ParticleSystem   m_particles;
    engine::particles::ParticleRenderer m_particleRenderer;
    std::vector<engine::particles::ParticleBillboard> m_particleAlpha;
    std::vector<engine::particles::ParticleBillboard> m_particleAdditive;
    // Partikel im Game-Reiter (eigene Kamera; Viewport und Game koennen gleichzeitig sichtbar sein).
    engine::particles::ParticleRenderer m_gameParticleRenderer;
    std::vector<engine::particles::ParticleBillboard> m_gameParticleAlpha;
    std::vector<engine::particles::ParticleBillboard> m_gameParticleAdditive;
    std::unordered_set<EntityID> m_particleSelection;   // Auswahl samt Kindern
    platform::StdTiming m_timing;
    engine::addons::camera::CameraBuildOptions m_cameraOptions{};
    engine::script::ScriptRegistry m_scriptRegistry;
    bool m_engineSchemaRegistered = false;
    bool m_renderFeaturesRegistered = false;
    bool m_forwardPlusActive = false;
    bool m_initialized = false;
    std::filesystem::path m_projectRoot;
    std::string           m_projectName;
    std::string           m_projectRuntimeTarget;
    renderer::DeviceFactory::BackendType m_projectBackend = renderer::DeviceFactory::BackendType::Vulkan;
    // Aktiver Szenenname (Dateiname ohne .json). Default: "editor_scene" für
    // Rückwärtskompatibilität mit bestehenden Projekten.
    std::string           m_currentSceneName = "editor_scene";
    // Alle aktuell geladenen Szenen: [0] = primäre, weitere = additiv geladen.
    // Wird mit EditorFrameContext::loadedEditorScenes synchronisiert.
    std::vector<std::string>         m_loadedSceneNames;
    // Szenenreihenfolge für Runtime-Indizierung: Index 0 = LoadScene(1).
    // Persistiert in krom-project.json als "sceneOrder":[...].
    std::vector<std::string>         m_sceneOrder;
    // Szenen die beim Build übersprungen werden; persistiert in krom-project.json.
    std::unordered_set<std::string>  m_buildExcludedScenes;
    // shared_ptr: der Worker-Thread haelt den Job, falls der Editor waehrend
    // eines Builds beendet wird (Thread wird dann abgekoppelt).
    std::shared_ptr<RuntimeBuildJob> m_runtimeBuildJob;

#ifdef KROM_EDITOR_HAS_IMGUI
    renderer::addons::editor::EditorState       m_editorState;
    renderer::addons::editor::AssetBrowserState m_assetBrowserState;
    std::unique_ptr<renderer::addons::editor::EditorFrameContext> m_editorFrameCtx;
    std::shared_ptr<renderer::addons::gtao::GtaoGpuResources>    m_gtaoResources;
    RenderTargetHandle                          m_previewRT;
    RenderTargetHandle                          m_materialPreviewRT;
    RenderTargetHandle                          m_prefabPreviewRT;
    renderer::EnvironmentHandle                 m_editorEnvironmentHandle = renderer::EnvironmentHandle::Invalid();
    // Gecachter Mesh-Handle für das Kamera-Gizmo — wird beim ersten EnsureCameraGizmos()
    // gesetzt und danach wiederverwendet (kein erneuter ImportBundle-Aufruf).
    MeshHandle                                  m_cameraGizmoMeshHandle;
    MeshHandle                                  m_materialPreviewSphereMeshHandle;
    EntityID                                    m_materialPreviewEntity = NULL_ENTITY;
    std::vector<EntityID>                       m_assetThumbnailEntities;
    RenderTargetHandle                          m_activeAssetThumbnailRT = RenderTargetHandle::Invalid();
    std::filesystem::path                       m_activeAssetThumbnailPath;
    math::Vec3                                  m_activeAssetThumbnailCenter{0.f, 0.f, 0.f};
    float                                       m_activeAssetThumbnailRadius = 1.f;
    void*                                       m_previewTextureId = nullptr;
    void*                                       m_materialPreviewTextureId = nullptr;
    void*                                       m_prefabPreviewTextureId   = nullptr;
    // Vorschaubilder, die gerade gerendert wurden: im naechsten Frame
    // zuruecklesen und in .krom/thumbnails speichern (Schluessel wie im Browser).
    std::vector<std::string>                    m_pendingThumbnailSaves;
    uint32_t                                    m_previewSampler = UINT32_MAX;
    std::unordered_map<uint32_t, void*>         m_editorTextureIdCache;
    // Viewport-Panel (Editor-Kamera) und Game-Reiter (Spielkamera).
    PanelTarget                                 m_sceneView;
    PanelTarget                                 m_gameView;
    engine::gameplay::GameSimulation            m_game;
    engine::script::PrefabInstantiateFn         m_playInstantiatePrefab;
    bool                                        m_playActive = false;
    bool                                        m_playPaused = false;
#endif
};

[[nodiscard]] renderer::DeviceFactory::BackendType SelectAppBackend();
[[nodiscard]] const char* BackendDisplayName(renderer::DeviceFactory::BackendType backend) noexcept;

} // namespace engine::app
