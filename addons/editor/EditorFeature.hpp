#pragma once

#include "renderer/RenderFeatureInterfaces.hpp"
#include "renderer/RenderFrameTypes.hpp"
#include "renderer/IDevice.hpp"
#include "renderer/Environment.hpp"
#include "ecs/World.hpp"
#include "renderer/MaterialSystem.hpp"
#include "assets/AssetRegistry.hpp"
#include "core/Types.hpp"
#include "core/Math.hpp"
#include "jobs/JobSystem.hpp"
#include "addons/script/ScriptRegistry.hpp"
#include "addons/gtao/GtaoPass.hpp"
#include <array>
#include <filesystem>
#include <functional>
#include <future>
#include <string>
#include <unordered_map>
#include <vector>

namespace engine::renderer { class ICommandList; }
namespace engine::assets { class AssetPipeline; }
namespace engine::addons::debug_draw { class DebugDrawRenderer; }

namespace engine::renderer::addons::editor {

struct AssetBrowserState;

struct EditorCameraState
{
    math::Vec3 position  = {0.f, 2.f, 10.f};
    float      pitchDeg  = -10.f;
    float      yawDeg    = 0.f;
    float      moveSpeed = 50.f;
    float      fovDeg    = 60.f;
    float      nearPlane = 0.05f;
    float      farPlane  = 2000.f;
};

enum class EditorTransformSpace : uint8_t
{
    Local = 0,
    World
};

enum class GizmoMode : uint8_t { Position = 0, Rotation, Scale };

// Gibt an, wie das aktuell selektierte Objekt ausgewählt wurde.
// Viewport = nur die angeklickte Entity transformieren (Kinder bleiben in World-Space).
// Outliner = Parent + alle Kinder transformieren (bisheriges Verhalten).
enum class SelectionSource : uint8_t { Outliner = 0, Viewport };

enum class EditorMaterialTemplate : uint8_t
{
    PbrLit    = 0,
    Unlit     = 1,
    LegacyLit = 2,
    Custom    = 3   // Eigener VS+FS — Shader-Pfade in MaterialAsset::vertexShaderPath/fragmentShaderPath
};

struct EditorAsyncMaterialLoadResult
{
    std::string           normalizedPath;
    assets::MaterialAsset asset;
};

// Snapshot eines direkten Kinder-Entities beim Gizmo-Drag-Start.
// Wird bei Viewport-Selektion genutzt um Kinder in World-Space zu fixieren.
struct GizmoChildData
{
    EntityID   entity       = NULL_ENTITY;
    math::Vec3 worldPos     = {0.f, 0.f, 0.f};
    math::Quat worldRot     = {0.f, 0.f, 0.f, 1.f};
    bool       inheritScale = false;
};

struct EditorState
{
    EntityID               selectedEntity              = NULL_ENTITY;
    std::string            selectedMaterialAssetPath;
    std::string            selectedPrefabAssetPath;
    std::string            selectedMaterialAssetNameDraft;
    std::string            pendingOpenMaterialAssetPath;
    std::string            queuedMaterialAssetPath;
    std::future<jobs::ValueResult<EditorAsyncMaterialLoadResult>> materialAssetLoadFuture;
    bool                   materialAssetLoadInFlight   = false;
    assets::MaterialAsset  selectedMaterialAssetData;
    bool                   selectedMaterialAssetLoaded = false;
    bool                   selectedMaterialAssetDirty  = false;
    EntityID               inspectorMaterialEntity     = NULL_ENTITY;
    int                    inspectorMaterialSlot       = -1;
    std::string            inspectorMaterialAssetPath;
    bool                   materialWindowOpen         = false;
    bool                   materialWindowFocusRequest = false;
    float                  materialPreviewPitchDeg    = 0.f;
    float                  materialPreviewYawDeg      = -25.f;
    float                  materialPreviewDistance    = 4.05f;
    bool                   cameraPreviewWindowOpen    = false;
    EntityID               cameraPreviewLastAutoOpenSelection = NULL_ENTITY;
    AssetBrowserState*     assetBrowser                = nullptr;

    // ── Prefab-Editor ─────────────────────────────────────────────────────────
    bool        prefabEditorOpen           = false;
    std::string prefabEditorAssetPath;     // absoluter Pfad zur offenen .prefab-Datei
    bool        prefabEditorDirty         = false;  // ungespeicherte Aenderungen
    // Orbit-Kamera fuer Prefab-Vorschau
    float       prefabPreviewPitchDeg     = 15.f;
    float       prefabPreviewYawDeg       = -30.f;
    float       prefabPreviewDistance     = 5.f;
    // Root-Entity der Preview-Instanz im Welt-Raum (getaggt mit PrefabEditorPreviewTag)
    EntityID    prefabPreviewRootEntity   = NULL_ENTITY;
    // Berechnetes Zentrum der Prefab-Bounding-Box (Kamera-LookAt-Ziel)
    math::Vec3  prefabPreviewCenter       = {0.f, 0.f, 0.f};
    // Berechneter Radius der Bounding-Sphere (fuer automatische Kamera-Distanz)
    float       prefabPreviewBoundsRadius = 1.f;
    std::string            activeTextureParamName      = "albedo";
    std::string            environmentTexturePath;
    float                  environmentIntensity        = 0.2f;
    bool                   environmentEnableIBL        = true;   // IBL-Beleuchtung (Irradiance/Prefilter)
    bool                   showSkyboxBackground        = true;   // Skybox-Cubemap als sichtbarer Hintergrund
    std::array<float, 4>   backgroundColor             = {0.05f, 0.05f, 0.05f, 1.0f};
    math::Vec3             ambientColor                = {0.06f, 0.06f, 0.08f};
    math::Vec3             ambientColorGround          = {0.01f, 0.01f, 0.02f};
    float                  ambientIntensity            = 1.0f;

    enum class AmbientSource : uint8_t { Skybox = 0, Gradient = 1, Color = 2 };
    AmbientSource          ambientSource               = AmbientSource::Skybox;
    renderer::addons::gtao::GtaoSettings gtaoSettings;
    uint32_t               editorDebugFlags            = 0u;
    EditorTransformSpace   transformSpace              = EditorTransformSpace::Local;
    EntityID               rotationEditEntity          = NULL_ENTITY;
    EditorTransformSpace   rotationEditSpace           = EditorTransformSpace::Local;
    math::Vec3             rotationEditEuler           = {0.f, 0.f, 0.f};
    math::Quat             rotationEditQuat            = {0.f, 0.f, 0.f, 1.f};
    bool                   rotationEditActive          = false;
    bool                   rotationEditInitialized     = false;
    EditorMaterialTemplate newMaterialTemplate         = EditorMaterialTemplate::PbrLit;
    EditorCameraState      editorCamera;
    bool                   closeConfirmOpen            = false;
    bool                   deleteConfirmOpen           = false;
    bool                   createProjectDialogOpen     = false;
    bool                   openProjectDialogOpen       = false;
    bool                   settingsWindowOpen          = false;
    bool                   cameraSettingsOpen          = false;
    std::array<char, 512>  codeEditorExecutableBuffer  = {};
    std::array<char, 512>  codeEditorArgumentsBuffer   = {"\"{file}\""};

    std::unordered_map<std::string, engine::script::ScriptFieldValue> runtimeScriptFieldValues;
    std::string            runtimeStateFilePath;
    std::string            runtimeStateLastError;
    double                 runtimeStatePollTimer       = 0.0;
    std::filesystem::file_time_type runtimeStateLastWriteTime{};

    // Console-Log-Puffer (Ring-Buffer, max 512 Einträge)
    struct LogEntry
    {
        engine::LogLevel level;
        std::string      message;
    };
    std::vector<LogEntry> consoleLogs;
    bool                  consoleScrollToBottom = true;
    bool                  consoleAutoScroll     = true;
    bool                   newSceneDialogOpen          = false;
    std::array<char, 64>   newSceneNameBuffer          = {};
    std::string            newSceneTargetDir           = {}; // leer = Standard Assets/Scenes/
    int                    projectBackendSelection     = 0;
    std::array<char, 128>  projectNameBuffer           = {"MyProject"};
    std::array<char, 512>  projectParentDirBuffer      = {};
    std::array<char, 512>  projectOpenPathBuffer       = {};

    // OBB-Handle-Drag & Hover (beide Phasen: DrawLightDebugLines + DrawEditorPanels)
    int          obbHoveredFace     = -1;   // vom letzten ImGui-Frame; DrawOBBDebugLines liest das
    int          obbDragFaceIdx     = -1;   // -1 = kein aktiver Drag
    float        obbDragDepth       = 1.f;  // Welt-Tiefe des Face-Centers beim Drag-Start
    math::Vec2   obbDragStartMouse  = {};   // Mausposition beim Drag-Start (Screen-Pixel)
    float        obbDragStartExtent = 0.f;  // halfExtents[axis] beim Drag-Start
    float        obbDragStartCenter = 0.f;  // centerOffset[axis] beim Drag-Start

    // Snap-Achsen-Erkennung
    math::Vec3   snapLastPos        = {};
    int          snapAxis           = -1;   // -1=alle, 0=X, 1=Y, 2=Z

    // Transform-Gizmo (3D-Pfeile zum Verschieben)
    int          gizmoHoveredAxis    = -1;  // -1=keiner, 0=X, 1=Y, 2=Z
    int          gizmoDragAxis       = -1;
    math::Vec3   gizmoDragStartPos   = {};
    math::Vec2   gizmoDragStartMouse = {};
    float        gizmoDragDepth      = 1.f;
    // Screen-Space-Achsrichtung eingefroren beim Drag-Start (verhindert Perspektiv-Jitter)
    float        gizmoDragSnx        = 1.f;
    float        gizmoDragSny        = 0.f;
    // Nach G-Snap: Bewegung gesperrt bis Maus losgelassen
    bool         snapMoveLocked      = false;

    // Gizmo-Modus: Position (Pfeile) oder Rotation (Ringe)
    GizmoMode    gizmoMode           = GizmoMode::Position;

    // Woher kam die letzte Selektion (Viewport-Klick oder Outliner-Klick).
    SelectionSource selectionSource = SelectionSource::Outliner;

    // Snapshot der direkten Kinder beim Drag-Start (nur bei Viewport-Selektion).
    // Wird genutzt um Kinder während des Parent-Transforms in World-Space zu fixieren.
    std::vector<GizmoChildData> gizmoDragChildren;
    // World-Transform des Parents beim Drag-Start (für Kompensations-Mathematik)
    math::Vec3  gizmoDragParentWorldPos   = {};
    math::Quat  gizmoDragParentWorldRot   = {0.f, 0.f, 0.f, 1.f};
    math::Vec3  gizmoDragParentWorldScale = {1.f, 1.f, 1.f};

    // Multi-Selektion (STRG+Klick)
    // Entities bleiben unter ihren Original-Parents — kein Reparenting.
    // Der Gizmo arbeitet am virtuellen Pivot (Mittelpunkt aller ausgewählten Entities).
    std::vector<EntityID>  multiSelection;
    math::Vec3             multiSelectionPivot = {0.f, 0.f, 0.f};
    // Weltpositionen + Rotationen aller Entities beim Gizmo-Drag-Start (Index = multiSelection-Index)
    std::vector<math::Vec3> multiSelectionDragStartPositions;
    std::vector<math::Quat> multiSelectionDragStartRotations;
    std::vector<math::Vec3> multiSelectionDragStartScales;

    // Rotations-Gizmo-Drag
    int          rotGizmoHoveredAxis    = -1;
    int          rotGizmoDragAxis       = -1;  // 0/1/2 = Ring, 3 = Trackball (Sphere)
    math::Quat   rotGizmoDragStartQuat  = {0.f, 0.f, 0.f, 1.f};
    math::Vec3   rotGizmoDragWorldAxis  = {0.f, 1.f, 0.f};
    float        rotGizmoDragStartAngle = 0.f;
    math::Vec2   rotGizmoDragScreenCenter = {};
    // Trackball (Sphere): Kamera-Achsen beim Drag-Start eingefroren
    math::Vec3   rotGizmoDragCamRight   = {1.f, 0.f, 0.f};
    math::Vec3   rotGizmoDragCamUp      = {0.f, 1.f, 0.f};
    math::Vec2   rotGizmoDragStartMouse = {};

    // Scale-Gizmo-Drag
    int          sclGizmoHoveredAxis    = -1;
    int          sclGizmoDragAxis       = -1;
    math::Vec3   sclGizmoDragStartScale = {1.f, 1.f, 1.f};
    math::Vec2   sclGizmoDragStartMouse = {};
    float        sclGizmoDragSnx        = 1.f;
    float        sclGizmoDragSny        = 0.f;

    // Bekannte Tags — wird durch Eingabe erweitert, Eintraege koennen geloescht werden
    std::vector<std::string> knownTags = { "Untagged", "Player", "Enemy", "Ground", "Trigger" };

    // Editierbare Layer-Namen (Index 0..31)
    // Index 0 und 1 sind reserviert (Default, EditorGizmo), ab 2 frei benennbar.
    std::array<std::string, 32> layerNames = {{
        "Default", "EditorGizmo", "Transparent", "UI",
        "User0", "User1", "User2", "User3",
        "","","","","","","","","","","","","","","","","","","","","","","",""
    }};
    bool layerNamesWindowOpen = false;
};

struct EditorFrameContext
{
    ecs::World&            world;
    MaterialSystem&        materials;
    assets::AssetRegistry& registry;
    EditorState&           state;
    jobs::JobSystem*       jobSystem = nullptr;
    assets::AssetPipeline* assetPipeline = nullptr;
    assets::ShaderTargetProfile shaderTarget = assets::ShaderTargetProfile::Generic;
    float                  deltaSeconds  = 0.f;
    std::function<void*(EntityID)> cameraPreviewTexture;
    std::function<void*()> materialPreviewTexture;
    std::function<void*(TextureHandle)> editorTextureId;
    RenderTargetHandle previewRT;
    uint32_t           previewWidth  = 320u;
    uint32_t           previewHeight = 180u;
    bool               previewFlipY  = false;
    RenderTargetHandle materialPreviewRT;
    uint32_t           materialPreviewWidth  = 256u;
    uint32_t           materialPreviewHeight = 256u;
    bool               materialPreviewFlipY  = false;
    std::function<void*()> prefabPreviewTexture; // ImGui-Texture-ID fuer Prefab-Vorschau
    RenderTargetHandle prefabPreviewRT;
    uint32_t           prefabPreviewWidth  = 512u;
    uint32_t           prefabPreviewHeight = 512u;
    bool               prefabPreviewFlipY  = false;
    engine::addons::debug_draw::DebugDrawRenderer* debugDraw = nullptr;
    std::function<void()> requestClose;
    std::function<bool()> saveScene;
    std::function<bool()> loadScene;
    std::function<bool()> saveProject;
    std::function<bool(const std::string&)> loadProject;
    std::function<bool(const std::string&, const std::string&, renderer::DeviceFactory::BackendType)> createProject;
    renderer::DeviceFactory::BackendType projectBackend = renderer::DeviceFactory::BackendType::Vulkan;
    std::function<void(renderer::DeviceFactory::BackendType)> setProjectBackend;
    std::function<void()> syncSceneState;
    std::function<bool(const std::string&, float, bool)> setEditorEnvironment;
    std::function<void()> clearEditorEnvironment;
    std::function<void(const math::Vec3&, float)> setEditorAmbientLight;
    std::function<void(const std::array<float, 4>&)> setEditorBackgroundColor;
    std::function<void(const renderer::addons::gtao::GtaoSettings&)> setGtaoSettings;
    std::function<void(uint32_t)> setEditorDebugFlags;
    // Öffnet einen nativen OS-Datei-Dialog für krom-project.json.
    // Gibt den ausgewählten Pfad zurück, oder "" wenn abgebrochen / nicht verfügbar.
    std::function<std::string()> browseForProjectFile;
    // Öffnet einen nativen OS-Ordner-Dialog für den Projektordner.
    std::function<std::string()> browseForProjectFolder;
    std::string           lastFileMessage;
    std::string           currentProjectName;
    // Pfad zum engine-seitigen editor/-Ordner (Icons, Logo usw.)
    // z.B. "F:/working/krom codex/editor"
    std::string           engineEditorDir;
    std::string           currentProjectRoot;
    std::string           engineAssetRoot;

    // Script-Registry fuer Serialisierung/Deserialisierung; null = kein Script-System aktiv.
    engine::script::ScriptRegistry* scriptRegistry = nullptr;

    // ── Scene-Management ─────────────────────────────────────────────────────
    // Name der primär aktiven Szene (ohne .json-Suffix), z.B. "Level1".
    std::string              currentEditorSceneName;
    // Alle .json-Dateien in Assets/Scenes/ (ohne Suffix) — aktualisiert bei
    // Projekt-Load / Szenenwechsel.
    std::vector<std::string> availableEditorScenes;
    // Alle aktuell geladenen Szenen: Index 0 = primäre, weitere = additiv geladen.
    std::vector<std::string> loadedEditorScenes;
    // Wechselt zu einer anderen Szene. additive=false → nicht-persistente
    // Entities werden zuerst entfernt, neue Szene wird primäre.
    // additive=true → bestehende Entities bleiben, neue Szene wird zusätzlich geladen.
    std::function<bool(const std::string& sceneName, bool additive)> switchEditorScene;
    // Erstellt eine neue leere Szene mit dem angegebenen Namen im Standard-Szenenordner.
    std::function<bool(const std::string& sceneName)>                newEditorScene;
    // Erstellt eine neue leere Szene in einem beliebigen Ordner (absoluter Pfad).
    std::function<bool(const std::string& sceneName, const std::string& targetDir)> newEditorSceneInDir;
    // Entlädt alle Entities einer additiv geladenen Szene (via EditorSceneTagComponent).
    std::function<bool(const std::string& sceneName)>                unloadEditorScene;
    // Benennt eine Szene um (Datei + alle internen Referenzen).
    std::function<bool(const std::string& oldName, const std::string& newName)> renameEditorScene;
    // Löscht eine Szene (Datei + alle internen Referenzen).
    // Die aktive primäre Szene kann nur gelöscht werden wenn es mindestens eine weitere gibt.
    std::function<bool(const std::string& sceneName)>              deleteEditorScene;
    // Löscht eine Szenen-Datei direkt über absoluten Pfad (auch außerhalb von Assets/Scenes/).
    std::function<bool(const std::string& absolutePath)>           deleteEditorSceneByPath;
    // ── Szenenreihenfolge ─────────────────────────────────────────────────────
    // Index 0 = Startszene = LoadScene(1) im Spiel.
    // Persistiert in krom-project.json als "sceneOrder":[...].
    std::vector<std::string> sceneOrder;
    // Setzt neue Reihenfolge und persistiert sie sofort.
    std::function<void(const std::vector<std::string>&)> setSceneOrder;
    // ── Build ─────────────────────────────────────────────────────────────────
    // Szenen die beim Build übersprungen werden (z.B. Test-Level, DevScenes).
    // Synchronisiert aus m_buildExcludedScenes des KromEditorApp.
    std::vector<std::string> buildExcludedScenes;
    // Setzt/entfernt eine Szene aus der Ausschlussliste und persistiert in krom-project.json.
    std::function<void(const std::string& sceneName, bool excluded)> toggleBuildExclude;
    // Exportiert alle nicht-ausgeschlossenen Szenen als .kscene — der eigentliche Build.
    std::function<bool()>                                            buildAllScenes;
    std::function<bool()>                                            exportProject;
    std::function<bool()>                                            playGame;

    EditorFrameContext(ecs::World& w, MaterialSystem& m, assets::AssetRegistry& r, EditorState& s)
        : world(w), materials(m), registry(r), state(s) {}
};

struct EditorBackendCallbacks
{
    std::function<bool()>                init;
    std::function<void()>                shutdown;
    std::function<void()>                newFrame;
    std::function<EditorFrameContext*()> getFrameContext;
    std::function<void(EditorFrameContext&)> drawUI;
    std::function<void(ICommandList&)>   renderDrawData;
    std::function<RenderTargetHandle()>  getBackbufferRT;
};

std::unique_ptr<IEngineFeature> CreateEditorFeature(EditorBackendCallbacks callbacks);

[[nodiscard]] bool BuildEditorCameraRenderView(
    const EditorState& state,
    uint32_t           viewportWidth,
    uint32_t           viewportHeight,
    renderer::RenderView& outView,
    math::Vec3         ambientColor     = {0.03f, 0.03f, 0.03f},
    float              ambientIntensity = 1.f) noexcept;

} // namespace engine::renderer::addons::editor
