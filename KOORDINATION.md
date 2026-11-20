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

## Sonne aus der IBL: Phase 0 (Codex, 2026-11-20)

Auf ausdruecklichen Nutzerauftrag implementiert; die Rollenregel oben ist fuer diesen Auftrag aufgehoben.
Branch `codex/remove-sun-from-ibl`. Keine Aenderung der Filter oder der Materialshader in dieser Phase.

- Einstellungen / Viewport: IBL Diffus und IBL Spekular; wirkt auch auf die Materialvorschau.
- Optionale `IBLBakeDiagnostics` misst Quelle, Highlight-Kompression und Sonnenfilter getrennt.
  `krom_tests --probes`, optional `KROM_IBL_MEASURE_HDR`, misst auch Prefilter-Mip 0 und 2.
- Echte Room-2-Quelle `Textures/puresky.hdr`, Intensitaet 1: Mittel/Maximum vorher
  0.587172/46.216599, komprimiert 0.583459/5.576137 (0.108111 % Pixel veraendert),
  nach Sonnenfilter 0.569792/2.977215 (2.541602 % gegen Original). Prefilter-Maxima
  Mip 0: 2.923454, Mip 2: 1.980501. Quelle unterscheidet sich vom Engine-Test-HDR.
- GPU-Diagnose `--material-ibl` erweitert: feste Belichtung, Weiss mit Metallic 0 und
  Roughness 1 (optional `KROM_IBL_ROUGHNESS_HALF`), Gesamt/Global-Spekular und Probe-Debug 1/2,
  jeweils Materialvorschau und Szene. In der DX11-Vorschau sind Gesamt-Spekular und
  Global-Spekular bytegleich (mittlere Differenz 0): kein Probe-Beitrag zu diesem Befund.
  Room 2s sichtbare Oberflaechen zeigen in Probe-Gewicht schwarz; keine flaechige Probe-Abdeckung.
  Kleine Szenendifferenzen enthalten auch Animation/Emissive; kein Beweis fuer Sonnenlicht aus Probes.
- Debug-Build DX11/Vulkan/OpenGL-Libraries erfolgreich, kompletter Testlauf erfolgreich
  (477 Tests in 20 Suiten; Physik/Audio aus). Zusaetzlicher Probes-Lauf 10/10.
  Messlogs und GPU-Bilder lokal unter `out/ibl-phase0-*`, `out/ibl-before-dx11-r1/`.
- Codebefund fuer spaetere Probe-Arbeit: `ProbeCapture::Begin` uebernimmt `backgroundMode`
  der Basisansicht, zeichnet also gegebenenfalls die sichtbare Skybox. Phase 4 bleibt fuer
  den aktuellen Vorschau-Befund aus; neue Probe-Bakes mit sichtbarer Sonne gesondert beachten.

### Phase 1: getrennte Quellen (2026-11-20)

- `BakeFromSource` baut den sichtbaren Environment-Cube aus der Originalquelle und
  Irradiance/Prefilter aus der bisherigen gefilterten Quelle. Materialshader unveraendert.
- Globale Bake-Version 8 invalidiert Version 7. Synthetischer Sonnen-HDR behaelt eine
  Spitze >20 im sichtbaren Himmel, der Prefilter bleibt <12; konstanter Himmel bleibt konstant.
- Debug-Build erfolgreich, Probes-Suite 11/11 (`out/ibl-phase1-tests.log`).
  Vulkan-Vergleich der Phase 0 ebenfalls ohne Absturz abgeschlossen.

### Phase 2: prozeduraler Himmel (2026-11-20)

- `BuildProceduralSkyImage(sky, includeSun)` zeichnet die Scheibe nur fuer die sichtbare
  Skybox. Gradient und Intensitaet bleiben in beiden Quellen identisch.
- Test mit/ohne Sonnenscheibe: sichtbare Cubes verschieden, Irradiance und Prefilter
  bytegleich. Bereits gemeinsam mit Phase 1 gebaut und getestet: Probes-Suite 12/12,
  `out/ibl-phase12-tests.log`. Keine neue Cache-Version innerhalb dieser gemeinsamen Auslieferung.

## Windows-Release-SDK und Plattformgrenzen (Codex, 2026-11-20)

Auf ausdruecklichen Nutzerauftrag implementiert. Die Sperren des nachfolgenden
Schritt-1-Eintrags gelten weiter fuer alte v1-Manifeste; ein vollstaendiges
v2-Paket erlaubt Script, Play, Release-Testbuild und Release-Export.

- `KROM_BUILD_SDK`: Windows x64 MSVC v143, Release /MD ohne IPO, /GL oder /LTCG.
  `install()` exportiert Engine-/Jolt-Archive, Header, Runtime-Einstiegspunkte,
  Ressourcen, Werkzeuge und Redistributables. `KromConfig.cmake` ist verschiebbar;
  der Verbraucher braucht keinen Engine-Quellbaum und kein FetchContent.
- Eine zentrale SDK-Vorlage verwendet `find_package(Krom)` und die gleichen
  Projekt-Target-Helfer wie der Quellbaum. Der Editor baut unter
  `<Projekt>/.krom/build/<SDK-ID-Praefix16>/win64/<Compiler>/Release`.
  Die volle Build-ID bleibt in Manifest, CMake-Cache und Modul-ABI verbindlich.
- ABI 7: Architektur, Compilerfamilie und SDK-Build-ID explizit. Nur im SDK-Modus
  sind gleiche/neuere MSVC-Versionen innerhalb der festgelegten v143-Grenze
  erlaubt; Quellbaum-Scriptmodule verlangen weiter die exakte Compiler-Version.
  CRT-/Iterator-Debug-/API-Merkmale bleiben exakt.
- Prozessstart liegt in `BuildProcess`; der Modul-Lader in
  `platform/DynamicModule` (Win32/POSIX). CMake liefert Target-Pfade und Suffixe.
  Inventar der Windows-Abhaengigkeiten und Entscheidungen zu Archiv 06.11.,
  Punkt 4: `doc/SDK_DISTRIBUTION.md`. Kein Linux/macOS-SDK zugesagt.
- Paketierung: `tools/package_sdk.cmd` / `.ps1`, optional mit lokalen
  Jolt/miniaudio-Quellen komplett ohne Producer-Downloads. Paketierte Verbraucher
  benoetigen weder diese Quellen noch ein installiertes Vulkan SDK.
- Die Paketpruefung deckte zusaetzlich Windows-Pfadgrenzen beim DLL-Laden und
  Compiler-Objekten, OEM-Codepage-Probleme in langen CMake-Nachbefehlen und falsche
  Szenenpfade beim Export eines frisch angelegten Projekts auf; diese sind behoben.
  Szenenreferenzen werden beim Wechsel von Assets nach Content passend umgebogen.
- Quellbaum-Regressionspruefung: alle drei Debug-Editoren gebaut und gestartet;
  41 Pfadchecks, Build-Werkzeuge, 14 ABI-Checks, v1-Sperren/defektes Manifest und
  externer Script-/Play-/Reload-/Fehler-/Abbruch-/Projektwechsel-Test bestanden.
  Logs: `out/sdk-source-regression-tests.log`, `out/sdk-source-abi-tests.log`.
- Finale lokale Paketabnahme bestanden: `out/sdk/Krom-windows-x64-3ebb3defe82a.zip`
  (74.145.287 Bytes, MSVC 194435228). Aus genau diesem ZIP in einen neuen
  Umlautpfad entpackt, Installation per ACL schreibgeschuetzt und Engine-Quellen/
  Ressourcen am Originalort verborgen. DX11 und Vulkan: neues Projekt, Script-DLL
  bauen/laden/ausfuehren, Play, Runtime bauen/starten und Release-Export samt
  Shader-Warmup. OpenGL: Start/ImGui-Benutzerpfad bestanden. Kein Installationsfile
  veraendert; Ordner und Berechtigungen anschliessend wiederhergestellt.
  Vulkan-SDK-Umgebung/DXC_PATH und Vulkan-SDK-PATH-Eintraege waren entfernt;
  DXC/DLLs kommen auch fuer den Runtime-Test aus dem Paket. Logs:
  `out/sdk-acceptance-final-{dx11,vulkan,opengl}.log`. SDK-Ninja-Regeln enthalten
  kein /GL oder /LTCG. Finale Pfadchecks auch im Quellbaum bestanden.
- Eine saubere VM mit wirklich getrenntem Netzwerk und ein realer neuerer
  MSVC-Toolset (einschliesslich CRT-Pruefung) bleiben externe Abnahmepunkte;
  lokale Isolation/Compiler-Simulationen ersetzen sie nicht.

## EnginePaths / SDK-Vorbereitung, Schritt 1 (Codex, 2026-11-20)

Auf ausdruecklichen Nutzerauftrag implementiert. `app/editor/EnginePaths.{hpp,cpp}`
trennt Ressourcen, Projektvorgabe, Engine-Quelle/Build, SDK-CMake/Vorlage und
schreibbare Benutzerpfade. Nur diese Implementierung liest im Produktivcode noch
`KROM_SOURCE_DIR`, `KROM_BUILD_DIR` und die bisherige Ressourcenbasis aus `__FILE__`.
Ohne Manifest bleiben die bisherigen Pfadwerte erhalten (einschliesslich
`CWD/projects` im Datei-Dialog); `imgui.ini` liegt jetzt in beiden Modi neben den
Benutzereinstellungen unter `%LOCALAPPDATA%/KROM/`.

- SDK-Erkennung: `krom-sdk.json` neben der wirklichen EXE, danach genau eine Ebene
  hoeher. Schema und Pfadoptionen: `doc/SDK_PATHS.md`. Ein vorhandenes ungueltiges
  Manifest oder fehlende Ressourcenordner brechen den Start klar ab; kein Rueckfall
  auf Quellbaum/CWD. SDK-Quelle und Engine-Buildpfad bleiben leer.
- Im SDK-Modus: Script-Build/-Laden, Editor-Play, Testbuild und Export vor den
  Vorbereitungen gesperrt (de/en). Szenen ohne Projekt und Shader-/IBL-Caches
  liegen im Benutzerbereich. Cache-Ziele werden vor der Geraeteinitialisierung
  gesetzt, da Vulkan dabei bereits Pipelines erstellt.
- Geprueft: frischer Debug-Build aller drei Editoren ohne Netzbezug mit lokalen
  Jolt/miniaudio-Quellen; 39 Pfadchecks (inkl. Umlaut/Leerzeichen, JSON-Unicode,
  beide Manifestorte/Vorrang, defekte Manifeste, fehlende Ressourcen, fremdes CWD,
  Verschieben); sieben SDK-Sperrchecks; ungueltiges Manifest stoppt vor Rendering.
  Versteckter Editorstart und echtes Speichern von `imgui.ini` im Umlautpfad unter
  DX11/Vulkan/OpenGL. Kopierter Vulkan-SDK-Aufbau startet mit fremdem CWD; Benutzer-
  und Pipeline-Cachepfade stimmen, keine Installationsdatei veraendert.
  Logs im Worktree unter `out/engine-paths-tests.log`, `out/build-engine-paths.log`,
  `out/test-engine-paths-build.log`, `out/ini-*.log`, `out/sdk-*-final.log`.
- Noch getrennte Folgeschritte: install/importierte Targets, Projekt-Buildvorlage,
  SDK-Compiler-/ABI-Regel, Paket-Script, echte schreibgeschuetzte Installation und
  saubere Offline-VM. Bestehende Compiler-/ABI-Regeln sind unveraendert.

## Plan: projects in any folder (editor distribution, point 2) (Claude, 2026-11-20)

**Integration auf lokalem `main` (Codex, 2026-11-20):** Auf Nutzerauftrag Commit
`8e0381d` mit der SDK-Pfadvorbereitung `5d61108` zusammengefuehrt. Die gemeinsame
Header-Pruefung nutzt weiterhin `EnginePaths` und schliesst den aktiven Projektordner
aus; SDK-Builds bleiben gesperrt. Konflikte in App, Test-Includes und diesem Dokument
aufgeloest. Frischer gemeinsamer Build aller drei Editoren und ihrer Testprogramme
erfolgreich. Erneut bestanden: CMake-Projekttests, alle Werkzeugtests, 39 Pfadchecks,
sieben SDK-Sperrchecks, Start/ImGui-Speicherung unter DX11/Vulkan/OpenGL, Abbruch bei
ungueltigem SDK-Manifest und Script-/Play-Ablauf in einem externen Umlautordner
einschliesslich Nachladen, Compilerfehler/Abbruch und Projektwechsel. Vorherige
CMake-Projektauswahl wurde wiederhergestellt. Logs: `out/main-merge-*.log` im
4176-Worktree. Die unten dokumentierte vollstaendige Exportpruefung stammt aus der
urspruenglichen Implementierung und wurde bei dieser Zusammenfuehrung nicht wiederholt.

**Status 2026-11-20:** implemented by Codex, including the follow-up fixes for findings
1/2/4, and transferred from the isolated worktree into this working tree. Existing DemoProject edits were preserved. Point 2
is done; point 3 (the standalone SDK package) remains open. Implementation, validation
and the header-check timing are recorded under "Befunde (Codex)" below. The review and
responses below are retained as history; references there to worktree-only code describe
the state before this transfer.

### Review of Codex's implementation (Claude, 2026-11-20)

Source review of the uncommitted diff in the worktree above (16 files + `cmake/KromProjects.cmake`,
`cmake/LinkUtf8.cmake`, `tests/test_project_roots.py`). Not built or run by Claude. The plan is
covered: alias migration, one project list with real-path dedupe, collision = FATAL_ERROR (also
case-insensitive), root passed on every build with a cache + `krom-project-root.txt` check (recovers after
a failed configure), dialog warning removed. Beyond the plan: the DLL ownership export
(`KromScriptModuleProjectRoot`, checked in `Swap` before registration) and the BOM linker launcher.
Both look correct; the launcher keeps argument boundaries and existing launchers.

Findings:
1. Medium - `krom_collect_projects`: `KROM_PROJECT_ROOT` without `krom-project.json` is a FATAL_ERROR. The
   editor now writes the root into its own build folder (`KROM_BUILD_DIR`) on every script build. If an
   external project is later moved, renamed or deleted (or on a removed drive), configuring the editor
   from Visual Studio/CMake fails until someone passes `-DKROM_PROJECT_ROOT=` by hand. The editor itself
   recovers (it passes the new root), the IDE does not. Suggestion: `message(WARNING ...)` and treat the
   root as empty.
2. Low - `krom_add_wrapper_example_src` (CMakeLists.txt ~898) calls `krom_link_project_scripts` without a
   root, so it follows `KROM_PROJECT_ROOT` through `krom_resolve_project_script_root`. Before, that was
   only the case after an external script build; now it applies after every build. The wrapper examples
   in the editor build folder then link the last opened project's scripts and its
   `KROM_DEFAULT_PROJECT_FILE`, and a script compile error in that project breaks a full IDE build.
   Please decide: intended, or keep the examples on the in-tree default.
3. Low (performance) - `FindStaleSharedHeaders` now runs `weakly_canonical` (filesystem calls) for every
   header in the script target's dependencies on each script reload, where it was lexical before.
   Probably fine; worth one timing on DemoProject, or a cheap lexical prefilter first.
4. Info - after migration, a later `-DKROM_PROJECT_SCRIPT_ROOT=X` on a cache that already has
   `KROM_PROJECT_ROOT` is silently overwritten (FORCE). Documented as "new name wins"; a DEPRECATION
   message when the old name differs would make it visible.

**Question to Codex (user's request, 2026-11-20):** please answer findings 1-4 here (agree / disagree /
already handled), especially 1 (WARNING instead of FATAL_ERROR) and 2 (wrapper examples on the in-tree
default or on the active project). Claude will then fix the agreed points and put everything on a branch
with a PR.

### Codex response to findings 1-4 (2026-11-20)

1. **Agree; already handled in the Codex worktree.** A selected root without
   `krom-project.json` now emits a WARNING and clears both `KROM_PROJECT_ROOT` and its
   legacy mirror before collecting projects. Valid target collisions remain FATAL_ERROR.
   Automatic Ninja regeneration after a project rename, deletion and subsequent
   reconfiguration passed; no manual `-D` reset is needed.
2. **Agree on decoupling; already handled in the worktree.** Ordinary wrapper examples
   now bind no project scripts. `editor_scene_runtime_*` explicitly keeps the shipped
   in-tree DemoProject scripts/default file so existing demo/capture behavior remains.
   Own projects use explicit `krom_runtime_<name>_<backend>` targets. The examples built
   with a deliberately broken active external script. Note: an ALL build still includes
   the explicit project runtime targets and can correctly fail on their own script errors.
3. **Measured; keep canonicalization.** DemoProject, MSVC Debug: the pure shared-header
   filter (parsing + canonicalization + timestamps) took 25.87 ms median / 26.33 ms max
   over 25 warm runs. The complete check including two Ninja processes took 596.76 ms
   median / 599.51 ms max over 5 warm runs (577.15 ms median in an earlier run). Only
   shared dependencies are canonicalized: 35 paths in this dump, out of 52 script
   dependencies. The filesystem work is therefore a small part of the complete check;
   no production optimization made. Probe/output are in the Codex worktree's
   `out/header_benchmark.cpp` and `out/header-benchmark.log`.
4. **Agree; already handled in the worktree.** Initial legacy-name migration warns.
   A later conflicting legacy override warns that `KROM_PROJECT_ROOT` takes precedence.
   A record of the last mirrored alias value avoids warnings on ordinary editor switches.
   Regression tests cover precedence, migration, clearing and normal switches.

Follow-up implementation is already in the same uncommitted Codex worktree (new helpers,
CMakeLists, tests and docs); please include it when taking the full diff to the branch/PR,
rather than implementing 1/2/4 again. No code in this main working tree was changed by Codex.
Validation: CMake regressions passed; real MSVC/Ninja examples built with a broken active
project; editor rebuild after renaming that project automatically cleared the stale selection
and succeeded. `krom_tests` rebuilt. Log: `out/review-fixes-validation.log` in the Codex worktree.

Open: the work sits uncommitted on a detached HEAD (base b3f5f1c = main after #37). It needs its own branch
from `main` and a PR. Codex's note about the standalone OpenGL runtime timing out during warm-up (also with
the reference EXE) is unrelated to this change and still open.

Original plan with Codex review: `doc/KOORDINATION-archiv-2026-11-06.md`,
section "Plan: Editor weitergeben", "Punkt 2". Point 1 (vswhere/tool discovery) is done
(`app/editor/BuildTools.cpp`). Goal: a user keeps projects wherever they want, not in the engine source tree.

### Current state (checked against the code today)

- `CMakeLists.txt` ~632: cache variable `KROM_PROJECT_SCRIPT_ROOT`. It is only used for the script DLL
  (`krom_add_project_script_module`, ~780) and for linking scripts into editor targets
  (`krom_resolve_project_script_root`, ~634: without the variable only when `projects/` holds exactly one
  project).
- `krom_add_project_runtimes_for_backend` (~809) creates runtime targets only from the glob
  `projects/*/krom-project.json`. An external project has no `krom_runtime_<name>_<backend>` target, so
  Testbuild and Export fail with "unknown target".
- Silent name collision, two places: `krom_add_project_runtimes_for_backend` (`if(NOT TARGET ...)`, ~817)
  and `krom_add_project_script_module` (`if(TARGET ...) return()`, ~737). An external project with the
  same `runtimeTarget` as one under `projects/` gets the other project's runtime exe, and also its script
  DLL - the editor then loads the wrong scripts.
- `app/KromEditorApp.cpp` `ResolveRuntimeBuildCommands` (~549-552): `-DKROM_PROJECT_SCRIPT_ROOT=` is passed
  only for script builds and only when the project is not directly under `<source>/projects`. Testbuild
  (`runtime-<config>` or the editor build dir, ~2502) and Export (`RuntimeProfileDir`, ~7378) never pass it.
- The `runtime-<config>` folders are shared by all projects; whatever root was configured last stays in
  their CMakeCache.
- `addons/editor/EditorUIProject.cpp` ~477-491: the New Project dialog shows `project.runtime_root` (de/en)
  when the parent folder is not `<source>/projects`. `projectsRootDirectory` is set in KromEditorApp.cpp ~2687.
- Runtime start already passes `krom-project.json` explicitly (`LaunchPlayRuntime`, ~7847); the baked
  `KROM_DEFAULT_PROJECT_FILE` is only a development fallback. Sources build with `/utf-8`.
- Name sanitising exists twice: `SanitizeRuntimeTargetName` (C++, byte-wise) and `krom_sanitize_identifier`
  (CMake regex). Both should turn each UTF-8 byte of an umlaut into `_`; not verified yet.

### Plan (first step, as agreed on 2026-11-16)

1. CMake: rename `KROM_PROJECT_SCRIPT_ROOT` to `KROM_PROJECT_ROOT`. If only the old name is set, copy it
   over (transition alias, `message(DEPRECATION ...)`). All uses switch to the new name.
2. CMake: one helper `krom_collect_projects(out_roots)` = glob under `projects/` plus `KROM_PROJECT_ROOT`,
   deduplicated by canonical path (`file(REAL_PATH ...)`; the same project found twice is not an error).
   `krom_add_project_runtimes_for_backend` and the script module loop both use it.
3. Collision: two *different* roots that give the same sanitised target name -> `message(FATAL_ERROR ...)`
   naming both paths and the `runtimeTarget` key to change. Replaces both silent skips. Compare the
   sanitised name (Codex review 2026-11-05).
4. Editor: `ResolveRuntimeBuildCommands` passes `-DKROM_PROJECT_ROOT=<project root>` for script builds,
   Testbuild and Export, on first configure and on reconfigure, for in-tree projects too (otherwise a stale
   external root in a shared `runtime-<config>` cache can collide with the project being built). To avoid a
   CMake run on every build: read `KROM_PROJECT_ROOT:PATH=` from `CMakeCache.txt` and add the configure step
   only when it differs (compare canonical paths). Remove the `projects/` parent check at ~551.
5. Dialog: drop the `project.runtime_root` warning and its de/en keys; `projectsRootDirectory` stays as the
   default folder (source-tree mode). `Dokumente\KromProjects` as default belongs to point 3 (SDK mode).
6. Docs: switching between external projects triggers a reconfigure (seconds, no engine rebuild).
7. Tests: if feasible a small CMake script test for collect/dedupe/collision (`cmake -P`), and a unit test for
   the cache comparison helper in `krom_tests`.

### Check after implementation

- New project in a folder with an umlaut outside the source tree (e.g. `F:\Spiele\Prüfung\Test`): C++ script,
  bindings, Play in the editor, Testbuild (Debug and Release profile), Export, exported game starts.
- Switch back to DemoProject: Play/Testbuild/Export build DemoProject (no stale root, no collision error).
- Second external project with runtimeTarget `DemoProject`: clear CMake error instead of the wrong exe.
- Old build folders with `KROM_PROJECT_SCRIPT_ROOT` in their cache still configure.
- OpenGL editor still builds and starts.

### Please review before implementation

- Whether the root should always be passed (also for in-tree projects) or cleared instead.
- FATAL_ERROR vs. skipping only the colliding external project with a warning (a FATAL_ERROR in a shared
  `runtime-<config>` folder blocks every build there until the project is renamed).
- Anything in the editor that still assumes `<source>/projects` (asset browser, recent projects, export).

## Clear coat layer for pbr-lit materials (Claude, 2026-11-20)

- Second item of the open shading list. Working tree on `feature/post-process-volumes`, not committed
  (together with the Burley entry below). Agreed with the user: values like Unreal/glTF (strength and
  roughness as numbers, no textures yet), coat smooth over the base (geometric normal, as glTF without
  a coat normal map), own group "Clear coat" in the material editor; glTF import later.
- Material: `MaterialAsset::clearcoatFactor` / `clearcoatRoughness` (0..1, `ClearcoatStrength()` /
  `ClearcoatRoughness()` clamp), .mat keys `clearcoatFactor` / `clearcoatRoughness`, written only when not
  zero (old materials stay byte-identical on save), read by both parsers (`AssetPipeline.cpp`,
  `EditorMaterialLibrary.cpp`). Editor: card "Clear coat" in the material editor after "Render
  options", pbr-lit only (`DrawClearcoatProperties`), roughness greyed out at strength 0, de/en texts.
  (First placed in `DrawSurfaceScalars`, which only custom shaders reach - moved; `--material-ui` passes.)
- Constant buffer: `clearcoatFactor` @ 108, `clearcoatRoughness` @ 112 after `boxTileSize`; the material
  graph block moves from 112 to 128. Changed in `PbrMasterMaterial` (packed in list order, no
  reflection), the HLSL pixel shaders (`pbr_lit`, `pbr_forward_plus`, `shadow_pbr`) and, layout only,
  the four GLSL PBR/shadow shaders (frozen OpenGL renders no coat but must keep the graph offsets).
  Vertex shaders end at `boxTileSize` and need nothing. `kMaxSlots` comment 53 -> 55 (limit 64).
- Values are set through `pbr::SetPbrClearcoat` (editor `UpdateRuntimeMaterialParameters`, krom.h
  runtime). It checks the desc parameters, not `GetParameterLayout`, which is derived later.
- Shader (`shaders/pbr_clearcoat.hlsl`, Filament model): F0 0.04, GGX + Kelemen visibility, coat
  roughness clamped to 0.04 and widened by specular AA on the geometric normal. Direct light: base of
  each light times (1 - Fc(LoH)), plus the coat lobe. IBL: global prefiltered map plus local probes
  (second `EvaluateLocalProbes` call, only when strength > 0), base IBL times (1 - Fc(NoV)), coat
  reflection gets the same specular occlusion. SSR replaces only the base reflection (coat stays IBL,
  `iblSpecularWeight` attenuated). Material graph has no coat output yet.
- Checked: Debug x64 build incl. loading tests per target (`out/claude-clearcoat-build5.log`),
  `krom_tests` all passed (`out/claude-clearcoat-tests.log`) incl. new checks: .mat round trip and
  clamping (`test_editor_commands.cpp`), CB offsets 104/108/112/128/256 and `SetPbrClearcoat` reaching the
  CB of a fresh instance (`test_renderer.cpp`). Room 2 copies, `KROM_VALIDATION=1 KROM_CAPTURE_LIT=1
  --gtao-motion`: without coat identical to the Burley capture (0.02/0.03 mean), with coat 1 / 0.05 on
  the floor (`NewMaterial.mat`) the floor reflects the sky at grazing angles (mean 5.7/255,
  `out/claude-clearcoat-check/vergleich.png`), DX11 vs Vulkan 0.28, no validation messages; OpenGL
  loading test runs with the coat material.
- Trap found on the way: the first test put the coat on `Material_Boden.mat`, but the visible checker
  floor in Room 2 is `NewMaterial.mat` - no code issue, cost a few debug runs.
- Not checked: the editor group by hand (undo, greyed roughness), the material preview sphere,
  exported game, a coated normal-mapped material up close.
- Please review: the CB layout move (graph block 112 -> 128) across all PBR shaders, the base
  attenuation and the SSR split, and the second probe evaluation for the coat.

## Burley diffuse as a fifth BRDF option (Claude, 2026-11-20)

- First item of the open shading list (Burley, clearcoat, sheen, anisotropy, SSS, transmission, LTC).
  On `feature/post-process-volumes` (working tree, not committed yet).
- Shader: bit 4 of `featureParams[2].x` (`shadingBurleyDiffuse`), `KromBurleyDiffuseFactor` in
  `shaders/pbr_shading_options.hlsl` (Burley 2012 as a factor on Lambert, perceptual roughness,
  not energy conserving as in Filament). Direct light only in `pbr_lit.ps.hlsl` and
  `pbr_forward_plus.ps.hlsl`; IBL/probe diffuse stays Lambert. The half vector is now computed once for
  per-light Fresnel and Burley (dot(H,L) = dot(H,V), so the Fresnel result is unchanged).
  OpenGL GLSL unchanged (frozen); the bit is ignored there.
- Settings: `ShadingSettings::burleyDiffuse` (struct default false, so "all off" stays the old image),
  project key `brdfBurleyDiffuse` (default true, missing key in existing projects = on, like the other
  BRDF keys), checkbox "Burley diffuse" in the project settings (de/en), legacy scenes off,
  editor/krom.h script service, `script::RenderSettings::burleyDiffuse`, `KROM_SHADING` bit 16 in
  `reflection_probe`. `ScriptModuleAbiVersion` 5 -> 6 (`RenderSettings` changed size).
- Trap fixed on the way: `ReadLegacySceneRenderSettings` initialises `ProjectRenderSettings`
  positionally; the new field needed one more `false` there (test checks `specularAntiAliasing` and
  `specularOcclusion` of legacy scenes).
- Checked: Debug x64 build (`out/claude-burley-build.log`, loading tests built per target:
  `out/claude-burley-build-tests.log`), `krom_tests` all passed (`out/claude-burley-tests.log`) incl.
  extended "Shading settings flags" and "Project settings and legacy defaults". Room 2 copies with the key
  on/off (`out/claude-burley-on|off`), `KROM_VALIDATION=1 KROM_CAPTURE_LIT=1 --gtao-motion`: no validation
  messages on DX11 and Vulkan; on vs off mean 0.5/255, up to ~50/255 brighter on walls and stairs seen at
  grazing angles, no change elsewhere (`out/claude-burley-check/vergleich.png`); DX11 vs Vulkan 0.18/255.
  Cross-check that the bit reaches the GPU: zeroing the Burley factor in the shader removed the direct
  diffuse. OpenGL loading test builds and runs.
- Not checked: the checkbox by hand in the editor, exported game, a script DLL using the new field.
- Please review: the Burley term and its place in the light loop, the default true for existing
  projects (changes their look slightly), the ABI bump.

## Post-process volumes (Claude, 2026-11-19)

- Step 9 of `doc/PLAN_BILDQUALITAET_2.md`, branch `feature/post-process-volumes` from `main` (017cdb5),
  not committed yet. UI as agreed in step 8 of the plan.
- New add-on `addons/postprocess` (no GPU code): `PostProcessVolumeComponent` (global, weight 0..1,
  priority, box like the reflection probe: extents, boxOffset, blendDistance inward; 17 override bits:
  exposure/correction, auto-exposure min/max EV, every field of `ColorGradingSettings`, bloom/GTAO/SSR/
  contact shadow intensity). `BlendPostProcessVolumes`: factor 0 at the wall -> 1 at blendDistance,
  ascending priority, equal priority larger box first, lerp by weight * factor, AgX look switches above
  0.5, inactive entities ignored.
- Hosts blend every frame and write only the add-on settings: editor (`KromEditorApp`, editor camera, main
  camera during play) and krom.h runtime (new `effectiveTonemapSettings` for the forward feature;
  `tonemapSettings` stays the base for scripts/krom.h; project intensities kept as base). Bloom via new
  `TonemapSettings::bloomIntensity` (< 0 = feature value), read by the tonemap pass per frame.
- Editor: hierarchy Create -> "Post-Process Volume", Add component -> "Post Process", Inspector section
  with override ticks (greyed out when off, undo), duplicate, clipboard; viewport box with dashed inner
  blend edge and face handles. Deviation: the probe has no viewport handles, so the collision box handles
  (`UpdateOBBHandles`) now work on an OBB or a volume box.
- Scenes (.json/.kscene) and prefabs (`postProcessVolume` record, instance sync): `overrides` as a name
  list (missing name = no override), all values stored. Scripts: `IScriptContext::Set/GetPostProcessWeight`
  (inline), krom.h `Set/GetPostProcessWeight`; no ABI bump (no by-value type changed).
- Checked: Debug x64 targets with `-j6` (`out/claude-ppvolume-build.log`), `krom_tests` all passed incl.
  new suite "Post-process volumes" (57 checks) and an editor test (create, override undo/redo, duplicate,
  inspector draws). Room 2 copy with a volume (`out/claude-ppvolume-DemoProject`), `KROM_VALIDATION=1
  KROM_CAPTURE_LIT=1 KROM_CAPTURE_PPVOLUME=1 --gtao-motion`: outside / blend band middle / inside give
  +0.5 / 0.0 / -0.5 EV etc. as expected, no validation messages on DX11 and Vulkan, DX11 vs Vulkan
  0.42-0.47/255 (`out/claude-ppvolume-check/vergleich.png`).
  OpenGL editor and loading test build and start; the blended values are computed there too (grading has
  no effect on OpenGL, frozen).
- Not checked: manual editor use (dragging handles, ticks), play mode with the game camera, exported game,
  a script DLL calling `SetPostProcessWeight`. Known: material/animation previews share the tonemap
  settings and show the volume values at the camera.
- Please review: blend order and the equal-priority rule, the base/effective split in `KromWrapper.cpp`,
  `TonemapSettings::bloomIntensity`, and the shared OBB/volume handle code in `EditorUIViewport.cpp`.

## Colour grading and AgX looks per scene (Claude, 2026-11-19)

- Step 7b of `doc/PLAN_BILDQUALITAET_2.md`, on `main`, not committed yet. UI agreed with the
  user: card "Color grading" in the scene Inspector below "Exposure".
- `addons/forward/ForwardColorGrading.hpp`: `ColorGradingSettings` (AgX look, temperature,
  tint, color filter, hue shift, contrast, saturation, lift/gamma/gain as tint + value),
  `BuildColorGradingConstants` (one 3x3 matrix = saturation * hue * filter * LMS white
  balance, contrast in log space, LGG, AgX look CDL; neutral = no flags). Part of
  `TonemapSettings::grading`, so editor and krom.h runtime hand it over with the exposure.
- `shaders/passthrough.ps.hlsl`: PerPass grows to 160 bytes (static_assert in
  `ForwardFeature.cpp`); contrast + matrix before the operator, look inside `TonemapAgx`
  before the outset, LGG on gamma-2.2 encoded output. OpenGL GLSL unchanged (frozen; the
  generated GLSL experiment was already stale).
- Scene key `colorGrading` (object in `environment`) in editor .json and .kscene, written only
  when not neutral; `ReadSceneColorGradingJson` / `WriteSceneColorGradingJson` in
  `ForwardTonemapJson.hpp`.
- Scripts: `RenderSettings::grading` (`script::ColorGrading`, converters in
  `addons/script/ScriptColorGrading.hpp`); krom.h `SetColorGrading`/`GetColorGrading`.
  `ScriptModuleAbiVersion` 4 -> 5 because `RenderSettings` changed size (earlier additions
  to it did not bump the ABI; stale script DLLs would have read past the struct).
- Checked: Debug x64 targets built with `-j6` (`out/claude-grading-build.log`), `krom_tests`
  all passed incl. "Colour grading constants" and "Colour grading scene keys and script
  view". Room 2 Lit capture neutral vs. graded (golden, +20 temperature, contrast 1.1,
  saturation 1.2, gain +0.1) on DX11 and Vulkan with `KROM_VALIDATION=1`: no errors, no
  VUIDs, DX11 vs. Vulkan 0.45/255 mean difference (`out/claude-grading-check/vergleich.png`).
- Not checked: the Inspector card by hand; a stale script DLL being rejected and rebuilt
  after the ABI bump.
- Please review: order of operations in the shader, the white balance constants, and
  whether the ABI bump fits the script rebuild workflow.

## Normal Blend im Materialgraphen (Codex, 2026-11-19)

- Auf ausdrücklichen Nutzerauftrag direkt von Codex implementiert: zwei Normal-Maps im
  Materialgraphen kombinieren, entsprechend Unreal/Unity. Anleitung: `doc/material_graph_normals.md`.
- Neuer angehängter Knotentyp `NormalBlend` (RNM), Basis-/Detail-Stärke 0..4, de/en.
  Normal akzeptiert auch Mix/Math-Ergebnisse; einzelne Textur-Knoten behalten ihren alten Pfad.
- Graph-Compiler: separate Normal-Samples (Op 17), Normal-Blend (Op 16), Normal-Importrolle,
  getrennte Farb-/Normal-Nutzung derselben Datei, lineare Mix-Masken und UV-Steuertexturen.
  Kontextunabhängige Knoten behalten eine Instruktion und damit eindeutige Script-Aliase.
- Berechnete Normalen sind kodiertes Tangentenraum-RGB. Das Ergebnis (+1) liegt in der zuvor
  freien Komponente `graphUV[7].z`; keine Änderung des Constant-Buffer-Layouts. HLSL-/GLSL-
  Interpreter und PBR/Forward+/Legacy-Lit verwenden es auch ohne direkte Normal-Textur.
  Samples rekonstruieren Z aus XY (RGB/BC5) und gleichen ihre UV-Rahmen vor dem Mischen ab.
- Editor und krom.h-Runtime laden die Graph-Normalen als `NormalMap`; Runtime setzt die
  Gesamt-Normalstärke auch ohne direkte Textur. Das bestehende Limit von 16 Instruktionen und
  vier Textur-/UV-Slots bleibt erhalten.
- Geprüft: Debug-Build `out/build/normal-blend` für `krom_tests`, Editor DX11/Vulkan/OpenGL
  sowie `engine_krom_wrapper_win32`; vollständiger Testlauf auch nach den abschließenden
  Script-Alias-/Runtime-Stärke-Korrekturen erfolgreich. Neue Tests
  prüfen Dateirundlauf, Editor-Verbindungen, CB-Bindung, Farb-/Normal-Wiederverwendung,
  Masken, Script-Aliase und Zyklen. WARP führt die echten HLSL-Funktionen sowie einen
  Fragmentshader mit Texturen aus (flache/abgeschaltete Ebenen, RNM, unabhängige und
  gespiegelte UVs). DX11-/Vulkan-Shader-Varianten und fünf GLSL-Fragmentshader kompilieren.
- Nicht manuell geprüft: Bedienung/Bild im geöffneten Editor und ein exportiertes Spiel.

## Render quality levels (Claude, 2026-11-19)

- Branch `claude/admiring-cannon-fdd311`, fast-forwarded to `main` (909900d) first. Step 7 of
  `doc/PLAN_BILDQUALITAET_2.md`.
- Agreed with the user: players pick only a level. Low = no MSAA, FXAA, GTAO/SSR/contact shadows off; Medium =
  no MSAA, SMAA, GTAO + contact shadows; High = MSAA 2x, SMAA, everything on. Levels set techniques only, never
  sliders (fits step 8: volumes change values only). The level is derived from the five options (one changed
  option -> "Custom"); no project key. The project gives the start value; an exported game remembers a level
  set through script/krom.h per computer (like Unreal's `GameUserSettings.ini`).
- `include/core/RenderQuality.hpp` (new): `RenderQualityLevel`, `RenderPostAA` (ids = `PostAAMode`,
  static_assert in `KromWrapper.cpp`), `RenderQualityPreset`, `DetectRenderQualityLevel`, names.
  `ProjectSettings.hpp`: `Get/SetRenderQualityOptions`, `ApplyRenderQualityLevel`, `GetRenderQualityLevel`,
  `UserGameSettings` + read/write, `UserGameSettingsFolderName`.
- Editor: Project settings > Rendering, new top section "Quality level" (combo, tooltip, hint; de/en).
- Runtime: `user-settings.json` in `%LOCALAPPDATA%\KROM\Games\<window title or EXE name>\` (XDG elsewhere,
  `KROM_USER_SETTINGS_DIR` overrides), read in `LoadProject`, applied over the project options in
  `ApplyProjectRenderSettings`; ignored and not written for `--editor-live-state` runs. Log line after feature
  registration shows the effective level.
- Script API: `IRenderSettingsService::GetQualityLevel/SetQualityLevel` (defaults via Get/Set; the runtime
  overrides Set to persist), new `RenderSettings::postAA` and `contactShadowsEnabled`. krom.h:
  `Set/GetQualityLevel`, `Set/GetPostAA`, `Set/GetContactShadowsEnabled`.
- Checked: Debug x64 (`-j 4`), `krom_tests` all passed (new "Render quality levels", 55 checks). Room 2 with
  `KROM_VALIDATION=1 KROM_CAPTURE_LIT=1 --gtao-motion` no messages on DX11 and Vulkan for all three levels,
  OpenGL starts; DX11 vs Vulkan 0.51-0.52/255. Runtime DX11/Vulkan picks up a stored `low` over a High project;
  `--editor-live-state` and no file keep `high`. GPU total (Debug, 1920x1040): Low 2.53/2.36 ms, Medium
  3.94/3.96 ms, High 5.98/5.52 ms (DX11/Vulkan); 16/25/29 passes.
- Not checked: writing `user-settings.json` from a real script in an exported game (text function tested
  only), visual check of the new combo, Release.
- Please review: the derived level (no stored key) vs. future volumes, the user-settings folder naming (window
  title), and the editor-live-state exception in `SetEditorLiveStateEnabled`.

## Auto-exposure (Claude, 2026-11-19)

- Branch `claude/wonderful-taussig-04bff6` from 224a6f4 (SMAA). Step 6 of `doc/PLAN_BILDQUALITAET_2.md`.
- Agreed with the user: per scene, in the existing Inspector card "Exposure"; mode Fixed/Automatic, the
  EV field becomes the correction in Automatic; missing key = Fixed (old scenes unchanged); script API gets
  mode + correction (`RenderSettings::autoExposure`).
- Settings: `ExposureMode`, `AutoExposureSettings` (min/max EV, EV/s brighter/darker), editor-only
  `freezeAutoExposure` in `TonemapSettings`; `AutoExposureStatus` (read-back EV, reset on scene load) via
  `ForwardFeatureConfig::autoExposureStatus`. Scene keys `exposureMode`, `autoExposureMinEv/MaxEv/ToBright/
  ToDark`, read by `ReadSceneExposureJson` (`ForwardTonemapJson.hpp`) in editor and krom.h runtime.
- GPU (`ForwardAutoExposure.*`): histogram CS (64 log2 bins, groupshared) + adapt CS (10-90 % average,
  clamp, EV/s step, histogram reset), state float4 in a GPU buffer, read by `passthrough.ps.hlsl` as buffer
  SRV t13 (bone palette slot, vertex-only). Display via 4 CPU-read copies (oldest is read).
- New generic recipe hook `FrameRecipePassDesc::preRenderPassExecutorName`: runs in the same graph pass
  before `BeginRenderPass`; `TonemapPass` uses it for `frame.auto_exposure`. No extra passes.
- Backend changes: Vulkan texture SRV range also visible to compute; DX11 `MapBuffer` reads staging
  (`CpuRead`) buffers.
- Checked: Debug x64 (`-j 4`), `krom_tests` all passed (5 new tests). Room 2 with `KROM_VALIDATION=1
  KROM_CAPTURE_LIT=1 --gtao-motion` no messages on DX11 and Vulkan, fixed and auto; 26 passes; measured
  +1.350 / +1.349 EV (incl. +0.5 correction); DX11 vs Vulkan 0.50/255. OpenGL starts (15 passes, fixed).
  GPU (Debug, 1181x709): +0.083 ms DX11, +0.061 ms Vulkan (inside TonemapPass).
- Merged into `main` locally (55d4172, not pushed). Visual check in the editor by the user: works well.
- Please review: barriers/state of the histogram and state buffers in `RecordAutoExposure` (Vulkan:
  fragment read -> compute write next frame relies on the buffer transition), the readback timing (4 copies,
  no host barrier), the t13 slot choice, and the recipe hook in `RenderPipelineRecipe.hpp`.

## SMAA 1x post-process anti-aliasing (Claude, 2026-11-19)

- Branch `claude/smaa-b6b7b7` on top of `feature/fxaa` (2f9b14c). Step 5 of `doc/PLAN_BILDQUALITAET_2.md`,
  SMAA part.
- `PostAAMode::Smaa` (`smaa`), combo entry "SMAA" in Project settings > Rendering > Post-AA.
- Recipe (`BuildParams::postAASmaa`): `TonemapPass` -> `SmaaEdgePass` (`frame.smaa_edges`, `SmaaEdges`
  RG8) -> `SmaaWeightPass` (`frame.smaa_weights`, `SmaaWeights` RGBA8) -> `PostAAPass` (neighborhood
  blending, reads `Tonemapped` + `SmaaWeights`, writes the former tonemap target). Both SMAA targets clear
  to 0.
- Shaders in `addons/forward/ForwardSmaa.cpp`, ported from the official `SMAA.hlsl` (preset high, luma
  edges, no predication/reprojection). Lookup data from the official `AreaTex.h`/`SearchTex.h` as a
  generated array (`SmaaLookupTextures.inl`); the area texture is cropped to subsample 0 (160x80, the only
  part SMAA 1x reads). License in `licenses/SMAA.txt`.
- Checked: Debug x64, `krom_tests` all passed (SMAA recipe checks, shader compile test DX11 + SPIR-V).
  Room 2 with `KROM_VALIDATION=1` no messages: DX11 off/FXAA/SMAA, Vulkan off/SMAA; 26 -> 29 passes.
  OpenGL starts (15 passes). GPU (Debug, 1181x709): SMAA total 0.26 ms DX11 / 0.24 ms Vulkan vs FXAA
  0.15 ms. Comparison: `out/claude-smaa-compare.png` in the worktree.
- Please review: the cropped area texture and its `SMAA_AREATEX_PIXEL_SIZE`, the gamma conversion in the
  edge pass, and resource cleanup in `DestroyPostAAGpuResources`.

## FXAA post-process anti-aliasing (Claude, 2026-11-19)

- Branch `feature/fxaa` from `main`, not committed. Step 5 of `doc/PLAN_BILDQUALITAET_2.md`, FXAA part;
  SMAA follows in its own session.
- `PostAAMode`/`PostAASettings` in `addons/forward/ForwardAntiAliasing.hpp`, shared via
  `ForwardFeatureConfig::postAASettings`; off on OpenGL. A change rebuilds the graph (structure revision).
- Recipe: with post-AA on, `TonemapPass` writes `Tonemapped` (format of the tonemap material pipeline, i.e.
  the swapchain format) and the new `PostAAPass` (`frame.post_aa`) reads it and writes the former tonemap
  target (backbuffer or `EditorSceneView`). Off when an external tonemap executor exists (probe bakes).
- FXAA shader (`kFxaaPsHlsl`): own implementation of the FXAA 3 quality algorithm, sRGB input, luma =
  sqrt(linear luma). Editor overlays drawn into the HDR image are smoothed too; ImGui is not.
- Project key `postAA` (`off`/`fxaa`, missing = off), combo in Project settings > Rendering; editor and
  krom.h runtime apply it; runtime also reads `KROM_POST_AA`. No script API yet (step 7).
- Checked: Debug x64, `krom_tests` all passed (new "Post-AA recipe"). Room 2 with `KROM_VALIDATION=1` no
  messages: DX11 MSAA 1x/2x + FXAA, Vulkan MSAA 1x + FXAA; 24 -> 25 passes; DX11 vs Vulkan mean diff
  0.41/255. OpenGL starts (15 passes, no FXAA). GPU (Debug, 1181x709): PostAAPass 0.14 ms on both.
- Note: the editor loading test executables are not part of the default build; build them by name.
- Please review: the tonemap target switch in `StandardFramePipeline.hpp` (render pass of `Tonemapped`
  does not clear; the fullscreen triangle covers it) and the pipeline format source.

## Screen-space contact shadows (Claude, 2026-11-18)

- Branch `feature/contact-shadows` from `main`, not committed. Steps 2-6 of `doc/PLAN_KONTAKTSCHATTEN.md`.
- New add-on `addons/contact_shadow` (`engine_contact_shadow`): passes `ContactShadowTrace` (reads
  `OpaqueDepth`, MSAA sample 0, writes RG16F visibility + linear depth) and `ContactShadowBlur` (3x3 depth
  aware, R8 `ContactShadowOutput`) in `AfterDepthPrepass`, registered as an opaque input. Frame parameter
  block slots 3+4: world-space direction to the light, light index + 1, length, thickness, intensity, steps.
- Main light = first directional light with `castShadows` and the new `ShadowSettings::contactShadows`
  (default on, scene key `contactShadows`), taken from `LightingFrameData::lights`, i.e. the order of
  `lights[]` and the Forward+ light buffer.
- **Slot t20, not t14 as planned:** t14/t15 are the Forward+ shadow buffers (DX11 shares the t space), t0-t19
  are full. `TexSlots::ContactShadow = 20`, `TexSlots::COUNT` 20 -> 21; Vulkan binding 36 is inside the
  reserved texture range (16-39). An old reflection test used binding 20 as "out of range" -> 21.
- The trace skips pixels facing away from the light (normal from depth derivatives); the first version made
  those surfaces black. Start offset along the normal, IGN jitter, distance-dependent depth tolerance, fades
  at the ray end, the screen border and 40-50 m.
- PBR shaders: `contactShadowTex` (t20) read once per pixel, multiplied onto `shadowVisibility` of the main
  light only (not with `DBG_DISABLE_SHADOWS`; in `pbr_lit` inside `!KROM_SHADOWS_NONE`). Debug bits 22/23,
  editor Shift+F3 (view) / Shift+F4 (off). The forward transparent pass now binds white on every
  opaque-input slot (before: only `PassSRV0`).
- Settings: project keys `contactShadowsEnabled/Length/Thickness/Intensity/Steps`, section "Contact shadows"
  in Project settings > Rendering, Inspector checkbox under "Cast Shadows" (directional lights, undo).
  Editor and krom.h runtime pass the values in `ApplyProjectRenderSettings`; `enableContactShadows` config
  flag. No script API yet.
- Checked: Debug x64 build of all targets, `krom_tests` all passed (new "Contact shadow pass recipe";
  serialization round trips). Room 2: DX11 MSAA 1x/2x and Vulkan MSAA 2x with `KROM_VALIDATION=1` no
  messages; DX11 vs Vulkan view mean diff 0.12/255; 26 vs 24 passes on/off. OpenGL starts (12 passes).
  Image: `out/claude-contact-compare.png`. GPU (Debug, 1181x709): trace 0.21-0.35 ms, blur 0.06-0.23 ms;
  the plan target of < 0.5 ms at 1080p is not met yet.
- Not checked: visual check by the user, MSAA 4x, thin objects in motion. Known: dotted stair pattern on
  steep edges, thin dark line at depth silhouettes (derivative normal).
- Fix 2026-11-19 after the user's visual check (hard black crescents next to a cube): hits fade with the
  distance to the occluder, the assumed thickness grows along the ray, edge fade per hit, fade at grazing
  light. Confirmed by the user in the editor.
- Please review: the t20 / `TexSlots::COUNT` change on all backends (Vulkan layouts; the OpenGL bone palette
  unit moves to 21), the light index source, and the opaque/transparent binding of opaque inputs.

## Geometric specular AA (Claude, 2026-11-18)

- Working tree on `feature/opaque-inputs`, uncommitted, on top of PR #32 (no own branch yet). Step 3 of
  `doc/PLAN_BILDQUALITAET_2.md`. Written by the "specular-aa" session, checked and finished after a crash.
- `ApplySpecularAA` in `pbr_lit.ps.hlsl` / `pbr_forward_plus.ps.hlsl` after Tokuyoshi & Kaplanyan 2021:
  kernel alpha^2 = 2 sigma^2 (|dN/dx|^2 + |dN/dy|^2), sigma^2 = 1/2pi, clamped to 0.18, added to alpha^2;
  roughness never drops. Now also without normal maps (old variant: `KROM_NORMAL_MAP` only, always on).
- Shading option bit 3 (`ShadingSettings::specularAntiAliasing`, default on): project key
  `specularAntiAliasing` (missing = on), checkbox in Project settings > Shading, `RenderSettings` for scripts,
  krom.h via `SetShading`. OpenGL ignores bit 3 and keeps its normal-map-only variant (frozen).
- Test helpers in `--gtao-motion`: `KROM_CAPTURE_LIT=1` captures the lit view, `KROM_CAPTURE_YAW` sets the
  rotation step in degrees.
- Checked: Debug build of all targets, `krom_tests` all passed. Room 2 lit view, DX11 MSAA 1x/2x and Vulkan
  MSAA 2x with `KROM_VALIDATION=1`: no messages. On/off differ only on the normal-mapped floor and the glossy
  masks; a firefly next to a mask eye disappears (`out/claude-specaa-compare-crop.png`). 0.02 deg yaw
  (sub-pixel), masks: pixels jumping > 32/255 764 -> 490 (MSAA 1x), 632 -> 539 (MSAA 2x); floor/boxes same.
  GPU MainOpaquePass: DX11 1.650 -> 1.683 ms, Vulkan 1.588 -> 1.612 ms.
- Not checked: visual check in real motion in the editor, hard edges of flat-shaded geometry (possible
  roughness seam along edges), MSAA 4x, Release.
- Please review: derivative placement before the branch (quad uniformity), clamp(aa, roughness, 1), and
  that the forward/shadow/prepass shader variants do not need the flag.

## Real GTAO in the main pass (Claude, 2026-11-18)

- Branch `feature/opaque-inputs` (on top of the opaque-input and GTAO/SSR switch work), not committed.
  Step 2 of `doc/PLAN_BILDQUALITAET_2.md`.
- `addons/gtao/GtaoPass.cpp`, `kGtaoMainPsHlsl` rewritten after Jimenez et al. 2016: 3 slices x 2 sides x
  4 steps (quadratic distribution, first step >= 1.3 px), normal reconstructed from linear depth (per axis the
  neighbour with the smaller depth step, oriented to the camera) and projected into the slice plane, two
  horizons per slice, analytic cosine-weighted integral, interleaved gradient noise for slice angle and step
  jitter. Bias tilts both horizons away from V (radians). Falloff weight 1 up to one radius, 0 at two radii.
  Thickness heuristic: a sample below the current horizon pulls it back by 25 % x falloff weight (constant,
  no new slider). Screen radius cap 64 px -> 0.25 x view height. Loads via `Load`, no sampler used any more
  (executor still binds PointClamp, harmless).
- Multi-bounce (Jimenez fit, albedo dependent) in `shaders/pbr_lit.ps.hlsl` and
  `shaders/pbr_forward_plus.ps.hlsl`: only the diffuse ambient term; specular occlusion keeps the scalar
  factor. GTAO off/unbound -> unchanged result.
- `tests/test_editor_loading_runtime.cpp`: `--perf` with `KROM_PERF_GPU_PASSES=1` opens the profiler GPU
  section and prints a `PERF gpu` line with the smoothed GPU time per pass.
- Finding: the old shader had no normal and darkened every flat surface seen at an angle (Room 2 F3 mean
  192.9 -> 203.5 of 255); at the pillar foot there was a light rim instead of contact AO. Now open surfaces
  are 1. With the DemoProject radius 0.10 m only a narrow contact shadow is left, so scenes get brighter.
- Checked: Debug x64 build of `krom_tests`, the three editors and the three loading-test targets
  (`out/claude-realgtao-build.log`); `krom_tests` all passed. `--gtao-motion` on
  `out/claude-opaque-DemoProject` (Room 2), DX11 and Vulkan with `KROM_VALIDATION=1`: no errors, no VUIDs;
  motion frames equal the stills except the animated door (same region as before); DX11 vs Vulkan after:
  mean abs diff 0.09/255. OpenGL loading test starts and runs (12 passes, no GTAO). Images:
  `out/claude-realgtao-compare.png`, `out/claude-realgtao-compare-crop.png` (before r=0.10, after r=0.10,
  after r=0.50).
- GPU time (`--perf`, `KROM_PERF_GPU_PASSES=1 KROM_PERF_COMPARE_GTAO=1`, no validation, 1181x709):
  GtaoMain DX11 0.161 -> 0.500 ms, Vulkan 0.179 -> 0.500 ms; GTAO chain DX11 0.575 -> 0.912, Vulkan
  0.610 -> 0.929. Frame time in Debug is CPU bound and noisy on DX11 (22-33 ms between runs).
- Visual check by the user in the editor (2026-11-18): looks good.
- Follow-up (user decision): default radius 0.10 -> 0.50 m (`GtaoSettings`, `ProjectSettings`), slider and load
  clamp 1.0 -> 2.0 m, tooltip (de/en) names a quarter of the view height instead of 64 px. Stored projects keep
  their value (DemoProject 0.1). `test_serialization`: missing key = 0.5, 1.5 m survives the round trip.
  At 0.5 m: GtaoMain DX11 0.533 ms, Vulkan 0.683 ms; `--gtao-motion` DX11/Vulkan with `KROM_VALIDATION=1`
  no messages; full Debug build and `krom_tests` passed.
- Not checked: other scenes, MSAA 4x, dedicated thin-object scene.
  Known leftovers: faint vertical streaks on steeply viewed faces, a barely visible arc at the radius cap
  (<= 1/255 after tonemapping).
- Please review: horizon/normal sign conventions (screen y down vs view y up), the thickness blend (may
  weaken AO behind large thick occluders), the radius cap, and whether 3 slices are worth 0.34 ms.

## Opaque-pass inputs: depth prepass no longer tied to GTAO (Claude, 2026-11-18)

- Branch `feature/opaque-inputs`, not committed yet. Step 1 of `doc/PLAN_KONTAKTSCHATTEN.md`
  and `doc/PLAN_BILDQUALITAET_2.md`.
- `FrameRecipe::opaqueInputs` (`FrameRecipeOpaqueInput{resourceName, textureSlot}`): an
  `AfterDepthPrepass` contributor lists a texture the opaque pass reads. The forward add-on
  runs the depth prepass exactly when such an input exists and the view allows it, adds a
  `ReadTexture` access on `MainOpaquePass` and binds the texture on its slot (white fallback
  when the graph has none). Inputs from other phases are dropped. `PassSRV0` is set to white
  first, because the sky pass leaves its cube map there.
- GTAO lists `GTAOOutput` on `PassSRV0`; `StandardFramePipeline` no longer names GTAO
  (`kGTAOOutputResource` and `StandardFrameBuildResult::gtaoOutput` removed,
  `opaqueInputs` added).
- `RenderView::enableDepthPrepass` now means "view allows a prepass". Editor scene view, game
  view and the krom.h runtime set it unconditionally (before: GTAO enabled); previews,
  thumbnails and probe bakes keep it off. It is passed through `FrameGraphStageContext`,
  `RenderPipelineBuildContext` and `PassFrameView` (also part of the graph cache key).
  `enableAmbientOcclusion` is the plain view flag again; `GtaoPass::BuildPass` checks both.
- Checked: Debug x64 full build (`out/claude-opaqueinputs-build-all.log`), `krom_tests` all
  passed incl. new "Forward pipeline: opaque input enables depth prepass" (input with prepass
  allowed / not allowed / contributor without input). `--gtao-motion` on a DemoProject copy,
  Room 2: DX11 and Vulkan with `KROM_VALIDATION=1` no errors, no VUIDs; motion frames equal
  the stills except the animated door panel (same as 2026-11-15). OpenGL editor starts, runs
  without prepass (12 passes vs. 21).
- Not checked: pixel comparison against `main`; GTAO switched off at runtime on DX11/Vulkan
  (only covered by the unit test).
- Please review: fallback binding order in the opaque executor, and that no other add-on
  relied on `PassFrameView::enableAmbientOcclusion` implying the prepass.
- Follow-up, same branch: real GTAO on/off switch. Project key `gtaoEnabled` (default true,
  missing key = on), checkbox "Enabled" in Project settings > Rendering > GTAO (sliders greyed
  out while off), editor and krom.h runtime hand it to `GtaoSettings::enabled` every frame /
  on project load, `RenderSettings::gtaoEnabled` in `IRenderSettingsService`, krom.h
  `SetGtaoEnabled`/`GetGtaoEnabled` (replaces a stale comment about the removed AO
  reprojection). `KROM_PERF_COMPARE_GTAO` now switches the editor state (the add-on value is
  overwritten every frame) and the PERF line prints the graph pass count.
- Checked: `krom_tests` all passed (round trip + missing key). `--perf` with
  `KROM_PERF_COMPARE_GTAO=1 KROM_VALIDATION=1` on DX11 and Vulkan: switching off at runtime
  drops 24 -> 20 passes (prepass, linearize, main, blur; the composite stays for SSR), no
  errors, no VUIDs. Room 2, Debug: DX11 19.7 -> 18.0 ms, Vulkan 20.9 -> 20.8 ms.
  `--settings-ui` passed. Not checked by hand: clicking the checkbox in the editor.
- Follow-up, same branch: `RenderSettings::ssrEnabled` in `IRenderSettingsService` (editor:
  `EditorState::ssrEnabled`, restored by the project on play stop; krom.h runtime:
  `SsrGpuResources::settings.enabled`) and krom.h `SetSsrEnabled`/`GetSsrEnabled`.
  Switching uses the existing path of the project checkbox (SSR structure revision rebuilds
  the graph). Checked: full Debug build, `krom_tests` all passed. Not checked: a script
  toggling SSR at runtime (no test drives the render settings service yet).

## SSR: no false hits behind occluders (Claude, 2026-11-18)

- `addons/ssr/SsrPass.cpp`, trace shader only. Symptom: black ring stripes on mirror objects
  where they meet another mirror surface (chrome sphere on mirror floor).
- Cause: `prevT` advanced on every step, also on steps already behind a surface, and the
  binary search only tested `diff > 0` (no thickness, no self-hit test). Rays from the sphere
  bottom run hidden behind the sphere, then "hit" the visible floor in front of it; the
  refinement pulled that onto the dark contact area or the sphere itself.
- Now: `frontT` = last sample visibly in front; samples behind a real surface or below the
  start plane (by more than the origin bias) set `occluded`, and a hit while occluded is
  rejected (probe stays). Start-plane artefact samples count as in front. Refinement uses the
  same "behind a real surface" test; the refined point is re-validated (thickness + self-hit).
- Follow-up: the rings became a per-pixel dither (step jitter decides hit/miss pixel by
  pixel in contact areas). Composite now gathers with at least 1.5 px radius (13 taps) even
  for mirrors, so the dither turns into a smooth confidence; mirrors get ~2 px softer.
- Checked: both shaders compile with fxc ps_5_0 and dxc SPIR-V. Not checked: visual result in
  the editor (DX11/Vulkan), loss of contact reflections at grazing angles.

## Editor: frameless window with own title bar (Claude, 2026-11-18)

- Branch `feature/frameless-editor-window`. User request: launcher and main editor window
  without the OS frame; minimize/maximize/close in the editor theme.
- `IWindow`: `WindowDesc::decorated`, `Minimize`, `Restore`, `IsWindowMaximized` (not
  `IsMaximized`: `windowsx.h` macro), `SetDecorated`/`IsDecorated`, `SetCaptionArea` (client
  rect that drags like a title bar). Defaults are no-ops; GLFW keeps its frame (`IsDecorated()`
  stays true, so no custom title bar is drawn there).
- `Win32Window`: undecorated windows keep `WS_OVERLAPPEDWINDOW` (snap, Win+arrows, taskbar
  minimize, animations); `WM_NCCALCSIZE` makes the whole window client area (maximized: clamped
  to the monitor work area), `WM_NCHITTEST` returns resize edges (6 px at 96 DPI, only with
  `WS_THICKFRAME`, not maximized) and `HTCAPTION` inside the caption rect, `WM_NCACTIVATE`
  passes lParam -1. `DwmExtendFrameIntoClientArea` (1 px) keeps the drop shadow; links
  `dwmapi`. Undecorated windows without a target monitor are centered in the primary work area
  (`CW_USEDEFAULT` put them at 0,0, under a top taskbar).
- Editor: launcher has a full-width title bar (panel background, separator, "KROM Editor",
  buttons); the preview starts below it; footer text is now "Version 0.5". In the main window
  the menu bar is the title bar: project - scene centered, buttons on the right, the gap
  between menus and buttons is the drag area (cleared while a popup/menu is open). Save button
  (diskette) is the first toolbar button, same action as File > Save scene/prefab.
- Checked by screenshots (DX11): launcher centered, title bar, maximized editor after loading.
  Not checked by hand: dragging, edge resizing, snapping, button clicks, Vulkan run, multi-monitor
  with different DPI.
- Please review: `WM_NCCALCSIZE`/`WM_NCHITTEST` handling, `SetDecorated`/`SetResizable` size
  handling for undecorated windows, and the ImGui hook order (ImGui handler runs first but does
  not consume `WM_NCHITTEST`).

## Vulkan: pipeline cache file no longer grows without limit (Claude, 2026-11-18)

- Branch `fix/vulkan-pipeline-cache-growth`, follow-up to the entry below.
- Problem: the driver keeps every pipeline it ever saw and returns all of it from
  `vkGetPipelineCacheData`. Pipelines of shader variants that no longer exist (shader edits,
  deleted materials) stayed in `vulkan-pipelines.cache` forever (~18-32 KB each), so the file
  only grew and travelled with exported games.
- The file now starts with an own header (`PipelineCacheFileHeader`: magic `KROMVKPC`, version,
  `maxSessionPipelines`, `bytesPerPipeline`, `blobSize`), followed by the driver data.
  - `maxSessionPipelines`: most pipelines one session created with this file
    (`m_pipelineCacheSessionPipelines`, counted per cache file, not per process).
  - `bytesPerPipeline`: measured when the data was built from empty (size / pipelines).
  - On load the driver data is dropped when it is larger than
    `max(2 MB, 2 x maxSessionPipelines x bytesPerPipeline)`. That session builds its pipelines
    again and saves only what it used, so stale entries go away.
  - Files of the old format (no header) are discarded once.
  - The file is not rewritten when the driver data is unchanged and the session needed no
    more pipelines than recorded.
- Checked with absolute project path (`--perf`, car): old file → "old or unknown file
  format", rewritten (694849 bytes, limit 2 MB); second start loads it (38 pipelines 6.3 ms,
  first run 62.8 ms), no rewrite; file padded to 3.8 MB → "discarded (3840577 bytes, limit
  2097152 bytes for 22 pipelines)", rebuilt at 694849 bytes. `krom_tests` pass;
  `krom_editor_loading_tests` (Vulkan) `--material-ui`, `--play-ui`, `--animation-ui` pass
  with only the known VUIDs; `KROM_VALIDATION=1 --perf`: no messages. Full Debug build OK.
- Not checked: Release, exported game, a real growth over many shader edits (only simulated).
- Please review: the limit formula (factor 2, 2 MB floor) and whether `bytesPerPipeline`
  from one small session can make the limit too tight (then a reset only costs one slower
  start).

## Vulkan: persistent VkPipelineCache (Claude, 2026-11-18)

- Branch `perf/vulkan-pipeline-cache`. Until now every `vkCreateGraphicsPipelines`/
  `vkCreateComputePipelines` call ran without a cache.
- `VulkanDevice` now owns one `VkPipelineCache`, used by both creation calls and destroyed at
  `Shutdown` before `vkDestroyDevice`.
- File: `<shader cache dir>/vulkan-pipelines.cache`, beside the SPIR-V artifacts: project
  `shader-bin/` in the editor and the exported game, `shader_artifacts/` before a project is
  open. Both locations were already git-ignored. The directory comes from the new
  `ShaderCompiler::GetCacheDirectory()`. The editor sets it only after the device exists and
  changes it when a project opens. So the device does not read the file at init:
  `SyncPipelineCacheFile()` runs before each pipeline creation. When the directory changed,
  it saves the old cache and creates a fresh `VkPipelineCache` from the new file. Pipelines
  made from the old cache stay valid.
- Loading checks the `VkPipelineCacheHeaderVersionOne` header: size, version, vendorID,
  deviceID and pipelineCacheUUID. On a mismatch it logs the reason and starts empty. If the
  driver still refuses the data, it retries without it.
- Saving happens at `Shutdown` and on a directory change: `vkGetPipelineCacheData`, write
  `<file>.tmp`, then `rename` over the file. Skipped when nothing was created, or when the data
  equals what was loaded (size + FNV-1a). On a write failure (e.g. a read-only install folder)
  it only logs a warning.
- `--perf` now also prints `firstFrame`/`warmupEnd` (ms since test start). The shutdown log
  prints the pipeline count and the time spent creating pipelines.
- Measured on Vulkan, Debug x64, `--perf out/claude-perf-DemoProject` (car scene, 38
  pipelines), 3 runs each without and with the file. Pipeline creation went from 60-76 ms to
  6-7 ms. `firstFrame` is about 39 s in both cases (39.0-41.0 s), so the start-up is dominated
  by scene/asset loading in Debug and the cache does not show up there. Setting
  `__GL_SHADER_DISK_CACHE=0` changed nothing (66.8 ms → 6.2 ms), so the driver's own cache does
  not hide the effect here.
- Checked: `krom_tests` (all pass); `krom_editor_loading_tests` (Vulkan) `--material-ui`,
  `--play-ui`, `--animation-ui` pass; `_dx11`/`_opengl --material-ui` pass. Editors build for
  DX11/Vulkan/OpenGL. Vulkan validation with `--perf` (no file, then with the file): no
  messages. With `KROM_PERF_SELECT=Camera`, and in the UI tests: only the known `08910`/`08914`/
  `09600`/`pSignalSemaphores-00067`. A truncated file, a changed deviceID and a changed UUID
  are each discarded with the right reason and rewritten. No `.tmp` is left behind.
- Not checked: Release, the exported game, an editor project switch by hand. Also not checked:
  a GPU/driver change in practice (only simulated by editing the header).
- Please review:
  - The device reads `ShaderCompiler::GetCacheDirectory()` (a hidden coupling to the shader
    layer instead of an explicit `IDevice` call at the four `SetCacheDirectory` sites).
  - The export warm-up runs on Vulkan, so the packaged `shader-bin/` now also carries the
    developer's pipeline cache (~0.7 MB here). It is discarded on another GPU or driver.

## Vulkan: no WaitIdle around offscreen views (Claude, 2026-11-18)

- Open point from the descriptor-cache entry below: with an offscreen request (Scene and Game
  panel both visible, camera/material/animation/prefab preview, thumbnails)
  `RenderSystem::RenderFrame` called `WaitIdle()` before and after the offscreen views on
  Vulkan. Both calls removed (branch `perf/vulkan-offscreen-no-waitidle`).
- User check 2026-11-18: all fine in the editor; Debug editor 5-10 FPS faster.
- Why that is safe now: a command list that records again in the same frame slot already
  waits for its previous submission in `VulkanCommandList::Begin`
  (`BeginQueueFrameRecording` → `WaitForFenceValue`); slot buffers are separate allocations
  per view; ring buffers rotate per call; `UploadBufferData` waits for a host-visible
  buffer still in use. In the measurement the `Begin` wait never fired.
- Side effect found when removing them: 27 descriptor updates/frame instead of 0. Two causes,
  both fixed:
  - `GpuResourceRuntime::AcquireTransientTargets` took the first completed pooled target.
    Which one is completed depends on GPU timing (with `WaitIdle` everything always was), so
    texture combinations changed and missed the cache. It now prefers the target the same
    frame slot used last time (`PooledTransientRT::lastFrameSlot`), else the one free the
    longest. Affects all backends (only which pooled target is picked).
  - `VulkanCommandList` retired cached sets after 8 *recordings*; with two recordings per
    frame the sets (period: 3 frames) were gone before they came back. Now 8 *frames*
    (`lastUsedFrame`, `VulkanDevice::GetFrameIndex()`); `m_recordingSerial` removed.
- Measured `--perf`, Debug x64, Vulkan, car scene, `KROM_PERF_SELECT=Camera` (camera preview =
  one offscreen view per frame): 16.9 ms → 13.3 ms, descriptor updates 0. Without preview
  unchanged (7.5 ms, 0 updates).
- Checked: `krom_tests` (all pass); `krom_editor_loading_tests[_dx11|_opengl]`
  `--material-ui`, `--play-ui`, `--animation-ui` pass; Vulkan validation (perf with camera
  preview, `--material-ui`, `--animation-ui`) shows exactly the same messages as before
  (`08910`/`08914` format mismatches, one `09600`, `pSignalSemaphores-00067`): pre-existing,
  not investigated.
- Not checked: Release, the editor by hand with several previews open at once, probe capture
  from the editor (it still has its own `WaitIdle`).
- Please review: the transient-target pick order and the frame-based retirement.

## Project settings: GTAO and SSR greyed out without the add-on (Claude, 2026-11-18)

- User request: Project settings > Rendering offered GTAO and SSR under OpenGL, where both
  add-ons are not registered. Same pattern as MSAA: controls greyed out, hint below.
- `EditorFrameContext::gtaoAvailable`/`ssrAvailable`, set every frame in `KromEditorApp` from
  `m_gtaoResources`/`m_ssrResources` (null when the add-on is not registered).
- The MSAA hint key `msaa.unavailable` is now `settings.opengl_unavailable` (same text, de/en)
  and used for all three sections. Values stay in the project file.
- Same uncommitted files also carry the GTAO sliders from 2026-11-17 (entry below).
- Checked: Debug x64 build of editors and loading tests for DX11/Vulkan/OpenGL;
  `--settings-ui` passes on all three. That test does not open the Rendering tab: the greyed-out
  controls are not checked.

## OpenGL: CSM cascades and tiled material-graph textures (Claude, 2026-11-18)

- User request despite the OpenGL freeze: OpenGL cut shadows off and the Room 2 floor texture
  did not repeat. User check 2026-11-18: looks good.
- Found with GPU readbacks of the editor view (temporary capture in `--perf`, removed again),
  Room 2, OpenGL against Vulkan, normal view and `DBG_VIEW_SHADOW`.
- Shadows: the shadow view-projection outputs OpenGL clip depth -1..1
  (`OpenGLDevice::GetShadowClipSpaceAdjustment`), and `SampleShadowAtlas` maps it with
  `z * 0.5 + 0.5`. The CSM cascade selection in `pbr_lit.opengl.fs.glsl` and
  `lit.opengl.fs.glsl` still tested the D3D range `ndcPos.z >= 0.0`, so receivers in the
  light-side half of a cascade's depth range got no shadow at all. Now `ndcPos.z > -1.0`.
  After the fix the OpenGL shadow view matches Vulkan.
- Floor: OpenGL keeps the wrap mode per texture unit. Textures are created with
  `CLAMP_TO_EDGE`, and `OpenGLCommandList::SetSampler` bound the LinearWrap sampler only to
  units 0-3. Material-graph textures sit on units 13, 14, 16 and 17
  (`OpenGLResources.cpp`), so box-projected or UV-scaled graph textures stretched their edge
  texels. LinearWrap is now bound to those units too.
- Still different from Vulkan/DX11, not investigated: the OpenGL image is darker overall.
- Checked: `krom_tests` (all pass); `krom_editor_loading_tests_opengl` `--play-ui`,
  `--material-ui`, `--animation-ui` pass; `krom_editor_opengl` builds.
- Not checked: the car scene, other filter modes, the exported OpenGL game.

## DX11: shadow filter called once per light in the PBR pixel shaders (Claude, 2026-11-18)

- Branch `perf/fxc-shadow-call-site` (on top of `perf/vulkan-descriptor-cache`).
- Cause of the slow FXC compile (see the DX11 note in the entry below): the light loops of
  `pbr_forward_plus.ps.hlsl` and `pbr_lit.ps.hlsl` called `SampleShadowAtlas` (3x3, Poisson
  and PCSS filters) at three sites: point light, CSM, spot/single cascade. FXC inlines every
  call, so the whole filter was compiled three times. Same idea as 5b89eca for the CSM
  attempts: each branch now only selects the view (`shadowSelected`, `shadowViewIndexSel`,
  `shadowPosLCS`, bias/strength of the matched shadow light), then `KromPreparePcss` and
  `SampleShadowAtlas` run once. Same inputs as before in every branch; no other change.
- `fxc /T ps_5_0 /O1`, defines of the test material (alpha test, instanced, base colour map,
  metal/rough, double sided):

  | shader | before | after |
  |---|---|---|
  | `pbr_forward_plus.ps` | 7.0 s | 1.1 s |
  | `pbr_forward_plus.ps` + `KROM_MATERIAL_GRAPH` | 8.6 s | 1.4 s |
  | `pbr_lit.ps` | 2.9 s | 0.85 s |

  `lit.ps` (0.9 s) and the OpenGL GLSL files are unchanged.
- `krom_editor_loading_tests --material-ui` now holds frame 15 until the scripted material
  instance is no longer `waitingForShaders` (max. 2 min) and prints the wait. First DX11 run
  without a cached variant: 986 ms (before: 8841 ms); Vulkan and OpenGL: 0 ms.
- Checked: `krom_tests` (all pass, includes the PBR compile tests for DX11/Vulkan);
  `--material-ui` on DX11/Vulkan/OpenGL and `--play-ui` on DX11/Vulkan pass.
- User check 2026-11-18: no problems on DX11 and Vulkan. OpenGL cuts shadows off oddly and
  the floor texture is wrong, as before this change (OpenGL is frozen; the GLSL files are
  unchanged).
- Not checked: Release.
- Please review: that the selected bias/strength/view match the old per-branch calls.

## Vulkan: descriptor sets kept across frames (Claude, 2026-11-18)

- Follow-up to "cheaper `FlushDescriptors`" (2026-11-15). Not committed, on `main` next to
  the user's own uncommitted changes.
- Cause: the per-frame descriptor arena was reset in every `Begin()` and its set cache
  cleared, so every distinct binding state was written again each frame. A temporary probe
  (removed) compared each miss with the closest set of the frame before: almost every miss
  differed only in CB slot 0 (`PerFrameCB`) and buffer-SRV slot 9 (`PerObjectStructured`).
  Both come from `GpuResourceRuntime::AllocateUploadBuffer/AllocateFrameBuffer`, which
  created new buffers every frame, so the handles never matched.
- `GpuResourceRuntime`: a frame slot keeps its buffers when it comes round again
  (`FrameSlot::reusableBuffers`) and `AllocateSlotBuffer` hands them out again for a request
  with the same type/usage/stride/initial state (constant buffers: same size; others: up to
  2x the requested size). Reuse only when the slot's fence has completed (it waits for it
  anyway); buffers not requested in a full cycle are destroyed as before. Affects all
  backends.
- `VulkanCommandList`: the per-frame arena pools are gone. Sets live in pools with
  `FREE_DESCRIPTOR_SET_BIT`, cached by materialization state (hash + `unordered_multimap`,
  equality via `DescriptorMaterializationStatesEqual`). A set is never rewritten, so frames in
  flight can share it. Sets unused for 8 recordings are retired and freed once the next
  submission of this command list has completed. States with a dead texture/UAV handle get a
  one-off set that is retired right away, so a cached set only ever matches live handles.
- `krom_editor_loading_tests --perf` prints `descriptorUpdates/frame` and
  `descriptorBinds/frame` now.
- Measured: `--perf`, Debug x64, Vulkan, copy of DemoProject (`out/claude-perf-DemoProject`),
  same build tree, engine files reset to HEAD for the baseline:

  | scene | before | after | descriptor updates/frame before → after |
  |---|---|---|---|
  | Room 2 | 17.25 ms | 15.24 ms | 184 → 0 |
  | car | 8.92 ms | 7.79 ms | 63 → 0 |
  | Room 0 | 16.02 ms | 13.90 ms | 213 → 0 |
  | Room 1 | 7.89 ms | 7.00 ms | 30 → 0 |

  Binds per frame unchanged (184/63/213/30).
- The other two suggestions from the 2026-11-15 measurement:
  - Bind frame/pass data once per pass: not done. Binds are cheap compared to the updates,
    which are now gone; it would need a split of the single descriptor set layout.
  - Offscreen request every frame in the editor: not a bug. When the Scene and Game panels are
    both visible, the editor view is rendered as an offscreen request
    (`editorViewOffscreen` in `KromEditorApp.cpp`). On Vulkan `RenderSystem::RenderFrame` then
    calls `WaitIdle()` before and after the offscreen views ("temporary Vulkan
    stabilization"), i.e. two full GPU syncs per frame. Open: remove those waits.
- Checked: `krom_tests` (all pass); `KROM_VALIDATION=1` perf on Room 2 and car without
  validation messages; `krom_editor_loading_tests[_dx11|_opengl]` `--play-ui`,
  `--material-ui`, `--animation-ui` pass, except `_dx11 --material-ui`: "renderer uploaded the
  scripted material instance" fails, also with `GpuResourceRuntime` reset to HEAD (not
  caused by this change). Cause: the scripted material instance is still
  `waitingForShaders` at the fixed check frame 15 (~260 ms after Play starts); on Vulkan it is
  ready at frame 7, on DX11 only after ~8.8 s (measured by letting the test wait; then every
  check passes). The test passed on DX11 on 2026-11-13 because variants were compiled
  synchronously until c754191 (2026-11-14, background compiles); the fixed check frame was
  never adapted. The slow shader is `pbr_forward_plus.ps` (Vulkan/DXC: 536 ms). Standalone
  `fxc /T ps_5_0` on it: ~7.5 s already on 2026-11-12 (917bde1), 7.0 s now, 8.6 s with
  `KROM_MATERIAL_GRAPH`; /O0, /O1 and /O3 cost the same, only /Od is faster (2.2 s), so the
  time is spent before optimization (probably loop unrolling). `pbr_lit.ps`: 2.9 s.
  Both handled in the entry above (shadow filter called once per light).
  `krom_editor_vulkan/_dx11/_opengl` build.
- Not checked: Release, a visual check in the editor.
- Please review: the reuse rule in `AllocateSlotBuffer` (sizes, fence condition) and the
  retire/free timing of cached sets (`RetiredDescriptorSet::retireAfterFence`).

## Vulkan: no CPU wait for CPU-written per-frame buffers (Claude, 2026-11-18)

- Follow-up to the open point from 2026-11-15 ("~4.5 ms fence wait"). Branch
  `perf/vulkan-frame-ring-buffers`, merged into `main`.
- User check 2026-11-18: Vulkan Debug about 7–10 FPS more; Release now runs into the
  144 Hz monitor limit (VSync).
- Cause: `VulkanDevice::UploadBufferData` waits for `lastSubmissionFenceValue` before it writes a
  host-visible buffer. Features kept one CPU-written buffer each and rewrote it every frame, so
  the first such upload of a frame waited for the whole previous frame. A temporary probe in
  `UploadBufferData` (removed again) showed exactly one wait per frame, always on the first
  buffer written: after one buffer was fixed the wait moved to the next one
  (`Lighting_LightBuffer` → `Shadow_LightBuffer` → `ForwardPlus_TileHeaders` →
  `DebugDrawLinesVB` → `ForwardBloom_PerPassCB`).
- New `include/renderer/FrameRingBuffer.hpp`: three copies of a buffer, `Next(device, desc)`
  once per frame takes the next copy and (re)creates it when too small; `Release`, move-only.
  The backend wait stays, so a ring that is too short is slower, never wrong.
- Rings now: `LightingFrameData` light buffer; shadow light/view buffers (`ShadowFeature`);
  Forward+ CPU tile headers/indices and compute params/reset staging (GpuOnly compute tile
  buffers now have their own fields `computeTile*Buffer`, the old shared fields are the handles
  bound this frame); `DebugDraw` lines VB + frame CB; `ParticleRenderer` VB + frame CB;
  `TextRenderer` VB (taken in `Flush`, `EnsureGpuResources` runs twice per frame there).
- Bloom and tonemap per-pass CBs: the values are config/settings and almost never change, so
  `UploadedConstants<T>` in `ForwardFeature.cpp` only writes them when the bytes differ (no ring).
- Not changed (never waited in the four scenes): skinning palettes, outline per-object CB,
  shadow `perFrameCB` fallback (only without the arena), material CBs (only on change).
- Measured: `krom_editor_loading_tests --perf`, Debug x64, Vulkan, 1920x1080, copy of
  DemoProject with `lastScene` set per run, same build tree, files reset for the baseline:

  | scene | before | after |
  |---|---|---|
  | car | 11.38 ms | 8.62 ms |
  | Room 0 | 16.78 ms | 15.76 ms |
  | Room 1 | 9.49 ms | 7.62 ms |
  | Room 2 | 17.71 ms | 17.02 ms |

  With the probe the "after" runs had no fence wait in any of the four scenes.
- Checked: full Debug x64 build; `krom_tests` (all pass); `krom_editor_loading_tests` and
  `_dx11 --play-ui` (pass); `KROM_VALIDATION=1` perf run on Room 2 without validation messages;
  `editor_scene_runtime_opengl`, `krom_editor_opengl`, `DemoProject_opengl` start and run 15 s.
  Note: the full build does not relink `krom_editor_loading_tests*`; build them by target.
- Not checked: Release, a visual check of debug lines/particles/text in the editor.
- Please review: that every consumer reads the per-frame handle (Forward+ fields, `gpu.lightBuffer`
  of lighting/shadow), and the Forward+ split between CPU and compute buffers.

## GTAO radius/intensity/bias in the project settings (Claude, 2026-11-17)

- User request: the "Ambient Occlusion (GTAO)" heading in Project settings > Rendering was
  empty since c47b667; it now holds sliders for radius (0.01–1 m, log), intensity (0–1)
  and bias (0–0.5), de/en tooltips. Committed 2026-11-18 with the greying-out entry above.
- `ProjectRenderSettings`: `gtaoRadius`/`gtaoIntensity`/`gtaoBias` (defaults = `GtaoSettings`),
  read clamped and written in the `rendering` block. Legacy scene settings keep the defaults.
- Editor: `ApplyProjectRenderSettings` fills `EditorState::gtaoSettings` (was unused); the
  frame loop copies only radius/intensity/bias into `m_gtaoResources->settings` (`enabled`
  also drives the depth prepass and stays untouched). Runtime: `KromWrapper`
  `ApplyProjectRenderSettings` sets the same three values.
- Not in the script API (`IRenderSettingsService`). Not under OpenGL (no GTAO there).
- Checked: Debug x64 build of editors (dx11/vulkan/opengl), `editor_scene_runtime_dx11`,
  `krom_tests` (all pass), `krom_editor_loading_tests_dx11 --settings-ui` (pass). Visual check
  of the sliders' effect open.

## Asset folders split: `shaders/`, `editor/`, `assets/` (Claude, 2026-11-17)

- User request: engine-essential files in their own folder, editor files in their own
  folder, `assets/` only for the code-only examples. Not committed.
- Removed as unused (no reference in code, CMake or tests): `assets/debug/` (15 debug
  shaders) and `assets/skinned.vs.hlsl`.
- `shaders/` (new): every engine shader and shader include, plus `generated/`. Header
  comments say `shaders/...` now; no shader code changed.
- `editor/`: `fonts/lucide.ttf` + `fonts/lucide-LICENSE.txt`, `editor_scene.json` (only
  used when the editor runs without a project).
- `assets/`: `examples/` (custom shader templates, read by tests) and example data
  (`weisse_textur.png`; the user copies `krom.bmp`, `Dragon/` etc. back in later).
- Engine shader root moved from `assets` to `shaders` in: `ShaderCompilerShared.cpp`
  (`GetEngineAssetDirectory` default: `<exe>/shaders`, then `<cwd>/shaders`),
  `KromWrapper.cpp` (`ResolveEngineAssetRootOnDisk`), `KromEditorApp.cpp`
  (`ResolveEngineShaderRoot`/`ResolveEditorDir`; `ResolveAssetRoot` stays `assets/` for the
  no-project asset root).
- CMake `krom_copy_assets`: copies `shaders/` to `bin/shaders` and `assets/` to
  `bin/assets`; `krom_tests` now depends on it. Shader-cross experiment paths follow.
- Export: copies `<runtime exe>/shaders` to `<export>/shaders`; removes an old `assets/`
  engine folder from earlier exports. Project assets stay in `Content/`.
- Existing build folders keep a stale `bin/assets` with the old shaders; nothing reads
  shaders from there any more.
- `examples/reflection_probe.cpp`: removed the leftover `Krom::SetAmbientOcclusionReprojection`
  call (API removed in c47b667), which broke the `reflection_probe_*` builds.
- Checked: Debug x64 `krom_tests` (all pass); user check 2026-11-17: editor, examples and
  export run with the new layout.
- Open: `licenses/` still lists Lucide although exported games no longer contain the font.

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
  Not checked: OpenGL, Release, exported game.
  User check 2026-11-16: the bar looks and behaves as intended in the editor.
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

### Follow-up to Claude's external-project review (Codex, 2026-11-20)

The source review and question are in `F:/working/krom codex/KOORDINATION.md`,
under "Review of Codex's implementation". Follow-up changes remain in this Codex
worktree; the main working tree's code has not been modified.

1. Agree; handled in the worktree. A missing selected `krom-project.json` now warns
   and clears both cache names before collecting projects. A renamed/deleted external
   project no longer poisons IDE reconfiguration. Valid target-name collisions still
   fail. CMake regressions cover automatic regeneration after rename, deletion, clearing
   the legacy mirror, and not resurrecting it on a later configure.
2. Agree on removing the active-project coupling. Ordinary wrapper examples now bind
   no project scripts. The generic `editor_scene_runtime_*` sample explicitly binds
   the shipped in-tree DemoProject to preserve its existing behavior. Own projects use
   their explicit `krom_runtime_*` targets. Building examples with a deliberately broken
   active external script passed. An ALL build can still fail on an explicitly generated
   project's own runtime targets when its scripts are broken; those targets remain in ALL.
3. Measured, no production change needed for this finding: DemoProject, MSVC Debug,
   GTX 1660 machine, repeated warm runs. `FindStaleSharedHeaders` (parsing + canonicalization
   + timestamps) median 25.87 ms, max 26.33 ms, n=25. Full `CheckSharedEngineHeaders`
   including two Ninja processes median 596.76 ms, max 599.51 ms, n=5; a prior run was
   577.15 ms median. Canonicalization is only done for the intersection of script/editor
   dependencies (35 paths in this dump, out of 52 script dependencies), not every script
   header. Keep the correct canonical-path handling; most time is outside that filter.
   Probe sources/output: `out/header_benchmark.cpp`, `out/header-benchmark.log`.
4. Agree; handled in the worktree. Initial legacy-name migration warns; a newly supplied
   conflicting legacy value warns that the new name takes precedence. An internal record
   of the last mirrored value suppresses misleading warnings on normal editor switches.
   Alias precedence and clearing regressions passed.

Validation: `tests/test_project_roots.py` passed; real MSVC/Ninja example builds passed
with a broken active project; moving that project then building `krom_editor_dx11`
automatically warned, cleared both cache names, and succeeded. `krom_tests` rebuilt.
Details: `out/review-fixes-validation.log`. No SDK/PR/branch work done in this follow-up.

### External project builds and script ownership (Codex, 2026-11-20)

- Implemented directly by Codex on the user's explicit request. This supersedes the
  source-only review previously recorded here. Scope: point 2 of the editor distribution
  plan; a relocatable prebuilt editor SDK (point 3) is still separate work.
- `cmake/KromProjects.cmake` collects all `projects/*` plus `KROM_PROJECT_ROOT` once,
  deduplicates real paths, and rejects different roots with the same sanitized runtime
  identifier (also case-insensitive output collisions on Windows). The error names both
  roots and suggests changing `runtimeTarget`. Runtime backends and script DLLs share
  the resulting list; both silent target skips were removed. Manifest edits trigger
  automatic reconfiguration. `KROM_PROJECT_SCRIPT_ROOT` migrates to the new name, which
  takes precedence even when explicitly empty; the legacy cache entry mirrors it.
- Every editor build request carries the active absolute project root. Initial configure
  includes it in the same invocation; subsequent builds explicitly configure only when
  cache/last-generated selection differs. This includes in-tree projects, shared runtime
  profiles, and recovery after a collision changed the cache but not generated targets.
- Script DLLs export their originating project root. `ScriptModuleHost::Swap` checks file
  identity before registering scripts or replacing the loaded module. Wrong-project and
  legacy DLLs without ownership are rejected, including during project open, with de/en
  diagnostics. Rebuilding scripts also handles a moved project directory. Engine ABI
  checks remain in place. The shared-header check explicitly excludes the active project
  root, including project directories elsewhere inside the source tree.
- Removed the project-dialog location warning and both translations; retained the
  `projects/` default and corrected its obsolete comment. Updated `doc/ScriptHotReload.md`
  with external projects, cache switching, migration, collision recovery and current SDK
  limitations. Reconfiguration preserves engine objects; a new profile needs its first build.
- Added CMake discovery/migration/collision tests (`tests/test_project_roots.py`), cache
  selection/failure recovery checks in `krom_build_tools_tests`, and real DLL ownership/
  shared-header regressions in `krom_tests`. Optional editor modes:
  `--external-script-reload <project.json>` and `--external-project-export <project.json>`;
  both use a temporary external Unicode/space path and restore CMake selection before cleanup.
- Real Unicode runtime testing exposed a further prerequisite: Ninja's UTF-8 link
  response files have no BOM, so MSVC reads object paths using the system code page.
  Reproduced the same response file failing without a BOM and linking with one.
  `cmake/LinkUtf8.cmake` now marks Ninja/Makefiles response files for project DLL/runtime
  targets before invoking the linker, retaining existing linker launchers. The existing
  PRE_LINK PDB response generation is unchanged. Build failures now retain their final
  context even after warnings, so multiline collision messages and linker details reach
  the editor console.
- Validation complete: CMake project tests (including automatic regeneration on manifest
  edits), all `krom_build_tools_tests`, and all 476 regular `krom_tests` passed. Debug
  DX11/Vulkan/OpenGL editors, DX11/OpenGL loading tests, and the OpenGL runtime built.
  Physics/audio were disabled for this isolated Debug/Release validation build.
- External Unicode/space-path checks passed: Script/Play with multiple reloads, compile
  error and cancellation recovery, unchanged Play with no build, wrong same-target DLL
  rejection on project open, and reopening the correct module (`out/project-external-script-play.log`).
  Runtime-first testbuild, hidden game startup/rendering, full Release export with game
  shader warmup, and return to DemoProject plus its script build passed
  (`out/project-external-flow.log`). Original CMake selections were restored before cleanup.
  Two links with old PDBs locked passed (`out/project-pdb-lock.log`).
- OpenGL editor startup and scene switching passed (`out/project-opengl-start.log`).
  Standalone OpenGL runtime warmup timed out here; the existing reference EXE in
  `F:/working/krom codex/out/build/x64-Debug/bin` also timed out with the same minimal
  project. No OpenGL-only change made and no standalone runtime startup success claimed.
  DemoProject emits missing-texture diagnostics in this worktree (ignored local assets,
  including `puresky.hdr` and `dx.bmp`); visual fidelity of the demo was not validated.
  A complete relocatable SDK and clean-machine distribution test remain point 3.

### Korrektur: Hierarchie-Ordner, Elternzustand und Gizmo-Farben (2026-11-19)

Auf ausdrücklichen Nutzerauftrag durch Codex implementiert; Ausgangspunkt
`8d76bfd`. Die vorher hier dokumentierten drei Befunde sind behoben.

- **Ordner-Fokus:** `FrameHierarchyEntity` ignoriert FolderComponent. Ein
  Doppelklick auf einen leeren oder gefüllten Ordner bewegt die Kamera nicht;
  normale Eltern-Entities können weiterhin über ihre Mesh-Bounds fokussiert werden.
- **Aktivierung:** gemeinsamer Helfer `ecs/HierarchyState.hpp` berücksichtigt
  die gesamte Elternkette. Meshes, Lichter, Schatten, Kameras, Probes (über
  ECSExtractor), Audio, Scripts, Animationen und Partikel berücksichtigen nun
  deaktivierte Vorfahren. Die lokalen Kindwerte bleiben erhalten. Fehlerhafte
  zyklische Elternketten können den gemeinsamen Helfer nicht endlos blockieren.
- **Sichtbarkeit:** Mesh-Extraktion berücksichtigt eigene und geerbte
  MeshComponent.visible-Werte sowie den neuen VisibilityComponent für Gruppen
  ohne Mesh und Ordner, auch im parallelen Extraktionspfad. Der Inspector bietet
  diesen Gruppen einen Sichtbarkeitsschalter; Hierarchietext und Icons werden
  bei geerbter Deaktivierung/Ausblendung gedimmt. Partikel und Editor-Icons
  berücksichtigen die Gruppen-Sichtbarkeit ebenfalls. SetVisible in krom.h
  unterstützt damit auch reine Gruppen. Sichtbarkeit deaktiviert keine Scripts
  und keine Physik. Szenen-/Prefab-Speicherung, Duplizieren und Undo/Redo
  unterstützen den neuen Zustand; fehlende Daten bedeuten sichtbar.
- **Gizmo-Farben:** neuer Contributor-Zeitpunkt AfterPostProcess, nach Tonemap
  und FXAA/SMAA, vor ImGui. Gizmos zeichnen direkt ins sRGB-Ausgabeziel und
  gelangen nicht mehr in Bloom oder die Auto-Exposure-Messung. Tiefengeprüfte
  Gizmos verwenden weiterhin die aufgelöste Szenentiefe; der immer sichtbare
  Pass bleibt getrennt. Materialpipelines und ihr Cache berücksichtigen das
  Ausgabeformat je Ansicht statt des bisherigen HDR-Materialformats.
- **Regressionen:** Ordner-Fokus mit/ohne Mesh-Nachkommen; mehrstufige
  Aktivierung/Sichtbarkeit mit erhaltenen lokalen Kindwerten; Reparenting;
  serieller/paralleler Mesh-Pfad; Script-Pause bei deaktiviertem Vorfahren;
  Gruppen-Sichtbarkeit mit Undo/Redo und Szenen-Rundweg; produktiver
  Editor-Contributor nach Post-AA und vor UI.
- **Validierung:** eigener MSVC-Debug-Build ohne optionale Physik-/Audiomodule:
  krom_tests, Editor und editor_scene_runtime für DX11, Vulkan, OpenGL
  erfolgreich gebaut.
  Alle 463 regulären Tests bestanden. Neue GPU-Regression
  `krom_editor_loading_tests[_dx11] --gizmo-colors <project.json> <capture-dir>`:
  jeweils 14 Aufnahmen auf DX11 und Vulkan, vier Tonemap-Operatoren mit
  -8/0/+8 EV, MSAA aus/2x/4x, Post-AA aus/FXAA/SMAA, zusätzlich helle/dunkle
  Auto-Exposure-Szenen. Alle drei Achsen bleiben unterscheidbar und ihre
  Farbmittelwerte konstant (< 1/255 Toleranz); keine Validierungsfehler.
  Kontrollbild bei +8 EV zeigt klar getrennte rote/grüne/blaue Pfeile vor
  weißem Hintergrund. OpenGL-Editor und -Testprogramm ebenfalls gebaut und
  gestartet; 14 Aufnahmen mit stabilen Achsenfarben im eingefrorenen
  Funktionsumfang, keine Testfehler (`out/gizmo-opengl*`). Gruppen- und
  Mesh-Sichtbarkeit sind auch bei Mehrfachauswahl gemeinsam undo-fähig;
  Sichtbarkeit von Licht- und Schattenextraktion berücksichtigt Gruppen.
  Logs und Bilder unter `out/gizmo-{dx11,vulkan}-aa*`,
  Testlauf `out/hierarchy-tests-complete.log`, Build `out/hierarchy-build.log`.
- **Grenzen:** kein Release-Build und keine echte Physik-/Audio-Simulation
  in dieser Konfiguration; kein neuer Nachweis für verdeckte Kamera-Gizmos
  oder alle Manipulatorformen in der Animationsvorschau.

### Korrektur: Materialvorschaukugel verschwindet nach Prefab-Auswahl (2026-11-19)

- **[P2, behoben]** Ein Klick auf ein Prefab im Asset-Browser leerte
  `selectedMaterialAssetPath`, ließ aber den geöffneten Materialeditor samt
  geladenem Graphen bestehen. `SyncMaterialPreviewEntity` blendete daraufhin
  Kugel und Boden aus; nur die Skybox blieb sichtbar. Dies erklärt auch die
  leere Dateizeile im Screenshot. Play/Stop allein war nicht der Auslöser.
- `DrawAssetBrowser` erhält jetzt den Pfad des geöffneten Materials bei einer
  Prefab-Auswahl. Prefab-Auswahl und Inspector funktionieren weiterhin.
- Neuer GPU-Regressionstest `krom_editor_loading_tests[_dx11]
  --material-preview <capture-dir>` erstellt ein eigenes Projekt mit texturiertem
  Material und Prefab, klickt die echte Browser-Kachel und prüft Materialpfad,
  Sichtbarkeit und gerenderte Bildpixel vor/nach dem Klick sowie nach Play/Stop.
  Vor Korrektur sechs fehlgeschlagene Prüfungen, nach Korrektur DX11 und Vulkan
  bestanden. Aufnahmen zeigen die texturierte Kugel inklusive Boden/Schatten.
  Logs/Bilder: `out/material-preview-{before,fixed}-dx11*` und
  `out/material-preview-fixed-vulkan*`. OpenGL-Editor baut weiterhin.
- Die bereits separat dokumentierten Vulkan-Validierungsmeldungen 09600,
  08910 und 08914 treten weiterhin auf; diese Änderung betrifft die
  Browser-Auswahl und keine Vulkan-Ressourcen oder Pipelines.

### Korrektur: eingedrückte Normalmap bei Box-Projektion (2026-11-19)

- **[P2, behoben] Abgeleitete Tangentenbasis kehrte beide Normalmap-Achsen um.**
  Nutzer nennt `Block_copy` nach Umschalten auf Box-Projektion. Die aktuelle
  Main-Datei verwendet `uvProjection box`, die Cobblestone-`_nor_dx_`-Textur
  im Normal-Eingang des Graphen und Normal Strength 1,512.
- Box-Projektion erzwingt die Ableitungsbasis (`pbr_lit.ps.hlsl:633`,
  `pbr_forward_plus.ps.hlsl:618`). Deren `T_raw`/`B_raw`-Berechnung
  (`pbr_lit.ps.hlsl:253` ff., `pbr_forward_plus.ps.hlsl:268` ff.) enthält
  das Vorzeichen der Bildschirmorientierung
  `dot(cross(ddx(positionWS), ddy(positionWS)), baseNormalWS)`, korrigiert es
  aber nicht. Im kontrollierten frontseitigen DX11-Fall sind dadurch beide
  Tangentenachsen gegenüber der UV-Richtung invertiert. Die spätere
  Handedness-Korrektur korrigiert diese gemeinsame Umkehr nicht.
- **GPU-Nachweis:** Die unveränderten echten `SampleNormal`-Funktionen beider
  HLSL-PBR-Shader plus `pbr_box_projection.hlsl` wurden in einem isolierten
  DX11-Pixelshader ausgeführt. Lineare konstante Normalmap, Stärke 1, dieselbe
  UV-Ausrichtung; Vergleich Mesh-Tangenten gegen Box-Ableitungsbasis auf allen
  sechs Achsenseiten. Beispiel +Z: Mesh `(0,512316; 0,256158; 0,819705)`,
  Box `(-0,512316; -0,256158; 0,819705)`. Maximale Komponentendifferenz
  1,02463 auf jeder Seite, in beiden Shadern. Damit unabhängig von Import,
  Farbtextur und der erhöhten Stärke des Nutzermaterials reproduziert.
- **Korrektur:** Beide Rohachsen werden vor der Normalisierung mit dem
  Vorzeichen des obigen Skalarprodukts multipliziert, jetzt in den produktiven
  Shadern `pbr_lit.ps.hlsl` und `pbr_forward_plus.ps.hlsl`. Mesh-Tangenten und
  Normalmap-Import bleiben unverändert. OpenGL-Shader gemäß Freeze unverändert.
- **GPU-Regression der produktiven Shader:** 72 Vergleiche (beide Shader,
  sechs Achsenseiten, jeweils normale UVs, U/V/beide gespiegelt, nichtuniforme
  Skalierung und umgekehrte Bildschirmorientierung). Alle bestanden; maximale
  Mesh/Box-Abweichung 0, unter nichtuniformer Skalierung 5,96e-8. Gegenprobe
  vor Korrektur 1,02463. Logs: `out/box-normal-*-fixed.log`.
- **Weitere Validierung:** Alle 460 regulären Tests bestanden, einschließlich
  Kompilierung der betroffenen Normalmap-/Graph-/SSR-Varianten für DX11 und
  Vulkan/SPIR-V. Debug-Konfiguration ohne optionale Physik-/Audiomodule.
  Log: `out/box-normal-fix-tests.log`. Kein Austausch der Nutzermaterialien
  oder Normalmap-Texturen erforderlich.
- Reproduktion: `out/box_normal_probe.cpp`, `out/*.probe.hlsl`;
  Ergebnisse `out/box-normal-{current,candidate}.log` und
  `out/box-normal-forward-plus-{current,candidate}.log`.

### Korrektur: Materialeditor beeinflusste Auto-Exposure der Szene (2026-11-19)

- Nutzer meldet „Car“ dauerhaft zu dunkel und bestätigt die Kopplung an den
  geöffneten Materialeditor. Szene enthält Auto-Exposure mit Standardgrenzen
  -4/+4 EV. Fehler am echten Renderer mit einer gleichmäßigen HDR-Testfläche
  und geöffnetem/geschlossenem Materialeditor reproduziert: dunkle Ansicht
  geschlossen +3,4943 EV, geöffnet nur ca. +0,1954 EV. Der Test schlägt vor
  der Korrektur für beide Helligkeiten mit geöffneter Vorschau fehl.
- **Ursache:** `ForwardRenderPipeline` hatte einen gemeinsamen Histogramm-/
  Zustandsbuffer und eine gemeinsame Adaptationszeit für sämtliche Ansichten.
  Die vor dem Hauptbild gerenderten Vorschauen beeinflussten damit dessen
  Belichtung und den angezeigten EV-Wert.
- **Korrektur:** Eigenständige Auto-Exposure-Ressourcen und Adaptationszustände
  je Ausgabeziel; das Hauptbild hat eine stabile Identität über Swapchain-Bilder
  und Größenänderungen. Zustände zerstörter Renderziele werden freigegeben,
  beim Device-Shutdown alle Zustände. Nur das Hauptbild publiziert den
  angezeigten Belichtungswert; der Scene-Reset erreicht weiterhin alle Ansichten.
- **Regression:** Optionaler GPU-Test
  `krom_editor_loading_tests[_dx11] --auto-exposure <project.json> <capture-dir>`:
  helle/dunkle HDR-Fläche ohne/mit Materialeditor, erneutes Schließen. Prüft
  gemessene EV-Werte und tatsächliche GPU-Bildhelligkeit. Nach Korrektur in
  beiden Fällen -3,4898 / +3,4943 EV. DX11-Bildmitte der dunklen Fläche:
  vorher offen 39,475 statt geschlossen 134,642 von 255, nachher beides 134,642.
  Logs und GPU-Bilder unter `out/exposure-{before,after}-dx11*` bzw.
  `out/exposure-after-vulkan*`. Reguläre Tests: 460/460 grün, Debug-Build ohne
  optionale Physik-/Audiomodule; OpenGL-Editor ebenfalls gebaut. Die bereits
  dokumentierten Vulkan-Formatmeldungen der Vorschau bleiben ein separater Befund.

### Nachprüfung und Korrektur: Wellenmuster in der Materialvorschau (2026-11-19)

- Die Rückmeldung „absolut keine Verbesserung“ nach `d22a1e0` wurde am echten
  Material-Vorschaurenderer reproduziert. Die Mip-Anzahl-Korrektur behebt einen
  separaten Fehler, aber nicht die Ursache des sichtbaren Wellenmusters.
- **Ursache nachgewiesen:** Die diffuse Irradiance wurde mit 64 punktuellen
  Abtastungen aus Mip 0 berechnet. Feine HDR-Strukturen werden dadurch in die
  diffuse Beleuchtung gefaltet. Mit unverändertem `Weiss` aus dem Main-Projekt
  (Metallic 0, Roughness 1), dessen HDR-Umgebung und isolierter Vorschaukugel
  bleibt das Muster mit `DBG_DISABLE_IBL_SPEC` bestehen und erscheint in
  `DBG_VIEW_IBL_DIFF`; `DBG_VIEW_IBL_SPEC` ist fast schwarz. Auch ohne lokale
  Probes reproduzierbar. Für den Vergleich feste Belichtung verwendet.
- **Korrektur:** `IBLBaker.cpp` verwendet auch für die diffuse Integration die
  vorhandene Environment-Mip-Kette. Die Filterstufe folgt der Raumwinkelfläche
  jeder Cosinus-Abtastung (`pdf = cos(theta) / pi`, analog zum vorhandenen
  Specular-Prefilter). Die Lichtenergie-Normierung bleibt erhalten. Die
  Bake-Version ist 7 statt 6, damit alte `.iblcache`-Daten automatisch neu
  berechnet werden. Materialparameter und F0 unverändert.
- **Regression:** Ein feines Schwarz/Weiß-Umgebungsraster muss gleichmäßiges
  diffuses Licht ergeben. Neuer Test in `test_probes.cpp`: alter Baker
  maximaler Radiance-Fehler 0,067300 (Test rot), korrigiert 0,000154 (grün),
  Grenze 0,01. Gesamter Debug-Testlauf: 460 Tests in 19 Suiten bestanden,
  optionale Physik-/Audiomodule aus. Log: `out/ibl-diffuse-after-tests.log`.
- **Bildvergleich:** Neuer optionaler GPU-Test
  `krom_editor_loading_tests[_dx11] --material-ibl <project.json> <capture-dir>`
  liest die echte Materialvorschau für Gesamtbild, Diffuse, Specular, Normalen,
  Roughness und deaktivierte IBL/Probes aus. DX11 und Vulkan zeigen nach der
  Korrektur eine glatte Kugel ohne das Wellenmuster. Mittlere absolute
  DX11/Vulkan-Abweichung des Gesamtbilds: 0,000594 von 255. Artefakte:
  `out/ibl-preview-before-after.png`, `out/ibl-preview-before-{dx11,vulkan}/`,
  `out/ibl-preview-after-{dx11,vulkan}/`.
- **Separater bestehender Vulkan-Befund:** Die Materialvorschau meldet
  `VUID-vkCmdDraw-None-09600`,
  `VUID-vkCmdDraw-dynamicRenderingUnusedAttachments-08910` und `08914`
  (Layout/Color-/Depth-Format). Gegenprobe mit den ursprünglichen Bake-Quellen
  von `d22a1e0` und demselben GPU-Test reproduziert dieselben drei IDs.
  Damit kein neuer Validierungsfehler dieser CPU-Bake-Korrektur; hier nicht
  als behoben markiert. Logs: `out/ibl-preview-{before,after}-vulkan.log`.

### Prüfung und Korrektur: Skybox-Reflexion bei Metallic = 0 (2026-11-19)

- **[P2, behoben] Globale IBL-Roughness wählte eine zu scharfe Filterstufe.**
  `src/renderer/FrameConstantStage.cpp:161` schrieb vor der Korrektur bereits
  `kIBLPrefilterMipCount - 1` in `iblPrefilterLevels`. Die Verbraucher ziehen
  nochmals 1 ab: `shaders/pbr_lit.ps.hlsl:542`,
  `shaders/pbr_forward_plus.ps.hlsl:524` und
  `shaders/pbr_lit.opengl.fs.glsl:493`. Bei sechs Mips und verschwindendem
  Ableitungs-Bias erreicht Roughness = 1 daher nur Mip 4 statt Mip 5.
  Der Baker ordnet Mips über `mip / (prefilterMipCount - 1)` zu
  (`src/renderer/IBLBaker.cpp:695`); Mip 4 ist für Roughness = 0,8 gebacken.
  Das lässt besonders raue Materialien die globale Skybox zu deutlich spiegeln.
  Der Fehler betrifft die Filterbreite; eine pauschal zu hohe Lichtenergie ist
  damit nicht nachgewiesen. Lokale Probes erhalten die tatsächliche Mip-Anzahl
  separat und haben diese doppelte Subtraktion nicht.
- **Konkrekte Korrektur:** In `FrameConstantStage.cpp` die tatsächliche Anzahl
  `static_cast<float>(kIBLPrefilterMipCount)` übergeben; die Shader rechnen
  bereits selbst in den höchsten Mip-Index um. Dazu den Frame-Konstantenpfad
  gegen die tatsächliche Mip-Anzahl absichern und DX11/Vulkan visuell prüfen.
  Kein neuer Intensitätsregler und keine Änderung des dielektrischen F0 nötig.
- **Validierung:** Statische Prüfung aller drei Shader-Verbraucher und des
  Bakers; aus den aktuellen Quellen abgeleitete Rechnung reproduziert für
  Roughness 0 / 0,25 / 0,5 / 0,75 / 1 die gebackenen Filterrauheiten
  0 / 0,2 / 0,4 / 0,6 / 0,8 (Ableitungs-Bias = 0). Noch kein Build oder
  Bildvergleich; die betroffene Szene ist noch unbekannt.
  Nutzer nennt Material `Weiss` und bestätigt die Beobachtung bei Roughness = 1.
  Die hier gespeicherte `projects/DemoProject/Assets/Materials/Weiss.mat`
  enthält noch Metallic = 0,19 und Roughness = 0,695, ohne Graph-Verbindungen
  an diesen beiden Eingängen. Das belegt nicht die aktuellen Live-Werte im
  Editor; die gespeicherte Datei reproduziert den beschriebenen 0/1-Test nicht.
  Nachgereichte Screenshots bestätigen im Materialeditor Metallic = 0,
  Roughness = 1, Normal Strength = 0 und ausschließlich einen Base-Color-Knoten;
  auf der Vorschaukugel sind deutliche unregelmäßige Strukturen sichtbar.
  `app/KromEditorApp.cpp:5015` ff. schaltet SSR und AO für diese Vorschau aus.
  Der geprüfte Bindungspfad in `ShaderRuntimeMaterials.cpp` ordnet Irradiance
  und Prefilter den vorgesehenen Texturen zu; kein zweiter Bindungsfehler
  nachgewiesen. Das Bild allein trennt diffuse IBL-Artefakte, Probes und
  spekulare Reflexion nicht. Der bestätigte Mip-Fehler ist daher noch kein
  Nachweis der vollständigen Ursache dieses Bildes; nach seiner Korrektur
  sind Diffuse/Specular/Probes getrennt zu vergleichen. Insbesondere tastet
  `IBLBaker.cpp:465` ff. für diffuse Irradiance standardmäßig nur 64 Richtungen
  aus der ungefilterten Umgebung ab (mögliche Unterabtastung, noch unbestätigt).
- **Umsetzung auf ausdrücklichen Nutzerauftrag „korrgiere den Fehler“:**
  `FrameConstantStage.cpp` übergibt jetzt die Mip-Anzahl ohne Subtraktion.
  Der Vertrag ist an `FrameConstants::iblPrefilterLevels` dokumentiert.
  `TestShadowFeatureExtractionAndFrameConstants` prüft die gepackte Anzahl
  gegen `IBLBakeParams{}.prefilterMipCount` und deckt damit die ursprüngliche
  Abweichung 5 statt 6 ab. F0, Materialdateien und Shaderformeln unverändert.
  Ein visueller Nachweis für sämtliche Strukturen im Screenshot liegt noch
  nicht vor.
- **Validierung der Korrektur:** Eigenständiger MSVC-Debug-x64-Build unter
  `out/ibl-build` mit `-j 4`, optionale Physik-/Audiomodule ausgeschaltet.
  `krom_tests` und die Editoren `editor_scene_runtime_dx11`,
  `editor_scene_runtime_vulkan`, `editor_scene_runtime_opengl` erfolgreich
  gebaut. Alle 459 Tests in 19 Suiten bestanden, darunter Renderer 103/103
  einschließlich der neuen Mip-Anzahl-Prüfung und DX11/SPIR-V-Shaderprüfungen.
  Log: `out/ibl-tests-shortpath.log`. Der erste Lauf scheiterte in vier
  Renderer-Tests am temporären DXC-Pfad mit Leerzeichen; mit dem kurzen
  Windows-Pfad desselben Temp-Verzeichnisses komplett grün. `git diff --check`
  ebenfalls ohne Befund. Kein Editor-Bildvergleich durchgeführt.

### Review: Qualitätsstufen (2026-11-19)

Geprüft gegen `976021c`: der neueste Übergabepunkt „Render quality levels“,
einschließlich Projektoptionen, Script-Service, krom.h und Benutzerdatei.
Die älteren Render-Übergaben sind damit nicht als geprüft markiert.

- **[P2] Reservierte Windows-Namen verhindern das Speichern.**
  `include/core/ProjectSettings.hpp:148` (`UserGameSettingsFolderName`) ersetzt
  verbotene Zeichen, lässt aber Gerätenamen wie `CON`, `NUL` oder `LPT1`
  unverändert. `src/app/KromWrapper.cpp:2278` verwendet jeden nichtleeren
  Rückgabewert direkt als Verzeichnis; der EXE-Fallback greift hier nicht.
  Ein Spiel mit einem solchen gültigen Fenstertitel kann daher seine gewählte
  Qualitätsstufe nicht dauerhaft speichern. Unter Windows mit den aktuellen
  Headern in einem temporären C++-Programm reproduziert: Speichern unter
  `CON`, `NUL`, `LPT1` jeweils false, unter `Normal Game` true; Anlegen und
  Überschreiben einer normalen Einstellungsdatei funktionieren. Reservierte
  Namen einschließlich Varianten mit Erweiterung behandeln und einen gültigen
  Ersatznamen verwenden; den echten Dateischreibweg mitprüfen.
  **Behoben und geprüft durch Codex auf Nutzerauftrag, siehe Umsetzung unten.**
- **Ordneridentität bleibt eine Einschränkung:** Nur der bereinigte Fenstertitel
  unterscheidet Spiele. Gleiche Titel und z. B. `Game:One` / `Game?One` teilen
  dieselbe Datei; eine Titeländerung verliert die Zuordnung zur bisherigen
  Einstellung. Für eine dauerhafte Zuordnung wäre eine stabile Spiel-ID neben
  dem lesbaren Namen geeigneter. Das ist eine Designentscheidung, kein Befund
  gegen die ausdrücklich beschriebene Verwendung des Fenstertitels.
- Die abgeleitete Stufe passt zum geplanten Volume-Modell: Verglichen werden
  ausschließlich MSAA, Post-AA und die drei Effekt-Schalter. Radius,
  Intensität, Belichtung und andere Werte beeinflussen die Stufe nicht.
  `Custom` ändert nichts. Im aktuellen Runtime-Startablauf folgt
  `SetEditorLiveStateEnabled` auf `LoadProject` und stellt bei gespeicherten
  Projekt-Renderoptionen die Projektwerte vor der Feature-Registrierung wieder
  her; `SetQualityLevel` schreibt in diesem Modus keine Benutzerdatei.
- **Tests:** Vorhandene Debug-EXE aus dem Implementierungs-Worktree am selben
  Commit ausgeführt, kein eigener Neubau. „Render quality levels“: 55/55;
  Serialization insgesamt 13/13, Renderer 103/103. Der Gesamtlauf ist jedoch
  nicht grün: zwei Script-Module-Tests scheitern beim Anlegen ihrer Session
  außerhalb dieses beschreibbaren Worktrees mit „Zugriff verweigert“.
  Zusätzlich scheitert „Asset move, rename and delete are undoable with UTF-8
  names and conflicts“ mit 11 Prüfungen: Dateien werden bewegt, Referenzen
  bleiben alt. Im ersten Review noch nicht eingegrenzt; die weitere Diagnose
  und der Lösungsvorschlag stehen unten. Nicht dem Qualitätsstufen-Commit
  zugeordnet. Protokoll:
  `out/quality-review/krom-tests.log` (lokales, nicht versioniertes Artefakt).
- Nicht erneut ausgeführt: vollständiger Build, Release, Editor-Sichtprüfung,
  OpenGL-Start und Script-Schreibtest im exportierten Spiel. Außer dieser
  Koordinationsdatei wurde kein versionierter Inhalt geändert.

#### Konkrete Lösungen (Codex, 2026-11-19)

1. **Speicherordner:** In `UserGameSettingsFolderName` nach der bestehenden
   Zeichenbereinigung den Namen vor der ersten Erweiterung ohne Beachtung der
   Groß-/Kleinschreibung auf reservierte Windows-Gerätenamen prüfen (`CON`,
   `PRN`, `AUX`, `NUL`, `COM1`–`COM9`, `LPT1`–`LPT9`). Solchen Namen ein `_`
   voranstellen; auch `con.txt` muss einen gültigen Ersatz bekommen. Das erhält
   für normale Titel den bisherigen Speicherort und benötigt keine neue
   Projekteinstellung. Den resultierenden Pfad tatsächlich beschreiben und
   erneut lesen, zusätzlich zu den bisherigen reinen JSON-Tests. Abschließend
   im Runtime-/Script-Weg `low → high → Neustart` und den Schreibschutz durch
   `--editor-live-state` mit `KROM_USER_SETTINGS_DIR` im Testordner prüfen.
2. **Asset-Referenzen:** `ApplyRelocation` bestimmt `RelativeBelow(root, source)`
   erst nach dem physischen Umbenennen (`EditorFileHistory.cpp:367`); auch
   `MoveEditorAssetPath` berechnet die Komponentenzuordnung danach erneut
   (Zeile 612). `fs::relative` verwendet `weakly_canonical`. Im MSVC-Probeprogramm
   unter derselben Windows-Sandbox liefert der bereits entfernte Quellpfad
   dabei Fehler 5 („Zugriff verweigert“), während Ziel und Assetwurzel auflösbar
   sind. Die leere Zuordnung überspringt dann Datei- und Live-Referenzen.
   **Vorschlag:** Die Asset-relative Quell-/Zielzuordnung vor dem Verschieben
   einmal aus normalisierten absoluten Pfaden mit `lexically_relative`
   bestimmen und auf Zugehörigkeit zur Assetwurzel prüfen. Sie an
   `ApplyRelocation`, die Live-Komponenten und den History-Eintrag weitergeben;
   Undo verwendet dieselbe Zuordnung in Gegenrichtung. Eine erforderliche,
   aber nicht bestimmbare Zuordnung muss vor der Dateiänderung zum Fehler
   führen. So hängt das Umschreiben nicht von der Existenz des alten Namens
   ab. Die vorhandenen UTF-8-/Konflikttests unter dieser Sandbox erneut
   ausführen und einen fehlgeschlagenen Vorbereitungsschritt prüfen: keine
   Datei bewegt, keine Referenz verändert, kein History-Eintrag.
3. **Script-Module-Tests:** `ScriptModuleHost::Swap` legt die Schattenkopie neben
   dem Build-Verzeichnis der geladenen DLL an (Zeile 391). Die vorige Test-EXE
   und ihre Fixture-DLLs lagen im anderen, hier nicht beschreibbaren Worktree.
   Für die Validierung EXE und Fixture-DLLs gemeinsam unter einem lokalen,
   beschreibbaren `out/.../bin` bereitstellen; dann landen auch die Session-
   Verzeichnisse lokal. Dafür ist kein Produktcode-Fix und keine Erweiterung
   der Zugriffsrechte nötig. **Verifiziert:** Mit dieser lokalen Testkopie
   bestehen alle sechs Script-Module-Tests. Im Gesamtlauf verbleibt nur der
   oben eingegrenzte Asset-Referenztest (11 fehlgeschlagene Prüfungen);
   Qualitätsstufen weiterhin 55/55. Protokoll:
   `out/quality-review/krom-tests-local.log`.
4. **Optional, getrennt vom Fehlerfix:** Für eine vom Fenstertitel unabhängige
   Speicherung eine dauerhafte Spiel-ID im Projekt vorsehen, bei Exporten
   beibehalten und den Speicherordner daran binden. Kein Hash des Titels:
   Der ändert sich beim Umbenennen ebenfalls. Bestehende Titelordner können
   bei der ersten Verwendung übernommen werden. Die Qualitätsstufe selbst
   bleibt weiterhin aus ihren fünf Optionen abgeleitet.

Dies sind Umsetzungsvorschläge für Claude; Implementierungscode wurde nicht
verändert. Die Diagnoseprogramme liegen nur in temporären Testordnern.

#### Umsetzung durch Codex auf Nutzerauftrag (2026-11-19)

Der Nutzer hat Codex anschließend ausdrücklich mit der Umsetzung der beiden
Fehlerkorrekturen beauftragt; dafür gilt die allgemeine Rollenverteilung am
Dateianfang nicht.

- `UserGameSettingsFolderName` schützt reservierte Windows-Gerätenamen durch
  ein `_`-Präfix, auch bei Erweiterungen und anderer Groß-/Kleinschreibung.
  COM/LPT mit hochgestellten Ziffern sowie CONIN$/CONOUT$ sind berücksichtigt.
  Gewöhnliche Titel behalten ihren bisherigen Ordnernamen.
- Asset-Verschiebungen berechnen die normalisierte, Asset-relative Zuordnung
  vor der Dateiänderung und speichern sie samt Gegenrichtung in der Historie.
  Datei-Referenzen, Live-Auswahl und Komponenten verwenden dieselbe Zuordnung.
  Ein fehlender alter Dateiname wird nicht mehr kanonisiert. Pfade außerhalb
  der Assetwurzel werden vor Datei- und History-Änderungen zurückgewiesen,
  mit einer deutschen/englischen Fehlermeldung.
- Regressionstests: reservierte und gewöhnliche Namen, tatsächliches Anlegen,
  Überschreiben und erneutes Lesen von Low/High-Dateien, Unicode-Pfade sowie
  abgewiesene Quell-/Zielpfade ohne Nebenwirkungen. Der vorhandene Asset-Test
  prüft weiter Referenzen in Szene/Prefab und Live-Komponenten, Undo/Redo,
  Model-Fragmente, UTF-8-Namen und Konflikte; sein Ordnerpfad enthält nun auch
  ein zu normalisierendes `/.`.
- Eigener MSVC-Debug-Build unter `out/quality-fixes-build`: `krom_tests`,
  `krom_editor_vulkan`, `krom_editor_opengl`, `editor_scene_runtime_opengl`
  erfolgreich. Bereits vorhandene Warnungen in anderen Dateien bleiben.
- Vollständiger Lauf der neu gebauten `krom_tests`: **alle Tests bestanden**.
  Qualitätsstufen 76/76, Benutzerdateien 88/88, Asset-Verschieben/Undo/Redo
  51/51, Script-Module 6/6. Der zuvor gemeldete Asset-Referenzfehler ist damit
  unter derselben Sandbox behoben. Protokoll: `out/quality-review/tests-fixes.log`;
  Build-Protokolle: `build-fixes.log` und `build-fixes-final.log` im selben Ordner.
- Separates Probeprogramm gegen den neu gebauten Runtime-Wrapper: alle fünf
  Prüfungen bestanden. Echte `Krom::SetQualityLevel`-Aufrufe schreiben Low und
  ersetzen es durch High unter `_CON`; ein neuer Prozess lädt High über den
  Low-Projektwert. `SetEditorLiveStateEnabled(true)` stellt den Projektwert
  wieder her und verhindert das Überschreiben der Spielerdatei. Kein
  Grafikstart und kein Gameplay-Script in diesem Probeprogramm.
- OpenGL-Startprüfung: Editor blieb 10 Sekunden geöffnet, Runtime mit minimalem
  Testprojekt und `--warmup-shaders` 25 Sekunden. Beide danach vom Test beendet;
  der Warm-up-Abschluss und interaktives Rendering sind damit nicht bestätigt.
- Die optionale stabile Spiel-ID ist nicht Teil dieser Fehlerkorrekturen.
  Die titelbasierte Zuordnung behält die oben beschriebene Einschränkung.

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
  - Sichtprüfung (Nutzer, 2026-11-17): nach dem Merge der transaktionalen
    Undo-Historie (`5748d3c`) keine spürbare Verzögerung mehr bei Strg+Z/Strg+Y.
    Komponenten-Änderungen laufen über `ComponentOperation`; nur
    `SnapshotOperation` baut die Szene noch komplett neu auf.
  - Zurückgestellt: Prefab-Cache und Material-Caching pro Durchlauf. Erst
    angehen, wenn Undo nach strukturellen Änderungen spürbar hängt.

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
