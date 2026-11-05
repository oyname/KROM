# Particle Script API – Single Particles and Frames (Draft v2)

Status: implemented. Extends `IParticleService` /
`IScriptContext` (`addons/script/`) and the particle addon (`addons/particles/`).

## 1. Types and rules

- `EntityID` names an emitter (an entity with `ParticleEmitterComponent`).
- `ParticleID` names one particle:

  ```cpp
  struct ParticleID
  {
      uint32_t system     = 0u;   // instance id of the ParticleSystem, 0 = invalid
      uint32_t slot       = 0u;
      uint32_t generation = 0u;   // >= 1 for a valid handle
      [[nodiscard]] bool IsValid() const noexcept { return system != 0u && generation != 0u; }
      bool operator==(const ParticleID&) const = default;
  };
  inline constexpr ParticleID NULL_PARTICLE{};
  ```

  A slot table maps `slot → (pool, index)` and is updated when a particle is
  swap-removed. Freeing a slot increments its generation, so an old handle can
  never address a particle created later. Rules that keep this true:

  - The slot table lives as long as its `ParticleSystem`. `Clear()`, scene
    changes, restarts and new play runs free the slots (generation + 1) but
    never reset the table or a generation.
  - Every `ParticleSystem` instance takes a new `system` id from a process-wide
    counter that starts at 1 and is never reused. A handle from a destroyed or
    replaced system (e.g. a new game simulation in the editor) therefore never
    matches the new system, even though its slots start again at generation 1.
  - Overflow: a slot whose generation would wrap past `UINT32_MAX` is retired
    and never handed out again. If the system id counter would wrap, creating
    another `ParticleSystem` is a fatal error (2³² instances per process are
    not a realistic case, but the guarantee must not silently break).
- Frame indices start at 0.
- Atlas rectangles are given in texture pixels, origin top left, y down.
- Positions are world coordinates, sizes are meters.
- Every particle keeps the **look** (texture + frame definitions) that its
  emitter had when the particle was spawned. Changing texture or frames on the
  emitter only affects particles spawned afterwards (see 5.1).
- Functions that change something return `false` for an invalid ID, a dead
  particle or an invalid parameter, and change nothing in that case.
- Without a particle service all functions are neutral (`false`,
  `NULL_PARTICLE`, empty optional).

### Handle lifetime

A `ParticleID` becomes invalid when the particle:

- reaches the end of its lifetime, dies on the floor (`dieOnFloor`) or is
  removed with `RemoveParticle`,
- is cleared together with its emitter: `StopParticles(clear = true)`,
  `PlayParticles` (restart), emitter entity destroyed or its component removed,
  scene change, or the editor's "only the selection plays" filter dropping the
  emitter.

Particles spawned with `SpawnParticle` live in their emitter's pool, so they
end together with the emitter, exactly like emitted particles.

## 2. Texture and frames (emitter)

| Function | Description |
|---|---|
| `SetEmitterTexture(EntityID emitter, string texturePath) → bool` | Assigns a texture, path relative to the asset root. Empty path = soft disc. Writes `ParticleEmitterComponent::texturePath`. |
| `SetEmitterAtlasGrid(EntityID emitter, uint columns, uint rows) → bool` | Splits the texture into equal cells, numbered row by row from the top left. Writes the existing `flipbookColumns` / `flipbookRows` and clears `frames`. `false` if a value is 0. |
| `SetEmitterFrames(EntityID emitter, span<const ParticleFrame> frames) → bool` | Defines freely placed rectangles; the order gives the frame indices. Replaces the grid. `false` if the list is empty or a frame is invalid. |
| `GetEmitterFrameCount(EntityID emitter) → uint` | Number of frames (grid: columns × rows). 0 without an emitter. |

```cpp
struct ParticleFrame
{
    uint32_t x = 0u, y = 0u;           // top left corner in pixels
    uint32_t width = 0u, height = 0u;  // > 0
    float    pivotX = 0.5f;            // anchor inside the rectangle, finite;
    float    pivotY = 0.5f;            // (0.5, 0.5) centre, (0.5, 1.0) bottom centre
    bool operator==(const ParticleFrame&) const = default;
};
```

- New component field: `std::vector<ParticleFrame> frames;`. Empty = grid mode
  (`flipbookColumns` × `flipbookRows`, all pivots centred), so existing scenes
  load unchanged.
- The particle system does not know texture sizes. Frames therefore stay in
  pixels and are converted to UVs in the renderer, which knows the resolution.
  The API cannot check bounds at call time. Rectangles that do not fit the
  texture are handled in the renderer and logged once per look/texture:
  - **Partly outside:** the frame keeps its original rectangle for every
    calculation (size relative to frame 0, pivot, floor contact). Only the
    part inside the texture is drawn, at the place it has inside the original
    rectangle. So the visible image neither grows, shrinks nor shifts when an
    animation passes through a clipped frame; the missing part is simply empty.
  - **Completely outside** (or texture not loaded yet / failed): nothing is
    drawn for this frame. The particle stays alive, keeps its frame index and
    keeps colliding with the original rectangle.
  - Frame 0 as reference always uses its original rectangle, clipped or not.
- Pivots outside 0..1 are allowed (anchor outside the image), NaN/Inf is not.
- Editor: grid mode shows the cells as frames. Moving or resizing one frame
  turns the grid into an explicit `frames` list (in pixels, using the loaded
  texture size), then updates that `ParticleFrame`. Frames are serialized with
  the component (scene, prefab and later `.particle` assets).

## 3. Size and pivot

- The size curve (`startSize` → `endSize`) and `SetParticleSize` describe the
  world size of the **reference frame** = frame 0.
- Every other frame is scaled by its pixel size relative to frame 0:
  `worldSize = particleSize × (frame.width / ref.width, frame.height / ref.height)`.
  So frames of different sizes keep their pixel proportions.
- Scalar size from the curve `s` → `particleSize = (s × ref.width / ref.height, s)`.
  For square cells this is the edge length `s`, so the current behaviour
  stays the same. Grids with non-square cells are no longer stretched to a square.
- The particle position is the pivot point. The quad is offset in the camera
  plane by the pivot (`ParticleBillboard` gets `Vec2 size` and `Vec2 pivot`
  instead of `float size`; quad placement is calculated on the CPU before the existing shaders run).

### Floor collision

Today the contact height is `floorY + size × 0.5` (centre of a square). The
new rule uses the bottom edge of the frame currently shown:

- `contactY = floorY + worldSize.y × (1 − frame.pivotY)`, with `worldSize`
  from the size rule above for the current frame.
- Centred pivot (`0.5`) gives `worldSize.y × 0.5`, i.e. today's behaviour for
  square cells. Bottom pivot (`1.0`) puts the pivot exactly on the floor.
  Pivots above the image (`pivotY < 0`) or below it (`pivotY > 1`) give the
  matching larger or negative offset.
- As today, the billboard is treated as upright: the tilt of the camera plane
  is ignored.
- Impact (bounce, friction, `dieOnFloor`) only happens when the particle moves
  down through `contactY` (`velocity.y < 0`).
- If only the size or frame changes (size curve, `SetParticleSize`, a new
  frame with a different height or pivot) and the bottom edge ends up below
  the floor, the particle is lifted to `contactY` without counting as an
  impact: no bounce, no friction, `dieOnFloor` does not trigger. If the bottom
  edge rises, the particle is not pulled down; gravity does that as usual.

## 4. Controlling single particles

| Function | Description |
|---|---|
| `SpawnParticle(EntityID emitter, Vec3 position) → ParticleID` | Creates exactly one particle now, with the emitter's current look and settings (lifetime, velocity, colour, gravity, floor…). Works on a stopped emitter. Counts against `maxParticles`. `NULL_PARTICLE` without an emitter or when the pool is full. |
| `SetParticlePosition(ParticleID, Vec3 position) → bool` | Moves the particle. While motion is enabled, velocity, gravity, drag and floor collision keep acting from the new position. |
| `SetParticleVelocity(ParticleID, Vec3 velocity) → bool` | Sets the velocity. With motion enabled, gravity and drag change it again in the next step, so `{0,0,0}` only stops the particle for one moment. |
| `SetParticleMotionEnabled(ParticleID, bool enabled) → bool` | `false`: the simulation no longer moves the particle (no velocity, gravity, drag or floor collision); only `SetParticlePosition` moves it. Lifetime, size curve, colour and frame animation keep running. The stored velocity is kept and applies again after `true`. Default `true`. |
| `SetParticleSize(ParticleID, Vec2 size) → bool` | World size of the reference frame, both components > 0. Replaces the size curve for this particle. |
| `SetParticleFrame(ParticleID, uint frame) → bool` | Shows the frame now and holds it. Ends any frame animation. |
| `IsParticleAlive(ParticleID) → bool` | `true` while the handle addresses a living particle. |
| `RemoveParticle(ParticleID) → bool` | Removes exactly this particle now. |

`SpawnParticle` is named differently from the existing
`EmitParticlesAt(template, position, count)` on purpose, so the two cannot be
confused. Frame indices are checked against the particle's own look.

## 5. Frame animation

| Function | Description |
|---|---|
| `PlayParticleFrames(ParticleID, uint firstFrame, uint lastFrame, float fps, bool loop = false) → bool` | Starts now at `firstFrame` and plays up to and including `lastFrame`. `fps` > 0 and finite. `firstFrame > lastFrame` plays backwards. Calling it again restarts the sequence. |
| `PlayParticleFramesOverLifetime(ParticleID, uint firstFrame, uint lastFrame) → bool` | Plays the sequence once over the lifetime remaining at the call. Also backwards. |
| `SetParticleFrameProgress(ParticleID, uint firstFrame, uint lastFrame, float progress) → bool` | Shows the frame for `progress` (clamped to 0..1, NaN → `false`). 0 = first, 1 = last frame. Ends any automatic animation; the code drives it from now on. |
| `SetParticleAnimationPaused(ParticleID, bool paused) → bool` | Pauses or resumes the automatic animation. Movement and lifetime keep running. |
| `GetParticleFrame(ParticleID) → optional<uint>` | Currently shown frame index (absolute). Empty for an invalid or dead particle. |

### 5.1 State per particle

```cpp
enum class FrameMode : uint8_t { Emitter, Fixed, Rate, OverLifetime, Manual };

struct Particle   // additions
{
    std::shared_ptr<const ParticleLook> look; // texture + frames at spawn time
    math::Vec2 sizeOverride{};                // x <= 0: use the size curve
    uint32_t   frame      = 0u;               // shown frame (absolute)
    uint32_t   firstFrame = 0u, lastFrame = 0u;
    float      fps        = 0.f;              // Rate
    float      animTime   = 0.f;              // seconds since Play…, frozen while paused
    float      animLength = 0.f;              // OverLifetime: remaining lifetime at the call
    FrameMode  frameMode  = FrameMode::Emitter;
    bool       loop       = false;
    bool       paused     = false;
    bool       motion     = true;             // SetParticleMotionEnabled
};
```

`ParticleLook` is immutable. The emitter builds a new one when texture or
frame data change, and particles that are already alive keep theirs. Particles
of one emitter normally share one `ParticleLook`.

### 5.2 Rules

With `n = |lastFrame − firstFrame| + 1` and `dir = ±1`:

- **Rate:** `k = floor(animTime × fps)`; with loop `k mod n`, otherwise
  `min(k, n − 1)`. Every frame, including the last one, is shown for `1/fps`.
  Without loop the last frame stays until the particle dies.
- **OverLifetime:** `t = animTime / animLength`. If `animLength <= 0`, the
  last frame is shown. Then the same mapping as Manual.
- **Manual (progress p):** `k = min(floor(p × n), n − 1)`, so every frame gets
  the same share of the range.
- Shown frame: `firstFrame + dir × k`.
- **Pause** freezes `animTime`, so OverLifetime does not jump on resume. It
  only acts on Rate and OverLifetime. `PlayParticleFrames…` resets pause.
  Lifetime keeps running on purpose, so a paused particle can die before its
  animation has finished.
- **Emitter** (default, no script call): uses the frame animation settings
  from section 8.1, captured in the particle's look when it is spawned.
- `SetParticleFrame` → Fixed, `SetParticleFrameProgress` → Manual.

## 6. Examples

```cpp
auto dust = ctx.SpawnParticle(dustEmitter, position);

ctx.SetParticleFrame(dust, 2);                       // hold frame 2
ctx.PlayParticleFrames(dust, 0, 3, 8.f);             // frames 0..3 at 8 fps
ctx.PlayParticleFrames(dust, 3, 0, 8.f, true);       // 3..0 backwards, looping
ctx.PlayParticleFramesOverLifetime(dust, 3, 0);      // backwards over remaining lifetime
ctx.SetParticleFrameProgress(dust, 0, 3, charge);    // driven by game logic

if (auto f = ctx.GetParticleFrame(dust); f && *f == 3)
    ctx.RemoveParticle(dust);
```

## 7. Implementation steps

Each step comes with tests that run without a graphics device (`tests/test_particles.cpp`).

1. **Handles:** slot table + generations, `SpawnParticle`, `IsParticleAlive`,
   `RemoveParticle`, `SetParticlePosition/Velocity`, `SetParticleMotionEnabled`.
   Tests: stale handle after death/remove/clear/restart/scene change, reused
   slot does not match an old handle, handle from a destroyed system does not
   match a new one, retired slot after generation overflow (counter set by a
   test hook), full pool, motion disabled ignores gravity but keeps lifetime.
2. **Animation:** per-particle state, `SetParticleFrame`, `Play…`,
   `SetParticleFrameProgress`, pause, `GetParticleFrame`. Tests: forward and
   backward, loop, last frame held, pause during OverLifetime, progress 0/1/clamp.
3. **Free frames and size:** `ParticleFrame`, `frames` field + serialization,
   `SetEmitterAtlasGrid/Frames`, `SetParticleSize`, `Vec2` size + pivot in
   `ParticleBillboard`, UV conversion and geometry clipping in the renderer.
   Tests: frame → billboard size/pivot, grid compatibility (square cells
   unchanged), old scenes load, floor contact with bottom/centre pivot, lift
   without impact on a frame change, partly/completely outside frames keep
   size and position.
4. **Look snapshot:** `ParticleLook` per particle. Tests: changing
   texture/frames leaves living particles unchanged.
5. **Editor:** frame view in the inspector, grid → explicit list on edit,
   move/resize updates `ParticleFrame`.

Script interface: `IParticleService` gets the new virtual functions,
`IScriptContext` the matching helpers next to the existing particle helpers.
Frames in `.particle` assets follow once `feature/particle-assets` is merged.

## Editor usage

Click the texture thumbnail or expand **Atlas Frames**. Select a frame by its
zero-based index or by clicking its rectangle. Drag the rectangle to move it,
the bottom-right handle to resize it, or the circle to change the pivot.
Pixel bounds and pivot coordinates can also be edited numerically. The first
edit converts a grid to explicit frames. **Use Grid** restores the regular
layout and discards the custom rectangles. Interactive grid conversion is
limited to 4096 non-empty cells. Script APIs support larger validated grids.

Legacy serialized flipbookRandomFrame settings migrate to frameAnimation = Random.
New files write the animation mode. The old C++ field was removed with ABI 4.
The Inspector exposes the Frame Animation block and atlas frame editing.

The script module ABI is version 4. Rebuild the editor and project script DLLs
after updating the engine.

## 8. Editor extensions (implemented)

Three additions that make the frame features usable without code. They build
on each other: 8.1 defines the animation, 8.2 names ranges, 8.3 shows both.
All edits go through the existing `ComponentUndoScope` (one undo step per edit
or drag), and all new labels get `de.json` / `en.json` keys.

### 8.1 Frame animation on the emitter

New component fields, shown as a **Frame Animation** block in the emitter
section (next to Atlas Frames):

```cpp
enum class ParticleFrameAnimation : uint8_t { OverLifetime, Rate, Fixed, Random };

inline constexpr uint32_t kLastParticleFrame = UINT32_MAX;   // "up to the last frame"

// ParticleEmitterComponent additions
ParticleFrameAnimation frameAnimation   = ParticleFrameAnimation::OverLifetime;
uint32_t               frameFirst       = 0u;
uint32_t               frameLast        = kLastParticleFrame;
float                  frameFps         = 12.f;   // Rate only, > 0
bool                   frameLoop        = true;   // Rate only
bool                   frameRandomStart = false;  // Rate only
```

| Mode | Behaviour |
|---|---|
| Over lifetime | Plays `frameFirst..frameLast` once over the particle's lifetime (section 5.2, OverLifetime). Default; with the default range this is today's behaviour. |
| Rate | Plays the range at `frameFps`, optionally looping (section 5.2, Rate). `frameRandomStart` starts each particle at a random time in `[0, n / fps)`, so particles do not run in sync. |
| Fixed | Shows `frameFirst`. |
| Random | Shows a random frame from the range, fixed for the particle's life. |

Rules:

- `frameFirst > frameLast` plays backwards, as in the script API.
- Range indices are resolved against the particle's look:
  `min(index, frameCount − 1)`. So `kLastParticleFrame` always means the last
  frame, and a range stays usable when frames are added or removed. The
  inspector shows a warning when a stored index is beyond the current frame
  count.
- The settings are part of `ParticleLook`. Changing them affects only
  particles spawned afterwards, like texture and frames.
- `SpawnParticle` uses them as the particle's initial animation. Any script
  call from section 5 replaces them for that particle.
- Migration: `flipbookRandomFrame = true` in old data loads as `Random`. If
  both keys exist, `frameAnimation` wins. New data no longer writes
  `flipbookRandomFrame`. The old C++ field is removed in script ABI 4.

Inspector: mode as a combo box; range as two frame fields with a checkbox
"to last frame" for `kLastParticleFrame`; FPS, loop and random start only
visible in Rate mode. The selected range is outlined in the atlas view.

### 8.2 Named clips

```cpp
enum class ParticleClipTiming : uint8_t { Rate, OverLifetime };

struct ParticleClip
{
    std::string        name;               // unique per emitter, 1..64 chars, case sensitive
    uint32_t           first = 0u;
    uint32_t           last  = kLastParticleFrame;
    ParticleClipTiming timing = ParticleClipTiming::Rate;
    float              fps   = 12.f;       // Rate only, > 0
    bool               loop  = false;      // Rate only
    bool operator==(const ParticleClip&) const = default;
};

// ParticleEmitterComponent addition
std::vector<ParticleClip> clips;
```

Script API:

| Function | Description |
|---|---|
| `PlayParticleClip(ParticleID, string_view name) → bool` | Plays the clip with its timing, like `PlayParticleFrames` / `PlayParticleFramesOverLifetime`. `false` for an unknown name. |
| `FindEmitterClip(EntityID emitter, string_view name) → optional<ParticleClip>` | Reads a clip, e.g. to drive it with `SetParticleFrameProgress(id, clip.first, clip.last, p)`. |

- Clips belong to `ParticleLook`. `PlayParticleClip` looks the name up in the
  particle's own look, so a clip edited later does not change living particles.
- Ranges follow the same resolving rule as 8.1.
- Scripts address clips by name, not by index. Reordering frames in the atlas
  only requires adjusting the clip ranges, not the code.

Inspector: a **Clips** list inside Atlas Frames with add, duplicate, delete
and reorder. Each row has name, first/last frame, timing, FPS and loop.
Duplicate or empty names are marked red and block nothing else, but
`PlayParticleClip` only finds the first clip with a name. Selecting a clip
outlines its range in the atlas and selects it in the preview (8.3).
Renaming a clip can break scripts that use the old name; the inspector cannot
detect this. A clip reference field for scripts may follow later.

### 8.3 Preview in the frame editor

A preview area below the atlas view, inside Atlas Frames:

- **Source:** combo box with "Emitter animation" (8.1) and every clip (8.2).
- **Controls:** play/pause, restart, scrub slider (0..1, uses the Manual
  mapping from 5.2) and the shown frame index. Scrubbing pauses playback.
- **Display:** checkerboard square (about 128 px). Every frame is drawn at its
  size relative to frame 0 (section 3), scaled so the largest frame of the
  range fits. The pivot is drawn as a cross, the floor contact line
  (`(1 − pivotY)` below the pivot, section 3) as a horizontal line, and the
  outline of frame 0 as a faint reference. A wobbling pivot or a jumping
  contact line shows badly placed frames immediately.
- Over-lifetime timing uses `particleLifetime` without its random part.
  Random mode cycles through random frames once per second; Fixed shows the
  frame.
- Runs without play mode and independently of the viewport simulation.
  Preview time is UI state only (ImGui storage): not serialized, no undo.

The frame mapping must not exist twice. Before 8.3, the mapping from 5.2
moves into one ImGui-free helper in the particle addon, e.g.
`ResolveParticleFrame(mode, first, last, fps, loop, time, length, frameCount)`,
used by both `ParticleSystem` and the preview.

### 8.4 Steps and tests

1. **8.1:** fields, serialization with migration, `ParticleLook` extension,
   `SpawnParticle` default, inspector block. Tests: each mode, backwards range,
   `kLastParticleFrame` resolving after frames are removed, random start
   deterministic for a fixed seed, legacy `flipbookRandomFrame` loads as
   Random, new key wins over the old one, round trip.
2. **8.2:** clips, serialization, `PlayParticleClip`, `FindEmitterClip`,
   inspector list. Tests: unknown name, both timings, clip edited after spawn
   leaves living particles unchanged, round trip.
3. **8.3:** extract `ResolveParticleFrame`, then the preview. Tests: helper
   gives the same frames as the simulation for all modes; the preview UI
   itself is checked by hand.

Adding the component fields and script functions changes the script module
ABI (version 4); editor and project script DLLs have to be rebuilt.

## Extension validation

The automated tests cover legacy migration, all animation modes, reverse ranges,
last-frame resolution, deterministic random starts, clip lookup and snapshots,
serialization, shared frame mapping, and clip list actions with undo. Preview
controls change only ImGui state. A manual visual check remains pending because
Windows window capture timed out.
