# KROM ENGINE API REFERENCE

This reference describes how to use the KROM ENGINE API.

The focus is on the direct engine path:
- engine initialization and the frame loop
- platform window and input
- backend selection (DX11 / DX12 / OpenGL / Vulkan)
- render features / AddOns
- ECS usage
- mesh and material setup

For architecture and high-level usage flow, see the API Overview.

> **Conventions.** Code samples assume `using namespace engine;`. Fully qualified,
> the types live in `engine::platform` (window/input), `engine::renderer`
> (`PlatformRenderLoop`, `RenderSystem`, `MaterialSystem`, `RenderView`,
> `DeviceFactory`), `engine::ecs` (`World`, `ComponentMetaRegistry`), `engine`
> (core components, `Scene`), and the AddOns under `engine::addons::*` /
> `engine::renderer::addons::*`.
>
> All signatures below are taken from the current headers. Where a subsystem is only
> summarized (asset loading), that is called out explicitly rather than invented.

---

## 1. Window and Platform

KROM does not expose a single Win32 window class. Windowing goes through a
platform-neutral abstraction (`IPlatform` creates an `IWindow`); concrete platforms
are Win32, GLFW/Linux, and a headless platform for tests.

### `platform::WindowDesc`

Configures the window and (optionally) the OpenGL context hints.

```cpp
struct WindowDesc
{
    uint32_t    width             = 1280u;
    uint32_t    height            = 720u;
    bool        visible           = true;
    bool        resizable         = true;
    WindowMode  windowMode        = WindowMode::Windowed;
    uint32_t    framebufferWidth  = 1280u;
    uint32_t    framebufferHeight = 720u;
    std::string title             = "KROM Engine";

    bool openglContext      = false;   // OpenGL backend only
    int  openglMajor        = 4;
    int  openglMinor        = 1;
    bool openglDebugContext = false;

    bool vsync = true;
};

enum class WindowMode { Windowed, Maximized, BorderlessWindowed, Fullscreen };
```

### `platform::IWindow`

Platform-neutral window interface.

```cpp
class IWindow
{
public:
    virtual bool             Create(const WindowDesc& desc) = 0;
    virtual void             Destroy() = 0;
    virtual WindowEventState PumpEvents(IInput& input) = 0;

    virtual bool        IsOpen() const = 0;
    virtual bool        ShouldClose() const;        // default: !IsOpen()
    virtual void        RequestClose() = 0;
    virtual void        Resize(uint32_t width, uint32_t height) = 0;
    virtual void        SetTitle(const char* title);

    virtual void*       GetNativeHandle() const = 0; // HWND / GLFWwindow*
    virtual uint32_t    GetWidth() const = 0;
    virtual uint32_t    GetHeight() const = 0;
    virtual uint32_t    GetFramebufferWidth() const;
    virtual uint32_t    GetFramebufferHeight() const;
    virtual float       GetDPIScale() const;
    virtual const char* GetBackendName() const = 0;
    virtual std::vector<std::filesystem::path> ConsumeDroppedFiles();
};
```

> `PumpEvents(IInput&)` both processes OS messages and feeds key/mouse events into the
> supplied input object. It returns a `WindowEventState` (quit requested, resized, new
> size). In normal use you do **not** drive `IWindow` directly —
> `PlatformRenderLoop::Tick` pumps events for you.

### `platform::IPlatform`

Owns platform initialization, the window factory, input, threading, and timing.

```cpp
class IPlatform
{
public:
    virtual bool             Initialize() = 0;
    virtual void             Shutdown() = 0;
    virtual void             PumpEvents() = 0;
    virtual double           GetTimeSeconds() const = 0;
    virtual IWindow*         CreateWindow(const WindowDesc& desc) = 0;
    virtual IInput*          GetInput() = 0;
    virtual IThreadFactory*  GetThreadFactory() = 0;
    virtual void             GetPrimaryMonitorSize(uint32_t& w, uint32_t& h) const;
};
```

You pass an `IPlatform&` into `PlatformRenderLoop::Initialize`; the loop creates the
window through it.

---

## 2. Input

KROM input is **instance-based** (no global/static input state). You obtain the active
`IInput` from the platform, or via `loop.GetInput()`.

### `platform::Key` / `platform::MouseButton`

```cpp
enum class Key : uint16_t {
    Unknown, Escape, Space, Enter, Tab, Backspace,
    LeftShift, RightShift, LeftCtrl, RightCtrl, LeftAlt, RightAlt,
    Left, Right, Up, Down,
    Home, End, PageUp, PageDown, Insert, Delete,
    A, B, /* ... */ Z,
    Num0, /* ... */ Num9,
    Plus, Minus, Equals,
    F1, /* ... */ F12,
    Count
};

enum class MouseButton : uint8_t { Left, Right, Middle, X1, X2, Count };
```

### `platform::IInput`

```cpp
class IInput
{
public:
    virtual void BeginFrame() = 0;   // once per frame, before queries

    // Fed by the windowing layer:
    virtual void OnKeyEvent(const InputKeyEvent&) = 0;
    virtual void OnMouseButtonEvent(const InputMouseButtonEvent&) = 0;
    virtual void OnMouseMoveEvent(const InputMouseMoveEvent&) = 0;
    virtual void OnMouseScrollEvent(const InputMouseScrollEvent&) = 0;

    // Keyboard:
    virtual bool KeyDown(Key) const = 0;      // currently held
    virtual bool KeyHit(Key) const = 0;       // pressed this frame
    virtual bool KeyReleased(Key) const = 0;  // released this frame

    // Mouse:
    virtual bool    MouseButtonDown(MouseButton) const = 0;
    virtual bool    MouseButtonHit(MouseButton) const = 0;
    virtual bool    MouseButtonReleased(MouseButton) const = 0;
    virtual int32_t MouseX() const = 0;
    virtual int32_t MouseY() const = 0;
    virtual int32_t MouseDeltaX() const = 0;
    virtual int32_t MouseDeltaY() const = 0;
    virtual float   MouseScrollDelta() const = 0;
    virtual void    SetCursorMode(bool captured, bool hidden);
};
```

### Example

```cpp
platform::IInput* input = loop.GetInput();

if (input->KeyDown(platform::Key::Left))   { /* continuous movement */ }
if (input->KeyHit(platform::Key::Space))   { /* one-shot action     */ }
```

> `BeginFrame()` is invoked by the platform/loop each frame; you normally only query.

---

## 3. Backend Selection

KROM has no per-app DX11 adapter-enumeration API. Backends are registered as factories
and selected by an enum at initialization time.

```cpp
// engine::renderer::DeviceFactory
enum class BackendType { Null, DirectX11, DirectX12, OpenGL, Vulkan };
```

- `Null` — headless/no-op device (tests, CI).
- `DirectX11`, `DirectX12`, `Vulkan` — modern HLSL path backends.
- `OpenGL` — isolated 4.1 legacy path (macOS).

You pass the chosen `BackendType` into `PlatformRenderLoop::Initialize`. Adapter/device
details are described by `IDevice::DeviceDesc` (optional debug layer, app name, etc.).

---

## 4. Runtime Loop — `renderer::PlatformRenderLoop`

The outer runtime entry point. It owns the window, input, device, and `RenderSystem`,
and runs one frame at a time.

```cpp
class PlatformRenderLoop
{
public:
    bool Initialize(DeviceFactory::BackendType backend,
                    platform::IPlatform& platform,
                    const platform::WindowDesc& windowDesc,
                    events::EventBus* eventBus = nullptr,
                    const IDevice::DeviceDesc& deviceDesc = {});
    void Shutdown();

    bool Tick(const ecs::World& world,
              const MaterialSystem& materials,
              const RenderView& view,
              platform::IPlatformTiming& timing,
              const FramePipelineCallbacks& callbacks = {});

    // Overload with offscreen render requests (render-to-texture):
    bool Tick(const ecs::World& world,
              const MaterialSystem& materials,
              const RenderView& view,
              platform::IPlatformTiming& timing,
              const FramePipelineCallbacks& callbacks,
              std::span<const OffscreenRenderRequest> offscreenRequests);

    void SetPreRenderCallback(std::function<void(float dt)> cb); // animation/physics/AI

    bool             ShouldExit() const noexcept;
    platform::IWindow* GetWindow() const noexcept;
    platform::IInput*  GetInput()  const noexcept;
    RenderSystem&    GetRenderSystem() noexcept;
};
```

### Functions

- `Initialize(...)` — creates window + device + swapchain and brings the renderer up.
  Register your render features **before** calling this (see §5).
- `Tick(world, materials, view, timing)` — pumps events and renders one frame from the
  current ECS state. Returns `false` when the window has been closed.
- `SetPreRenderCallback(cb)` — runs once per frame after event pumping and before
  rendering; this is where game-side systems (animation, physics, AI) advance.
- `GetRenderSystem()` — access point for feature registration and stats.
- `Shutdown()` — clean teardown (safe at end of run).

---

## 5. Render Features / AddOns — `RenderSystem`

KROM is AddOn-based: the renderer only extracts and draws what registered **features**
describe. Register them on the loop's `RenderSystem` before `Initialize`.

```cpp
auto& rs = loop.GetRenderSystem();
rs.RegisterFeature(addons::mesh_renderer::CreateMeshRendererFeature());
rs.RegisterFeature(addons::lighting::CreateLightingFeature());
rs.RegisterFeature(renderer::addons::forward::CreateForwardFeature());   // or Forward+
```

Verified feature factories and their namespaces:

| Factory | Namespace | Purpose |
| --- | --- | --- |
| `CreateMeshRendererFeature()` | `engine::addons::mesh_renderer` | mesh extraction/draw |
| `CreateLightingFeature()` | `engine::addons::lighting` | light extraction |
| `CreateForwardFeature(config = {})` | `engine::renderer::addons::forward` | forward render pipeline (Forward+ via config) |
| `CreateShadowFeature()` | `engine::addons::shadow` | shadow maps / CSM |
| `CreateGtaoFeature(outResources = nullptr)` | `engine::renderer::addons::gtao` | GTAO ambient occlusion |
| `CreateOutlineFeature(getSelected, shouldOutline)` | `engine::renderer::addons::outline` | screen-space selection outline (HLSL only) |

Registration order matters only in that all features must be registered **before**
`Initialize`. Additional shipped AddOns (camera-view building, debug draw, particles,
animation, prefab, editor) follow the same `Create…Feature()` pattern. After a frame,
read back stats via `loop.GetRenderSystem().GetStats()`.

> Bloom and tonemapping are **not** separate user features — they are stages of the
> standard forward frame, toggled per view through `RenderView::enableBloom` and driven
> by the tonemap material (see §12). GTAO ambient occlusion is gated by
> `RenderView::enableAmbientOcclusion`; shadows by `RenderView::enableShadows`.

---

## 6. ECS Core

### `ecs::ComponentMetaRegistry`

Declares which component types exist and how they are reflected. The `World` is built
**from** a populated registry — there is no default `World`.

```cpp
ecs::ComponentMetaRegistry registry;
RegisterCoreComponents(registry);                          // engine
addons::mesh_renderer::RegisterMeshRendererComponents(registry);
addons::camera::RegisterCameraComponents(registry);        // engine
addons::lighting::RegisterLightingComponents(registry);    // engine
```

### `ecs::World`

```cpp
class World
{
public:
    explicit World(ComponentMetaRegistry& registry);   // no default ctor

    EntityID CreateEntity();
    void     DestroyEntity(EntityID id);
    bool     IsAlive(EntityID id) const noexcept;
    size_t   EntityCount() const noexcept;

    template<typename T, typename... Args> T& Add(EntityID id, Args&&...);
    template<typename T> void   Remove(EntityID id);
    template<typename T> T*      Get(EntityID id) noexcept;
    template<typename T> bool    Has(EntityID id) const noexcept;

    template<typename... Ts, typename Func> void View(Func&& func);   // iterate matches
    template<typename Func> void ForEachAlive(Func&& func) const;
};
```

> Note the names vs. other engines: the ECS container is `ecs::World` (not a
> "Registry"), and iteration is `world.View<A, B>(fn)`.

### Example

```cpp
ecs::World world(registry);

EntityID e = world.CreateEntity();
world.Add<TransformComponent>(e);
world.Add<WorldTransformComponent>(e);
world.Add<NameComponent>(e, NameComponent{"Cube"});

world.View<TransformComponent, MeshComponent>(
    [](EntityID id, TransformComponent& t, MeshComponent& m) { /* ... */ });
```

### Handle Types

Resources are referenced by typed, generation-checked handles (`MeshHandle`,
`MaterialHandle`, `ShaderHandle`, `TextureHandle`, `BufferHandle`,
`EnvironmentHandle`, …). Construct test/explicit handles with `Handle::Make(index, gen)`
and check with `IsValid()`.

---

## 7. Core Components (`engine`, via `RegisterCoreComponents`)

### `TransformComponent`

```cpp
struct TransformComponent
{
    Vec3 localPosition{0,0,0};
    Quat localRotation = Quat::Identity();
    Vec3 localScale{1,1,1};
    bool inheritParentScale = true;
    bool dirty = true;
    uint32_t localVersion, worldVersion, parentWorldVersion;

    void SetEulerDeg(float pitch, float yaw, float roll) noexcept;
    void RotateLocalEulerDeg(float pitch, float yaw, float roll) noexcept;
    void RotateWorldEulerDeg(float pitch, float yaw, float roll) noexcept;
};
```

### `WorldTransformComponent`

```cpp
struct WorldTransformComponent
{
    Vec3 position{0,0,0};
    Quat rotation = Quat::Identity();
    Vec3 scale{1,1,1};
    Mat4 matrix  = Mat4::Identity();
    Mat4 inverse = Mat4::Identity();
};
```

### `BoundsComponent`

Local + world AABB and bounding sphere used by visibility/culling.

```cpp
struct BoundsComponent
{
    Vec3  centerLocal{0,0,0},  extentsLocal{1,1,1};
    Vec3  centerWorld{0,0,0},  extentsWorld{1,1,1};
    float boundingSphere = 1.f;
    uint32_t lastTransformVersion = 0u;
    bool  localDirty = true;
};
```

Other core components: `ParentComponent`, `ChildrenComponent`, `NameComponent`,
`GuidComponent`, `OBBComponent`, `ActiveComponent`, `TagComponent`.

---

## 8. Render Components (AddOns)

### `MeshComponent` (`engine`, mesh_renderer AddOn)

```cpp
struct MeshComponent
{
    MeshHandle  mesh;
    bool        visible        = true;
    bool        castShadows    = true;
    bool        receiveShadows = true;
    uint32_t    layerMask      = renderer::LAYER_DEFAULT;
    std::string meshAssetPath;   // "model.glb#mesh/<i>" or "mesh.kmesh"
};
```

### `MaterialComponent` (`engine`, mesh_renderer AddOn)

Supports an entity-wide material plus per-submesh slot overrides.

```cpp
struct MaterialComponent
{
    struct SlotOverride { uint32_t submeshIndex; MaterialHandle material; std::string materialAssetPath; };

    MaterialHandle material;            // entity-wide
    uint32_t       submeshIndex = 0u;
    std::string    materialAssetPath;
    std::string    baseColorTexturePath;
    std::vector<SlotOverride> slotOverrides;

    const SlotOverride* FindSlotOverride(uint32_t index) const noexcept;
};
```

### `CameraComponent` (`engine`, camera AddOn)

```cpp
struct CameraComponent
{
    ProjectionType projection   = ProjectionType::Perspective; // or Orthographic
    float fovYDeg   = 60.f, nearPlane = 0.1f, farPlane = 1000.f;
    float orthoSize = 10.f, aspectRatio = 16.f/9.f;
    uint32_t cullingMask = /* all except editor-gizmo layers */;
    bool isMainCamera = false;
    BackgroundMode backgroundMode = BackgroundMode::ClearColor;
    std::array<float,4> clearColor = {0,0,0,1};
};
```

> Cameras do not render themselves. The camera AddOn builds a `RenderView` (§9) from a
> camera entity; the `RenderView` is what `Tick` consumes.

### `LightComponent` (`engine`, lighting AddOn)

```cpp
enum class LightType : uint8_t { Directional, Point, Spot };

struct LightComponent
{
    LightType  type        = LightType::Point;
    Vec3       color       {1,1,1};
    float      intensity   = 1.f;
    float      range       = 10.f;
    float      spotInnerDeg = 15.f, spotOuterDeg = 30.f;
    bool       castShadows = false;
    uint32_t   layerMask   = 0xFFFFFFFFu;
    ShadowSettings shadowSettings;
};
```

---

## 9. `renderer::RenderView`

A plain per-frame value type describing the active view. Fill it directly, or let the
camera AddOn produce it from a camera entity.

```cpp
struct RenderView
{
    Mat4 view = Mat4::Identity();
    Mat4 projection = Mat4::Identity();
    Vec3 cameraPosition{0,0,0};
    Vec3 cameraForward{0,0,1};
    Vec3 ambientColor{0.03f,0.03f,0.03f};
    float ambientIntensity = 1.f;
    float nearPlane = 0.1f, farPlane = 1000.f;
    uint32_t debugFlags = 0u;
    uint32_t visibilityLayerMask = LAYER_ALL;
    uint32_t lightLayerMask      = LAYER_ALL;
    BackgroundMode backgroundMode = BackgroundMode::ClearColor;
    bool enableBloom = true;
    bool enableShadows = true;
    bool enableAmbientOcclusion = true;
    std::array<float,4> clearColor{0.3f,0.3f,0.3f,1.f};
};
```

---

## 10. Mesh Data (`engine::assets`)

CPU-side geometry uses **flat float arrays** (not vectors of vec3).

```cpp
struct SubMeshData
{
    std::vector<float>    positions;  // 3 floats per vertex
    std::vector<float>    normals;    // 3 floats
    std::vector<float>    tangents;   // 4 floats (xyz + handedness)
    std::vector<float>    uvs;        // 2 floats
    std::vector<float>    colors;     // 4 floats (RGBA)
    std::vector<uint32_t> indices;
    std::vector<float>    boneWeights; // 4 floats
    std::vector<uint32_t> boneIndices; // 4 uints
    uint32_t              materialIndex = 0u;

    std::vector<uint8_t>  rawInterleavedBytes; // optional .kmesh fast path
    uint32_t              rawVertexStride = 0u;
};

struct MeshAsset : AssetBase
{
    std::vector<SubMeshData>    submeshes;
    std::vector<MaterialHandle> materialHandles; // index == SubMeshData::materialIndex
    GpuUploadStatus gpuStatus{};
    void ComputeBounds(Vec3& outMin, Vec3& outMax) const;
};
```

CPU assets (`MeshAsset`, texture assets) live in the `AssetRegistry`; GPU upload is
handled separately by the backend runtime. Loading and importing go through the
**asset pipeline**, not direct renderer calls — see §10.1.

### 10.1 `assets::AssetPipeline`

Connects the asset registry, filesystem, shader-compile path, and GPU upload. You give
it a registry, a device (for upload), and optionally a filesystem; then set the asset
root and register importers.

```cpp
class AssetPipeline
{
public:
    AssetPipeline(AssetRegistry& registry,
                  renderer::IDevice* device = nullptr,
                  platform::IFilesystem* fs = nullptr);

    void SetAssetRoot(const std::filesystem::path& root);

    // Importers are format-agnostic; ImportBundle() picks by extension.
    void RegisterMeshImporter(std::unique_ptr<IAssetImporter> importer);
    ImportedAssetBundle ImportBundle(const std::string& path);   // .glb/.fbx/.obj/.usd...

    MeshHandle     LoadMesh(const std::string& path);
    TextureHandle  LoadTexture(const std::string& path);
    ShaderHandle   LoadShader(const std::string& path,
                              ShaderStage fallbackStage = ShaderStage::Vertex);
    MaterialHandle LoadMaterial(const std::string& path);
    bool           LoadScene(const std::string& path, Scene& scene);

    void PollHotReload();             // re-import changed source files
    void BuildPendingShaderCaches();  // compile queued shaders
    void UploadPendingGpuAssets();    // push CPU assets to the GPU

    TextureHandle GetGpuTexture(TextureHandle) const noexcept;
    ShaderHandle  GetGpuShader(ShaderHandle) const noexcept;
};
```

### Typical asset setup

```cpp
assets::AssetRegistry registry;
assets::AssetPipeline pipeline(registry, loop.GetRenderSystem().GetDevice());
pipeline.SetAssetRoot("assets/");
pipeline.RegisterMeshImporter(std::make_unique<addons::gltf::GltfImporter>());

MeshHandle    mesh = pipeline.LoadMesh("models/cube.glb");
TextureHandle tex  = pipeline.LoadTexture("textures/albedo.png");

pipeline.BuildPendingShaderCaches();
pipeline.UploadPendingGpuAssets();   // before the first frame that uses them
```

> Hand the registry to the renderer with `loop.GetRenderSystem().SetAssetRegistry(&registry)`
> so shader and environment resolution can see it. `glTF`/`.glb` import is provided by the
> `engine::addons::gltf::GltfImporter` AddOn.

---

## 11. Materials (`engine::renderer` + material AddOns)

Materials live in a `MaterialSystem`. The ergonomic path is the material AddOns
(Unlit / Lit / PBR); the low-level path is `MaterialRuntimeBridge::RegisterMaterial`.

### Unlit

```cpp
MaterialSystem ms;
unlit::UnlitMaterialCreateInfo info{};
info.name = "UnlitTest";
info.vertexShader   = vs;
info.fragmentShader = fs;
info.enableEmissiveMap = true;
info.alphaTest = true;

unlit::UnlitMaterial mat = unlit::UnlitMaterial::Create(ms, info);
mat.SetBaseColorFactor({0.8f, 0.7f, 0.6f, 1.f});
mat.SetAlbedo(albedoTexture);
MaterialHandle handle = mat.Handle();
```

### Lit

```cpp
lit::LitMaterialCreateInfo info{};
info.name = "LitTest";
info.vertexShader = vs; info.fragmentShader = fs; info.shadowShader = shadowVs;
info.specularStrength = 0.6f; info.roughnessFactor = 0.2f;
lit::LitMaterial mat = lit::LitMaterial::Create(ms, info);
```

### PBR (master + cached instance permutations)

```cpp
pbr::PbrMasterMaterial::Config config{};
config.vs = vs; config.fs = fs; config.shadow = shadowVs;
config.vertexLayout = layout;

pbr::PbrMasterMaterial master = pbr::PbrMasterMaterial::Create(ms, config);

MaterialHandle inst = master.CreateInstance("Inst")
    .BaseColor(albedoTex)
    .Normal(normalTex)
    .Emissive(emissiveTex)
    .Roughness(0.15f)
    .Metallic(0.9f)
    .IBL(true)
    .Build();
```

Material descriptors carry feature flags (`MaterialFeatureFlags::PbrMetalRough`,
`BaseColorMap`, `NormalMap`, `EmissiveMap`, `IblMap`, `AlphaTest`, `Unlit`, …) and a
render policy (cull mode, double-sided). `doubleSided = true` overrides the cull mode to
`None`.

### Low-level registration

```cpp
MaterialDesc desc; desc.name = "LoopMat";
MaterialRuntimeDesc runtime;
runtime.renderPass     = StandardRenderPasses::Opaque();
runtime.vertexShader   = vs;
runtime.fragmentShader = fs;
MaterialHandle h = MaterialRuntimeBridge::RegisterMaterial(ms, std::move(desc), runtime);
```

---

## 12. Post-Processing, Offscreen Rendering, and Environment

KROM has **no** generic "create post-process pass" API. Post effects are either stages
of the standard forward frame (bloom, tonemap) or dedicated AddOns (GTAO, outline),
toggled per view.

### Bloom / Tonemap

Built into the standard forward recipe.

- `RenderView::enableBloom` turns bloom on/off for that view.
- Tonemapping runs as the final fullscreen pass. Supply a passthrough tonemap material
  once before the first frame:

```cpp
loop.GetRenderSystem().SetDefaultTonemapMaterial(tonemapMat, materials);
```

  (A fullscreen passthrough shader: no vertex buffer, no depth test. If you instead set
  an `onTonemap` frame callback, that takes precedence.)

### GTAO ambient occlusion

```cpp
std::shared_ptr<gtao::GtaoGpuResources> gtaoResources;
rs.RegisterFeature(renderer::addons::gtao::CreateGtaoFeature(&gtaoResources));
// gtaoResources lets you tweak GTAO settings at runtime
```

Gated per view by `RenderView::enableAmbientOcclusion`.

### Outline (editor selection)

```cpp
rs.RegisterFeature(renderer::addons::outline::CreateOutlineFeature(
    /* getSelected      */ [&]{ return selectedEntity; },
    /* shouldOutline    */ [&](EntityID e){ return IsSelectable(e); }));
```

Renders nothing when no entity is selected. HLSL backends only (disabled on OpenGL).

### Offscreen rendering (render-to-texture)

Use the second `Tick` overload with one or more `OffscreenRenderRequest`s. Each request
renders the world from its own `RenderView` into a render target you allocated from the
device.

```cpp
struct OffscreenRenderRequest
{
    const RenderView*  view = nullptr;
    RenderTargetHandle outputRT;
    TextureHandle      outputTex;
    uint32_t           viewportWidth = 0u;
    uint32_t           viewportHeight = 0u;
    const FramePipelineCallbacks* callbacks = nullptr;
};

// Allocate the target via the device (renderer::IDevice::CreateRenderTarget),
// then pass a span of requests:
OffscreenRenderRequest req{};
req.view = &previewView;
req.outputRT = previewRT;
req.viewportWidth = 512u; req.viewportHeight = 512u;

const OffscreenRenderRequest requests[] = { req };
loop.Tick(world, materials, mainView, timing, callbacks, requests);
```

### Environment / IBL

Image-based lighting is managed by the render system, not a per-frame view field.

```cpp
EnvironmentHandle env = rs.CreateEnvironment(envDesc);
rs.SetActiveEnvironment(env);
// ...
rs.DestroyEnvironment(env);
```

Materials opt into IBL through their feature flags (e.g. PBR `.IBL(true)`, §11).

---

## 13. Minimal Initialization Example

This mirrors the engine's own end-to-end path (see `tests/test_renderer.cpp`).

```cpp
using namespace engine;

// 1. Components.
ecs::ComponentMetaRegistry registry;
RegisterCoreComponents(registry);
addons::mesh_renderer::RegisterMeshRendererComponents(registry);
addons::camera::RegisterCameraComponents(registry);
addons::lighting::RegisterLightingComponents(registry);

// 2. Loop + render features (before Initialize).
renderer::PlatformRenderLoop loop;
auto& rs = loop.GetRenderSystem();
rs.RegisterFeature(addons::mesh_renderer::CreateMeshRendererFeature());
rs.RegisterFeature(addons::lighting::CreateLightingFeature());
rs.RegisterFeature(renderer::addons::forward::CreateForwardFeature());

// 3. Window + device, then initialize.
platform::WindowDesc desc{};
desc.width = 1280u; desc.height = 720u; desc.title = "KROM";

renderer::IDevice::DeviceDesc deviceDesc{};
deviceDesc.enableDebugLayer = true;
loop.Initialize(renderer::DeviceFactory::BackendType::DirectX11, platform, desc, &bus, deviceDesc);

// 4. World + material.
ecs::World world(registry);
renderer::MaterialSystem materials;
// ... register/create a material, obtain MaterialHandle `mat` ...

// 5. A renderable entity.
EntityID e = world.CreateEntity();
world.Add<TransformComponent>(e);
world.Add<WorldTransformComponent>(e);
world.Add<MeshComponent>(e, MeshComponent{meshHandle});
world.Add<MaterialComponent>(e, MaterialComponent{mat});
world.Add<BoundsComponent>(e, BoundsComponent{
    .centerWorld={0,0,0}, .extentsWorld={0.5f,0.5f,0.5f}, .boundingSphere=0.87f});

// 6. View + frame loop.
renderer::RenderView view{};
view.view           = Mat4::LookAtRH({0,0,-5}, {0,0,0}, Vec3::Up());
view.projection     = Mat4::PerspectiveFovRH(60.f * math::DEG_TO_RAD, 16.f/9.f, 0.1f, 100.f);
view.cameraPosition = {0,0,-5};

platform::StdTiming timing;
while (!loop.ShouldExit())
    loop.Tick(world, materials, view, timing);

loop.Shutdown();
```

---

## 14. Minimal ECS Render Path

For a visible renderable entity you need (core + mesh_renderer components):

- `TransformComponent`
- `WorldTransformComponent`
- `MeshComponent` (with a valid `MeshHandle`)
- `MaterialComponent` (with a valid `MaterialHandle`)
- `BoundsComponent`

For the active view, either:

- fill a `RenderView` by hand (matrices + camera position/forward), **or**
- create a camera entity (`TransformComponent`, `WorldTransformComponent`,
  `CameraComponent` with `isMainCamera = true`) and let the camera AddOn build the
  `RenderView`.

And — required for anything to appear — the matching **component metas** and **render
features** must be registered before `Initialize` (§5, §6).
