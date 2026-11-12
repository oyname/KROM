# Plan: Bildqualität

Gemeinsamer Stand der Auftragsreihe „Bildqualität“ (neun Aufträge, je eine Sitzung).
Jede Sitzung liest diese Datei zuerst und trägt am Ende ein, was erledigt ist, was
offen bleibt und was sich gegenüber dem Plan geändert hat.

## Arbeitszweig

`feature/bildqualitaet`, abgezweigt am 2026-11-12 von `origin/main` (`287a721`,
Merge von PR #23).

Beim Abzweigen lagen im Arbeitsordner fremde, nicht committete Änderungen
(Animationseditor/Gizmo-Arbeit: `addons/editor/*`, `addons/animation/AnimationController.cpp`,
`app/KromEditorApp.cpp`, `include/renderer/RenderLayers.hpp`, eine Zeile in
`addons/forward/ForwardFeature.cpp`, `tests/test_animation.cpp`,
`tests/test_editor_commands.cpp`, dazu DemoProject-Szenen und -Materialien). Sie gehören
nicht zu dieser Reihe, bleiben unangetastet und werden hier nicht committet. Wer
`ForwardFeature.cpp` oder `KromEditorApp.cpp` ändert, committet nur die eigenen Hunks
(`git add -p`), solange diese Änderungen offen sind.

## Aufträge

| Nr. | Auftrag | Status |
|-----|---------|--------|
| 1 | Bestandsaufnahme und Plan-Datei | erledigt (2026-11-12) |
| 2 | GPU-Zeitmessung pro Pass | erledigt (2026-11-12), Messwerte Testraum offen |
| 3 | Tonemapping und Exposure | erledigt (2026-11-12), Kern-Umbau offen |
| 4 | Anti-Aliasing mit MSAA | in Arbeit: Plan vorgelegt (2026-11-12), wartet auf Freigabe |
| 5 | Kern-Vorarbeit: Parameterblöcke für AddOns | offen |
| 6 | Lokale Probes: Konzept | offen |
| 7 | Lokale Probes: Umsetzung und Specular Occlusion | offen |
| 8 | Feinschliff: GTAO, Schattenfilter, BRDF | offen |
| 9 | Materialgraph erzeugt Shader-Code: Konzept | offen |
| 12a | Job-System: Fehler im Pool beheben | erledigt (2026-11-12) |
| 12b | Job-System: Kopplung in `RenderWorld` auflösen | erledigt (2026-11-12) |
| 12c | Job-System: Pool der Engine zuordnen, Shader-Caches umstellen | offen |
| 13 | Eine Shader-Quelle statt zwei: Versuch und Konzept | erledigt (2026-11-12), Konzept wartet auf Freigabe |

## Geprüfte Befunde (Stand `287a721`, 2026-11-12)

Bewertung: **stimmt**, **stimmt nicht** oder **liegt anders**.

### 1. Tonemapping: Reinhard pro Kanal ohne Exposure — stimmt, mit Ergänzung

- `assets/passthrough.ps.hlsl` und `assets/passthrough.opengl.fs.glsl`: HDR + Bloom
  (`bloomParams.y`), danach `v / (v + 1)` pro Kanal. Kein Exposure-Faktor, keine
  Gamma-Stufe im Shader (die sRGB-Umwandlung macht das Backbuffer-Format).
- Das Material entsteht in `src/app/KromWrapper.cpp` (`InitializeTonemapMaterial`,
  Zeile 2673).
- Ergänzung: Der Editor baut dasselbe Material ein zweites Mal selbst
  (`app/KromEditorApp.cpp`, ab Zeile 7740, gleiche Shader-Dateien). Auftrag 3 muss beide
  Stellen bedienen oder sie zusammenlegen.

### 2. Kein Anti-Aliasing — stimmt

- `sampleCount` ist überall 1: `ForwardFeature.cpp` (485, 604), `GtaoPass.cpp`,
  `OutlinePass.cpp`, `ShaderRuntimeMaterials.cpp:374`, `GpuResourceRuntime.cpp:631`.
- Die Typen tragen das Feld schon (`TextureDesc`, `RenderTargetDesc`, `PipelineDesc`,
  Pipeline-Key in `RendererTypes.hpp`), und DX11 reicht es an `SampleDesc.Count` durch
  (`DX11Resources.cpp`). Vulkan setzt dagegen fest `VK_SAMPLE_COUNT_1_BIT`
  (`VulkanDevice.cpp:1384`, `:1759`), OpenGL hat keinen Multisample-Pfad. Einen
  Resolve-Schritt gibt es in keinem Backend.
- FXAA, TAA, SMAA: kein Code vorhanden (nur ein Kommentar in `RenderGraph.hpp`).

### 3. IBL ist eine einzige globale Umgebung — stimmt, ein Detail liegt anders

- `EnvironmentSystem` hält mehrere Einträge, aber genau einen aktiven
  (`SetActiveEnvironment`, `m_active`); gebunden werden ein Irradiance-, ein
  Prefiltered-Cubemap und die gemeinsame BRDF-LUT.
- Lokale Probes und Lightmaps: kein Code (Suche nach „probe“/„lightmap“ trifft nur
  die Physik).
- Zweiter UV-Satz, liegt anders: `VertexSemantic::TexCoord1` existiert im Kern und in
  den Backends (DX11-Semantik, OpenGL-Attribut `aTexCoord1`). Es gibt aber keine Daten
  dafür: `SubMeshData` hat nur `uvs`, und `GpuResourceRuntime.cpp` (223, 353) füllt
  `TexCoord1` mit denselben `uvs` wie `TexCoord0`. Für Lightmaps fehlt also der
  Datenkanal im Mesh-Asset und im Import, nicht die Semantik.

### 4. Occlusion wirkt als ein Faktor auf diffus + spekular — stimmt

- `assets/pbr_forward_plus.ps.hlsl:607`: `ambientIBL = (iblDiffuse + iblSpecular) * ao`,
  danach Zeile 618 `ambientIBL *= gtaoFactor`. Material-AO und GTAO dämpfen beide Anteile
  gleich. Eine Specular Occlusion gibt es nicht.
- Dasselbe Muster steht in `pbr_lit.ps.hlsl` (617, 628) und `pbr_lit.opengl.fs.glsl` (578,
  dort ohne GTAO).

### 5. GTAO des Vorframes ohne Reprojektion — stimmt

- `pbr_forward_plus.ps.hlsl:615`: `aoUV = positionCS.xy / screenSize`, ein Sample aus
  `gtaoAO` (t8, History des Vorframes). Keine Bewegungsvektoren, keine Reprojektion,
  keine zeitliche Akkumulation in `GtaoPass.cpp` (die „History“ ist nur der Puffer des
  letzten Frames).
- Zusatz: GTAO ist auf OpenGL abgeschaltet (`GtaoFeature.cpp:77`, Warnung „not supported
  on OpenGL“); `pbr_lit.opengl.fs.glsl` enthält keinen GTAO-Code.

### 6. Schatten-Filter PCF 2x2 oder 3x3 auf festem Gitter — stimmt

- `SampleShadowAtlas` (`pbr_forward_plus.ps.hlsl:402`): `shadowFilterMode` 0 = ein Tap,
  1 = 2x2 mit ±0,5 Texel, ≥2 = 3x3 mit ganzen Texeln. Jeder Tap ist ein manueller
  bilinearer Vergleich (`CompareShadowManualBilinear`), dazu ein Receiver-Plane-Bias aus
  Bildschirmableitungen. Keine Rotation, kein Rauschen, kein PCSS.
- Dieselbe Funktion liegt in fünf Dateien: `pbr_forward_plus.ps.hlsl`, `pbr_lit.ps.hlsl`,
  `lit.ps.hlsl`, `pbr_lit.opengl.fs.glsl`, `lit.opengl.fs.glsl`.

### 7. Forward+ mit 16x16-Tiles; Culling per Compute? — liegt anders

- Tile-Größe 16 stimmt (`kForwardPlusTileSize`, `FORWARD_PLUS_TILE_SIZE`).
- Das Culling läuft im Normalfall auf **der CPU**, nicht per Compute Shader.
  `ForwardFeatureConfig::cullingMode` steht auf `CPU`, und weder `KromWrapper.cpp` noch
  der Editor noch die Beispiele setzen etwas anderes. Der Compute-Pfad ist vorhanden
  (Shader als Quelltext in `ForwardPlusLighting.cpp`, Dispatch in
  `ForwardFeature.cpp:963 ff.`, Rückfall auf CPU, wenn das Backend kein Compute meldet),
  wird aber von niemandem eingeschaltet; der Kommentar im Header nennt ihn „vorbereitet“.
- **OpenGL hat kein Forward+.** `ForwardFeature.cpp:1415` schaltet dort auf klassisches
  Forward zurück, der Shader ist `pbr_lit.opengl.fs.glsl` mit einer Schleife über alle
  Lichter des Frames (`kMaxLightsPerFrame = 7`). Eine Datei
  `pbr_forward_plus.opengl.fs.glsl` gibt es nicht.
- Folge für die Regel „auf allen drei Backends gleich“: DX11 und Vulkan rendern mit
  `pbr_forward_plus.ps.hlsl` (bzw. `pbr_lit.ps.hlsl` ohne Forward+), OpenGL immer mit
  `pbr_lit.opengl.fs.glsl`. Jede Beleuchtungsänderung betrifft also drei Shader-Dateien,
  und OpenGL hat weder Tile-Listen noch GTAO.

### 8. Der Kern kennt einzelne Features beim Namen — stimmt

- GTAO: `include/renderer/RenderFrameConstants.hpp` hat feste Felder `gtaoRadius`,
  `gtaoBias`, `gtaoIntensity`, `gtaoEnabled` (Offset 2320–2332, `static_assert` auf 2336
  Bytes) und die Debug-Bits `DBG_VIEW_GTAO`/`DBG_DISABLE_GTAO`. Befüllt werden sie über
  `IFrameConstantsContributor`, die Felder selbst sind aber Kern. Dazu ein AO-Slot in
  `RenderRuntimeResourceBindings.hpp`.
- Tonemap: `defaultTonemapMaterial` und `tonemapMaterialSystem` in `FrameGraphStage.hpp`,
  `RenderFrameOrchestrator.hpp`, `RenderPipelineInterfaces.hpp`,
  `RenderRuntimeMaterialBindings.hpp`; `RenderSystem::SetDefaultTonemapMaterial`.
- Bloom: `enableBloom` in `FrameGraphStage.hpp`, `RenderPipelineInterfaces.hpp`,
  `RenderFrameTypes.hpp` (View); Slot `BloomTexture = 12` in `ShaderBindingModel.hpp`.
- Auch die Schattenfelder (`shadowFilterMode`, Kaskaden, Atlas-Rechtecke) stehen fest in
  `FrameConstants`.
- Folge: Exposure (Auftrag 3) und Probe-Parameter (Auftrag 7) haben heute keinen Weg in
  den Shader, ohne `FrameConstants` um benannte Felder zu erweitern. Das ist der Grund für
  Auftrag 5; Auftrag 3 braucht entweder den vorhandenen PerPass-Block des
  Tonemap-Materials (`bloomParams.w` ist frei) oder muss nach
  Auftrag 5 kommen.

### 9. Materialgraph wird im Shader interpretiert — stimmt

- `kMaterialGraphMaxNodes = 16`, `kMaterialGraphMaxTextures = 4`
  (`include/assets/MaterialGraph.hpp`). Der Compiler erzeugt eine Instruktionsliste
  (`CompiledMaterialGraph`), keinen Shader-Code.
- `assets/pbr_material_graph.hlsl` (`EvaluateMaterialGraph`) läuft per `[loop]` über
  `graphOutputs.z` Instruktionen mit einer `if/else`-Kette über den Opcode und einem
  Wertefeld `gKromGraphValues[16]`. GLSL-Gegenstück: `pbr_material_graph.opengl.glsl`.

## Ausgangszustand (2026-11-12)

- Gebaut: Debug x64 (`out/build/x64-Debug`, Ninja, MSVC), Ziele `krom_editor_vulkan`,
  `krom_editor_dx11`, `krom_editor_opengl`, `krom_tests`. Alle vier gelinkt, 0 Fehler,
  22 Warnungen (C4834 u. ä., schon vorher vorhanden). Log:
  `out/claude-bildqualitaet-baseline-build.log`.
- `krom_tests`: 19 Suiten, 419 Tests bestanden, 0 fehlgeschlagen, Exit 0. Log:
  `out/claude-bildqualitaet-baseline-tests.log`.
- Einschränkung: Gebaut und getestet wurde der Arbeitsordner, also `287a721` **plus** die
  oben genannten fremden, nicht committeten Änderungen. Der reine Commit-Stand wurde nicht
  getrennt gebaut.
- Nicht geprüft: Start der Editoren, Laufzeit auf den drei Backends, Bild. Release-Build,
  GLFW-Konfiguration (`x64-Debug-glfw`), Beispiele und die übrigen Testprogramme
  (`krom_editor_loading_tests*`, `krom_scene_lifetime_tests`, `krom_surface_tests`,
  `krom_build_tools_tests`) wurden nicht gebaut oder ausgeführt.

## Auftrag 12a: Job-System, Fehler im Pool (2026-11-12)

Commits: `85b00b4` (Befund 1), `991c602` (Befund 2), `cdf0e72` (Befund 3, Feld entfernt).
Geändert: `src/jobs/JobSystem.cpp`, `include/jobs/JobSystem.hpp`, `include/jobs/TaskGraph.hpp`,
`tests/test_jobs.cpp`. Die öffentliche Schnittstelle ist für Aufrufer unverändert.

- **Befund 1 (ParallelFor greift auf zerstörte Stack-Objekte zu): stimmt, behoben.**
  Zähler, Mutex, Condition Variable und Ergebnisse liegen jetzt in einem Block, den die
  Batches per `shared_ptr` mit besitzen; der Zähler wird unter dem Mutex geführt. `fn`
  bleibt beim Aufrufer, die Batches rufen es nur vor dem Herunterzählen auf. Die
  Batch-Aufteilung ist unverändert (wichtig für 12b).
  - `TaskGraph::Execute`: dasselbe Muster, stimmt, behoben. Bei einem Fehler kehrte es
    zurück, während andere Aufgaben der Ebene noch liefen und auf den Graphen zugriffen
    (`Scene::Update` legt den `FrameScheduler` auf den Stack). Jetzt wird die ganze Ebene
    abgewartet und der erste Fehler gemeldet.
  - `RenderFrameOrchestrator.cpp`: kein Fehler dieser Art. Die drei Jobs greifen zwar per
    Referenz auf Stack-Objekte zu, aber alle drei `future.get()` laufen vor jedem
    Rücksprung. Nicht geändert.
  - Übrige Aufrufer (`BoundsSystem`, `MeshRendererExtraction`, `IBLBaker`,
    `ShaderRuntimeAssets`, `RenderWorld`, `EditorMaterialLibrary`): nur über `ParallelFor`
    (damit behoben) oder mit Wert-Captures.
- **Befund 2 (wartender Thread führt Hintergrund-Jobs aus): stimmt, behoben.** Wer in
  `ParallelFor` wartet, hilft nur noch bei Frame-Jobs. `WaitIdle` nimmt weiter aus beiden
  Warteschlangen.
- **Befund 3:**
  - Warnung bei `ParallelFor` aus einem Worker: kommt im normalen Betrieb nicht vor. Alle
    Aufrufe von `ParallelFor` liegen im Haupt-Thread außerhalb von Jobs; nur ein Test löst
    die Warnung aus. Nicht geändert.
  - `Dispatch` liest `m_initialized`/`m_workerCount` ohne Sperre: im Betrieb nicht
    gleichzeitig mit `Shutdown`. Der Pool gehört `RenderSystem`, `Initialize`/`Shutdown`
    laufen im Haupt-Thread, und nur Haupt-Thread und eigene Worker erreichen den Pool.
    `Shutdown` schreibt die Felder erst nach dem `join` der Worker. Nicht geändert; wird
    in 12c wichtig, falls der Pool dann von weiteren Threads erreichbar ist.
  - `Job::dependencyCounter`: unbenutzt, entfernt.
- **Prüfung:**
  - Neuer Belastungstest (20000 kurze `ParallelFor` mit 4 Batches). Er hat den Fehler vor
    der Behebung gezeigt: Im eigenständigen Testprogramm (Debug, ohne Sanitizer) Absturz
    mit Zugriffsverletzung in 3 von 3 Läufen, dazu Meldungen „resource deadlock would
    occur“ aus dem Sperren des zerstörten Mutex. Nach der Behebung 30 Läufe ohne Fehler.
  - AddressSanitizer (MSVC, `/fsanitize=address`, `detect_stack_use_after_return=1`): Der
    Lauf vor der Behebung meldete nichts (der Wettlauf trat dort nicht auf). Nach der
    Behebung 5 Läufe ohne Meldung. ThreadSanitizer gibt es unter MSVC nicht; kein Lauf.
  - Test für Befund 2 und Test für `TaskGraph` schlugen vor der jeweiligen Behebung fehl
    und bestehen danach.
  - Debug x64: `krom_editor_vulkan`, `krom_editor_dx11`, `krom_editor_opengl`, `krom_tests`
    gebaut, 0 Fehler. `krom_tests`: 422 bestanden, 0 fehlgeschlagen
    (`out/claude-jobs12a-build.log`, `out/claude-jobs12a-tests.log`). Wie beim
    Ausgangszustand mit den fremden offenen Änderungen im Arbeitsordner.
  - Nicht geprüft: Start der Editoren, Laufzeit, Bild.
- **Offene Punkte, nicht umgesetzt:**
  - `RenderFrameOrchestrator.cpp` (ab Zeile 197) liest `value->second` aus den drei
    `ValueResult`, ohne zu prüfen, ob ein Wert da ist. Wirft ein Job eine Ausnahme oder
    lehnt der Pool ihn beim Herunterfahren ab, ist `value` leer und der Zugriff undefiniert.
  - Dieselbe Stelle wartet mit `future.get()`, ohne mitzuhelfen. Sind alle Worker mit
    langen Hintergrund-Jobs belegt, steht der Frame, bis einer frei wird. Dasselbe gilt für
    `TaskGraph::Execute`. Es gibt keine für Frame-Jobs reservierten Worker.
  - `ShaderRuntimeAssets.cpp:273` verwirft das Ergebnis von `ParallelFor`.
  - Die Warnung „aufgerufen aus Worker-Thread“ erscheint bei jedem Aufruf; falls später
    Jobs `ParallelFor` nutzen, braucht sie eine Begrenzung.

## Auftrag 12b: Job-System, Kopplung in RenderWorld (2026-11-12)

Commit `3797da7`. Geändert: `src/renderer/RenderWorld.cpp`, `tests/test_renderer.cpp`.
Die Schnittstelle von `ParallelFor` ist unverändert; kein anderer Aufrufer musste angepasst
werden.

- **Befund 1 (RenderWorld rechnet die Batch-Aufteilung nach): stimmt, behoben.**
  `BuildDrawLists` (paralleler Pfad) bestimmte den Ergebnis-Platz als `begin / batchSize` mit
  einer Kopie der Formel aus `ParallelForImpl`. Jetzt sammelt jeder Aufruf in einen eigenen
  Vektor und übergibt ihn unter einem Mutex mit dem ersten Index seines Bereichs; der
  serielle Merge sortiert nach diesem Index. Damit sind Reihenfolge der `objectConstants`,
  `cbOffset` und Draw-Listen dieselben wie vorher, unabhängig davon, wie `ParallelFor` teilt.
- **Teilung durch null: kein Fehler.** Der parallele Pfad läuft nur bei `IsParallel()`
  (Worker-Zahl > 0) und mindestens 256 Proxies; `numBatches` war dort immer mindestens 1. Die
  Rechnung ist mit der Behebung ohnehin entfallen.
- **Andere Aufrufer:** `BoundsSystem.cpp`, `MeshRendererExtraction.cpp`, `IBLBaker.cpp` und
  `ShaderRuntimeAssets.cpp` schreiben je Element in den Platz `i` bzw. `face` des Elements,
  nicht je Batch. Sie hängen nicht von der Aufteilung ab. Kein Aufrufer liest mehr
  `WorkerCount()`, um eine Aufteilung nachzurechnen (`EnvironmentSystem.cpp` nutzt es nur für
  eine Log-Zeile).
- **Prüfung:**
  - Neuer Test „RenderWorld parallel draw lists match serial“: 1003 Proxies mit sichtbaren,
    weggeschnittenen, nur schattenwerfenden, nicht werfenden und materiallosen Einträgen,
    opak und transparent. Vergleicht seriell gegen parallel ohne Pool, mit
    nicht initialisiertem Pool und mit 1, 2, 3, 5, 8, 16 Workern: `objectConstants`
    bytegenau, jede Draw-Liste (Entity, Material, Mesh, `cbOffset`, Sortierschlüssel),
    `VisibleCount` und `visible` je Proxy.
  - Gegenprobe: Mit abgeschalteter Sortierung der Teilergebnisse schlug der Test in 3 von 3
    Läufen fehl (4 von 6 Worker-Zahlen), mit Sortierung besteht er in 3 von 3 Läufen.
  - Debug x64: `krom_editor_vulkan`, `krom_editor_dx11`, `krom_editor_opengl`, `krom_tests`
    gebaut, 0 Fehler. `krom_tests`: 423 bestanden, 0 fehlgeschlagen
    (`out/claude-jobs12b-build.log`, `out/claude-jobs12b-tests.log`). Wie zuvor mit den
    fremden offenen Änderungen im Arbeitsordner.
  - Nicht geprüft: Start der Editoren, Laufzeit, Bild. Eine Szene mit mindestens 256
    Renderables nimmt im Editor den parallelen Pfad.
- **Offene Punkte, nicht umgesetzt:**
  - Die Teilergebnisse werden je Aufruf neu angelegt (vorher auch). Wiederverwendbare Puffer
    über Frames wären möglich, sind aber eine Optimierung, kein Fehler.

## Auftrag 13: Eine Shader-Quelle, Versuch und Konzept (2026-11-12)

Konzept: `doc/KONZEPT_SHADER_EINE_QUELLE.md` (Empfehlung, Tabelle der 16 GLSL-Dateien,
Reihenfolge, offene Fragen). Kurzfassung:

- **Versuch:** Tonemap-Pass (`fullscreen.vs.hlsl`, `passthrough.ps.hlsl`) über DXC →
  SPIR-V → SPIRV-Cross (C-API aus dem Vulkan-SDK) nach GLSL 4.10 übersetzt. Werkzeug
  `tools/shader_cross/krom_shader_cross.cpp`, CMake-Ziel `krom_generate_glsl_experiment`
  (beide `EXCLUDE_FROM_ALL`), Ergebnis eingecheckt unter `assets/generated/`.
- **Schalter:** Umgebungsvariable `KROM_GLSL_EXPERIMENT_GENERATED_TONEMAP=1`, nur unter
  OpenGL, in `src/app/KromWrapper.cpp` und `app/KromEditorApp.cpp`. Standard bleibt die
  handgeschriebene Fassung; alle handgeschriebenen GLSL-Dateien sind unverändert
  vorhanden. Kern unverändert.
- **Selbst geprüft:** `ibl_sphere_opengl` mit erzeugter Fassung pixelgleich zur
  handgeschriebenen (0 von 921.600 Pixeln, Gegenprobe erkennt Unterschiede);
  `krom_editor_opengl` startet mit Schalter ohne Shaderfehler; Reflection gleich (neuer
  Test in `krom_tests`). PBR-, Lit-, Unlit- und Schatten-Shader ergeben gültiges GLSL 4.10
  (glslang), Skinning und Forward+ nicht (`StructuredBuffer`).
- **Empfehlung:** Weg 1 (HLSL als einzige Quelle) mit dem Grundsatz aus Weg 3 für das, was
  GL 4.1 nicht kann. Übersetzung zur Laufzeit im Shader-Compiler ist eine Kernänderung
  und wartet auf Freigabe (Frage 1 im Konzept).
- **Für spätere Aufträge:** Überholt durch die Entscheidung vom 2026-11-12 (OpenGL
  eingefroren, siehe „Entscheidungen des Nutzers“). Die Umstellung kommt ans Ende.
- Gebaut: Debug x64 `krom_editor_vulkan/dx11/opengl`, `krom_tests`, `ibl_sphere_opengl`.
  `krom_tests` 424 bestanden, 0 fehlgeschlagen (`out/claude-xc-build.log`,
  `out/claude-xc-tests.log`). Nicht geprüft: DX11 und Vulkan zur Laufzeit (nicht
  angefasst), Editor-Bild unter OpenGL durch Ansehen.

## Auftrag 2: GPU-Zeitmessung pro Pass (2026-11-12)

- **Vorher:** Nur Vulkan maß die GPU-Zeit des ganzen Frames (`BackendFrameDiagnostics::gpuFrameMs`,
  Zeile „GPU“ im Performance-Overlay). Diese Messung bleibt unverändert; die neue misst je Pass.
- **Kern (allgemeine Fähigkeit):** `IDevice` bekommt `GetGpuTimestampCapacity`,
  `SetGpuTimestampsEnabled`, `GetGpuTimestampFrameId`, `TakeCompletedGpuTimestamps`;
  `ICommandList` bekommt `WriteGpuTimestamp`. Alle mit leerer Voreinstellung (Backend ohne
  Unterstützung meldet Kapazität 0). Neu `GpuPassProfiler` (`include/renderer`,
  `src/renderer`): ordnet Zeitstempel den Pass-Namen zu, addiert gleichnamige Pässe mehrerer
  Ansichten eines Frames, glättet exponentiell mit 0,5 s. `FrameExecutionStage` klammert
  jeden RenderGraph-Pass ein (inklusive seiner Zustandsübergänge).
  `RenderSystem::SetGpuPassTimingEnabled` / `GetGpuPassTimings`.
- **Backends:** DX11 (`DX11GpuTimestamps`, Ring aus 5 Sätzen mit `TIMESTAMP_DISJOINT`),
  OpenGL (`OpenGLGpuTimestamps`, `glQueryCounter`, Funktionen optional geladen), Vulkan
  (eigener Query-Pool je Frame-Slot, Reset beim ersten Graphics-`Begin`, Auslesen nach dem
  Fence in `BeginFrame`). Je bis zu 256 Zeitstempel pro Frame. Ergebnisse kommen ohne
  Warten 2–5 Frames später. Abgeschaltet: keine Queries, `WriteGpuTimestamp` kehrt sofort
  zurück; im Frame bleibt nur ein Zeigervergleich je Pass.
- **Editor:** Einstellungen › Viewport › „GPU-Zeit pro Pass anzeigen“ öffnet das Fenster
  „GPU-Zeit pro Pass“ (Zeile je Pass, ms, Summe, Bildzeit). Die Messung läuft nur, solange
  das Fenster offen ist. Der Schalter wird wie das Performance-Overlay in
  `editor/editor.settings` gespeichert, nicht in der Szene, weil er eine Anzeige-, keine
  Szeneneinstellung ist.
- **Ohne Editor:** `RenderSystem` aus C++, oder für Programme über `krom.h` die
  Umgebungsvariable `KROM_GPU_PASS_TIMING_LOG=1` (Log-Zeile alle 2 s).
- **Selbst gemessen** (`ibl_sphere_*`, Debug x64, GTX 1660, 1280×720, Werte in ms, je eine
  der Log-Zeilen):
  - DX11: Summe 0,56; GtaoBlur 0,29, GtaoComposite 0,05, GtaoMain 0,05, Sky 0,03,
    Opaque 0,03, Tonemap 0,04, Bloom zusammen 0,05.
  - Vulkan: Summe 0,59; GtaoBlur 0,29, GtaoMain 0,07, GtaoComposite 0,05, Sky 0,05,
    Opaque 0,04, Tonemap 0,03, Bloom zusammen 0,06.
  - OpenGL: Summe 0,22 (kein GTAO); Sky 0,05, Opaque 0,04, Tonemap 0,04, Bloom zusammen 0,08.
- **Messwerte Testraum offen:** Eine Szene „Testraum“ gibt es im DemoProject nicht (nur
  Asset-Ordner dieses Namens, benutzt von „Room 2“). Ablesen durch den Nutzer: Szene
  öffnen, Game-Ansicht bzw. Fenster auf 1920×1080, Einstellungen › Viewport › „GPU-Zeit pro
  Pass anzeigen“, nach einigen Sekunden die Werte eintragen. Je Backend den passenden Editor
  starten (`krom_editor_dx11/vulkan/opengl`). Hinweis: Im Editor addiert das Fenster die
  Pässe aller Ansichten (Szene-, Game-Ansicht, Vorschauen).
- **Nutzer-Messung im Editor (2026-11-12, Debug, Summe aller Ansichten, Werte in ms):**

  | Pass | DX11 | Vulkan | OpenGL |
  |---|---|---|---|
  | ShadowPass | 5,790 | 0,106 | 0,198 |
  | MainOpaquePass | 0,993 | 0,821 | 0,857 |
  | GTAO (4 Pässe) | 0,532 | 0,578 | – |
  | Bloom (3 Pässe) | 0,047 | 0,057 | 0,071 |
  | TonemapPass | 0,034 | 0,034 | 0,052 |
  | UIPass | 0,092 | 0,094 | 0,135 |
  | Summe | 7,549 | 1,770 | 1,414 |
  | Bildzeit | 10,5 | 18,9 | 12,0 |

  Die GPU ist auf keinem Backend der Engpass (Summe deutlich unter der Bildzeit).
  **Offen:** ShadowPass unter DX11 ist rund 50-mal so teuer wie unter Vulkan und OpenGL.
  Vermutung, nicht geprüft: Er ist der erste Pass des Frames, und unter DX11 enthält sein
  Start-Zeitstempel die Leerlaufzeit der GPU vor dem Frame. Ebenso möglich ist echte
  Mehrarbeit im DX11-Schattenpfad.
- Tests: neuer Test „GPU pass profiler“ (Zuordnung, Summe über Ansichten, Glättung,
  ungültige Zeitstempel, Abschalten). `krom_tests` 425 bestanden.
- Nicht geprüft: das Editor-Fenster selbst (nur gebaut), Messung bei 1920×1080, Summe gegen
  Bildrate im Editor.

## Auftrag 3: Tonemapping und Exposure (2026-11-12)

- **Shader** (`assets/passthrough.ps.hlsl`, nur HLSL, OpenGL ist eingefroren): HDR + Bloom,
  dann Exposure (`2^EV`), dann Operator: Reinhard (0), ACES Fitted (1, Hill), AgX (2, Basis-Look,
  Fit von Wrensch), Khronos PBR Neutral (3). PerPass-Block: `bloomParams`, neu
  `tonemapParams` (x = Exposure-Faktor, y = Operator). Ausgabe linear für alle Operatoren:
  AgX liefert displaykodierte Werte und wird mit `pow(x, 2.2)` zurück nach linear gebracht,
  ACES Fitted und PBR Neutral arbeiten linear. Keine eigene Gammakurve im Pass.
- **Farbraum geprüft:** DX11 und Vulkan schreiben in sRGB-Ziele (Backbuffer und
  Editor-Ansicht `RGBA8_UNORM_SRGB`), die die Kurve einmal anwenden. OpenGL bleibt beim
  bisherigen Shader (Reinhard, ohne Exposure).
- **Forward-AddOn:** Neu `addons/forward/ForwardTonemap.hpp` (`TonemapOperator`,
  `TonemapSettings`, Namen für Dateien). `ForwardFeatureConfig::tonemapSettings` ist ein
  geteiltes Objekt, das der Tonemap-Pass jeden Frame liest. Eigener PerPass-Puffer für den
  Tonemap-Pass, der immer gebunden wird; vorher wurde `PerPass` ohne Bloom gar nicht gebunden.
- **Szene:** Im Block `environment` der Editor-`.json` und der `.kscene` stehen
  `tonemapOperator` (`reinhard`/`aces`/`agx`/`pbrNeutral`) und `exposureEv`.
  - Neue Szenen und Programme ohne Szene: **AgX**, 0 EV.
  - **Alte Szenen ohne diese Schlüssel laden mit Reinhard und 0 EV**, also mit dem alten Bild.
- **Editor:** Szene-Inspector › Karte „Tonemapping“ mit Operator und Belichtung (EV,
  −10 bis +10).
- **Ohne Editor:** `Krom::SetTonemapping(op, ev)` / `Krom::GetTonemapping()` in `krom.h`, oder
  `ForwardFeatureConfig::tonemapSettings` aus C++.
- **Kernfelder nicht herausgelöst (offener Punkt):** `defaultTonemapMaterial`,
  `tonemapMaterialSystem` und `enableBloom` bleiben in `RenderSystem`, `FrameGraphStage`,
  `RenderFrameOrchestrator`, `RenderPipelineInterfaces`, `RenderRuntimeMaterialBindings` und
  `RenderView`. Neue Kernfelder kamen nicht hinzu. Umbau für später:
  - Das Tonemap-Material gehört dem Forward-AddOn. Das AddOn legt es in `Initialize` selbst an
    (Shader über den `ShaderRuntime` aus dem Initialisierungskontext); `KromWrapper.cpp` und
    `KromEditorApp.cpp` bauen es dann nicht mehr.
  - `enableBloom` an `RenderView` wird durch einen allgemeinen Mechanismus „Pass je Ansicht
    abschalten“ ersetzt (z. B. eine Liste abgeschalteter Feature-IDs an der Ansicht). Der
    Editor nutzt das für Vorschauen.
  - Danach entfallen die Felder aus sechs Kern-Headern und aus dem Hash in
    `FrameGraphStage.cpp`. Aufwand: mittel, berührt alle Aufrufer von `RenderFrame` und die
    Vorschauen im Editor.
- **Auch offen:** Bloom-Stärke und GTAO-Einstellungen haben bisher keinen Weg in Editor und
  Szene (Bloom fest 0,12 in der App; für GTAO gibt es einen Setter ohne Oberfläche). Der
  Auftrag nahm an, dass es diesen Weg gibt.
- **Geprüft:**
  - DXC übersetzt den Shader für SPIR-V und DXIL. `ibl_sphere_dx11/vulkan/opengl` laufen
    ohne Shaderfehler (DX11 über fxc zur Laufzeit).
  - DX11 gegen Vulkan: 114.429 Pixel verschieden (max. 17/255). Mit altem Shader waren es
    110.119 (max. 16), die Abweichung bestand also schon vorher.
  - OpenGL pixelgleich zum Stand vor dem Auftrag.
  - Test „Tonemap operator names“; `krom_tests` 426 bestanden.
- **Nicht geprüft:** Bild der einzelnen Operatoren (kein Urteil möglich); dass Reinhard mit
  0 EV pixelgleich zum alten Bild ist (nur hergeleitet: gleiche Rechnung, Faktor 1);
  Bedienung im Editor.

## Auftrag 4: MSAA – Plan (Teil 1, 2026-11-12, noch nicht freigegeben)

Am Code geprüft:
- `sampleCount` ist überall 1. Fest gesetzt ist er unter anderem in
  `GpuResourceRuntime.cpp` (Renderziele des Graphen), `ShaderRuntimeMaterials.cpp`
  (Material-Pipelines), `VulkanDevice.cpp` (Images und Pipelines), `ForwardFeature.cpp`,
  `GtaoPass.cpp` und `OutlinePass.cpp`.
- DX11 reicht die Zahl schon durch. Vulkan und OpenGL können heute keine MS-Ziele anlegen.
- Die Szenentiefe ist der D24S8-Anhang von `HDRSceneColor`. GTAO liest sie direkt
  (`GetRenderTargetDepthTexture`).

**Ziele und Pässe**
- Mehrfach abgetastet: neues Ziel `HDRSceneColorMS` (RGBA16F + D24S8, N Samples).
- In das MS-Ziel zeichnen: Sky, MainOpaque, Transparent, Partikel, DebugDraw. Deren
  Pipelines brauchen N Samples: Sky (Forward-AddOn), Material-Pipelines (Opaque und
  Transparent), Partikel und DebugDraw.
- Einfach abgetastet bleiben: Shadow, GTAO, Outline, Editor-Gizmos (zeichnen nach dem
  Resolve), Bloom, Tonemap, UI.

**Graph bei MSAA an:**
Shadow → Sky/Opaque/Transparent/Partikel/DebugDraw (MS) → **MsaaResolve** → GTAO →
Outline/Editor-Overlays → Bloom → Tonemap → UI. Der Resolve schreibt in das bisherige
`HDRSceneColor`. Damit lesen GTAO, Outline, Editor, Bloom und Tonemap unverändert.
Bei MSAA aus entfällt der Resolve, und die Szenenpässe zeichnen wie heute direkt in
`HDRSceneColor`.

**Resolve-Rechnung:** eigener Vollbild-Shader statt API-Resolve.
- Farbe gewichtet mit `1/(1+Leuchtdichte)` je Sample und danach normiert. Ein heller Sample
  dominiert die Kante damit nicht; der einfache Mittelwert vor dem Tonemapping lässt helle
  Kanten flimmern.
- API-Resolve (`ResolveSubresource`, `vkCmdResolveImage`, `glBlitFramebuffer`) ist
  billiger, kann nur den einfachen Mittelwert und rechnet je Backend leicht anders.
- Kosten des Shaders bei 1080p und 4x: geschätzt 0,1–0,2 ms.

**Einfach abgetastete Tiefe:** Derselbe Resolve-Shader schreibt die Tiefe von Sample 0 als
`SV_Depth` in den D24S8-Anhang von `HDRSceneColor`. GTAO, Outline und die Editor-Gizmos
finden die Tiefe also dort, wo sie sie heute suchen.
- MS-Tiefe als Textur lesen können alle drei Backends: DX11 SM5 `Texture2DMS` (die Tiefe hat
  schon eine SRV), Vulkan mit gesampletem MS-Image, GL 4.1 `sampler2DMS`.
- Echter Tiefen-Resolve ginge unter DX11 gar nicht, unter Vulkan nur mit
  `VK_KHR_depth_stencil_resolve`, unter GL nur per Blit. Ein Tiefen-Prepass kostet einen
  zweiten Geometriedurchlauf. Beides verworfen.
- Grenze: An Kanten passt die Tiefe zu einem Sample, die Farbe zum Mittel. GTAO hat dort
  einen Pixel Ungenauigkeit.

**Forward+:** bleibt unverändert. Die Tile-Listen entstehen aus Lichtgrenzen auf dem
Bildschirm, ohne Tiefe. Der Pixelshader läuft je Pixel, nicht je Sample, und `SV_Position`
bleibt die Pixelmitte, damit bleibt auch der Tile-Index gleich.

**Nötige Kernänderungen (allgemein, keine Feature-Namen):**
1. `FrameRecipeResourceDesc` / RG-Ressource bekommen `sampleCount`; `GpuResourceRuntime` legt
   Ziele damit an.
2. Material-Pipelines nehmen die Sample-Zahl des aktuellen Ziels. `ShaderRuntime` bekommt
   dafür beim Binden die Sample-Zahl des Passes. `PipelineKey.sampleCount` gibt es schon.
3. `IRenderPipeline` bekommt eine Revisionsnummer, die in den Graph-Schlüssel
   (`FrameGraphStage::ComputeStructureKey`) eingeht. Ein Wechsel der Stufe baut den Graphen
   damit ohne Neustart neu.
4. Backends: Vulkan MS-Images und `rasterizationSamples`; OpenGL MS-Texturen. DX11 kann es
   schon.

**Einstellung:** `msaaSamples` (1/2/4) in den Forward-Einstellungen. Sie wird wie das
Tonemapping im `environment`-Block der Szene gespeichert; alte Szenen laden mit „aus“.
Editor-Karte im Szene-Inspector, `Krom::SetMsaa`.

**Kosten (geschätzt, nicht gemessen):**
- Speicher bei 1080p und 4x: HDR-MS 66 MB + Tiefe-MS 33 MB, also rund 100 MB zusätzlich.
  Heute belegen HDR und Tiefe zusammen etwa 25 MB.
- GPU: Opaque steigt bei 4x erfahrungsgemäß um 30–100 %. Im Editor waren es 0,82–0,99 ms
  (Auftrag 2), also etwa +0,3–0,9 ms. Dazu kommen Resolve 0,1–0,2 ms sowie Sky, Partikel und
  DebugDraw anteilig.
- Insgesamt unter 1,5 ms bei 1080p, gut im Rahmen von 16,7 ms.

**Bewertung:** MSAA ist in dieser Architektur machbar, ohne den Renderer umzubauen. Der
größte Posten sind die Backends (Vulkan, OpenGL) und die Sample-Zahl der Material-Pipelines.
FXAA oder SMAA als Übergang sind nicht nötig.

**Nicht abgedeckt:** Kanten innerhalb von Alpha-Test-Materialien (Laub, Zäune); dafür bräuchte
es Alpha-to-Coverage. Spiegelndes Flimmern innerhalb von Flächen bleibt ebenfalls, MSAA wirkt
nur an Dreieckskanten.

**Offene Entscheidung:** OpenGL ist eingefroren. MSAA unter OpenGL bräuchte einen
GLSL-Resolve-Shader und MS-Texturen im GL-Backend. Vorschlag: OpenGL bleibt ohne MSAA, die
Einstellung ist dort ausgegraut.

## Was sich gegenüber dem Plan geändert hat

- Auftrag 1: keine Abweichung. Zwei Befunde liegen anders als angenommen (Nr. 3
  UV-Satz, Nr. 7 Culling/OpenGL); beide wirken auf spätere Aufträge:
  - Auftrag 4 (MSAA): Vulkan und OpenGL brauchen Backend-Arbeit (Sample-Zahl, Resolve),
    DX11 nur den Resolve.
  - Aufträge 7 und 8: Beleuchtungsänderungen in drei PBR-Shadern nachziehen; für OpenGL
    ist zu entscheiden, wie „gleich aussehen“ ohne GTAO gemeint ist.

## Entscheidungen des Nutzers

- 2026-11-12: OpenGL bleibt im jetzigen Zustand (klassisches Forward, kein Forward+, kein
  GTAO). Die Reihe gleicht das nicht an. „Auf allen drei Backends gleich“ heißt: Jede
  Änderung wird auch in den OpenGL-Shadern nachgezogen, soweit sie nicht an GTAO oder den
  Tile-Listen hängt.
- 2026-11-12: Exposure kommt in Auftrag 3, die Reihenfolge der Aufträge bleibt. Weg: über
  den PerPass-Block des Tonemap-Passes. Der Block gehört dem Forward-AddOn
  (`ForwardFeature.cpp`, Puffer `ForwardBloom_PerPassCB`, 256 Bytes, davon 16 belegt), nicht
  dem Kern. Dort ein eigenes `float4` für Tonemap-Parameter ergänzen, `bloomParams.w` nicht
  belegen. Auftrag 5 ist dafür nicht nötig.
- 2026-11-12: OpenGL wird für diese Reihe eingefroren (Weg 3 aus Auftrag 13). Die
  Bildaufträge 3, 7 und 8 ändern nur die HLSL-Shader (DX11, Vulkan); die GLSL-Dateien unter
  `assets/*.opengl.*.glsl` werden nicht nachgezogen. Die Regel „auf allen drei Backends
  gleich“ gilt bis auf Weiteres nur für DX11 und Vulkan; unter OpenGL fehlen die neuen
  Bildfunktionen. Die Umstellung auf HLSL als einzige Quelle (Weg 1,
  `doc/KONZEPT_SHADER_EINE_QUELLE.md`) kommt ans Ende der Liste, die offenen Fragen dort
  bleiben bis dahin offen. Der Tonemap-Versuch bleibt abschaltbar im Code.

## Offen / für den Nutzer zu entscheiden

- OpenGL (letzte Priorität, nicht streichen): Nutzer meldet am 2026-11-12 im OpenGL-Editor
  „Schatten passt nicht“. Ursache nicht untersucht. Der Tonemap-Versuch aus Auftrag 13
  ändert keine Schattenberechnung, nur die Abbildung der Helligkeit; zu klären ist, ob das
  Bild ohne `KROM_GLSL_EXPERIMENT_GENERATED_TONEMAP` gleich aussieht (dann vorbestehend,
  vgl. offenes OpenGL-Schattenschwimmen bei CSM).

- Derzeit nichts.
