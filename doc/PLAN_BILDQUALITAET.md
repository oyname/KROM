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
| 2 | GPU-Zeitmessung pro Pass | offen |
| 3 | Tonemapping und Exposure | offen |
| 4 | Anti-Aliasing mit MSAA | offen |
| 5 | Kern-Vorarbeit: Parameterblöcke für AddOns | offen |
| 6 | Lokale Probes: Konzept | offen |
| 7 | Lokale Probes: Umsetzung und Specular Occlusion | offen |
| 8 | Feinschliff: GTAO, Schattenfilter, BRDF | offen |
| 9 | Materialgraph erzeugt Shader-Code: Konzept | offen |

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

## Offen / für den Nutzer zu entscheiden

- Derzeit nichts.
