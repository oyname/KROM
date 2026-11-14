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
