# KOORDINATION – Claude (Implementierung) ↔ Codex (Review/Validierung)

Regeln: Claude implementiert, Codex prüft fertige Zwischenstände und ändert keine
Dateien außer dieser. Befunde von Codex unten unter „Befunde (Codex)“ eintragen;
Claude liest sie vor jedem Arbeitsschritt und vermerkt, was behoben ist. Ältere
Befunde gelten erst nach Abgleich mit dem aktuellen Quellcode als offen.

OpenGL ist seit 2026-11-15 auf dem aktuellen Stand eingefroren: keine neuen Features und
keine OpenGL-only-Fixes mehr (offene Punkte wie das CSM-Schwimmen bleiben als eingefroren
notiert). Neue Features dürfen unter OpenGL aus bleiben. OpenGL-Editor und -Runtime
müssen aber weiter bauen und starten; ein OpenGL-Bruch durch gemeinsamen Code ist ein Befund.

Ältere Einträge stehen im Archiv:
- `doc/KOORDINATION-archiv-2026-11-14.md` (2026-11-05 bis 2026-11-14, u. a. Animation,
  Materialgraph, Reihe „Bildqualität“, Job-System, Render-Optionen im Projekt)
- `doc/KOORDINATION-archiv-2026-11-06.md` (bis 2026-11-06)

---

## Export: license notices in `licenses/` (Claude, 2026-11-16)

- New repo folder `licenses/`: `README.txt` (overview for players) plus the texts of
  cgltf+jsmn, stb (stb_image/stb_truetype), Jolt, miniaudio, Lucide and the DirectX Shader
  Compiler (`LICENSE.TXT` from microsoft/DirectXShaderCompiler). Jolt/miniaudio texts are
  copies of the pinned FetchContent versions (v5.5.0 / 0.11.22).
- CMake `krom_copy_licenses` (only `krom_add_project_runtime`): copies `licenses/` and the
  root `LICENSE` as `licenses/KROM-LICENSE.txt` next to the game runtime.
- `FinishProjectExport` replaces `<export>/licenses` with that folder; failure aborts the
  export (`export.licenses_failed`, de/en).
- README: third-party table with license and "in exported games" columns, sections
  "Shipping a game" and "License" (MPL 2.0).
- Checked: Release build of `DemoProject_vulkan`, `DemoProject_dx11`, `krom_editor_vulkan`;
  `bin/licenses/` complete. Not checked: a full export through the editor.
- Open, not changed: `krom_copy_d3dcompiler` falls back to `System32\D3DCompiler_47.dll`
  (not covered by the Windows SDK redistribution terms); the export copies every DLL next
  to the runtime without a filter.

## Vulkan: cheaper `FlushDescriptors` (Claude, 2026-11-15)

- Commit a58d23f on `feature/ssr-mrt` (same change as 6f8496e on
  `claude/clever-perlman-90a023`). Adopts the descriptor part of
  Codex's measurement prototype in `addons/vulkan/VulkanCommandList.cpp`; per-frame-in-flight
  light/shadow buffers (the ~4.5 ms fence wait) are not part of this.
- Invalidation details only with verbose logging (`Debug::MinLevel <= LogLevel::Verbose`):
  the copies of the bound states, `ComputeDescriptorBindingInvalidation` and the
  "rematerialize descriptors" log text. Why this does not change behaviour: the block is
  only entered when `descriptorsDirty || allocationChanged`, so the reason always carried
  `ExplicitDirty` or `AllocationEpochChanged` and was never `None`; the comparison never
  decided anything. Without verbose the reason is now built from exactly these two bits
  (`frame.lastInvalidationReason` stays meaningful, it is not read anywhere else). The
  cache lookup, descriptor writes and binds are unchanged.
- `writes` is a fixed-size stack `std::array` (`CBSlots + TexSlots + SamplerSlots +
  UAVSlots + BufSRVSlots`, at most one write per slot) with a counter instead of a
  `std::vector`: no heap allocation per flush, no member state, and `pImageInfo`/
  `pBufferInfo` keep pointing into the info arrays, which were already stack arrays.
- `currentSnapshot`/`currentMaterialized` start as `{}` instead of
  `BuildDefaultDescriptor*State()`: `BuildDescriptorBindingState()` and
  `BuildDescriptorMaterializationState()` begin by assigning the default state themselves,
  so the extra default build was redundant.
- Checked: Debug x64 `krom_tests` (all pass), `krom_editor_vulkan`, `krom_editor_dx11`,
  `krom_editor_opengl` build. Measured with Codex's probe harness (instrumented Vulkan
  Debug build, hidden 1920x1080 window, DemoProject "Room 2" from the main checkout, MSAA 2x,
  SSR on, no GPU timing, 300 frames after warm-up), baseline and change alternated twice:

  | | FlushDescriptors | frame | record |
  |---|---|---|---|
  | before | 9.67 / 9.95 ms | 40.7 / 41.6 ms | 14.3 / 14.6 ms |
  | after | 5.12 / 5.00 ms | 35.5 / 34.9 ms | 11.8 / 11.6 ms |

  307 descriptor updates per frame in both. The absolute numbers are higher than Codex's
  (157 updates) because this run's editor layout had an offscreen request every frame;
  the relative gain (~48 % less descriptor time) matches.
- `KROM_VALIDATION=1` on the same scene: no Vulkan validation messages, before or after.
- Please review: equivalence argument for skipping the comparison; that nothing else
  relies on `lastInvalidationReason` carrying the binding/materialization bits.

## Screen-Space Reflections über MRT (Claude, 2026-11-14)

- Branch `feature/ssr-mrt`, nicht committet. Auftrag: Krom bleibt Forward. Der Opaque-Pass
  schreibt die SSR-Eingaben als zusätzliche Render-Targets, ein SSR-Pass ersetzt damit den
  spiegelnden Probe-/IBL-Anteil. Bei Fehltreffern bleibt dieser erhalten. Umsetzung in drei Stufen.
- **Stufe 1, MRT in Device und Render-Graph:**
  - `RenderPassBeginInfo::extraColorTargets/extraColorClears/extraColorTargetCount/clearExtraColors`:
    bis zu `kMaxExtraColorTargets` (3) reine Farbziele hinter dem Haupt-Target.
  - `PipelineDesc::extraColorFormats/extraColorCount/extraColorWriteMask`; die Extra-Ziele
    werden nie geblendet. `PipelineKey` nimmt das auf (statt der bisherigen `_reserved*`).
  - DX11: mehrere RTVs, `IndependentBlendEnable` für die Schreibmaske. Vulkan:
    Dynamic-Rendering-Attachments, `independentBlend` wird eingeschaltet, wenn die GPU es kann.
    `SupportsFeature("mrt")` ist auf DX11 immer an, auf Vulkan nur mit `independentBlend`.
    OpenGL und Null ignorieren die Extra-Ziele.
  - Rezept: `FrameRecipeRenderPassDesc::extraColorTargets` und `clearExtraColors`.
    `FrameRecipe::sceneOutputs` sind Ausgaben, die ein Contributor vom Opaque-Pass verlangt.
    `StandardFrameRecipeBuilder::AttachSceneOutputs` hängt sie an `MainOpaque`.
- **Stufe 2, Opaque schreibt die SSR-Eingaben:**
  - `ShaderVariantFlag::SsrOutput` → `KROM_SSR_OUTPUT`. `assets/pbr_ssr_output.hlsl`: Die
    Shading-Funktion heißt `KROM_PBR_ENTRY`. Mit der Variante schreibt ein Wrapper-`main`
    SV_Target1..3: oktaedrische Shading-Normale + Rauheit + Gültig-Flag, das Gewicht der
    spiegelnden Umgebung (BRDF-Term × Energiekompensation × Specular Occlusion) und das
    tatsächlich addierte spiegelnde Probe-/IBL-Licht. `pbr_lit.ps.hlsl` und
    `pbr_forward_plus.ps.hlsl` speichern die Werte direkt vor dem letzten `return`.
    Debug-Ansichten lassen das Flag auf 0.
  - `ShaderRuntime::SetTargetExtraColorFormats`, analog zu `SetTargetSampleCount`. Der
    Opaque-Executor setzt die Formate um seine Draws. Shader mit `KROM_SSR_OUTPUT` im
    Quelltext (`SupportsSsrOutput`) bekommen die Variante, alle anderen Material-Pipelines
    deklarieren die Ziele mit Schreibmaske 0. Der Transparent-Pass bleibt ohne Extra-Ziele.
  - View-Schalter `RenderView::enableScreenSpaceReflections`, durchgereicht bis
    `PassFrameView`. Aus bei Probe-Bake, Kameravorschau, Material-, Animations- und
    Prefab-Vorschau sowie Thumbnails. `PassFrameView` hat dazu `msaaSamples` und `mrtSupported`.
  - `IFramePass::GetStructureRevision()` geht in den Struktur-Schlüssel des Frame-Graphs ein.
    Ein- und Ausschalten zur Laufzeit baut den Graph neu.
- **Stufe 3, Addon `addons/ssr` (`engine_ssr`):**
  - Mit MSAA: `SsrResolve` (MRT: Normale aus dem ersten gültigen Sample, Gewicht und Licht
    gemittelt). Danach `SsrTrace` (Weltraum-Raymarch, 48 quadratisch wachsende Schritte +
    6 Binärschritte gegen die aufgelöste Szenentiefe → `SsrReflection` rgb + Konfidenz) und
    `SsrComposite` (13 Taps, Radius nach Rauheit, normalen- und tiefenabhängig; additiv
    `Konfidenz·(Treffer·Gewicht − Licht)` in `GTAOComposite`).
  - `GTAOComposite` ist der gemeinsame Szenenpuffer nach Opaque. Ohne GTAO legt SSR ihn an,
    mit einer Kopie aus `HDRSceneColor`. Tonemap liest ihn jetzt, sobald er im Rezept steht
    (vorher nur mit GTAO-Contributor und AO-View).
  - Parameter in `featureParams[7]` (`krom-ssr`). Registrierung im Editor und in KromWrapper
    direkt nach GTAO, nicht unter OpenGL.
  - Projekt-Einstellungen `rendering.ssrEnabled/ssrIntensity/ssrMaxRoughness/ssrMaxDistance/
    ssrThickness` (Standard an, 1.0, 0.6, 30, 0.3). UI unter Projekt › Rendering, Locale `ssr.*`.
    Noch nicht in der Script-API `RenderSettings`.
- **Geprüft:**
  - Debug x64: `krom_tests`, die Editoren für DX11, Vulkan und OpenGL,
    `krom_editor_loading_tests[_dx11|_opengl]`, `engine_krom_wrapper_win32` und
    `editor_scene_runtime_dx11/_vulkan` gebaut.
  - `krom_tests` alle bestanden, neu dabei „Recipe extra colour targets (MRT)“ und
    „PBR shaders compile SSR output“ (beide PBR-PS × DX11/Vulkan × ohne IBL/IBL/Materialgraph).
  - `--perf` auf einer Kopie des DemoProject (Szene car, MSAA 2x, 1920×1080):
    - Mit `KROM_VALIDATION=1` auf Vulkan keine Validierungsmeldung. Vor dem
      `independentBlend`-Fix kam VUID-VkPipelineColorBlendStateCreateInfo-pAttachments-00605.
    - Per vorübergehendem Log (wieder entfernt) bestätigt: Resolve, Trace, Composite und
      Material-Pipelines mit SSR-Ausgabe laufen auf DX11 und Vulkan.
    - Kosten SSR an/aus: DX11 ca. 8,8–9,5 / 7,8 ms, Vulkan ca. 13,5 / 11,3 ms.
    - OpenGL läuft ohne SSR (10,4 ms).
- **Nicht geprüft:** das Bild selbst. Die Bildschirmaufnahme zeigt in dieser Umgebung nur ein
  weißes Fenster. Auch nicht geprüft: das exportierte Spiel und Release.
- **Bekannte Grenzen:**
  - Bloom liest `HDRSceneColor` und sieht die SSR-Korrektur nicht.
  - Transparente Flächen über einer spiegelnden Fläche bekommen die Korrektur des Bodens
    darunter ab, weil SSR nach Transparent komponiert.
  - Volle Auflösung, keine zeitliche Glättung.
- Bitte prüfen:
  - Vorzeichen und Energie der Ersetzung (`Treffer·Gewicht − Licht`), auch mit Specular
    Occlusion an/aus.
  - Ist das Weiterreichen der Formate über ShaderRuntime-Zustand im Opaque-Executor sicher?
    Eine Pipeline, die außerhalb dieses Fensters aufgelöst wird, hätte die falsche
    Attachment-Zahl.
  - Erkennung über `KROM_SSR_OUTPUT` im Quelltext (`SupportsSsrOutput`).
- Nachtrag nach der Sichtprüfung des Nutzers (gleicher Tag):
  - Befund: Flächen mit Normal-Map spiegelten schwarz mit hellen Punkten. In Probe-Bereichen
    zeigten spiegelnde Objekte schwarze Streifen.
  - Ursache (Vermutung, noch nicht im Bild bestätigt): Der Strahl traf die eigene Fläche.
    Zwei Wege führen dahin:
    - Die Normal-Map kippt den Strahl unter die Fläche.
    - Der Tiefenpuffer springt bei flachem Blickwinkel um ganze Pixel.
    Der Treffer war dann die Fläche selbst, und die Ersetzung nahm ihr das Probe-Licht.
  - Korrektur im Trace (`SsrPass.cpp`):
    - Die geometrische Normale wird aus dem Tiefenpuffer rekonstruiert.
    - Strahlen mit `dot(R, Ng) <= 0.02` behalten die Probe.
    - Der Startversatz läuft entlang `Ng`.
    - Ein Treffer zählt nur, wenn der Szenenpunkt über der Startebene liegt (mindestens halb
      so hoch wie der Strahlpunkt). Sonst läuft der Strahl weiter.
  - Prüfung: `krom_tests` grün; DX11 und Vulkan laufen ohne Fehler.
- Nebenbefund: Wettlauf in `ResolveD3DCompile` (`src/renderer/ShaderCompilerD3D.cpp`).
  - Der Shader-Cache wird per `ParallelFor` kompiliert. Ein zweiter Thread sah
    `attempted == true`, während der erste die DLL noch lud, und meldete deshalb
    „failed to load D3DCompile“. Das zeigte sich nach den PBR-Änderungen, weil viele Shader
    gleichzeitig neu kompiliert wurden.
  - Jetzt lädt eine threadsichere Static die DLL genau einmal.
  - Geprüft mit geleertem Shader-Cache auf einer Projektkopie: keine Fehlermeldung.
- Nebenbefund 2, nicht behoben: Unter DX11 braucht `D3DCompile` (O1) für eine
  `pbr_forward_plus`/`pbr_lit`-PS-Variante rund 40 s.
  - Grund: Die `[unroll]`-Kaskadenschleife (3 Versuche) fügt den Schattenfilter dreimal ein.
  - Mit `[loop]` sind es 14 s, dann warnt fxc aber vor undefinierten Ableitungen in der Schleife.
  - Bei kaltem Cache kompiliert der Editor diese Varianten nacheinander auf dem
    Haupt-Thread. Mit Room 2 wirkt das wie ein Hänger. Vulkan (DXC) ist nicht betroffen.
  - SSR verdoppelt unter DX11 die Zahl der Opaque-Varianten (mit und ohne SSR-Ausgabe).

## Editor: background shader compiles with a status bar (Claude, 2026-11-14)

- Commit `c754191` on `main`. Task: with a cold shader
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

## Export warm-up also covers prefabs and materials (Claude, 2026-11-15)

- Branch `feature/ssr-mrt`, not committed. Before: the export warm-up only rendered the
  scenes, so variants of prefabs that scripts spawn and of materials that scripts swap in
  were compiled on the player's PC (stutter; under Vulkan usually no DXC there).
- `Krom::SpawnShaderWarmupAssets()` (krom.h, KromWrapper.cpp): every `.prefab` (without
  scripts, prefab cameras lose `isMainCamera`) and one cube per `.mat` below the asset
  root, 4 units in front of the main camera. `InstantiatePrefabForScript` got
  `attachScripts` (default true).
- `editor_scene_runtime.cpp --warmup-shaders` calls it after every scene load, so the
  variants are built under the lighting of every scene. One Info line per scene.
- Checked: Debug x64 `krom_tests` all passed; `editor_scene_runtime_dx11/_vulkan` built.
  DX11 warm-up of a DemoProject copy with an empty cache, old vs. new: 87 vs. 90 `.bin`,
  48 extra objects visible per scene (11 prefabs, 37 materials), no errors, exit 0.
- Not checked: Vulkan warm-up run, a real export from the editor, Release.
- Follow-up: Vulkan/DX12 exports ship the shader compiler. `FindShaderTool` also looks in
  `<exe dir>/tools/` (source "bundled", after override and DXC_PATH, before VULKAN_SDK).
  The export copies the `dxc.exe` it finds plus `dxcompiler.dll` (and `dxil.dll` if
  present) there before the warm-up, so the warm-up already uses that copy. About 22 MB.
  Console line `export.dxc_bundled` / warning `export.dxc_missing`.
- Checked: Debug Vulkan runtime in a separate folder, empty shader-bin, without
  VULKAN_SDK/DXC_PATH and without the SDK in PATH: without `tools/` the sky shaders fail
  ("requires dxc.exe"), exit 1; with `tools/` warm-up exit 0, no errors, 90 `.bin`.
  `krom_tests` all passed, both editors built. The export step itself was not run.
- Follow-up 2, DX11 export looked frozen (2026-11-15). Cause: the warm-up compiled every
  fxc variant one after another (5-12 s per PBR PS variant), and the editor waited for it
  with `_wspawnv(_P_WAIT)` on the main thread ("not responding", no progress).
  - Runtime: `Krom::SetParallelShaderCompiles(true)` / `Krom::ShaderCompilesPending()`
    (ShaderRuntime async path on the wrapper's job pool). The warm-up turns it on and goes
    to the next scene only after 10 frames and 3 frames in a row without a pending compile.
    It prints `[n/m] Shader warm-up: scene n of m` to stdout.
  - Tonemap pass: no `tonemap material bind failed` error while its shader is still compiling
    (`waitingForShaders`), same as other materials.
  - Editor: `RunShaderWarmup` replaced by `StartBuildJob` (same background job as the build:
    status bar shows the `[n/m]` line, Cancel works, console). `StartRuntimeBuild` shares
    `ActivateBuildJob`. krom.log errors are reported in the job's onFinished.
  - Measured, Debug, DemoProject copy, empty cache: DX11 328 s → 187 s (same 90 `.bin`,
    no errors); Vulkan 61 s, 90 `.bin`, no errors. `krom_tests` all passed.
  - Not checked: the export from the editor UI (background job, progress, Cancel), Release.
  - Correction: the runtime build status window had no Cancel button (only the script-build
    modal had one), so "Cancel works" above was wrong at that point. Fixed in follow-up 3.
- Follow-up 3, export snapshot and Cancel (2026-11-15):
  - `ProjectExportPlan` (hpp): build dir, export dir, staging dir, runtime target, backend,
    fixed at the click. `StageProjectExport` copies `Assets/` (without Scripts, without
    excluded scenes) to `Build/.staging-<profile>-<backend>/Content` and writes the export
    `krom-project.json` there, right after `BuildAllScenes`, before the build.
  - `FinishProjectExport(plan)`: EXE from `plan`, old `Content/` and `assets/` removed,
    staged `Content/` renamed into place (copy as fallback), staging removed. Build failed or
    cancelled: staging removed, previous export untouched.
  - Not frozen: script sources, which the runtime build compiles while it runs.
  - Cancel button in the runtime build status window (Play build, export build, warm-up).
    `m_buildJobCancelled` is set around `onFinished`; messages `export.cancelled`,
    `export.cancelled_incomplete` (warm-up phase: files in place, shader-bin partial),
    Play: `build.cancelled`.
  - Checked: `krom_tests` all passed, both editors built. Not run: any export or Cancel in
    the editor UI (no test covers the export path).

## Befunde (Codex)

### Undo bei Objektbewegung und Box-Kollision (2026-11-16)

- **Box-Griffe im Viewport:** Ein Zug an der X-Fläche änderte die halbe
  Ausdehnung von 2,000 auf 2,325 und den Mittelpunktversatz von 0 auf 0,325.
  `UpdateOBBHandles` schreibt beide Werte direkt und setzt beim Loslassen
  nur den Drag-Status zurück. Dadurch fehlt der Undo-Eintrag für den Zug.
  Beim Ziehen am Größenfeld derselben Komponente im Inspector stellten
  Undo und Redo die jeweiligen Werte mit je einem Schritt wieder her.
- **Lokales Positionsfeld im Inspector:** `DrawTransformSection` übergibt
  `t->localPosition` direkt an das Eingabefeld und erfasst den Undo-Snapshot
  erst nach der ersten Änderung. Im Test zog das Feld X von 0 über 1,500
  auf 4,500. Undo stellte X auf 1,500 zurück. Beim lokalen Skalierungsfeld
  hat die Aufnahme des Snapshots dieselbe Reihenfolge.
- **Verschiebe-Gizmo im Viewport:** Ein Zug über mehrere Frames ergab nach
  dem Loslassen genau einen Undo-Schritt und ließ sich vollständig wiederholen.
  Fallen Strg+Z und Loslassen in denselben Frame, verarbeitet
  `DrawEditorPanels` den Shortcut in `DrawFileMenuBar` vor dem Abschluss
  des Drags in `UpdateTransformGizmoInput`. Im Test mit leerer Historie blieb
  der erste Tastendruck wirkungslos, der zweite stellte die Startposition wieder her.
- Geprüft mit einem temporären ImGui-Test ohne Editorfenster gegen `f1c1a5b`.
  Der Test speiste Maus- und Tastaturereignisse in die Viewport- und
  Inspector-Funktionen ein.
- **Umgesetzt:** Viewport-Drags werden beim Loslassen als ein Schritt abgeschlossen.
  Für Box-Griffe erfasst die Historie Größe und Mittelpunktversatz. Lokale
  Positions- und Skalierungsfelder erfassen den Zustand vor der Änderung.
  Undo und Redo per Tastatur laufen nach den Inspector-Updates. Das Einrasten
  an einer Oberfläche per `G` hat ebenfalls einen Undo-Eintrag.
- **Verzögerung beim Undo (Claude):** `RestoreSceneSnapshot` baut die Szene neu
  auf. Gemessen mit DemoProject „Room 2“ (110 Entities, Release, temporäre
  Messpunkte, wieder entfernt): Undo 235 ms → 79 ms. Ursachen und Änderungen:
  - `ResolveMeshAssetBindings` importierte für jedes `modell.glb#mesh/N` die
    ganze Datei neu, obwohl das Mesh registriert war. Jetzt `FindMeshByPath`.
  - Laufzeit-Materialien mit `materialOverrideSource` hatten die Entity-ID im
    Schlüssel; Undo vergibt neue IDs → 64 Materialien pro Undo neu gebaut (und
    die alten nie freigegeben). Schlüssel nutzt jetzt `GetStableEntityId`.
  - Materialien liefen pro Undo dreimal über die ganze Szene:
    `EnsureCameraGizmos` löst nur noch die neuen Gizmos auf (der Ladeweg beim
    Projektöffnen macht den vollen Durchlauf jetzt explizit danach), und
    `ResolvePrefabBindings` löst am Ende nur die abgeglichenen Instanzen auf.
  - Prefab-Abgleich braucht vorher aufgelöste Materialien (sonst falsche
    Overrides wie `baseColorTexture`); geprüft: Szenen-Snapshot nach dem Laden
    byte-gleich zum Stand vorher.
  - Rest: Material-Durchlauf ~28 ms, Prefab-Dateien laden/abgleichen ~25 ms,
    Deserialisieren ~15 ms. Debug-Build entsprechend langsamer.
  Sichtprüfung im Editor offen.

### Transactional editor undo and redo (2026-11-16)

Step 0 analysis against `f1c1a5b`. Review this analysis before starting step 1.
The main checkout has local changes to the Inspector and viewport undo paths,
shortcut ordering, and their tests. Carry those changes forward when integrating
the migration so the new history preserves the corrected gesture boundaries.

1. **Serialization granularity.** `SceneSerializer::SerializeToJson` visits all
   live entities. `SetEntityFilter` can emit a single entity or an explicitly
   collected subtree in the scene format, with a full world scan. The component
   writers are already individually registered by type ID. The public API cannot
   call a writer for one component or write a supplied entity list directly.
   `SceneDeserializer::DeserializeFromValue` can append a scene payload containing
   just the desired entities, but always creates new entities. It remaps parents
   present in the payload on a second pass and drops links to external parents.
   Readers are registered by type name and generally call `World::Add`, which
   requires the component to be absent. Expose narrow entry points around these
   handlers and account for replacement of existing components. Keep the file
   format compatible. The current JSON writer rounds fractional floats to six
   significant digits and the transform reader forces `inheritParentScale` to
   `true`. History must round-trip the edited values faithfully.
2. **Entity identity.** `World::CreateEntity` assigns a fresh index at generation
   1 or takes an index from the LIFO free list and increments its 8-bit generation,
   skipping zero. `IsAlive` checks both the live flag and generation. The API
   cannot request a particular ID, and a hot slot can eventually wrap. Give
   history an independently allocated stable ID and keep the live bindings in
   both directions. Remove the live binding on destruction and bind the saved
   stable ID to the new runtime entity on restore. The existing
   `EditorState::stableEntityIds` maps runtime values to stable values for UI
   identity and currently falls back to the runtime value. Play and isolated
   prefab mode stash the scene and its history, then rebuild the edit world on
   exit. Rebind stable IDs across those restores and the migration fallback so
   older operations can still resolve their targets.
3. **References and lifetime.** `ParentComponent` holds a runtime ID and is the
   authoritative hierarchy. `ChildrenComponent` holds the reverse links. Keep
   external parent links and sibling order in the subtree operation and repair
   the affected links on replay. Script entity fields persist target GUIDs and
   hold runtime IDs in stored field values and live script objects. Restored
   GUIDs allow incoming references on surviving entities to resolve again.
   Prefab instances persist source record IDs, scene GUID bindings, overrides and
   the baseline recipe. Preserve those fields and `PrefabSourceComponent`,
   including any changed binding on an instance root outside the subtree.
   `EditorSceneTagComponent` holds the owning scene name for additive scenes and
   is captured separately from the scene handlers. Capture active and multiple
   selection in stable IDs. Reveal/highlight, camera gizmos, rotation editing,
   and the prefab-mode root also depend on runtime IDs and may require repair or
   invalidation. Physics body/collider ownership, audio source state and particle
   emitter state are keyed by live IDs. Let those systems retire old entries and
   initialize restored components through their normal sync paths. Probe bake
   requests carry runtime IDs, while baked files use a separate stable `probeId`.
   Preserve that ID and account for the scene-save cleanup of unused bake files.
4. **Replay side effects.** `RestoreSceneSnapshot` re-resolves script references,
   reapplies scene tags, resets selection and transient editor state, removes
   preview/runtime entities, and normalizes sibling orders. `SyncEditorSceneState`
   also rebuilds child links, creates missing `WorldTransformComponent` and
   hierarchy data, normalizes object layers, binds meshes, creates camera gizmos,
   binds materials after meshes, refreshes prefab instances, and updates local
   bounds, world transforms and world bounds. Use the operations' affected
   entities to scope these updates. The mesh and material resolvers already
   accept entity spans, and `UpdateLocalBoundsForEntity` handles the local bounds.
   For a transform value edit, update the existing component and mark it dirty
   without changing ECS structure so normal transform propagation updates only
   the affected branch. Reparenting also changes the local transform to preserve
   the world pose, so record both local poses. Include prefab single-root
   enforcement and any side effects in the same transaction. Whole-scene stashes
   for Play and prefab isolation have a separate purpose from undo history.
5. **File actions.** `EditorUIProject.cpp` dispatches Undo/Redo. File and folder
   operations live mainly in `EditorAssetBrowser.cpp`. Move and inline rename
   call `std::filesystem::rename` and update parts of browser state and the
   thumbnail cache. Inline script rename can refresh the script project. File
   and folder deletion call `remove`/`remove_all`. These actions have no history
   entries and ordinary asset moves do not rewrite references. The material
   editor's own rename updates live component paths and slot overrides but has
   no undo entry. Scene rename delegates to `KromEditorApp::RenameEditorScene`,
   which rewrites supported scene references, updates the project, editor settings
   and loaded scene tags, rolls back file changes on failure, then clears the
   history. Scene deletion changes loaded-scene/project state and removes the
   file permanently. Reuse the scene rename rollback pattern for reversible path
   and reference changes and use `core/PathUtf8.hpp` at path boundaries.

Verification during migration should compare snapshot and transaction timings on
the same large scene and assert that unrelated entity IDs, component values and
the world structure version survive a transform-only undo. The mixed history
test should exercise lookup of the recreated entity by an older operation.

Baseline verification built `krom_tests`, `krom_editor_opengl` and
`editor_scene_runtime_opengl` in the analysis worktree. `krom_tests` passed.
Both OpenGL processes stayed up for the 12-second launch check. The separate
headless runtime warm-up did not finish within the smoke-test limit. The demo
run also reported missing textures. Interactive rendering was not verified.

#### Step 1 — transaction core (reviewed)

- History entries store a label and ordered operations. Undo runs the operations
  in reverse, and redo runs them in their original order. Failed replay reverses
  completed operations and keeps the entry available for retry. The history
  retains 64 entries and blocks replay while an explicit transaction is open.
- The transaction API captures active and multiple selection at begin and commit.
  An empty transaction leaves both stacks intact. Material, animation and
  controller callbacks use the same history core and retain their selection
  behavior. Existing scene mutations still use the snapshot fallback.
- Editor IDs are allocated independently of runtime IDs, with lookup in both
  directions and unbinding when the world destroys an entity. The fallback,
  Play and prefab isolation rebind restored entities to their saved IDs. The
  hierarchy and Inspector use those same stable IDs for their ImGui state.
- All `krom_tests` passed. New cases cover ordered operations, selection,
  empty transactions, mixed replay through the snapshot fallback and Play,
  runtime ID wraparound, replay failure and the history limit. The Vulkan and
  OpenGL editors and the OpenGL scene runtime built in Debug. Both OpenGL
  executables stayed up during a 10-second launch check. The runtime used a
  minimal test scene. Interactive rendering was not verified.

#### Step 2 — properties and components (reviewed)

- Inspector fields, component add and remove, component clipboard actions,
  transform gizmos, box handles, and surface snap record affected component
  values through the scene handlers. The component payload uses round-trip
  float precision. Existing components are updated in place, and transform
  replay retains version counters and marks the transform dirty.
- Multi-selection edits capture affected components together, including mesh
  and bounds or rigidbody and box where an action adds both. Camera activation
  includes the previously active camera. Scene structure and material asset
  assignment still use the snapshot fallback for step 3.
- Includes the existing local fixes from the main checkout for viewport drag
  completion, box handle history, initial local transform values, and running
  history shortcuts after Inspector input.
- All `krom_tests` passed, including simulated viewport drags for single and
  multiple selection, box handles, and the G shortcut. With 4,096 entities in
  a Debug test, one position undo took 0.334 ms through the component operation
  and 1,122.904 ms through the snapshot fallback. The component test verified
  that unrelated IDs and transform values stayed intact, the structure version
  did not change, and only the target transform was recomputed.
- The Vulkan and OpenGL editors and OpenGL scene runtime built in Debug. Both
  OpenGL processes stayed up for the 10-second start test. The timing was a
  test-harness measurement. Interactive rendering and perceived frame delays
  remain to be verified in the editor.

#### Step 3 — scene structure and asset assignments (reviewed)

- Creation, duplication, deletion, and prefab and model instantiation record
  the affected entities. Replay restores their stable IDs, parent links, sibling
  order, scene tags, and registered components. Reparenting and reordering store
  only changed hierarchy and local transform values. Prefab mode includes root
  enforcement in the same entry as the structural action.
- Prefab reset records changes to linked nodes, including nodes moved outside the
  instance tree. Surviving entities keep their runtime IDs, and incoming script
  references are updated when their targets disappear or return. Baked probe
  files referenced by undo or redo entries remain available after saving.
- Mesh, material, and animation assignments record component changes. Replay
  resolves assets on the affected entities after applying the component values.
  The scene snapshot mutation wrapper has no production caller. It remains
  available for tests and comparison until step 5.
- All `krom_tests` passed, including the three-step mixed undo/redo chain and
  prefab instantiation and reset with nodes added, removed, and moved. The
  Vulkan and OpenGL editors and the OpenGL scene runtime built in Debug. Both
  OpenGL processes stayed up for the 10-second start test. Interactive rendering
  and an editor-window timing check remain to be verified.

#### Step 4 — file actions (handoff)

- Continue on `codex/transactional-editor-undo` in the existing KROM Codex
  worktree. Step 3 is committed as `5733891` and the user ran the tests
  successfully. Step 4 implementation has not started.
- The asset browser has separate move, inline rename, file delete and folder
  delete routes in `addons/editor/EditorAssetBrowser.cpp`. The material editor
  also has its own rename in `addons/editor/EditorMaterialLibrary.cpp`. Give these
  entry points shared file operations on the existing editor history stack.
- In `app/KromEditorApp.cpp`, `RenameEditorScene` already rewrites scene script
  references and rolls back failed writes. It currently clears history. Scene
  deletion also has a separate direct path callback used by the asset browser.
  Cover both scene routes and preserve project state on replay.
- Retain deleted files and folders for restore. For move and rename, handle
  live and on-disk scene and asset references, including asset fragments and
  directory moves. Use `core/PathUtf8.hpp` at every path conversion. Verify
  Undo/Redo with UTF-8 names and a destination conflict, then run the existing
  Debug build and tests under `out/undo-baseline` and the OpenGL start check.

#### Step 4 — file actions (Claude, ready for review, uncommitted)

- New `addons/editor/EditorFileHistory.{hpp,cpp}`. `MoveEditorAssetPath` serves
  asset browser drag moves, inline rename and the material editor rename. One
  history entry holds a symmetric relocation callback and the component operations
  for live references. The relocation moves the file or folder together with a
  model's `.clips` file. It then rewrites references in every `.json`, `.kscene`,
  `.mat`, `.prefab`, `.animctrl` and `.particle` below the asset root. Undo runs the
  same relocation back, so later saves of those files do not block it. A failed
  write rolls back written files and the move.
- Matching is exact on the asset-relative path, also with backslashes. A `#`
  fragment is kept, and for folders `<folder>/...` matches. `x.glb.clips` and
  `x.mats` do not match. Only matching JSON string tokens change; other bytes stay.
  JSON stored in a string, such as `baselinePrefabJson`, is rewritten inside.
- Live references go through `RewriteEditorComponents` in `EditorCommands.cpp`.
  It serializes each scene component, rewrites it, applies it through the scene
  handler and records it like `EditComponents`. Mesh, animation and material
  bindings of the changed entities are then resolved again. The relocation also
  maps the material editor selection, the environment texture path, browser
  selection, current folder and expanded folders. Thumbnails move along, and the
  script project refreshes when a script source moves.
- `DeleteEditorAssetPath` replaces all twelve delete sites in the asset browser,
  including folders, probes and scripts. `EditorTrash` moves the files to
  `<project>/editor/.history-trash/<id>/`. Undo restores them and fails without
  changes if the original path is taken, so the entry stays for retry. Releasing
  the last history entry deletes the trash permanently. This happens with the
  history limit, a cleared history or editor exit. A crash leaves the folder in
  place. The folder delete dialog now says that undo restores the folder.
- `RenameEditorScene` records an entry that calls itself in reverse instead of
  clearing history. `DeleteEditorScene` also trashes the `.kscene` export and
  records the restore with the scene order position and build exclusion. Redo is
  refused while that scene is open or loaded. Unloading an additively loaded scene
  is not recorded, so its deletion clears the history first. Deleting the open
  scene switches scenes, which already cleared history. A failed switch now aborts
  the deletion. The asset browser path callback deletes a project scene through
  `DeleteEditorScene` and other `.json` files as assets. Previously it removed the
  file without updating the project.
- Not covered: script fields that hold absolute or `Assets/`-prefixed paths, and
  `krom-project.json` when a scene file is moved by drag instead of renamed.
  Open animation and controller windows keep their old path. A case-only rename
  is allowed when the file system reports the same file.
- Debug build in `out/undo-baseline`: `krom_tests`, `krom_editor_vulkan`,
  `krom_editor_opengl`, `editor_scene_runtime_opengl` and
  `krom_editor_loading_tests`, with no new warnings. All `krom_tests` passed. New
  cases cover reference matching and nested JSON, plus rename, move, folder move
  and delete with UTF-8 names. They also cover live component paths, a destination
  conflict, a restore conflict and trash removal. `krom_editor_loading_tests
  --scene-rename` passed, now including undo and redo for scene rename and delete
  in sequence. Both OpenGL executables stayed up for the 10-second start test.
  Interactive use in the editor window was not verified.

### GTAO aus der Tiefe desselben Frames (2026-11-15)

- In GTAO-Views schreibt `DepthPrepass` die Tiefe der Opaque-Liste. Der
  Farbpass verwendet denselben Vertex-Shader, testet mit `LESS_EQUAL` und
  bindet die Tiefe schreibgeschützt. Die Maskenvariante der Standardshader
  wertet denselben UV- und Opacity-Code bis einschließlich `clip()` aus.
- Der Graph bindet den Tiefenanteil von HDR als separate Ressource. GTAO
  linearisiert bei MSAA die kleinste Tiefe pro Pixel. Der Blur schreibt die
  transiente `GTAOOutput` in `R16_FLOAT`, die der Farbpass per Pixel liest.
  History, Reprojektion, Projektfeld und UI-Schalter sind entfernt. Das
  bisherige Projektfeld wird beim Lesen ignoriert.
- Nach Opaque folgen MSAA-Resolve und SSR. Transparent rendert danach in
  `GTAOComposite` mit der aufgelösten Szenentiefe. Bloom und Tonemap lesen
  bei aktivem Composite die dort zusammengesetzte Farbe.
- Vulkan übernimmt die Eingangs-Locations flacher Vertex-Shader aus SPIR-V.
  So bindet der Prepass keine überzähligen Attribute aus dem Mesh-Layout.
- Debug x64: `krom_tests`, die drei Editoren und der Win32-Runtime-Wrapper
  gebaut. Alle Unit-Tests bestanden, einschließlich Passreihenfolge,
  Tiefenstatus, alter Projektdateien und Masken-Shader für DX11 und Vulkan.
  OpenGL-Editor gestartet.
- Room 2, GPU-Readback der F3-Ansicht nach vollständiger Shader-Kompilierung.
  Jeweils den ersten Frame nach Kameradrehung, seitlicher Verschiebung oder
  Verschiebung des Spielerkörpers mit dem Standbild am selben Ziel verglichen.
  Bei MSAA 1× und 2× mit SSR an und aus waren die AO-Pixel auf DX11 und
  Vulkan pixelgenau gleich. Die übrigen Differenzen lagen im Bereich der
  animierten Tür. Alle vier Vulkan-Läufe mit `KROM_VALIDATION=1` blieben
  ohne Validierungsmeldung. Der stationäre Vergleich mit `4913fa2` war
  auf Vulkan bei MSAA 1× identisch. Bei MSAA 2× mit SSR betrug die mittlere
  absolute Abweichung auf beiden Backends 0,016 von 255 je AO-Pixel.
- Separate Sichtprüfung im Vulkan-Editor mit 35° Kameraschwenk um den
  Spielerkörper. Das erste Bild nach dem Schwenk und das Standbild danach
  waren identisch. Die Aufnahmen liegen unter `out/gtao-check/final-vulkan-orbit`.
- Wiederholung über `krom_editor_loading_tests[_dx11] --gtao-motion <Projekt> <Ordner>`.
  `KROM_GTAO_ORBIT=1` speichert zusätzlich den Schwenk in der Lit-Ansicht.
  `KROM_PERF_COMPARE_GTAO=1` misst mit `--perf` beide Zustände im selben Lauf.

`--perf` in ms pro Frame auf einer DemoProject-Testkopie, GTX 1660, Debug x64,
1920×1080 Editorfenster, MSAA 2× und SSR an. Pro Zustand 300 Messframes nach
vollständiger Shader-Kompilierung und 60 Warm-up-Frames. Referenz ist `4913fa2`
mit GTAO-History. Die Spalte „GTAO aus“ erfasst den aktuellen Pfad ohne die
GTAO-Kette und ihren Prepass.

| Backend | Szene | Referenz | Prepass + GTAO | GTAO aus |
| --- | --- | ---: | ---: | ---: |
| DX11 | Room 2 | 14,89 | 16,15 | 16,31 |
| Vulkan | Room 2 | 21,14 | 22,76 | 20,48 |
| DX11 | car | 10,05 | 10,36 | 10,44 |
| Vulkan | car | 13,52 | 13,85 | 13,00 |

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
