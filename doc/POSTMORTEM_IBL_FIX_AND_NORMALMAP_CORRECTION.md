# Postmortem — IBL Flicker & Normalmap Artifacts

## Summary

Two visually similar rendering issues were observed

1. Bright flickering on cube surfaces during rotation
2. White speckled artifacts on the ground

Although both appeared related at first glance, they had completely different root causes

- The flicker was caused by double-counted lighting in the IBL pipeline
- The white dots were caused by incorrect normal map orientation handling

Both issues were fixed independently.

---

## Issue 1 — Bright Flickering on Cube (IBL  Reflection Path)

### Symptom

- Surfaces briefly flashed very bright  almost white
- Occurred only during camera or object rotation
- Independent of normal maps

---

### Root Cause

The HDR environment map used for IBL contained a strong sun hotspot.

At the same time, the scene also used an analytical directional light representing the sun.

Result
- The sun contribution was effectively counted twice
  - once via direct lighting
  - once via IBL reflection (prefiltered cubemap)

Because specular reflections depend heavily on view direction

- During rotation, the reflection vector aligned with the hotspot
- This caused sudden high-intensity spikes → visible as flicker

---

### Fix

The IBL baking process was corrected in `IBLBaker.cpp`

1. Highlight compression
   - Reduces extreme HDR intensity range before processing

2. Local hotspot removal
   - Detects and removes very small, extremely bright regions (sun)
   - Prevents them from dominating the prefiltered environment

3. Apply before generating
   - Irradiance map
   - Prefilter cubemap

Relevant areas
- `IBLBaker.cpp (265–298)`
- `IBLBaker.cpp (300–360)`
- `IBLBaker.cpp (687–692)`

---

### Cache Invalidation

To ensure the fix takes effect

- `kBakeVersion` was incremented in
  - `IBLCacheSerializer.hpp (39–41)`

Without this
- Old cached cubemaps (with hotspot) would still be used
- Fix would appear inconsistent or ineffective

---

## Issue 2 — White Dots on Ground (Normal Map Path)

### Symptom

- Small white speckles  dots on surfaces
- Visible mostly on textured ground
- Dependent on normal maps

---

### Root Cause

Normal maps with `_nor_dx_` naming were being flipped twice

1. Importer stage
   - Applied a backend-agnostic flip

2. Scene setup
   - Applied an additional flip for non-DX11 backends

Result
- Normals were inconsistently oriented
- Shading normals occasionally pointed in invalid directions
- Caused localized lighting spikes → visible as white dots

---

### Fix

Corrected the normal map handling

- Removed the blind flip in the importer
- Kept only the intentional backend-specific correction in the scene

Changes

- `TextureImporter.cpp (268–300)`
  - ❌ Removed global flip

- `PbrShadowScene.cpp (17–24, 286–291)`
  - ✅ Retained controlled flip logic

---

## Key Takeaways

### 1. IBL Must Not Contain Primary Light Sources
- HDR environments with strong sun hotspots must be
  - cleaned
  - compressed
  - or filtered

Otherwise
- Double lighting is inevitable when combined with analytic lights

---

### 2. Cache Versioning Is Critical
- Any change in bake logic requires
  - explicit version bump
- Otherwise
  - debugging becomes misleading due to stale data

---

### 3. Normal Map Conventions Must Be Centralized
- Flips must not be duplicated across pipeline stages
- Responsibility must be clearly defined
  - Importer OR runtime — not both

---

### 4. Visual Artifacts Can Be Misleading
Two completely different bugs produced visually similar symptoms

 Symptom              Actual Cause              
----------------------------------------------
 Bright flicker      IBL double sun           
 White speckles      Normal map orientation   

---

## Final State

After fixes

- No more flickering during rotation
- No more white dots on surfaces
- IBL pipeline is stable and physically consistent
- Normal map handling is deterministic across backends