# KROM ENGINE ARCHITECTURE

## Overview

KROM is a modular C++ 3D engine built around a clear separation between runtime control,
scene state, frame extraction, frame preparation, frame graph compilation and backend
execution.

The core design is straightforward: the engine does not mix gameplay-facing scene data,
visibility processing and low-level draw execution into one large rendering path.
Instead, scene state is stored in the ECS, extracted into render-facing data, prepared
into frame-local workloads, compiled into an executable frame graph and then submitted
through the active rendering backend.

That separation is one of the defining strengths of the engine.

Scene state lives in `ecs::World`. Higher-level scene helpers such as `Scene` build on
top of that world. The renderer does not treat ECS data as immediate draw input. It
first converts relevant world state into dedicated render structures
(`RenderSceneSnapshot` / `RenderSceneStorage`), then builds draw queues and graph data,
and finally executes the prepared frame through the backend interfaces.

This gives the engine a stable and understandable architecture. Scene code can evolve
without rewriting the backend layer, rendering features can be added without collapsing
the runtime structure, and backend-specific execution remains isolated behind a clean
interface.

> All type names and signatures below are taken from the current headers. Namespaces:
> runtime/renderer types are in `engine::renderer`, ECS in `engine::ecs`, scene helpers
> in `engine`, the render graph in `engine::rendergraph`, and features in
> `engine::addons::*` / `engine::renderer::addons::*`.

## Architecture Overview

The following diagram shows the internal structure of the engine from the runtime layer
down to backend execution.

```text
PlatformRenderLoop
  -> RenderSystem
      -> RenderFrameOrchestrator
          -> FrameExtractionStage
          -> FrameConstantStage
          -> FrameShaderStage
          -> FrameUploadStage
          -> FrameGraphStage
          -> FrameExecutionStage
      -> FeatureRegistry
      -> ShaderRuntime
      -> GpuResourceRuntime
      -> EnvironmentSystem
      -> JobSystem
      -> ecs::World / Scene
          -> RenderSceneSnapshot
          -> ECSExtractor
          -> RenderSceneStorage (RenderWorld = legacy facade)
      -> Backend Interfaces
          -> IDevice
          -> ISwapchain
          -> ICommandList
          -> IFence
```

---

## High-Level Structure

```text
PlatformRenderLoop
  -> RenderSystem
      -> RenderFrameOrchestrator
          -> FrameExtractionStage
          -> FrameConstantStage / FrameShaderStage / FrameUploadStage
          -> FrameGraphStage
          -> FrameExecutionStage
      -> FeatureRegistry
      -> ShaderRuntime
      -> GpuResourceRuntime
      -> RenderSceneStorage
      -> IDevice / ISwapchain / ICommandList / IFence
```

At runtime, `PlatformRenderLoop` drives the outer loop and platform interaction.
`RenderSystem` owns the renderer-facing runtime state. `RenderFrameOrchestrator`
executes the frame pipeline stage by stage. The backend interfaces execute the compiled
result on the active graphics API.

That gives the engine a clean top-down flow:

- platform and window control at the top
- renderer orchestration in the middle
- backend execution at the bottom

---

## Runtime Layer

The runtime entry layer is `PlatformRenderLoop`.

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
};
```

This class owns the outer application flow:

- platform hookup
- window creation
- event pumping
- resize handling
- per-frame renderer execution

It is intentionally kept focused. Platform and lifecycle concerns stay outside the
renderer so that rendering code does not get polluted with windowing and host logic.

> `FramePipelineCallbacks` lives in `engine::renderer` (declared in
> `RenderPipelineTypes.hpp`), not in the `rendergraph` namespace.

---

## Renderer Core

The central renderer-facing runtime object is `RenderSystem`.

```cpp
class RenderSystem
{
public:
    bool Initialize(DeviceFactory::BackendType backend,
                    platform::IWindow& window,
                    const platform::WindowDesc& windowDesc = {},
                    events::EventBus* eventBus = nullptr,
                    const IDevice::DeviceDesc& deviceDesc = {});
    void Shutdown();

    void HandleResize(uint32_t width, uint32_t height);
    bool RenderFrame(const ecs::World& world,
                     const MaterialSystem& materials,
                     const RenderView& view,
                     const platform::IPlatformTiming& timing,
                     const FramePipelineCallbacks& callbacks = {});

    bool RegisterFeature(std::unique_ptr<IEngineFeature> feature);
    const RenderStats& GetStats() const noexcept;
};
```

`RenderSystem` is the CPU-side owner of the renderer. It manages the persistent
rendering subsystems and delegates the actual per-frame build and execution to
`RenderFrameOrchestrator`.

Important owned systems include:

- `FeatureRegistry`
- `ShaderRuntime`
- `GpuResourceRuntime`
- `EnvironmentSystem` (IBL)
- `JobSystem`
- `RenderPassRegistry`
- backend objects such as `IDevice`, `ISwapchain`, `ICommandList` and `IFence`

That is a healthy split. `RenderSystem` owns renderer lifetime and major services, while
frame assembly itself is handled by dedicated stage logic inside the orchestrator.

---

## Frame Orchestration

Per-frame execution is coordinated by `RenderFrameOrchestrator`.

```cpp
class RenderFrameOrchestrator
{
public:
    bool Execute(const RenderFrameOrchestratorContext& context,
                 RenderFrameExecutionState& state);
};
```

This is a major structural advantage of the engine.

The frame is not buried inside one giant render function. Instead, it is processed
through explicit stages and tracked through `RenderFrameExecutionState`, which carries a
`FrameStageStatus` per step plus the typed result of each stage.

The tracked steps (in order) are:

- extraction
- frame constants (prepare-frame)
- shader request collection
- material request collection
- queue building
- upload request collection
- shader commit
- material commit
- upload commit
- frame graph build
- execution

`RenderFrameExecutionState::Succeeded()` and `FirstFailure()` make the whole frame
inspectable: a failure reports exactly which stage stopped the frame. This makes the
renderer far easier to understand, debug and extend than a monolithic one-pass
architecture.

> The orchestrator context (`RenderFrameOrchestratorContext`) is what wires the stages
> to the backend for the frame: device, swapchain, command lists, fence, gpu/shader
> runtimes, feature registry, job system, output target, and begin/end/present flags
> (the same context drives both the main frame and offscreen render requests).

---

## ECS as Scene Foundation

Scene state is fundamentally ECS-driven. The core world container is `ecs::World`.

```cpp
class World
{
public:
    explicit World(ComponentMetaRegistry& registry);   // no default ctor

    EntityID CreateEntity();
    void DestroyEntity(EntityID id);
    bool IsAlive(EntityID id) const noexcept;

    template<typename T, typename... Args> T& Add(EntityID id, Args&&...);
    template<typename T> T*   Get(EntityID id) noexcept;
    template<typename T> bool Has(EntityID id) const noexcept;
    template<typename... Ts, typename Func> void View(Func&& func);
};
```

The ECS is not just a convenience layer. It is the primary source of truth for the
scene.

Its implementation supports:

- entity lifecycle management
- archetype-based chunked storage
- typed component access
- efficient iteration through views (matched by component signature)
- controlled read phases (`ScopedReadPhase`) for safer concurrent world access

That design is important because the rendering pipeline depends on stable and structured
scene data rather than ad hoc scene traversal.

---

## Scene Layer

The higher-level scene helper is `Scene`.

```cpp
class Scene
{
public:
    explicit Scene(ecs::World& world);

    EntityID CreateEntity(std::string_view name = "Entity");
    void SetParent(EntityID child, EntityID parent);
    void DetachFromParent(EntityID child);

    void Update(jobs::JobSystem& js);
    void PropagateTransforms();
    void DestroyEntity(EntityID id);
    EntityID FindByName(std::string_view name) const;
};
```

`Scene` is a convenience layer built on top of `ecs::World`.

Its responsibilities include:

- entity naming
- hierarchy management (`ParentComponent` / `ChildrenComponent`)
- transform propagation (it owns a `TransformSystem` and `BoundsSystem`)
- scene-level creation and destruction helpers

That means the ECS remains the actual data foundation, while `Scene` provides a more
ergonomic way to work with scene structure.

---

## Extraction Boundary

One of the most important architectural boundaries in KROM is the split between live
scene state and render-facing frame data.

This happens through:

- `RenderSceneSnapshot` — the read-facing handle to one frame's extracted scene
- `ECSExtractor` — pulls relevant world state into render structures
- `RenderSceneStorage` — owns the extracted proxies, per-feature frame data and built
  draw queues for the frame

The renderer does not work directly on mutable ECS state while preparing GPU work.
Instead, it first extracts the relevant frame data into a dedicated render
representation.

```cpp
// Owns extracted renderables, feature frame data, and built draw queues for one frame.
class RenderSceneStorage
{
public:
    void AddRenderablesBulk(std::vector<RenderProxy> proxies);

    void BuildDrawLists(const math::Mat4& view,
                        const math::Mat4& viewProj,
                        float nearZ, float farZ,
                        const MaterialSystem& materials,
                        const RenderPassRegistry& renderPassRegistry,
                        uint32_t layerMask = 0xFFFFFFFFu,
                        jobs::JobSystem* jobSystem = nullptr);

    const std::vector<RenderProxy>& GetProxies() const noexcept;
    RenderQueue&        GetQueue() noexcept;
    RenderExtractionView GetExtractionView() noexcept;   // write side (features)
    RenderFrameDataView  GetFrameDataView() const noexcept; // read side

    template<typename T> T& GetOrCreateFeatureData(std::string_view name);
    template<typename T> T* GetFeatureData() noexcept;
};
```

> **`RenderWorld` is now a legacy facade** over `RenderSceneStorage`, kept for focused
> runtime tests and older internal paths. New frame/extraction contracts use
> `RenderSceneSnapshot`, `RenderSceneStorage`, `RenderExtractionView`,
> `RenderFrameDataView`, or `RenderQueue` directly.

This is exactly the kind of boundary a renderer needs. It keeps rendering deterministic,
inspectable and easier to evolve.

---

## Frame Data Flow

The next diagram shows how live scene state turns into a rendered frame.

```text
ecs::World / Scene
  -> RenderSceneSnapshot
      -> ECSExtractor
          -> RenderSceneStorage
              -> RenderQueue
                  -> RenderGraph (FrameGraphStage)
                      -> FrameExecutionStage
                          -> IDevice / ICommandList / ISwapchain
                              -> Final Image
```

The mental model is simple:

1. scene state lives in the ECS
2. extraction builds stable frame-local data
3. render preparation builds queues and frame inputs
4. graph compilation defines executable frame order
5. the backend executes and presents the final image

---

## Render Representation

The extracted render state is built from lightweight render-facing proxy structures.

### RenderProxy

```cpp
struct RenderProxy
{
    EntityID       entity;
    MeshHandle     mesh;
    MaterialHandle material;
    math::Mat4     worldMatrix;
    math::Mat4     worldMatrixInvT;
    math::Vec3     boundsCenter;
    math::Vec3     boundsExtents;
    float          boundsRadius = 1.f;
    uint32_t       submeshIndex = 0u;
    uint32_t       layerMask    = 0xFFFFFFFFu;
    bool           castShadows  = true;
    bool           visible      = true;
    BufferHandle   boneBuffer;   // invalid if not skinned
};
```

`RenderProxy` is intentionally flat and render-oriented. It lets the renderer operate on
stable frame data instead of live scene component state.

### Lights and other feature data

There is **no** global `LightProxy` list on the render world. Per-feature frame data —
lights included — is written into the frame's feature-data registry during extraction
and read back by name/type. For example, the lighting AddOn extracts `LightComponent`s
into its own `LightingFrameData`, retrieved through
`GetFeatureData<…>()` / `GetOrCreateFeatureData<…>(name)`. This keeps optional rendering
features (lighting, shadows, GTAO, …) decoupled from the core proxy structure.

---

## Queue-Oriented Render Preparation

After extraction, visible render work is organized through `RenderQueue`, `DrawList` and
`DrawItem`.

```cpp
struct DrawItem
{
    SortKey        sortKey;
    MeshHandle     mesh;
    MaterialHandle material;
    EntityID       entity;
    uint32_t       layerMask = 0xFFFFFFFFu;

    BufferHandle gpuVertexBuffer;
    BufferHandle gpuIndexBuffer;
    BufferHandle boneBuffer;          // invalid if not skinned
    uint32_t     gpuIndexCount   = 0u;
    uint32_t     gpuVertexStride = 0u;
    uint32_t     submeshIndex    = 0u;

    uint32_t cbOffset = 0u, cbSize = 0u;
    uint32_t instanceCount = 1u, firstInstance = 0u;

    bool hasGpuData() const noexcept;
    bool operator<(const DrawItem& o) const noexcept; // by sortKey
};

struct DrawList
{
    RenderPassID          passId = StandardRenderPasses::Opaque();
    std::vector<DrawItem> items;
    bool                  sorted = false;
    void Sort();
};

struct RenderQueue
{
    std::vector<DrawList>                    lists;
    std::unordered_map<RenderPassID, size_t> indices;
    std::vector<PerObjectConstants>          objectConstants;
    uint32_t                                 activeShadowResolution = 0u;

    DrawList& GetOrCreateList(RenderPassID passId) noexcept;
    DrawList* FindList(RenderPassID passId) noexcept;
    void      SortAll();
};
```

That means the CPU-side renderer is queue-oriented rather than immediate-command
oriented — and the queue is keyed by **render pass id**, not a fixed set of categories.
Draw lists are created on demand per pass (opaque, shadow, transparent, UI, …) via
`GetOrCreateList(passId)`, which lets features contribute their own passes without
modifying a hard-coded queue layout.

The engine first decides what should be rendered in each pass, and only later turns that
prepared work into backend submission. This keeps frame preparation understandable and
keeps execution logic separate from scene analysis.

---

## Frame Stages

The frame pipeline is split into dedicated stage classes, run in order by the
orchestrator:

- `FrameExtractionStage` — reads world-facing scene state and builds extracted frame data
- `FrameConstantStage` — builds frame-level constants (camera/view/lighting globals)
- `FrameShaderStage` — collects and commits required shader variants
- `FrameUploadStage` — collects and commits GPU uploads (geometry, constants)
- `FrameGraphStage` — builds the executable render graph from prepared frame state
- `FrameExecutionStage` — submits the graph through the backend and presents

(Material collection/commit and queue building happen within this sequence; see the
`RenderFrameExecutionState` step list above. There is no single monolithic
"preparation" class — preparation is the constant/shader/upload trio.)

This staged model is one of the clearest signs that the engine is built with structure
rather than with one large mixed render path.

---

## Feature-Oriented Rendering

KROM uses a `FeatureRegistry` to extend renderer behavior through features instead of
hardwiring everything into one fixed path. Features implement `IEngineFeature`; they
register components, pass contributors, and frame data, and are initialized/shut down
with the renderer.

The `addons/forward` module demonstrates this with `ForwardFeature` (the forward / Forward+
render pipeline). Lighting, shadows, GTAO, outline, mesh rendering, camera-view building,
debug draw, particles and animation are all shipped as features that plug into the same
registry.

That gives the renderer room to grow while keeping the core architecture stable:

- core runtime systems stay centralized
- render behavior can be extended through features
- pass logic does not collapse into one giant renderer file

This is a strong design choice because it keeps the engine modular even as rendering
capabilities expand.

---

## Backend Abstraction

Backend execution is isolated behind interfaces such as `IDevice`, `ISwapchain`,
`ICommandList` and `IFence`.

```cpp
class IDevice
{
public:
    virtual bool Initialize(const DeviceDesc& desc) = 0;
    virtual void Shutdown() = 0;
    virtual void WaitIdle() = 0;

    virtual std::unique_ptr<ISwapchain> CreateSwapchain(const SwapchainDesc& desc) = 0;

    virtual BufferHandle       CreateBuffer(const BufferDesc& desc) = 0;
    virtual TextureHandle      CreateTexture(const TextureDesc& desc) = 0;
    virtual RenderTargetHandle CreateRenderTarget(const RenderTargetDesc& desc) = 0;

    virtual ShaderHandle   CreateShaderFromSource(...) = 0;
    virtual ShaderHandle   CreateShaderFromBytecode(...) = 0;
    virtual PipelineHandle CreatePipeline(const PipelineDesc& desc) = 0;

    virtual std::unique_ptr<ICommandList> CreateCommandList(QueueType queue = QueueType::Graphics) = 0;
    virtual std::unique_ptr<IFence>       CreateFence(uint64_t initialValue = 0u) = 0;

    virtual void BeginFrame() = 0;
    virtual void EndFrame() = 0;
};
```

Backends are registered as factories and selected by
`DeviceFactory::BackendType { Null, DirectX11, DirectX12, OpenGL, Vulkan }`. This
boundary is critical:

- the renderer decides what work needs to happen
- the backend decides how that prepared work is executed

That split prevents API-specific execution details from leaking upward into high-level
frame preparation code.

---

## Resource Runtime Layer

KROM separates renderer structure from GPU resource lifecycle through dedicated runtime
systems such as:

- `GpuResourceRuntime`
- `ShaderRuntime`
- `MaterialSystem`
- `EnvironmentSystem` (IBL)
- `PipelineCache`

That matters because shader management, GPU uploads, material preparation, environment
lighting and pipeline reuse are handled by dedicated subsystems instead of being
scattered across the renderer.

This leads to a cleaner internal structure:

- `MaterialSystem` owns material-side state
- `ShaderRuntime` resolves shader-related runtime work (variants, environment state)
- `GpuResourceRuntime` realizes GPU resources
- `EnvironmentSystem` owns IBL environments
- `PipelineCache` avoids redundant pipeline creation

That separation makes the renderer easier to maintain as the engine grows.

---

## Render Graph Layer

KROM includes a dedicated render graph layer under `rendergraph/`.

Key parts include:

- `RenderGraph` — passes, resources, and their dependencies
- `CompiledFrame` — the validated, executable result of compiling the graph
- `ResourceAliaser` — transient-resource lifetime/aliasing analysis

The pipeline that *populates* the graph is feature-driven: the active render pipeline
(e.g. `StandardFramePipeline` from the forward AddOn) builds the graph from a frame
recipe and the registered pass contributors.

This means the engine does not stop at queue building. It also compiles prepared work
into a validated execution structure that reasons about:

- pass order
- resource usage and aliasing
- frame compilation
- executable frame structure

This is the layer where prepared renderer data becomes actual frame execution logic.

---

## Parallelism Model

The engine uses two distinct CPU work systems.

### JobSystem

`jobs::JobSystem` provides worker-thread based execution for local data-parallel work
(e.g. parallel extraction and draw-list building).

### TaskGraph

`jobs::TaskGraph` provides explicit task dependency management for staged work.

That is the right split. A renderer benefits from separating:

- coarse frame dependency structure
- local data-parallel work

This keeps the overall frame stable and understandable while still allowing heavy
workloads to scale across worker threads.

---

## Why the Architecture Works

The architecture holds together because every layer has a clear role.

- `PlatformRenderLoop` owns runtime and platform flow
- `ecs::World` owns authoritative scene state
- `Scene` provides higher-level scene convenience
- extraction converts scene state into render-facing frame data
  (`RenderSceneSnapshot` / `RenderSceneStorage`)
- `RenderQueue` organizes renderable work per render pass
- `RenderFrameOrchestrator` executes the frame pipeline in explicit, inspectable stages
- `rendergraph` compiles prepared work into an executable, validated frame structure
- backend interfaces isolate API-specific execution

That gives the engine a stable shape.

The important point is not just that the engine is modular. It is that the boundaries are
placed in the right places: runtime is separate from rendering, rendering is separate
from live scene state, feature behavior is separate from the core path, and backend
execution is separate from frame preparation.

That is why the architecture remains understandable and extensible even as the renderer
becomes more capable.
