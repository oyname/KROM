# Post Mortem: DebugDraw Depth-Test via Separate SceneDepth Resource

## Summary

An attempt was made to wire a dedicated `SceneDepth` render graph resource to the
DebugDraw pass so that debug lines would be occluded by scene geometry. The change
compiled cleanly but introduced a structural inconsistency between the render graph
declaration and the actual GPU render pass binding. The fix was reverted in favor of
a permanent overlay mode (depth test disabled, no depth attachment).

## What Was Attempted

Variant C: DebugDraw with optional depth test, controlled by `DebugDrawConfig::depthTest`.

Changes applied:

- `StandardFrameResourceID::SceneDepth` added as a core resource (`D32_FLOAT`,
  `RGResourceKind::DepthStencil`, viewport-sized, always allocated).
- `MainOpaque` pass: `AddAccess(SceneDepth, WriteDepthStencil)` added.
- `DebugDraw` pass: `AddAccess(SceneDepth, ReadDepthStencil)` added.
- `DebugDraw.cpp` PSO: `pso.depthFormat` changed from `D24_UNORM_S8_UINT` to `D32_FLOAT`.
- `DebugDrawConfig::depthTest` kept at `true` as default.

## What Went Wrong

`ConfigureRenderPass` sets `pass.renderPass.targetResourceName` to `HDRSceneColor`.
`BeginRenderPass` resolves the render target from that name alone:

```cpp
rp.renderTarget = ctx.GetRenderTarget(targetId);  // HDRSceneColor only
```

There is no separate depth attachment handle in `RenderPassBeginInfo`. The runtime
does not pick up `SceneDepth` from the access list and bind it as the DSV. Instead,
`GpuResourceRuntime` automatically creates an implicit depth buffer attached to the
`HDRSceneColor` render target, using format `D24_UNORM_S8_UINT`.

Result: two independent depth surfaces existed simultaneously.

| Surface | Format | Bound to DebugDraw pass |
|---|---|---|
| HDRSceneColor implicit depth | D24_UNORM_S8_UINT | Yes (via BeginRenderPass) |
| SceneDepth (named resource) | D32_FLOAT | No (declared but never bound) |

The render graph declared a `ReadDepthStencil` access on `SceneDepth`, but the
actual GPU command used the HDRSceneColor implicit depth. The `SceneDepth` resource
was allocated, transitioned, and tracked — but never attached.

On top of that, the PSO declared `pso.depthFormat = D32_FLOAT`, while the surface
actually bound at draw time was `D24_UNORM_S8_UINT`. On Vulkan this is a hard
validation error. On DX11 the behavior is undefined.

## Root Cause

`RenderPassBeginInfo` carries a single `renderTarget` handle. It has no `depthStencil`
field. The recipe system and `BeginRenderPass` were designed for the common case where
color and depth are co-located on the same render target object.

Separating them requires `RenderPassBeginInfo` to carry an explicit depth handle:

```cpp
struct RenderPassBeginInfo {
    RenderTargetHandle renderTarget;
    TextureHandle      depthStencil;   // missing
    // ...
};
```

Without this, any attempt to bind a named depth resource to a pass that also writes a
separate color render target is structurally unsupported. The render graph access
declaration and the GPU binding are decoupled with no enforcement between them.

## Decision

**Variant A — permanent overlay** was chosen:

- `DebugDrawConfig::depthTest = false` (default, overlay behavior)
- `pso.depthStencil.depthEnable = false`
- `pso.depthFormat = Format::Unknown`
- No `SceneDepth` resource. No depth access in the DebugDraw recipe.

Debug lines are always visible, regardless of occluding geometry. This is often the
correct behavior for spatial debugging anyway — an occluded AABB gives no information.

## Path to Variant B (Depth-Tested Debug Lines)

Prerequisite: `RenderPassBeginInfo` must support separate color and depth attachments.

Steps required:

1. Add `TextureHandle depthStencil` (or `RGResourceID`) to `RenderPassBeginInfo`.
2. Extend `ConfigureRenderPass` / `FrameRecipePassDesc` to carry a separate depth
   resource name alongside the color target name.
3. In `GpuResourceRuntime::BeginRenderPass`, resolve the depth handle from the named
   resource and bind it as the DSV.
4. Stop auto-creating an implicit depth buffer when a named depth resource is
   explicitly declared for the pass.
5. Re-introduce `SceneDepth` as a core resource (D32_FLOAT, DepthStencil).
6. MainOpaque writes it. DebugDraw reads it (when `depthTest = true`).
7. PSO: `pso.depthFormat = D32_FLOAT`, `pso.depthStencil.depthEnable = m_config.depthTest`.

Until step 1–4 are in place, `DebugDrawConfig::depthTest = true` must not be set.
The field is kept in the struct as a forward-compatibility marker only.

## Lessons

- A render graph access declaration is not a binding guarantee. The actual GPU binding
  is determined by `BeginRenderPass`, which currently only consults the color target.
  Access declarations drive barriers and lifetime tracking, not attachment setup.

- Format mismatches between the PSO (`pso.depthFormat`) and the surface bound at
  draw time are silent on DX11 and fatal on Vulkan. Any change to depth format must
  be verified against what `BeginRenderPass` actually attaches, not what the render
  graph resource declares.

- When `GpuResourceRuntime` auto-creates an implicit depth buffer for a render target,
  it is invisible to the recipe system. Named depth resources and implicit depth
  buffers can coexist without either side knowing about the other.
