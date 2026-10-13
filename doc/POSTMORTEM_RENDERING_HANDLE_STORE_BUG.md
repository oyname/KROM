# Rendering Bug Post Mortem

## Summary

DX11 and OpenGL showed sporadic bad frames after the engine had been running for a while. Typical symptoms were:

- flat gray output
- clear-color-only frames
- missing geometry
- backend-dependent presentation of the same underlying fault

Vulkan remained stable.

The root cause was a bug in the handle-store generation comparison used by the DX11 and OpenGL backends. After enough handle reuse, valid frame-local buffer handles could no longer be resolved from the backend resource stores, even though the handles themselves still looked valid on the engine side.

## Impact

The bug affected frame-local constant buffer resources, especially:

- `perFrameCB`
- `perObjectArena`

When those buffers stopped resolving correctly:

- DX11 often rendered with incomplete or stale constant-buffer state
- OpenGL often collapsed earlier into flat Opaque output
- Vulkan did not exhibit the bug because its store implementation already handled generation masking correctly

## User-Visible Symptoms

- DX11: occasional wrong or flat output, especially after longer runtime
- OpenGL: occasional clear-color or flat-color frames
- Vulkan: stable, no visible anomaly from this bug

The problem was intermittent and became easier to reproduce after the engine had been running for some time, which matched a reuse / generation-wrap issue rather than a deterministic shader or pass setup bug.

## Root Cause

The DX11 and OpenGL backend stores tracked slot generations internally as full `uint32_t` counters, but the engine handles only carry the masked generation bits.

That meant:

1. a resource handle was created with a masked generation value
2. later, after enough slot reuse, the backend slot generation advanced beyond that masked range
3. `Get()` / `Remove()` compared the wrong representation
4. valid handles stopped resolving

As a result, the engine still believed `perFrameCB` and `perObjectArena` were valid, but the backend resource store lookup failed for those handles.

This was confirmed by diagnostics:

- the shared bind path intended to bind `PerFrame + PerObject + PerMaterial`
- in bad DX11 frames only `PerMaterial` resolved in the backend
- `PerFrame` and `PerObject` were present as engine handles but missing at backend lookup time

## Why Vulkan Was Not Affected

Vulkan used a different store implementation that already compared generations correctly using the masked handle bits.

So the same engine-level workflow was stable there, which is why Vulkan initially looked like evidence against a shared engine bug. In reality, Vulkan simply did not inherit the same store bug.

## Investigation Timeline

Several hypotheses were explored before the actual cause was isolated:

- constant buffer binding order
- descriptor / SRV / RTV hazards
- tonemap pass problems
- fullscreen pass state
- material lifetime and deferred destruction
- focus / present / resize interactions

Those investigations were useful because they progressively ruled out:

- RenderGraph wiring
- tonemap source handle mismatch
- material binding as the primary cause
- SRV / RTV aliasing in DX11

The decisive turning point was comparing:

- current bad frame vs last healthy frame
- intended constant-buffer bindings vs backend lookup success

That made it clear that the failure was not "the wrong pass is running", but "the backend can no longer resolve specific valid-looking frame-local buffer handles".

## Fix

The store logic in DX11 and OpenGL was corrected so that `Get()` and `Remove()` compare the masked generation bits consistently with the handle representation.

This aligned those backends with the already-correct Vulkan implementation.

## Additional Hardening

Independent of the root cause, the constant-buffer binding code was hardened:

- DX11 now explicitly unbinds constant-buffer slots on invalid lookup instead of silently leaving old state alive
- OpenGL now explicitly unbinds UBO bindings on invalid lookup instead of preserving stale state
- OpenGL constant-buffer range binding was also made more consistent

These changes were not the root-cause fix, but they improve renderer correctness and make similar bugs easier to detect.

## Lessons Learned

### 1. Handle stores must compare the same representation they emit

If a handle carries masked generation bits, the store must compare against the same masked form. Mixing full internal counters with masked external handles is a classic long-runtime failure mode.

### 2. Vulkan stability does not prove the engine path is correct

Different backend implementations can mask or avoid the same engine bug. Vulkan was useful as a control, but not as proof that shared engine logic was sound.

### 3. Intent vs backend-resolution diagnostics are extremely valuable

The most useful diagnostic was not "what got drawn", but:

- what the engine intended to bind
- what the backend actually resolved

That immediately narrowed the problem from rendering behavior to resource-lifetime / lookup behavior.

### 4. Defensive unbinding is the correct backend policy

If a lookup fails, leaving previous CB / UBO state alive is not robust. Explicitly unbinding invalid slots is a better contract and prevents stale-state masking.

## Final Outcome

After fixing the generation comparison bug in the DX11 and OpenGL stores, the renderer ran significantly longer without reproducing the anomaly, indicating that the actual long-runtime root cause had been removed.

## Relevant Files

- `addons/dx11/DX11Device.hpp`
- `addons/opengl/OpenGLDevice.hpp`
- `addons/dx11/DX11CommandList.cpp`
- `addons/opengl/OpenGLCommandList.cpp`
- `include/renderer/AnomalyDiagnostics.hpp`
- `addons/forward/ForwardFeature.cpp`
