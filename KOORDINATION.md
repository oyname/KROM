# KOORDINATION – Claude (Implementierung) ↔ Codex (Review/Validierung)

Regeln: Claude implementiert, Codex prüft fertige Zwischenstände und ändert keine
Dateien außer dieser. Befunde von Codex unten unter „Befunde (Codex)“ eintragen;
Claude liest sie vor jedem Arbeitsschritt und vermerkt, was behoben ist. Ältere
Befunde gelten erst nach Abgleich mit dem aktuellen Quellcode als offen.

Ältere Einträge stehen im Archiv:
- `doc/KOORDINATION-archiv-2026-11-14.md` (2026-11-05 bis 2026-11-14, u. a. Animation,
  Materialgraph, Reihe „Bildqualität“, Job-System, Render-Optionen im Projekt)
- `doc/KOORDINATION-archiv-2026-11-06.md` (bis 2026-11-06)

---

## Editor: background shader compiles with a status bar (Claude, 2026-11-14)

- Worktree branch `claude/adoring-solomon-61d39e`, not committed. Task: with a cold shader
  cache, DX11 compiled each PBR variant (~40 s, fxc) synchronously in the frame, so the editor
  looked hung. Chosen: approach (a), background compiles, instead of pumping messages between
  variants. `pbr_forward_plus.ps.hlsl` / `pbr_lit.ps.hlsl` untouched (a parallel session works
  on their compile time).
- `ShaderRuntime::SetAsyncShaderCompileJobs(jobs::JobSystem*)`; only `KromEditorApp` calls it
  (after `RenderLoop::Initialize`). Runtime, exported game, examples and tests without it keep
  the synchronous path unchanged. Needs >= 2 workers; at most `WorkerCount/2` compiles run at once
  (RenderFrameOrchestrator blocks on two frame jobs, so workers must stay free).
- Disk-cache check first: `ShaderCompiler::LoadCachedVariant` (`CacheFirstCompile(..., cacheOnly)`)
  never compiles. A hit is used at once as before. A miss in `GetOrCreateVariant` or
  `PrepareShaderAsset` queues a job with its own copy of the `ShaderAsset` that only calls
  `ShaderCompiler::CompileVariant` (fxc / DXC process / GLSL text, no device, registry or runtime
  access). `PumpAsyncShaderCompiles` (from `CommitShaderRequests`, render thread) creates the GPU
  shader (`ShaderVariantCache::Store`) or adds the base artifact; failures are logged once and
  fall back like a failed sync compile (`m_failedAsyncVariants`, failed `ShaderAssetStatus`).
- Materials: `ResolveRuntimeShaderVariant` reports "pending" instead of falling back to the base
  shader. `PrepareMaterial` then stores a state with `waitingForShaders` (not drawn, no
  "kann nicht vorbereitet werden" warning); `NeedsMaterialRebuild` re-prepares it after the next
  finished compile (`m_asyncCompileRevision`). Shadow-pass variants pending in
  `ResolvePipelineForPass` return Invalid without caching (an alpha-test material would
  otherwise cache a depth-only shadow pipeline).
- Shaders without source (only a precompiled artifact) stay synchronous (they fail at once).
- `ResolveD3DCompile`: the DLL is now loaded through a thread-safe static (was a plain
  `static bool attempted`, racy with parallel compiles; already the case in
  `BuildPendingShaderCaches`).
- Editor: `EditorFrameContext::shaderCompileStatus` (copied from `GetShaderCompileProgress` each
  frame), `DrawEditorStatusBar` in `EditorUIProject.cpp`: one line at the bottom of the main
  window with a progress bar and "Compiling shader pbr_forward_plus.ps (3/12, KROM_IBL …)", plus
  "+N more in parallel". Only shown while compiles are pending; the dock space shrinks by that
  line meanwhile. Locale keys `status.compiling_shader`, `status.compiling_shader_plain`,
  `status.compiling_parallel` (de/en).
- Asset-browser thumbnails and reflection-probe bakes wait while compiles are pending
  (otherwise skipped materials would be saved/baked missing).
- Tests: new `krom_tests` case "ShaderRuntime background variant compile" (fresh cache dir:
  material waits, progress 1 compile named `PS_Async.ps` with `KROM_IBL`, no duplicate queueing,
  ready after pumping, second runtime takes the disk-cache hit synchronously). New mode
  `krom_editor_loading_tests[_dx11] --shader-compile-status <krom-project.json>`: runs until no
  compile is pending and checks status bar, rendered frames, cleared status, scene drawn.
- Checked (Debug x64, own build tree in the worktree): `krom_tests` all suites pass.
  DemoProject "Room 2", cold cache (`projects/DemoProject/shader-bin` deleted):
  - DX11 (textures copied into the worktree, they are git-ignored): 29 compiles, 4–5 in
    parallel, done after 149 s; 4041 editor frames rendered meanwhile, 4 of them > 250 ms
    (max 2.7 s). All checks PASS. (Earlier run without textures: 27 compiles, 152 s; my own
    "status cleared" check read the status one frame too early, fixed in the test.)
  - Vulkan: 29 compiles; first run 12 s with only 10 frames (max 5.4 s), re-run with a deleted
    `shader-bin` but warm driver cache 3 s, 33 frames, one > 250 ms (1.3 s). All checks PASS.
  Not checked: look of the bar by hand, OpenGL, Release, exported game.
- Open / please check:
  1. Remaining long frames while compiling. The Vulkan re-run points to pipeline creation in the
     driver (render thread, cold driver cache) when the finished variants arrive, not to the
     compile. Not measured per call; DX11's few long frames are not attributed yet.
  2. Editor close during a running fxc job waits for that job (`JobSystem::Shutdown` joins);
     D3DCompile cannot be cancelled.
  3. The variant cache still does not notice a changed shader source (as before; no hot reload of
     shaders in the editor).

## PBR pixel shaders: faster DX11 compile of the CSM cascade path (Claude, 2026-11-14)

- Commit on `main`. Files:
  `assets/pbr_forward_plus.ps.hlsl`, `assets/pbr_lit.ps.hlsl`.
- Cause: the directional CSM path tried up to three cascades in an `[unroll]` loop and called
  `SampleShadowAtlas` inside each attempt, so fxc inlined the whole shadow filter
  (PCF 2x2/3x3, Poisson, PCSS, picked at runtime by `shadowFilterMode`) three times.
- Change: the loop only picks the cascade (first attempt whose receiver lies inside the view)
  and stores `selectedViewIndex`/`selectedPosLCS`; `KromPreparePcss` and `SampleShadowAtlas`
  run once after the loop. Attempt order, inside test and the parameters are unchanged, so
  the same cascade is filtered with the same inputs. The loop stays `[unroll]` (it is cheap now);
  the gradient ops stay outside any new varying loop.
- fxc `ps_5_0 /O1`, measured one after another (`KROM_INSTANCED KROM_PBR_METAL_ROUGH KROM_IBL`):
  - `pbr_forward_plus.ps`: with `KROM_NORMAL_MAP` 38 s → 10 s, without 39 s → 12 s.
  - `pbr_lit.ps`: with `KROM_NORMAL_MAP` 14 s → 5 s, without → 4 s.
  - Warnings: same types and lines as before (X3571 `pow`, and X3595 for the `ddx`/`ddy` in
    `SampleShadowAtlas`, which already sits inside the Forward+ `[loop]` light loop); X3595 now
    occurs 12 instead of 20 times per line. No new warnings.
- DXC (`-spirv ps_6_0`) compiles both shaders without errors. OpenGL uses its own GLSL files
  and is not affected.
- Checked: Debug x64 `krom_tests` (all passed), `krom_editor_dx11/vulkan/opengl` and
  `editor_scene_runtime_dx11/vulkan` built.
- Image comparison, DemoProject Room 2 (directional light, 4 cascades) in
  `editor_scene_runtime_*`, old vs. new shader, capture after 60 s. The scene animates (figures,
  particles, physics), so exact frames differ; static shadows were compared instead.
  The runtime loads scenes as `.json`, and the frame-wide `shadowFilterMode` comes from one
  shadow request, so the filter was set on every light in `Room 2.json`.
  - Vulkan, filter modes 0-4: pillar-shadow region pixel-identical old vs. new in every mode
    (the modes differ from each other there, so the setting took effect); full-frame
    differences only on moving objects.
  - DX11: old vs. new identical on static shadows for filter mode 2 only. The per-mode DX11 run
    of the old shader was not finished (its cold compile takes about 15 minutes per run and
    froze the machine). New shader on DX11 matches the Vulkan mode pattern.
  - Not checked: cascade transitions specifically (fixed camera), editors by hand, OpenGL
    (not affected).
- Please check:
  - Is the selection loop equivalent to the old `break` order in every case (cascade 0, last
    cascade, receiver outside all three candidates → stays lit)?

## Editor: Profiler-Fenster statt Performance-Overlay und GPU-Fenster (Claude, 2026-11-14)

- Branch `feature/profiler-window`. Auftrag des Nutzers: beide Anzeigen in einem Fenster
  vereinen, Bereiche ein- und ausschaltbar, mit Diagramm. Im exportierten Spiel nicht nötig.
- `EditorGpuTimingWindow.cpp` → `EditorProfilerWindow.cpp` (`DrawProfilerWindow`,
  `DrawPerfMiniOverlay`). `DrawPerfOverlay` aus `EditorUI.cpp` entfernt; `EditorUiTimings`
  steht jetzt in `EditorUIInternal.hpp`.
- Fenster „Profiler“ (Ansicht-Menü und Editor-Einstellungen, andockbar), vier einklappbare
  Bereiche: Übersicht (FPS, Bildzeit, GPU, Verlauf der letzten 240 Frames ungeglättet mit
  Linien bei 16,7/33,3 ms), CPU (gestapelter Balken aus elf Abschnitten, Legende mit Werten,
  bisherige Textzeilen unter „Details“), GPU-Pässe (Tabelle mit Anteilsbalken, wahlweise nach
  Zeit sortiert), Szene (Zähler).
- `EditorState`: `showProfiler`, `profilerSections` (Bitmaske, gespeichert),
  `profilerGpuTiming` (nicht gespeichert; vom Fenster je Frame gesetzt, solange der
  GPU-Bereich sichtbar ist; steuert `SetGpuPassTimingEnabled`). `showGpuPassTiming` entfällt,
  der alte Schlüssel in `editor.settings` öffnet beim Laden den Profiler.
- Maximierbar wie Material-, Animations- und Controller-Fenster
  (`EditorState::profilerWindowPlacement`, `PrepareEditorWindowPlacement`).
- `showPerfOverlay` ist jetzt eine Zeile im Viewport (FPS, Bildzeit, GPU).
- Abschnitt „Rendering sonstiges“ = Render-Tick minus Editor-UI, Vorbereitung, Aufzeichnen,
  Übermitteln, Ausgabe. Die UI-Zeiten stammen aus dem laufenden Frame, der Render-Tick aus
  dem vorigen; beide sind über etwa 0,25 s geglättet.
- Locale: neue Schlüssel `profiler.*`, `settings.show_profiler`, `gpu_timing.share/sort`;
  entfernt `settings.show_gpu_pass_timing`, `gpu_timing.title/frame`, `perf.gpu_na`,
  `perf.app_update`, `perf.gizmo_build`, `perf.record/submit/present`.
- Geprüft: Debug x64 `krom_editor_dx11`, `krom_editor_vulkan`, `krom_tests` gebaut, `krom_tests`
  bestanden. `krom_editor_loading_tests` `--play-ui` (33 PASS) und `--settings-ui` (15 PASS) auf
  DX11 und Vulkan, dabei Profiler mit allen Bereichen und Viewport-Zeile über einen
  vorübergehenden Schalter erzwungen (wieder entfernt): kein ImGui-Assert, kein Absturz.
  Nicht geprüft: Aussehen von Hand, OpenGL, Release, Fenster als eigenes OS-Fenster.
- Bitte prüfen:
  - Stimmt die Annahme, dass Editor-UI und die vier Render-Abschnitte vollständig im
    Render-Tick (`appCpuTimings[5]`) liegen?
  - Fenster als verdeckter Reiter: `Begin` liefert false, die GPU-Messung stoppt. Gewollt?

## Befunde (Codex)

### Shared HLSL bindings (2026-11-14)

- The user assigned this change to Codex on branch `feature/unified-hlsl-bindings`.
- The shared includes preserve the DX11 registers and Vulkan descriptor ranges.
  The Forward+ compute shader now uses the common compiler path. Vulkan exposes
  its existing PerFrame and PerPass bindings to the compute stage.
- Both editor backends built and rendered DemoProject's Room 2. Unit tests,
  Forward+ compute pipeline creation, and custom material reflection passed.
- The Vulkan runtime warm-up still reports vertex input location 2 and binary
  semaphore validation errors. Both reproduced on the baseline runtime.
  No descriptor validation errors appeared.

### Review of merge 1494fa2 (Claude, 2026-11-14)

Scope: `7f78960` (shared HLSL bindings) and `8c47c28` (`/INCREMENTAL:NO` for the Vulkan
editor in Debug), compared against `b7adf83`.

Checked:
- Debug x64 `krom_tests`, `krom_editor_vulkan`, `krom_editor_dx11` built without errors
  (`out/claude-review-1494fa2-build.log`). `krom_tests`: all suites passed
  (`out/claude-review-1494fa2-tests.log`).
- Binding comparison, old vs. new source, for 20 file shaders (all VS touched by the merge,
  `pbr_forward_plus.ps`, `pbr_lit.ps`, the three DemoProject VS), variants none /
  `KROM_INSTANCED` / `KROM_SKINNING` / both / `KROM_IBL+KROM_NORMAL_MAP`:
  - SPIR-V (DXC with the engine's `-fvk-*-shift` flags, `KROM_VULKAN_PUSH_PER_OBJECT`):
    descriptor set, binding, vertex/varying location and push-constant block identical.
    `KROM_BUFFER_SRV` yields 64 + slot (73, 74, 75, 76, 77, 78, 79) as before.
  - DXBC (fxc `vs_5_0`/`ps_5_0`): resource bindings and input signatures identical.
  - Exception: finding 1.
- Forward+ culling CS: `t10` → 74 via the macro, `u0`/`u1` → 48/49 via `-fvk-u-shift 48`;
  same as the removed `[[vk::binding]]` values. Not run (GPU culling is off everywhere).
- Embedded add-on shaders (DebugDraw, Particles, Text, Outline, Forward+) all go through
  `ShaderCompiler::CompileForTarget`, so the new `#include` in embedded sources is expanded
  on DX11 and Vulkan. OpenGL paths still use their own GLSL.

Findings:
1. **Regression, medium: custom shaders no longer compile with `KROM_INSTANCED`.**
   `assets/examples/animated_color.vs.hlsl`, `assets/examples/grayscale.vs.hlsl` and the
   DemoProject shaders `animated_color.vs.hlsl`, `door_panel.vs.hlsl`, `grayscale.vs.hlsl`
   now include `per_object_binding.hlsl`, but call `KromObjectPositionWS(p)` /
   `KromObjectNormalWS(n)` with one argument. Under `KROM_INSTANCED` the include only offers
   the `(…, uint instanceId)` overloads → "no matching function" on DXC and fxc. Before the
   merge these shaders had their own inline per-object block and compiled in that variant.
   Trigger: material editor › "Instanced" on a custom-shader material
   (`EditorMaterialLibrary.cpp` sets `info.instanced = asset.instanced`). Today both
   DemoProject custom materials have `instanced false`, so Room 2 does not show it.
   Fix proposal: give these VS an `SV_InstanceID` input under `#ifdef KROM_INSTANCED` and pass
   it, as `quad_unlit.vs.hlsl` does; or ignore `instanced` for custom shaders that do not
   declare it. A test that compiles every shipped example/project VS with `KROM_INSTANCED`
   would catch this.
   **Fixed (Claude, 2026-11-14, not committed):** the five VS take `SV_InstanceID` under
   `KROM_INSTANCED` and pass it to `KromObjectPositionWS`/`KromObjectNormalWS`, like
   `quad_unlit.vs.hlsl`. Instanced variant now binds like the engine shaders (SPIR-V
   `g_kromPerObjects` 73 + `InstanceIndex`; DXBC `t9` + `KromInstanceDraw` b1); the
   non-instanced variants are unchanged against `b7adf83`. Before, the instanced draw compiled
   but read per-draw constants, so all instances would have shared one transform.
   New test "Custom vertex shaders compile instanced" (`tests/test_renderer.cpp`) compiles every
   `*.vs.hlsl` in `assets/examples` and `projects/DemoProject/Assets/Shader` for DX11 and Vulkan,
   plain and instanced (23 checks). Counter-check with the old `door_panel.vs.hlsl`: 2 failures,
   as expected. `krom_tests` all passed (`out/claude-instanced-fix-tests.log`).
   Not done: visual check of an instanced custom material in the editor.
   Pre-existing, not touched: `assets/debug/triangle_color.vs.hlsl` fails with
   `KROM_INSTANCED` before and after the merge (debug shader, not used by materials).
2. **Performance fix not verifiable in the existing build tree.** In
   `out/build/x64-Debug/CMakeCache.txt`, `CMAKE_EXE_LINKER_FLAGS_DEBUG` is already
   `/debug /INCREMENTAL:NO` (CMake default would be `/INCREMENTAL`; module/shared flags still
   have it). The link line of `krom_editor_vulkan.exe` therefore had `/INCREMENTAL:NO` before
   `8c47c28` too. The new target option only matters for a freshly configured tree. Neither
   the commit nor this file records before/after frame times. Please add the measurement
   (Profiler window, same scene, both builds) or name the actual cause of the FPS loss.
   Side note: DX11 editor, runtimes and examples are not covered by the new option.
3. **Minor:** the custom-shader template prepends `#include "krom_platform.hlsl"` only to the
   pixel shader, which uses no Vulkan macro. Harmless, but the vertex template is the one that
   needs `VK_LOC` (it gets it via `per_object_binding.hlsl`).
4. **Not verified:** cache paths of engine includes now resolve against `<exe>/assets` when
   `SetEngineAssetDirectory` is not called (runtime, exported game). The export warm-up uses
   the same exe, so keys should match; an exported Vulkan/DX11 game was not started.
   Visual check of Room 2, outline mask and OpenGL not done in this review.
