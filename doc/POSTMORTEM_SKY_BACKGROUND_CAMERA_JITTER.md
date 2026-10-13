# Postmortem - Sky Background Camera Jitter

## Summary

The editor scene runtime showed visible jitter in the HDR/IBL background while the main camera was moving.

The scene itself loaded correctly:

- editor project and scene were loaded
- main camera was found
- meshes rendered
- HDR environment was active
- the image was stable while the camera was not moving

The important clue was:

```text
The farther the camera moved away, the worse the jitter became.
```

That made the final root cause clear: this was not a camera smoothing problem, not GTAO, and not scene loading. It was floating-point precision loss in the sky direction calculation.

---

## Symptom

- HDR/IBL background appeared to wobble or stutter during camera movement
- objects could appear slightly unstable because the moving background made the whole frame feel unstable
- stopping camera movement made the image calm again
- the editor view did not show the same problem
- the problem became more visible as the camera position grew farther from the origin

---

## False Leads

Several areas looked suspicious during debugging:

- camera input smoothing / inertia
- variable render delta time
- wrapper frame order
- GTAO
- camera scale
- skybox translation handling

Some of these were real cleanup opportunities, but they were not the final cause of the background jitter.

---

## Root Cause

The forward sky shader reconstructed the environment lookup direction through world-space positions.

Earlier code used the inverse view-projection matrix to reconstruct points on the near/far plane and then derive a direction.

Later this was improved to:

```text
viewDir = normalize(worldFar - cameraPositionWS)
```

This removed the worst translation dependency, but it still had a precision problem.

At large world coordinates, both `worldFar` and `cameraPositionWS` are large floating-point values. Subtracting two large, nearby values to get a direction causes precision loss.

Result:

- near the origin, the error is small
- farther from the origin, the error grows
- the cubemap sample direction changes slightly from frame to frame
- the HDR background appears to jitter while the camera moves

This matched the observed behavior exactly.

---

## Why The Editor Looked Better

The editor camera path is simpler and usually operates around a controlled editor camera state. It does not expose the same runtime movement path and large accumulated camera coordinates in the same way.

This made the issue look like a runtime wrapper or input problem at first, even though the visual artifact was ultimately in the sky shader.

---

## Fix

The sky background must not depend on camera world position at all.

For an infinitely distant environment map, the lookup direction should depend only on:

- screen position
- projection
- camera rotation

It must not depend on:

- camera translation
- camera scale
- world-space far point subtraction

The final shader computes the sky direction explicitly from the camera basis:

Conceptually:

```text
viewDir = normalize(cameraRight * screenX + cameraUp * screenY + cameraForward)
```

This is the standard skybox behavior:

- camera translation is ignored
- camera rotation is respected
- roll around the camera's own Z axis is respected
- no large world-space float subtraction is performed

The camera basis is taken from frame constants:

- `cameraRight` from the view matrix
- `cameraUp` from the view matrix
- `cameraForward` from `cameraForwardWS`

The projection focal values are still used so the screen ray matches the camera FOV/aspect.

Changed file:

- `addons/forward/ForwardFeature.cpp`

---

## OpenGL Specific Correction

OpenGL needed one additional correction in the fullscreen sky path.

The OpenGL fullscreen sky vertex shader uses a different vertical framebuffer convention from the HLSL path. After the translation-free sky fix, this showed up as:

- HDR texture upside down
- roll around Z looked mostly correct
- pitch around X felt wrong

The fix was to flip the screen-ray Y coordinate in the GLSL sky fragment shader:

```glsl
vec2 clipXY = vec2(vTexCoord.x * 2.0 - 1.0, 1.0 - vTexCoord.y * 2.0);
```

This keeps the same camera-basis sky calculation but matches OpenGL's fullscreen sky orientation.

---

## Related Camera Cleanup

The runtime camera builder was also corrected so camera scale does not enter the view matrix.

Camera views should be built from:

```text
position + rotation
```

not full TRS with scale.

Changed file:

- `addons/camera/CameraViewBuilder.cpp`

This was not the final jitter fix by itself, but it is still the correct behavior for cameras.

---

## Lessons Learned

Skyboxes and environment backgrounds should be translation-free.

Avoid this pattern for sky lookup:

```text
worldFar - cameraPosition
```

It seems reasonable, but it becomes unstable at large world coordinates.

Prefer this pattern:

```text
screen ray from projection focal values
combine with explicit camera right/up/forward basis
sample environment cubemap
```

This is stable because it avoids subtracting large world-space floats.

Do not "fix" this by guessing `transpose(mat3(viewMatrix))` vs `mat3(viewMatrix)` per backend. That is fragile and easy to break for roll/pitch. The robust version is to build the world-space sky ray from named camera basis vectors.

---

## Verification

After the fix:

- moving the main camera no longer makes the HDR background jitter
- the artifact no longer becomes worse with distance from the origin
- roll around the camera Z axis rotates the sky correctly
- OpenGL sky orientation is corrected with the GLSL Y-ray flip
- DX11, Vulkan, and OpenGL runtime targets build successfully
