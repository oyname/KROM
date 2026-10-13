# Post Mortem: Vulkan Render-Path Regression

## Summary

The Vulkan backend regressed badly under the `PbrShadowScene` stress path while DX11 and OpenGL stayed near expected frame times.

Initial Vulkan behavior:

- Frame time around `70+ ms`
- Recording time around `60+ ms`
- Descriptor binds around `474` per frame

Final Vulkan behavior after the fixes:

- Frame time around `22.5-24.0 ms`
- Recording time around `16.5-18.3 ms`
- Descriptor binds reduced to `6` per frame

The root cause was not queue submit, present, swapchain acquire, or CPU-side frame-parallel work. The real issue was a bad Vulkan binding model in the draw hot path, amplified by sort order that was not state-friendly enough for an explicit API.

## Backend Comparison

| Backend | Frame (ms) | Execute (ms) | Record (ms) | Present (ms) | Remat | Alloc | Update | Bind |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| OpenGL | 19.1-19.5 | 14.0-14.2 | 13.4-13.7 | 0.42-0.44 | 0 | 0 | 0 | 0 |
| DX11 | 19.7-20.2 | 14.4-15.3 | 14.3-15.1 | 0.07-0.14 | 0 | 0 | 0 | 0 |
| Vulkan | 22.5-24.0 | 16.8-18.5 | 16.5-18.3 | 0.07-0.10 | 6 | 6 | 6 | 6 |

## Symptom

Vulkan was much slower than DX11 and OpenGL in the same scene, despite similar scene size and upload volume.

Observed stress metrics before the main fixes:

- `workers=11`
- `parallel ~0.05-0.11 ms`
- `prepare/shaders/materials` all tiny
- `backendAcquire/backendSubmit/backendPresent` also tiny
- `record` dominated the frame

This meant the backend was losing time mostly during command recording, not during presentation or queue submission.

## Root Cause

There were two coupled problems.

### 1. Per-object data forced draw-by-draw Vulkan descriptor rebinding

The old Vulkan path represented `PerObject` through descriptor-set state and dynamic uniform buffer offsets.

That meant:

- nearly every object changed the dynamic offset
- changing the offset made the bound descriptor state differ
- the command list called `vkCmdBindDescriptorSets()` again for almost every draw

This created the observed:

- `bind=474`
- very high `record`
- very high `execute`

This problem did not show up in the same way on DX11 or OpenGL because they do not use Vulkan-style descriptor-set rebinding for the same engine-level operation.

### 2. Opaque sorting was not sufficiently state-friendly for an explicit API

Opaque objects were sorted primarily by:

- pass
- layer
- pipeline hash
- fine depth

This was good enough for correctness and acceptable for DX11/OpenGL, but still left Vulkan doing too many state transitions between different materials that shared the same pipeline.

The engine therefore kept bouncing between material states more than necessary.

## What We Measured

The added diagnostics showed clearly that:

- frame-parallel preparation was not the bottleneck
- Vulkan queue submit and present were not the bottleneck
- descriptor rematerialization was initially severe
- after descriptor caching, rematerialization dropped, but bind count stayed high
- the remaining hot path was still per-draw binding churn

The most useful counters were:

- `record`
- `execute`
- `remat`
- `alloc`
- `update`
- `bind`

## Fixes Applied

### 1. Added instrumentation

Added timing and backend diagnostics for:

- frame-parallel preparation stages
- graph build
- upload collection and commit
- command recording
- submit
- present
- backend descriptor activity

This made the Vulkan bottleneck visible instead of guessed.

### 2. Fixed descriptor rematerialization churn

Descriptor allocation and update behavior was improved by:

- caching materialized Vulkan descriptor sets within the frame
- stopping needless rematerialization on offset-only changes
- fixing descriptor pool accounting for static vs dynamic uniform-buffer descriptor types

This brought:

- `remat` down dramatically
- `alloc` down dramatically
- `update` down dramatically

But it did not yet solve the bind storm.

### 3. Moved Vulkan `PerObject` away from per-draw descriptor rebinding

The main structural fix was:

- keep shared HLSL shader source
- use a Vulkan-specific compile path via define
- feed `PerObject` through push constants on Vulkan
- leave DX11/OpenGL on the old `cbuffer PerObject : register(b1)` path

This removed the worst Vulkan-only per-draw binding cost while keeping the engine shader model unified.

### 4. Fixed push-constant shader integration and matrix packing

Two correctness issues appeared during the Vulkan push-constant rollout:

- DXC SPIR-V required `[[vk::push_constant]]` on a global struct variable, not on a `cbuffer`
- the initial matrix packing was wrong because the engine stores matrices column-major but the Vulkan helper path needed explicit row packing

Fixing those issues restored correct object transforms and stable Vulkan shader compilation.

### 5. Made opaque sorting state-friendlier

Opaque `SortKey` ordering was changed so that Vulkan sees much longer runs of compatible state.

Instead of effectively favoring fine depth too early, the key now groups by:

- pass
- layer
- pipeline hash
- material key
- coarse depth

This preserved front-to-back intent while making explicit APIs much happier.

## Why Vulkan Was Hit Harder Than DX11/OpenGL

DX11 and OpenGL hide more binding and state-management work behind the driver.

Vulkan is explicit:

- bad binding frequency decisions show up directly in CPU recording cost
- descriptor churn is not automatically hidden by the driver
- sort order matters more when bindings are explicit and expensive

So the same engine-level scene could look acceptable on DX11/OpenGL while Vulkan exposed the architectural weakness immediately.

## Outcome

The final result is that Vulkan is no longer catastrophically slower.

Before:

- Vulkan was tens of milliseconds behind
- command recording was the dominant failure point

After:

- Vulkan is only a few milliseconds behind DX11/OpenGL
- descriptor-set binds dropped from `474` to `6`
- descriptor rematerialization remained low
- frame time is now within a competitive range for the current renderer architecture

## Lessons Learned

- Explicit APIs punish weak binding models quickly.
- Descriptor caching alone is not enough if per-object data still forces rebinding.
- Sort order is part of backend performance architecture, not just visual correctness.
- Shared shader source does not mean shared runtime data path; Vulkan-specific feeding can still be necessary.
- Performance work without instrumentation wastes time.

## Future Recommendations

- Keep the Vulkan `PerObject` push-constant path unless object payload size grows beyond what is practical.
- Preserve state-friendly opaque sorting as the default renderer behavior.
- Add a regression test or stress validation path that watches:
  - `record`
  - `bind`
  - `remat`
  - `frame`
- When DX12 is added later, do not repeat the old per-draw descriptor-binding model. Use the Vulkan lessons directly.

