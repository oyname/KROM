# KROM ENGINE API OVERVIEW

This document provides a high-level overview of how to use the KROM ENGINE API.

It focuses on the practical engine path, the main objects you interact with, and the minimal steps required to get a scene on screen.

This document is intentionally usage-oriented.
For architectural structure, see the Architecture document.
For exact types, methods, fields, and function behavior, see the API Reference.

> **Note on namespaces.** The code samples below assume `using namespace engine;` for
> readability. Fully qualified, the main objects live in `engine::renderer`
> (`PlatformRenderLoop`, `MaterialSystem`, `RenderView`), `engine::ecs` (`World`,
> `ComponentMetaRegistry`), and `engine` (`Scene`). AddOn factories live under
> `engine::addons::*` and `engine::renderer::addons::*`.

---

## Practical Usage Flow

The following diagram shows the practical user-facing path from initialization to the per-frame loop.

```text
Register Components (ComponentMetaRegistry)
  -> Register Render Features / AddOns (RenderSystem)
      -> Initialize PlatformRenderLoop
          -> Create ecs::World (with the registry)
              -> Create Scene
                  -> Create MaterialSystem
                      -> Create Camera + Renderable Entities
                          -> Build a RenderView (by hand or via the camera AddOn)
                              -> Update ECS State
                                  -> loop.Tick(world, materials, view, timing)
                                      -> Update ECS State
                                      -> loop.Tick(...)
```

The short version is:

1. register the component metas you will use
2. register the render features / AddOns you need
3. initialize the runtime
4. create the ECS world (passing the registry) and scene
5. set up materials, camera and renderable entities
6. produce a `RenderView` and call `Tick(...)` every frame

> **Why steps 1 and 2 come first.** KROM is AddOn-based. The renderer extracts and
> renders **only** what registered features and component metas describe. Skip them and
> `Tick(...)` runs successfully but produces an empty frame.

---

## 1. Purpose

This document answers one practical question:

**How do you use the engine to create a window, set up rendering, build a scene, and run frames?**

KROM can be approached in two ways:

1. **Direct engine path**
   - explicit setup of platform, window and renderer
   - explicit component and feature/AddOn registration
   - direct access to ECS world, scene data and rendering flow
   - intended for real engine usage, custom setup and advanced work

2. **Simplified entry path**
   - the `Krom::` wrapper (see `KromWrapper`) wraps common setup steps,
     including component and feature registration
   - reduces boilerplate
   - intended for quick demos and onboarding

This overview focuses on the **direct engine path**, because that is the path you use when working with the engine itself.

---

## 2. The Main Objects You Use

When using the engine directly, you mainly interact with a small set of runtime objects.

### `ecs::ComponentMetaRegistry`

This describes which component types exist and how they are reflected.

You use it to:

- register the core components (`RegisterCoreComponents`)
- register per-AddOn components (mesh renderer, camera, lighting, ...)
- hand the populated registry to the `ecs::World`

The world is constructed **from** a registry — it has no default constructor.

### `PlatformRenderLoop`

This is the outer runtime entry point.

You use it to:

- initialize the runtime (platform, window, device, renderer)
- register render features / AddOns through its `RenderSystem`
- run one frame at a time (`Tick`)
- shut the runtime down cleanly

### `RenderSystem` (via `loop.GetRenderSystem()`)

This is where rendering features / AddOns are registered.

You use it to:

- register features such as the mesh renderer, lighting, and a render pipeline
  (Forward or Forward+)
- read back per-frame statistics

### `ecs::World`

This is the core scene data container.

You use it to:

- create entities
- add components
- query scene data
- store the actual world state

### `Scene`

This is the higher-level scene helper built on top of the ECS world.

You use it to:

- create named entities
- build hierarchies
- manage parent-child relationships
- propagate transforms

### `MaterialSystem`

This manages material-side data used by rendering.

You use it to:

- create or register material state
- provide material data used by renderable entities

### `RenderView`

This is a plain value type describing the active view for one frame.

It carries view/projection matrices, camera position/forward, and per-view toggles
(shadows, bloom, ambient occlusion, background mode, layer masks).

You either:

- fill its matrices directly, or
- let the camera AddOn build it from a camera entity.

`RenderView` is **not** itself a camera entity — it is the per-frame snapshot the
renderer consumes.

---

## 3. Basic Usage Flow

The direct usage flow is:

1. build a `ComponentMetaRegistry` and register the component metas
2. create the platform and window description
3. register render features / AddOns on the render system
4. initialize `PlatformRenderLoop`
5. create the `ecs::World` (passing the registry)
6. create scene entities (camera + renderables)
7. create or assign materials
8. produce a `RenderView`
9. call `Tick(...)` every frame
10. shut the runtime down when finished

That is the practical engine path.

---

## 4. Typical Startup Path

A typical startup sequence looks like this:

1. register components into a `ComponentMetaRegistry`
2. register render features / AddOns
3. define a window description
4. initialize the runtime loop
5. create `ecs::World` from the registry
6. create `Scene`
7. create `MaterialSystem`
8. create camera and scene entities
9. build a `RenderView`
10. call `Tick(...)` every frame

In short:

- register components and features first
- initialize runtime second
- build scene third
- run frames last

---

## 5. Minimal Initialization Example

A minimal direct initialization flow looks like this:

```cpp
using namespace engine;

// 1. Component metas: the renderer only sees what is registered here.
ecs::ComponentMetaRegistry registry;
RegisterCoreComponents(registry);
addons::mesh_renderer::RegisterMeshRendererComponents(registry);
addons::camera::RegisterCameraComponents(registry);
addons::lighting::RegisterLightingComponents(registry);

// 2. Runtime loop + render features / AddOns.
renderer::PlatformRenderLoop loop;
loop.GetRenderSystem().RegisterFeature(addons::mesh_renderer::CreateMeshRendererFeature());
loop.GetRenderSystem().RegisterFeature(addons::lighting::CreateLightingFeature());
loop.GetRenderSystem().RegisterFeature(renderer::addons::forward::CreateForwardFeature());

// 3. Platform + device, then initialize.
platform::WindowDesc windowDesc{};
windowDesc.width  = 1280u;
windowDesc.height = 720u;
windowDesc.title  = "KROM";

loop.Initialize(backendType, platform, windowDesc);   // eventBus / deviceDesc optional

// 4. World (built from the registry) + scene-side systems.
ecs::World     world(registry);
Scene          scene(world);
MaterialSystem materials;

// 5. Per-frame view (filled by hand here; the camera AddOn can build it instead).
renderer::RenderView view{};
view.view           = /* world-to-view matrix */;
view.projection     = /* projection matrix */;
view.cameraPosition = { 0.f, 0.f, -5.f };
view.cameraForward  = { 0.f, 0.f,  1.f };

while (running)
{
    // update ECS / camera state here
    loop.Tick(world, materials, view, timing);
}

loop.Shutdown();
```

This is the basic pattern you will follow in most applications using the direct path.

> The exact feature set you register depends on what you render: swap
> `CreateForwardFeature()` for the Forward+ pipeline when you need many dynamic lights,
> and add the shadow / post-processing AddOns as required.

---

## 6. Creating Scene Content

Once the runtime is initialized, you create scene content through the ECS world and scene helpers.

Typical steps are:

1. create an entity
2. add transform data (`TransformComponent` + `WorldTransformComponent`)
3. add render-related components (mesh, material, bounds)
4. assign mesh and material references (meshes/textures come from the asset pipeline)
5. ensure the object is visible to the active view

In practice, the renderer works from scene state. You do not submit loose draw calls manually. You create entities and resources, and the renderer builds the frame from that data.

The corresponding component metas must have been registered up front (see Section 5),
otherwise the components are invisible to extraction.

---

## 7. Creating a Camera

To render anything visible, you need a valid `RenderView` for the frame.

There are two ways to obtain it:

- **By hand:** fill `view.view`, `view.projection`, `view.cameraPosition`, and
  `view.cameraForward` yourself (as in Section 5).
- **Via the camera AddOn:** create a camera entity (transform + camera components),
  then let the camera view builder produce a `RenderView` from it each frame.

Either way, the `RenderView` is what `Tick(...)` consumes. If no valid view exists, the
renderer has no meaningful point of view and the scene will not render as expected.

---

## 8. Creating a Visible Object

A visible object is created through normal scene and resource setup.

Typical requirements are:

- a valid transform
- renderable state (mesh + material components)
- mesh reference
- material reference
- valid bounds for visibility processing

This is important:

A first visible object is **not** a special case in the API. It follows the same engine model as every other rendered object.

That means the normal workflow is:

1. create entity
2. add required components (whose metas were registered)
3. assign mesh/material data
4. run the frame

---

## 9. What Happens Per Frame

From the user side, one frame usually means one call to:

```cpp
loop.Tick(world, materials, view, timing);
```

That single call drives the full renderer frame for the current world state, using the
features you registered before `Initialize`.

From a practical usage perspective, the frame loop normally looks like this:

1. update your game or application state
2. write changes into ECS components
3. update camera/view state (matrices in the `RenderView`)
4. call `Tick(...)`
5. repeat

So the normal user-facing model is:

- update scene state
- let the renderer consume it
- render one frame

---

## 10. The Practical Mental Model

The simplest correct way to think about the engine is this:

- `ComponentMetaRegistry` declares which components exist
- registered **features / AddOns** declare what the renderer can do
- `ecs::World` stores the scene
- `Scene` helps build and organize that scene
- `MaterialSystem` provides material-side data
- `RenderView` defines how the scene should be viewed this frame
- `PlatformRenderLoop` renders the current frame

That is the core usage model.

You are not expected to build a frame manually. You register your components and
features, build scene state, and then run the renderer.

---

## 11. Minimal Requirements for Something Visible

To get a visible result on screen, you generally need **four** pieces:

### Registered components and features

The component metas and the render features / AddOns (mesh renderer, lighting, a render
pipeline) must be registered before initialization. Without them, frames run but render
nothing.

### A working runtime

The platform loop and renderer must initialize successfully.

### A valid view

A `RenderView` with sensible view/projection matrices (filled by hand or built from a
camera entity).

### A renderable scene object

At least one entity must contain the data required to be treated as visible render
content (transform, mesh, material, bounds).

If any of those pieces is missing, you will usually get an empty frame.

---

## 12. First Visible Scene: Practical Path

A first visible scene usually follows this path:

1. register component metas
2. register render features / AddOns
3. initialize the runtime
4. create the ECS world from the registry
5. create the scene helper
6. create a camera (or build a `RenderView` by hand)
7. create one renderable object
8. assign its mesh and material
9. run the frame loop

That is the basic path from engine startup to first image.

---

## 13. Summary

The practical usage path is:

1. register components (`ComponentMetaRegistry`)
2. register render features / AddOns (`loop.GetRenderSystem().RegisterFeature(...)`)
3. initialize `PlatformRenderLoop`
4. create `ecs::World` (from the registry)
5. create `Scene`
6. create `MaterialSystem`
7. build a `RenderView`
8. call `Tick(...)` every frame
9. shut down cleanly when done

The important point is simple:

**You use KROM by registering your components and features, building scene state, and running the frame loop.**

You do not work in an immediate-mode rendering style.
You create and update world data, and the engine renders that world for you.
```
