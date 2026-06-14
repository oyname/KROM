#pragma once

#include <cstdint>

#include "core/Math.hpp"
#include "core/Types.hpp"
#include "platform/IInput.hpp"
#include "platform/IWindow.hpp"
#include "renderer/Environment.hpp"
#include "renderer/RendererTypes.hpp"

#include "ecs/ComponentMeta.hpp"
#include "ecs/Components.hpp"
#include "ecs/World.hpp"
#include "renderer/RenderLayers.hpp"
#include "addons/script/ScriptList.hpp"

#include <array>
#include <filesystem>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

using LPENTITY = engine::EntityID;
using LPMATERIAL = engine::MaterialHandle;
using LPTEXTURE = engine::TextureHandle;
using LPMESH = engine::MeshHandle;

struct KromSurfaceHandle
{
    uint32_t id = 0u;
    [[nodiscard]] bool IsValid() const noexcept { return id != 0u; }
    bool operator==(const KromSurfaceHandle&) const noexcept = default;
    bool operator!=(const KromSurfaceHandle&) const noexcept = default;
};
using LPSURFACE = KromSurfaceHandle;

inline constexpr LPENTITY NULL_LPENTITY = engine::NULL_ENTITY;
inline const LPMATERIAL NULL_MATERIAL = engine::MaterialHandle::Invalid();
inline const LPTEXTURE NULL_TEXTURE = engine::TextureHandle::Invalid();
inline const LPMESH NULL_MESH = engine::MeshHandle::Invalid();
inline const LPSURFACE NULL_SURFACE{};

// Scene-Handle: identifiziert eine geladene Scene (inkl. async-geladene)
struct KromSceneHandle
{
    uint32_t id = 0u;
    [[nodiscard]] bool IsValid() const noexcept { return id != 0u; }
    bool operator==(const KromSceneHandle&) const noexcept = default;
    bool operator!=(const KromSceneHandle&) const noexcept = default;
};
using LPSCENE = KromSceneHandle;
inline const LPSCENE NULL_SCENE{};

namespace Krom {

using Vec2 = engine::math::Vec2;
using Vec3 = engine::math::Vec3;
using Vec4 = engine::math::Vec4;
using Quat = engine::math::Quat;

namespace MaterialModel {
constexpr int Lit = 0;
constexpr int Unlit = 1;
constexpr int PBR = 2;
} // namespace MaterialModel

struct ColorInput
{
    const char* texture = nullptr;
    Vec4 color{1.0f, 1.0f, 1.0f, 1.0f};

    operator const char*() const noexcept { return texture; }
};

inline ColorInput Color(float r, float g, float b, float a = 1.0f)
{
    return ColorInput{nullptr, {r, g, b, a}};
}

inline ColorInput Texture(const char* path)
{
    return ColorInput{path, {1.0f, 1.0f, 1.0f, 1.0f}};
}

struct PBR
{
    ColorInput color = Color(1.0f, 1.0f, 1.0f, 1.0f);
    const char* normal = nullptr;
    float normalStrength = 1.0f;
    const char* orm = nullptr;
    float roughness = 0.8f;
    float metallic = 0.0f;
    float occlusion = 1.0f;
    Vec2 uvScale{1.0f, 1.0f};
    Vec2 uvOffset{0.0f, 0.0f};
};

struct MeshVertex
{
    Vec3 position{0.0f, 0.0f, 0.0f};
    Vec3 normal{0.0f, 0.0f, 0.0f};
    Vec2 uv{0.0f, 0.0f};
    Vec4 color{1.0f, 1.0f, 1.0f, 1.0f};
};

struct MeshData
{
    const char* name = "ProceduralMesh";
    const MeshVertex* vertices = nullptr;
    uint32_t vertexCount = 0u;
    const uint32_t* indices = nullptr;
    uint32_t indexCount = 0u;
    bool generateNormals = true;
    bool generateTangents = true;
};

namespace Renderer {
constexpr int DX11 = 0;
constexpr int OpenGL = 1;
constexpr int DX12 = 2;
constexpr int Vulkan = 3;
} // namespace Renderer

[[nodiscard]] int DefaultRenderer() noexcept;
[[nodiscard]] const char* RendererName(int backend) noexcept;
[[nodiscard]] bool IsRendererAvailable(int backend);

struct AdapterInfo
{
    uint32_t    index         = 0u;
    std::string name;
    size_t      dedicatedVRAM = 0u;   // Bytes; 0 = unbekannt
    bool        isDiscrete    = true; // false = integrierte GPU
    int         featureLevel  = 0;   // DX: 100/110/120 usw.; GL: major*10+minor
};

// Listet alle verfuegbaren GPU-Adapter fuer ein Backend auf.
// Kann vor Graphics() aufgerufen werden.
[[nodiscard]] std::vector<AdapterInfo> EnumerateAdapters(int backend);

struct GraphicsConfig
{
    int backend = Renderer::Vulkan;
    uint32_t width = 1280u;
    uint32_t height = 720u;
    std::string title = "KROM";
    bool enableDebugLayer = false;
    bool enableForwardPlus = true;
    bool enableGtao = true;
    bool resizable = true;
    // VSync an → GPU Present blockiert bis zum nächsten Vblank (stabile Bildrate
    // auf Monitor-Frequenz, aber dropped Frames verdoppeln das dt → sichtbares
    // Ruckeln bei schweren Szenen). VSync aus → GPU läuft frei, dt ist klein und
    // konsistent → flüssigste Kamerabewegung für Runtime-Anwendungen.
    bool vsync = false;
    engine::platform::WindowMode windowMode = engine::platform::WindowMode::Windowed;
    std::array<float, 4> clearColor = {0.05f, 0.05f, 0.05f, 1.0f};
    engine::math::Vec3 ambientColor{0.02f, 0.02f, 0.03f};
    float ambientIntensity = 1.0f;
};

using InitFn = std::function<bool()>;
using TickFn = std::function<void(float)>;

using Key = engine::platform::Key;
using MouseButton = engine::platform::MouseButton;

void SetAssetRoot(const std::filesystem::path& path);
const std::filesystem::path& GetAssetRoot();

// Überschreibt den Shader-Cache-Pfad. Optional — SetAssetRoot() setzt ihn
// automatisch neben das Asset-Verzeichnis, sodass Editor und Wrapper
// denselben Cache teilen.
void SetShaderCacheDir(const std::filesystem::path& path);

void SetConfig(const GraphicsConfig& config);
bool Graphics(const GraphicsConfig& config);
bool Graphics(int backend,
              int width,
              int height,
              const char* title = "KROM",
              float clearR = 0.05f,
              float clearG = 0.05f,
              float clearB = 0.05f,
              bool resizable = true);
bool Graphics(int width,
              int height,
              const char* title = "KROM",
              float clearR = 0.05f,
              float clearG = 0.05f,
              float clearB = 0.05f,
              bool resizable = true);

void OnInit(InitFn fn);
void OnUpdate(TickFn fn);

bool KeyDown(Key key);
bool KeyHit(Key key);
bool MouseButtonDown(MouseButton button);
int MouseDeltaX();
int MouseDeltaY();
float MouseScrollDelta();

void Quit();
bool AppRunning();

LPENTITY CreateEntity(const char* name = nullptr);
bool IsAlive(LPENTITY entity);
bool DestroyEntity(LPENTITY entity);
bool SetName(LPENTITY entity, const char* name);
const char* GetName(LPENTITY entity);
LPENTITY FindChild(LPENTITY rootEntity, const char* name);
int CountChildren(LPENTITY entity);
LPENTITY GetChild(LPENTITY entity, int index);
// Nur Rendering an/aus — Entity laeuft weiter durch Systeme und Transform.
bool SetVisible(LPENTITY entity, bool visible, bool recursive = true);

// Vollstaendige Deaktivierung — entspricht Unity's gameObject.SetActive().
// Entity wird aus Rendering UND Systemen entfernt.
bool SetActive(LPENTITY entity, bool active, bool recursive = true);
bool SetParent(LPENTITY child, LPENTITY parent, bool keepWorldTransform = true);
bool Detach(LPENTITY child, bool keepWorldTransform = true);
bool SetPosition(LPENTITY entity, const engine::math::Vec3& position);
bool SetPosition(LPENTITY entity, float x, float y, float z);
bool SetRotationEulerDeg(LPENTITY entity, float pitch, float yaw, float roll);
// Gibt (pitch, yaw, roll) in Grad aus der WorldTransform-Quaternion zurück.
// Konvention: ZYX-Euler (Reihenfolge: Roll → Pitch → Yaw).
engine::math::Vec3 GetRotationEulerDeg(LPENTITY entity);
bool SetScale(LPENTITY entity, const engine::math::Vec3& scale);
bool SetScale(LPENTITY entity, float x, float y, float z);
bool SetScale(LPENTITY entity, float uniformScale);
bool MoveEntity(LPENTITY entity, float x, float y, float z);
bool TurnEntity(LPENTITY entity, float pitch, float yaw, float roll);
bool GetWorldBounds(LPENTITY entity,
                    engine::math::Vec3& outCenter,
                    engine::math::Vec3& outExtents,
                    float& outBoundingSphere);
bool LookAt(LPENTITY entity, const engine::math::Vec3& target, const engine::math::Vec3& up = engine::math::Vec3::Up());
bool LookAt(LPENTITY entity, LPENTITY targetEntity, const engine::math::Vec3& up = engine::math::Vec3::Up());

LPENTITY CreateCamera(const char* name = "MainCamera",
                      const engine::math::Vec3& position = {0.0f, 2.0f, 10.0f},
                      float fovYDeg = 60.0f,
                      bool isMainCamera = true);

LPENTITY CreateDirectionalLight(const char* name = "Sun",
                                const engine::math::Vec3& position = {5.0f, 8.0f, 5.0f},
                                const engine::math::Vec3& eulerDeg = {-50.0f, -30.0f, 0.0f},
                                const engine::math::Vec3& color = {1.0f, 0.98f, 0.95f},
                                float intensity = 3.0f);
LPENTITY CreatePointLight(const char* name = "PointLight",
                          const engine::math::Vec3& position = {0.0f, 2.0f, 0.0f},
                          const engine::math::Vec3& color = {1.0f, 1.0f, 1.0f},
                          float intensity = 4.0f,
                          float range = 10.0f);
LPENTITY CreateSpotLight(const char* name = "SpotLight",
                         const engine::math::Vec3& position = {0.0f, 3.0f, 0.0f},
                         const engine::math::Vec3& eulerDeg = {-60.0f, 0.0f, 0.0f},
                         const engine::math::Vec3& color = {1.0f, 1.0f, 1.0f},
                         float intensity = 6.0f,
                         float range = 15.0f,
                         float innerDeg = 15.0f,
                         float outerDeg = 30.0f);
LPENTITY CreateEntityFromAsset(const char* path, const char* name = nullptr);
bool PrintAssetTree(const char* path);

LPMESH LoadMesh(const char* path);
LPTEXTURE LoadTexture(const char* path);
LPMATERIAL CreateTextureMaterial(const char* albedoTexturePath,
                                 int materialModel = MaterialModel::Lit,
                                 engine::math::Vec4 baseColorFactor = {1.0f, 1.0f, 1.0f, 1.0f},
                                 float roughness = 0.8f,
                                 float metallic = 0.0f);
LPMATERIAL CreateTextureMaterial(LPTEXTURE texture,
                                 int materialModel = MaterialModel::Lit,
                                 engine::math::Vec4 baseColorFactor = {1.0f, 1.0f, 1.0f, 1.0f},
                                 float roughness = 0.8f,
                                 float metallic = 0.0f);

// Lädt ein Material aus einer .mat-Datei (Editor-Workflow).
// Für rein programmatische Nutzung: CreateLitMaterial() verwenden.
LPMATERIAL LoadMaterial(const char* path);

// Erstellt ein Lit-Material direkt aus Code — kein .mat-File nötig.
// albedoTexturePath kann nullptr sein (dann reine Farbe via baseColorFactor).
// Der Handle kann direkt an SetMaterial() / SetMaterialRecursive() übergeben werden.
LPMATERIAL CreateLitMaterial(const char* albedoTexturePath = nullptr,
                              engine::math::Vec4 baseColorFactor = {1.0f, 1.0f, 1.0f, 1.0f},
                              float roughness = 0.8f,
                              float metallic = 0.0f);

bool SetMesh(LPENTITY entity, LPMESH mesh);
bool SetMaterial(LPENTITY entity, LPMATERIAL material);
bool SetMaterialRecursive(LPENTITY rootEntity, LPMATERIAL material);
bool SetTexture(LPENTITY entity,
                const char* albedoTexturePath,
                int materialModel = MaterialModel::Lit,
                bool recursive = true);
bool SetTexture(LPENTITY entity,
                LPTEXTURE texture,
                int materialModel = MaterialModel::Lit,
                bool recursive = true);
bool SetPBR(LPENTITY entity, const PBR& material, bool recursive = true);
bool SetMeshFromAsset(LPENTITY entity, const char* path);
bool SetMaterialFromAsset(LPENTITY entity, const char* path);
LPMESH CreateMeshAsset(const MeshData& mesh);
LPENTITY CreateMeshEntity(const MeshData& mesh);
LPENTITY CreateMesh(const char* name = "Mesh");

LPSURFACE CreateSurface(LPENTITY meshEntity);
int AddVertex(LPSURFACE surface, float x, float y, float z, float u = 0.0f, float v = 0.0f);
int AddVertex(LPSURFACE surface, float x, float y, float z, float u, float v, float nx, float ny, float nz);
bool AddTriangle(LPSURFACE surface, int v0, int v1, int v2);
bool VertexNormal(LPSURFACE surface, int index, float nx, float ny, float nz);
bool VertexTexCoords(LPSURFACE surface, int index, float u, float v);
int CountVertices(LPSURFACE surface);
int CountTriangles(LPSURFACE surface);

engine::renderer::EnvironmentHandle CreateEnvironmentFromHDR(const char* path,
                                                             float intensity = 1.0f,
                                                             bool enableIBL = true);
void SetActiveEnvironment(engine::renderer::EnvironmentHandle environment);

// -------------------------------------------------------------------------
// Scene-Management
// -------------------------------------------------------------------------

// Lädt eine .kscene synchron. Alle nicht-persistenten Entities werden vorher
// entfernt (exklusiver Wechsel). Gibt einen Scene-Handle zurück.
bool LoadProject(const char* projectFilePath);
void SetEditorLiveStateEnabled(bool enabled);
bool EditorLiveStateEnabled();

LPSCENE LoadScene(const char* path);
LPSCENE LoadScene(int sceneIndex);

// Lädt eine .kscene additiv — bestehende Entities bleiben erhalten.
LPSCENE LoadSceneAdditive(const char* path);
LPSCENE LoadSceneAdditive(int sceneIndex);

// Startet das Laden einer .kscene im Hintergrund (I/O + JSON-Parsing).
// Die Scene-Entities werden erst mit ActivateScene() in die World eingefügt.
LPSCENE LoadSceneAsync(const char* path);

// Gibt true zurück wenn der async-Ladevorgang abgeschlossen ist.
bool IsSceneReady(LPSCENE scene);

// Überträgt eine async-geladene Scene in die World (nur Main-Thread).
// Bei synchron geladenen Scenes ein No-Op (gibt true zurück).
bool ActivateScene(LPSCENE scene);

// Entfernt alle Entities der Scene aus der World (außer persistente).
void UnloadScene(LPSCENE scene);

// Entfernt alle nicht-persistenten Entities und Scenes.
void UnloadAll();

// Markiert eine Entity als persistent — sie überlebt UnloadScene / LoadScene.
void SetPersistent(LPENTITY entity);

// Markiert alle Entities einer Scene als persistent.
void SetScenePersistent(LPSCENE scene);

// Sucht eine Entity anhand ihres Namens (erste Übereinstimmung in der World).
LPENTITY FindEntity(const char* name);

// Gibt die aktive Main-Camera zurueck; Fallback ist die erste CameraComponent.
LPENTITY GetMainCamera();

// -------------------------------------------------------------------------
// Layer & Tag API
// -------------------------------------------------------------------------

// Layer-Konstanten direkt im Krom-Namespace verfuegbar machen.
namespace Layer {
    inline constexpr uint32_t Default     = engine::renderer::LAYER_DEFAULT;
    inline constexpr uint32_t EditorGizmo = engine::renderer::LAYER_EDITOR_GIZMO;
    inline constexpr uint32_t Transparent = engine::renderer::LAYER_TRANSPARENT;
    inline constexpr uint32_t UI          = engine::renderer::LAYER_UI;
    inline constexpr uint32_t User0       = engine::renderer::LAYER_USER_0;
    inline constexpr uint32_t User1       = engine::renderer::LAYER_USER_1;
    inline constexpr uint32_t User2       = engine::renderer::LAYER_USER_2;
    inline constexpr uint32_t User3       = engine::renderer::LAYER_USER_3;
    inline constexpr uint32_t All         = engine::renderer::LAYER_ALL;
    inline constexpr uint32_t None        = engine::renderer::LAYER_NONE;
} // namespace Layer

// Setzt die layerMask der MeshComponent einer Entity (Bit 0..31, siehe Layer::*).
bool        SetLayer(LPENTITY entity, uint32_t layerMask);

// Gibt die layerMask der MeshComponent zurueck (oder LAYER_DEFAULT wenn keine Mesh vorhanden).
uint32_t    GetLayer(LPENTITY entity);

// Setzt die cullingMask der CameraComponent (welche Layer die Kamera sieht).
bool        SetCameraLayers(LPENTITY camera, uint32_t cullingMask);

// Gibt die cullingMask der CameraComponent zurueck (oder LAYER_ALL wenn keine Kamera).
uint32_t    GetCameraLayers(LPENTITY camera);

// Setzt oder aktualisiert das Tag einer Entity (erstellt TagComponent falls noetig).
bool        SetTag(LPENTITY entity, const char* tag);

// Gibt den Tag-String der Entity zurueck, oder "" wenn kein Tag vorhanden.
const char* GetTag(LPENTITY entity);

// Sucht die erste Entity mit dem gegebenen Tag (linear, erste Uebereinstimmung).
LPENTITY    FindEntityByTag(const char* tag);

// Sucht eine Entity nur innerhalb der angegebenen Scene.
LPENTITY FindEntityInScene(LPSCENE scene, const char* name);

// -------------------------------------------------------------------------

int Run();
int Run(TickFn fn);
void WarmUpShaders();
void Shutdown();

// =========================================================================
// Ebene 2 — Component-Zugriff
// =========================================================================

// Gibt die Component-Registry zurueck. Funktioniert auch vor Graphics3D(),
// sodass RegisterComponent<T>() schon im Programmkopf aufgerufen werden kann.
[[nodiscard]] engine::ecs::ComponentMetaRegistry& GetComponentRegistry();

// Gibt die ECS-World zurueck. Erfordert vorherigen Graphics3D()-Aufruf.
[[nodiscard]] engine::ecs::World& GetWorld();

// Meldet einen eigenen Komponenten-Typ an. Muss vor dem ersten
// AddComponent<T>() / world.View<T>() fuer diesen Typ aufgerufen werden.
template<typename T>
inline void RegisterComponent(const char* name = nullptr)
{
    engine::ecs::RegisterComponent<T>(GetComponentRegistry(), name);
}

// Gibt einen Zeiger auf die Komponente T der Entity zurueck, oder nullptr.
template<typename T>
[[nodiscard]] inline T* GetComponent(LPENTITY e)
{
    return GetWorld().Get<T>(e);
}

// Fuegt der Entity eine Komponente T hinzu und gibt eine Referenz zurueck.
// Assertion wenn T bereits vorhanden.
template<typename T>
inline T& AddComponent(LPENTITY e, T component = T{})
{
    return GetWorld().Add<T>(e, std::move(component));
}

template<typename T>
[[nodiscard]] inline bool HasComponent(LPENTITY e)
{
    return GetWorld().Has<T>(e);
}

template<typename T>
inline void RemoveComponent(LPENTITY e)
{
    GetWorld().Remove<T>(e);
}

// =========================================================================
// Ebene 3 — Systeme
// =========================================================================

// Systeme laufen jeden Frame nach dem User-Tick-Callback, vor TransformSystem.
using SystemFn = std::function<void(engine::ecs::World&, float dt)>;
void RegisterSystem(SystemFn fn);

// =========================================================================
// Script-System (addon/script)
// =========================================================================
#ifndef KROM_SCRIPT
#define KROM_SCRIPT(Type)
#endif

// Klassen-basiertes Script-System. Scripts werden in .kscene als Array von
// Klassennamen serialisiert: "scripts": ["MyScript", "OtherScript"]
//
// Nutzung:
//   1. Script-Klasse von engine::script::ComponentScript ableiten.
//   2. Im Init-Code per GetScriptRegistry().Register<MyScript>("MyScript")
//      registrieren.
//   3. Per AddScript() zur Laufzeit hinzufügen oder .kscene-Schlüssel nutzen.

// Gibt die globale ScriptRegistry zurück (zum Registrieren von Klassen).
[[nodiscard]] engine::script::ScriptRegistry& GetScriptRegistry();

// Registriert eine Script-Klasse.
template<typename T>
inline void RegisterScriptClass(const char* name)
{
    GetScriptRegistry().Register<T>(name);
}

// Fügt einer Entity zur Laufzeit ein Script hinzu.
inline bool AddScript(LPENTITY entity, const char* className)
{
    if (!entity.IsValid()) return false;
    engine::ecs::World& world = GetWorld();
    auto* sl = world.Get<engine::script::ScriptList>(entity);
    if (!sl)
    {
        engine::script::ScriptList newList;
        newList.SetOwnerEntity(entity);
        world.Add<engine::script::ScriptList>(entity, std::move(newList));
        sl = world.Get<engine::script::ScriptList>(entity);
    }
    if (!sl) return false;
    return sl->Add(className, entity, GetScriptRegistry());
}

} // namespace Krom
