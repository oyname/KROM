# Konzept: Eine Shader-Quelle statt zwei

Auftrag 13, Stand 2026-11-12, Branch `feature/bildqualitaet`.

Heute liegt jeder Shader zweimal vor: HLSL für DX11 und Vulkan, eine von Hand
geschriebene GLSL-4.10-Datei (`*.opengl.*.glsl`) für OpenGL. Dieses Dokument hält den
Versuch am Tonemap-Pass fest und schlägt vor, wie die doppelte Pflege endet.
Entscheidungen, die bei dir liegen, stehen gesammelt am Ende.

## 1. Ausgangslage, am Code geprüft

- **Dateien:** Unter `assets/` liegen 22 HLSL-Dateien (4044 Zeilen) und 16 GLSL-Dateien
  (2048 Zeilen). 12 der 16 GLSL-Dateien sind Shader mit `#version 410 core`, die übrigen
  vier sind Include-Dateien (`per_frame`, `skinning`, `pbr_box_projection`,
  `pbr_material_graph`).
- **Nicht mitgezählt, aber betroffen:** 18 GLSL-Shader stehen als Zeichenketten im C++
  der AddOns: `ForwardFeature.cpp` (Himmel, Bloom), `OutlinePass.cpp`, `DebugDraw.cpp`,
  `ParticleRenderer.cpp`, `TextRenderer.cpp` und `GLShaderReflector.cpp`. Dazu kommen
  Paare unter `assets/debug/` und `assets/examples/` sowie die Custom-Shader der Projekte
  (DemoProject: drei Paare).
- **Werkzeuge:** Vulkan übersetzt HLSL bereits mit `dxc.exe` nach SPIR-V
  (`ShaderCompilerDXC.cpp`, als externer Prozess). Der GLSL-Weg reicht den Quelltext an
  den Treiber durch (`ShaderCompilerGLSL.cpp`).
- **Bindung unter OpenGL:** Das Backend bindet nach dem Linken über Namen
  (`OpenGLResources.cpp`): Uniform-Blöcke `PerFrame`/`PerObject`/`PerMaterial`/`PerPass`
  an 0 bis 3, Sampler über eine feste Namensliste (`uAlbedo`, `albedo`, `shadowMapRaw`,
  `uHDRInput` …) und Vertex-Attribute über `aPosition` … `aColor`.
- **Custom-Shader:** `AssetPipeline.cpp` sucht zu `x.vs.hlsl` die Datei `x.opengl.vs.glsl`
  (`OpenGlShaderVariantPath`). Der Export füllt `shader-bin/`; ein exportiertes Spiel
  kompiliert nichts.
- **Profilname:** Das Zielprofil heißt `OpenGL_GLSL450`, alle Shader sind aber 4.10.

## 2. Versuch am Tonemap-Pass

### 2.1 Aufbau

- **Werkzeug:** Neu ist `tools/shader_cross/krom_shader_cross.cpp`, ein kleines
  Kommandozeilenprogramm auf der C-Schnittstelle von SPIRV-Cross. Bibliothek und DLL
  `spirv-cross-c-shared` kommen aus dem Vulkan-SDK. Die DLL ist unabhängig von der
  MSVC-Laufzeit und läuft deshalb auch im Debug-Build. Das Programm übersetzt SPIR-V nach
  GLSL 4.10 und setzt die Namen so, wie das OpenGL-Backend sie bindet:
  - Uniform-Block `type.PerPass` → Block `PerPass`, Instanz `cb_PerPass` (GLSL verbietet
    gleiche Namen für Block und Instanz).
  - Kombinierte Sampler → Name der HLSL-Textur (`uHDRInput`, `uBloomTexture`).
  - Ein- und Ausgänge `in.var.<SEMANTIK>`/`out.var.<SEMANTIK>` → `krom_<SEMANTIK>` auf
    beiden Seiten, Vertex-Eingänge → `aPosition`, `aNormal` … wie im Backend.
  - Kein `layout(binding)`, weil GL 4.1 das ohne Erweiterung nicht kennt.
- **CMake:** Das Ziel `krom_generate_glsl_experiment` erzeugt aus `fullscreen.vs.hlsl`
  und `passthrough.ps.hlsl` die Dateien `assets/generated/fullscreen.opengl.vs.glsl` und
  `assets/generated/passthrough.opengl.fs.glsl`. DXC läuft dabei mit `-spirv -HV 2021
  -O3 -fvk-use-gl-layout -D KROM_OPENGL=1`. Beide Ziele sind `EXCLUDE_FROM_ALL`, und die
  erzeugten Dateien sind eingecheckt, ein normaler Build braucht also weder DXC noch
  SPIRV-Cross.
- **Schalter:** Die Umgebungsvariable `KROM_GLSL_EXPERIMENT_GENERATED_TONEMAP=1` wählt
  unter OpenGL die erzeugte Fassung, in `src/app/KromWrapper.cpp` und
  `app/KromEditorApp.cpp`, je `InitializeTonemapMaterial`. Ohne Variable bleibt alles wie
  bisher. Ist sie gesetzt, steht im Log die Warnung `… tonemap pass uses generated GLSL
  (experiment)`. Der Engine-Kern ist nicht geändert.

### 2.2 Selbst ausgeführt

| Frage | Ergebnis |
|---|---|
| GLSL 4.10, das der Treiber annimmt? | Ja. `ibl_sphere_opengl` und `krom_editor_opengl` laufen mit Schalter ohne Compile- oder Link-Fehler (GTX 1660, Debug x64). glslang prüft und linkt das Paar ebenfalls fehlerfrei. |
| Gleiches Bild? | `ibl_sphere_opengl`, Fensterinhalt 1280×720 per `PrintWindow` aufgenommen: handgeschrieben gegen erzeugt **0 von 921.600 Pixeln verschieden**, zweimal wiederholt. Gegenprobe mit absichtlich gespiegelter erzeugter Fassung: 917.286 Pixel verschieden. Der Vergleich erkennt Unterschiede also. |
| Namen für die Bindung per Name? | Ohne Nachbearbeitung nicht: Der Block heißt `type_PerPass`, die Sampler `SPIRV_Cross_CombineduHDRInputuSampler`, die Varyings `in_var_TEXCOORD0`/`out_var_TEXCOORD0`. Mit den Namensregeln des Werkzeugs passen sie ohne Änderung am Backend. |
| Bildorientierung, Tiefe, Matrizen | Kein Eingriff nötig: weder DXC noch SPIRV-Cross spiegeln Y, solange `-fvk-invert-y` und `--flip-vert-y` aus bleiben. Die handgeschriebenen Dateien spiegeln V zweimal (im Vertex- und im Fragment-Shader), die erzeugten gar nicht; das Ergebnis ist dasselbe UV. Deshalb müssen Vertex- und Fragment-Shader **gemeinsam** umgestellt werden. Matrizen erscheinen als `layout(row_major) mat4` mit `v * M`, rechnerisch gleich zur handgeschriebenen Form `M * v`. |
| Reflection der Constant Buffer | Funktioniert. Neuer Test „Generated tonemap GLSL matches hand-written“ in `krom_tests`: `ReflectGlslStd140Block("PerPass")` liefert für beide Fassungen dieselben Felder, Offsets und Größen, `ReflectGlslTextureBindings` dasselbe Ergebnis. |
| Zeit | SPIRV-Cross: Tonemap etwa 1 ms je Stufe, `pbr_lit.ps` 13 ms, `pbr_forward_plus.ps` 21 ms. DXC einschließlich Prozessstart: 55–65 ms für kleine Shader, 296 ms für `pbr_lit.ps`. |
| Shader-Cache und Export | Die erzeugte Datei ist für die Engine eine gewöhnliche GLSL-Quelle. Sie geht unverändert durch `CacheFirstCompile` und landet als GLSL-Quelltext in `shader-bin/`. Am Cache ist nichts zu ändern. |

### 2.3 PBR-Fall, praktisch angefangen

- **Übersetzbar:** Folgende Shader habe ich durch DXC, das Werkzeug und glslang laufen
  lassen, mit `KROM_IBL`, `KROM_NORMAL_MAP`, Base-Color-, ORM- und Emissive-Map, jeweils
  mit und ohne `KROM_SKINNING`: `pbr_lit.ps`, `pbr_lit.vs`, `lit.vs/ps`,
  `quad_unlit.vs/ps`, `shadow.vs/ps` und `shadow_pbr.vs/ps`. Alle ergeben gültiges
  GLSL 4.10.
- **Nicht übersetzbar:** Alle `skinned_*.vs.hlsl` und `pbr_forward_plus.ps.hlsl`, weil
  `StructuredBuffer` zu einem std430-Speicherpuffer wird, den GL 4.1 nicht kennt.
- **Blöcke:**
  - `PerObject` ist feldgleich mit der handgeschriebenen Fassung.
  - `PerFrame` ebenfalls, hat aber zusätzlich die vier GTAO-Felder am Ende; das ändert
    keine Offsets.
  - `PerMaterial` ist gleich bis auf die Materialgraph-Felder, die im Versuch nicht
    aktiviert waren.
- **Nicht ausgeführt:** Kein PBR-Shader wurde unter OpenGL gestartet und kein Bild
  verglichen. Die Liste unten ist aus dem Vergleich der Quelltexte hergeleitet.

Stellen im HLSL, die unter GL 4.1 anders sein müssen, und wie sie zu behandeln sind:

1. **Schattenpfad (`SampleShadowAtlas`, in fünf Dateien):**
   - HLSL rechnet `v = 0.5 - y*0.5` und `depth = z`.
   - Die GL-Fassung rechnet `v = y*0.5 + 0.5` und `depth = z*0.5 + 0.5`, weil die
     Schattenmatrix unter OpenGL nach [-1, 1] abbildet (`GetClipSpaceAdjustment`).
   - Lösung: eine Hilfsfunktion `KromShadowNdcToUvDepth()` in einem gemeinsamen Include,
     mit `#ifdef KROM_OPENGL` für die beiden Zeilen.
2. **Schattenkarte und Sampler-Zustand:**
   - HLSL liest `shadowMap` (t4) mit `sPoint`.
   - Unter GL gehört der Sampler-Zustand zur Textureinheit; Einheit 4 trägt den
     Vergleichs-Sampler. Die GL-Fassung liest deshalb über eine zweite Einheit 9
     (`shadowMapRaw`).
   - Die Übersetzung muss je Paar aus Textur und Sampler eine eigene Einheit vergeben
     können (siehe 4.2), oder das HLSL deklariert unter `KROM_OPENGL` eine eigene Textur
     auf t9.
3. **GTAO-Abschnitt:**
   - `SV_POSITION` im Pixelshader ist unter GL unten links verankert
     (`gl_FragCoord`), unter D3D oben links.
   - GTAO gibt es unter OpenGL nicht. Der Abschnitt kommt unter
     `#if !defined(KROM_OPENGL)`.
4. **Skinning:**
   - `StructuredBuffer<float4x4>` wird unter `KROM_OPENGL` zu `Buffer<float4>` mit vier
     `Load` je Matrix und `GetDimensions`.
   - Selbst ausgeführt an einem Beispiel: DXC und SPIRV-Cross machen daraus
     `samplerBuffer`, `texelFetch` und `textureSize`, genau wie in
     `skinning.opengl.glsl`.
   - Die Uniform `uBoneCount` entfällt dann. Die Eingänge `BLENDINDICES`/`BLENDWEIGHT`
     brauchen `[[vk::location(7/8)]]` oder Einträge in der Namensliste des Werkzeugs.
5. **Texturnamen:** HLSL und GL heißen verschieden, zum Beispiel `tAlbedo`/`uAlbedo` bei
   `quad_unlit` und `shadowMap`/`shadowMapRaw` bei PBR. Lösung: Benennung nach Register,
   nicht nach HLSL-Name (siehe 4.2).
6. **Forward+:** Nicht übersetzbar (Tile-Listen als `StructuredBuffer`), und unter
   OpenGL ohnehin nicht vorgesehen. OpenGL bleibt beim klassischen `pbr_lit`.
7. **Größe:** Erzeugtes `pbr_lit.fs` hat 1536 Zeilen gegen 723 handgeschriebene. Die
   Lesbarkeit leidet; für den Treiber ist das unerheblich.

## 3. Die drei Wege und die Empfehlung

- **Weg 1, HLSL als einzige Quelle (DXC → SPIR-V → SPIRV-Cross → GLSL 4.10):**
  Der Versuch zeigt, dass das mit GL 4.1 sauber geht. Am Tonemap-Pass ist das Ergebnis
  pixelgleich, und alle Nicht-Forward+-Shader ergeben gültiges GLSL. Die Arbeit liegt
  nicht in der Übersetzung, sondern in einer Handvoll `KROM_OPENGL`-Stellen und einer
  festen Namensregel.
- **Weg 2, gemeinsame Rechenteile über Makro-Kopf:** Ein- und Ausgänge sowie Bindungen
  bleiben doppelt. Das sind genau die Stellen, an denen die beiden Fassungen heute
  auseinanderlaufen. Außerdem ist jede neue Funktion in einer dritten Mischsprache zu
  schreiben.
- **Weg 3, OpenGL einfrieren:** Billig, aber dann bekommt OpenGL auch keine Korrekturen
  mehr, etwa am Schattenfilter oder am Tonemapping.

**Empfehlung: Weg 1 mit dem Grundsatz aus Weg 3 für das, was GL 4.1 nicht kann.**
HLSL ist die einzige Quelle. Was unter GL 4.1 nicht übersetzbar ist (Compute,
`StructuredBuffer`, `RWTexture`), steht hinter `#if !defined(KROM_OPENGL)` oder hat eine
einfachere GL-Alternative im selben HLSL. Weg 2 wird nicht gebraucht.

## 4. Umsetzung im Einzelnen

### 4.1 Wo übersetzt wird

**Variante A, zur Laufzeit im Shader-Compiler (empfohlen):**
- In `CacheFirstCompile` übersetzt ein HLSL-Asset für das Ziel OpenGL über DXC und
  SPIRV-Cross. Heute ist dieses Ziel für HLSL ein Fehler.
- Das Ergebnis ist ein GLSL-Quelltext-Artefakt und landet wie bisher in `shader-bin/`.
- Der Export füllt `shader-bin/` schon heute; das exportierte Spiel braucht also weiterhin
  keinen Compiler.
- Defines (`KROM_SKINNING` …) und Shader-Varianten funktionieren ohne weiteres Zutun,
  weil der Compiler sie ohnehin kennt.
- Das ist eine **Kernänderung**: Der Shader-Compiler in `src/renderer` bekommt einen
  dritten Übersetzungsweg. Er ist allgemein (Quellsprache → Ziel), nicht
  feature-spezifisch. Weil er zum Kern gehört, gebe ich ihn erst nach deiner Freigabe an.

**Variante B, beim Build:** Wie im Versuch erzeugt ein CMake-Schritt die GLSL-Dateien
und checkt sie ein.
- Kein Kerneingriff.
- Aber: Jede Kombination von Defines braucht eine eigene Datei. Die GL-Shader arbeiten
  heute mit `#ifdef` zur Laufzeit, die Varianten entstehen also erst beim Laden. Für die
  Engine-Shader wären das Dutzende Dateien, und Custom-Shader müssten beim Speichern
  übersetzt werden.

Empfehlung: **A.** B taugt nur für einzelne Pass-Shader ohne Varianten (Tonemap, Bloom,
Himmel).

### 4.2 Namensregel für die Bindung

Heute bindet das Backend über eine gewachsene Namensliste (`albedo` und `uAlbedo` auf 0,
`shadowMap` auf 4, `shadowMapRaw` auf 9 …). Vorschlag:

- **Texturen:** Das Werkzeug benennt den kombinierten Sampler nach HLSL-Registern:
  `krom_t<Textur>_s<Sampler>`, zum Beispiel `krom_t4_s2`. DXC behält die Register als
  Binding-Decoration, also ist der Name eindeutig ableitbar. Das OpenGL-Backend zerlegt
  den Namen nach dem Linken, bindet Textur-Slot 4 an eine freie Einheit und setzt dort
  das Sampler-Objekt für s2. Damit ist auch der `shadowMapRaw`-Sonderfall erledigt.
- **Blöcke:** `krom_b<N>` oder der Name des cbuffers. Der Name genügt, die vier Blöcke
  sind fest.
- **Alte Liste:** Bleibt für handgeschriebene Shader erhalten, bis keiner mehr übrig ist.

**Alternative:** `GL_ARB_shading_language_420pack` mit `layout(binding = N)`. Das ist
kürzer, setzt aber eine Erweiterung voraus, die das Backend heute ausdrücklich als
optional behandelt (Kommentar bei `uEnvironment`). Es löst auch das Problem des
Sampler-Zustands je Einheit nicht. Empfehlung: Namensregel.

### 4.3 Die 16 GLSL-Dateien

| GLSL-Datei | HLSL-Gegenstück | Einstufung |
|---|---|---|
| `fullscreen.opengl.vs.glsl` | `fullscreen.vs.hlsl` | **erzeugbar, ausgeführt.** Nur gemeinsam mit dem Fragment-Shader umstellen (V-Spiegelung, siehe 2.2). |
| `passthrough.opengl.fs.glsl` | `passthrough.ps.hlsl` | **erzeugbar, ausgeführt, pixelgleich.** |
| `quad_unlit.opengl.vs.glsl` | `quad_unlit.vs.hlsl` | erzeugbar (glslang); Namensregel nötig |
| `quad_unlit.opengl.fs.glsl` | `quad_unlit.ps.hlsl` | erzeugbar (glslang); Namensregel nötig (`tAlbedo`/`uAlbedo`) |
| `shadow.opengl.vs.glsl` | `shadow.vs.hlsl` + `skinned_shadow.vs.hlsl` | erzeugbar; Skinning braucht Define (4) |
| `shadow.opengl.fs.glsl` | `shadow.ps.hlsl` | erzeugbar (glslang) |
| `shadow_pbr.opengl.vs.glsl` | `shadow_pbr.vs.hlsl` + `skinned_shadow_pbr.vs.hlsl` | erzeugbar; Skinning braucht Define (4) |
| `shadow_pbr.opengl.fs.glsl` | `shadow_pbr.ps.hlsl` | erzeugbar (glslang) |
| `lit.opengl.vs.glsl` | `lit.vs.hlsl` + `skinned_lit.vs.hlsl` | erzeugbar; Skinning braucht Define (4) |
| `lit.opengl.fs.glsl` | `lit.ps.hlsl` | **Defines nötig:** Schatten-NDC (1), Schattenkarte (2) |
| `pbr_lit.opengl.vs.glsl` | `pbr_lit.vs.hlsl` + `skinned_pbr_lit.vs.hlsl` | erzeugbar; Skinning braucht Define (4) |
| `pbr_lit.opengl.fs.glsl` | `pbr_lit.ps.hlsl` | **Defines nötig:** (1), (2), (3) |
| `per_frame.opengl.glsl` | cbuffer `PerFrame` in den HLSL-Dateien | Include, **entfällt** (Felder geprüft, siehe 2.3) |
| `skinning.opengl.glsl` | Bone-Code in `skinned_*.vs.hlsl` | Include, **entfällt**; besser ein gemeinsamer HLSL-Include mit dem Define aus (4) |
| `pbr_box_projection.opengl.glsl` | `pbr_box_projection.hlsl` | Include, entfällt (in `pbr_lit.ps` mitübersetzt) |
| `pbr_material_graph.opengl.glsl` | `pbr_material_graph.hlsl` | Include, entfällt; **nicht ausgeführt** mit `KROM_MATERIAL_GRAPH`, das Array `gKromGraphValues[16]` und der dynamische Index sind aber GL-4.1-tauglich |

**Keine Datei muss handgeschrieben bleiben.** Für die 18 GLSL-Zeichenketten im C++
gilt dasselbe Muster: Sie werden HLSL-Zeichenketten oder Dateien. Das gehört in einen
eigenen Schritt (5.4), denn diese AddOns übersetzen heute selbst mit
`CreateShaderFromSource`.

### 4.4 Was OpenGL bewusst nicht bekommt

- **Grundsatz:** OpenGL bekommt eine Funktion, wenn sie aus dem HLSL ohne nennenswerten
  Mehraufwand übersetzbar ist. Sonst läuft OpenGL ohne sie weiter. Das betrifft heute
  Forward+-Culling, GTAO, Compute-Shader und alles mit `StructuredBuffer` oder
  `RWTexture`.
- **Im HLSL:** Solche Teile stehen hinter `#if !defined(KROM_OPENGL)`. Fehlt der
  Ausschluss, schlägt die Übersetzung beim Laden mit einer klaren Meldung fehl.
- **In der Engine:** Jedes AddOn meldet, ob es auf dem aktiven Backend wirkt. GTAO und
  Forward+ tun das schon heute per Log-Warnung (`GtaoFeature.cpp`, `ForwardFeature.cpp`).
  Daraus wird eine abfragbare Eigenschaft der AddOn-Schnittstelle, etwa
  `IsSupportedOn(ShaderTargetProfile)`. Das ist eine allgemeine Kernschnittstelle, keine
  feature-spezifische.
- **Im Editor:** Einstellungen eines AddOns, das auf dem aktiven Backend nicht wirkt,
  sind ausgegraut. Daneben steht der Hinweis „Unter OpenGL nicht verfügbar“ als
  Tooltip, der Wert bleibt erhalten und wird gespeichert. So ändert niemand etwas, das
  unter OpenGL nichts bewirkt, und beim Wechsel auf Vulkan ist die Einstellung wieder da.

### 4.5 Neue Abhängigkeiten

- **SPIRV-Cross:** Apache-2.0-Lizenz.
  - Im Versuch: C-Schnittstelle (`spirv-cross-c-shared`) aus dem Vulkan-SDK, nur im
    Werkzeug.
  - Für Variante A zwei Möglichkeiten:
    - `spirv-cross.exe` als externes Werkzeug wie heute `dxc.exe`, gesucht über
      `ShaderToolPaths`. Das ist am wenigsten Bauaufwand, aber die Namensregeln
      verlangen die API; die Kommandozeile kann kombinierte Sampler nicht umbenennen.
    - Die C-DLL zur Laufzeit laden (`LoadLibrary`, wie heute `d3dcompiler_47.dll`). Das
      bleibt unabhängig von der MSVC-Laufzeit.
  - Empfehlung: **DLL laden.**
- **DXC:** Wird schon für Vulkan gebraucht, also nichts Neues. Unter OpenGL kommt DXC
  damit zum ersten Mal ins Spiel; ein Rechner nur mit OpenGL braucht es dann im Editor
  ebenfalls.
- **Engine-Paket und Editor-Installation (Auftrag 10 ist noch nicht bearbeitet):**
  - Der Editor liefert `dxcompiler.dll`, `dxc.exe` und `spirv-cross-c-shared.dll` mit
    oder findet sie im Vulkan-SDK, wie heute DXC.
  - Das Engine-Paket braucht die DLLs nur, wenn es selbst Shader übersetzt (Editor,
    Entwicklung). Ein exportiertes Spiel braucht keine von beiden, weil `shader-bin/`
    das fertige GLSL enthält.
  - Die Lizenztexte (Apache 2.0, DXC: University of Illinois/NCSA) gehören in die
    Third-Party-Hinweise.

### 4.6 Custom-Shader und Materialgraph

- **Custom-Shader:** Künftig reicht HLSL. `OpenGlShaderVariantPath` sucht weiter zuerst
  eine `*.opengl.*.glsl`-Datei; fehlt sie, wird das HLSL übersetzt.
  - Vorhandene Paare laufen weiter; die handgeschriebene Fassung hat Vorrang.
  - `doc/custom_shader_guide.md` bekommt einen Abschnitt „Nur HLSL schreiben“ und die
    Regel für GL-untaugliche Konstrukte.
  - Das Mapping für eigene Texturen per `layout(binding)` (heute 420pack) entfällt, weil
    die Namensregel das Register mitbringt.
- **Materialgraph (Auftrag 9):** Erzeugt künftig nur HLSL. Der GL-Weg ergibt sich aus der
  Übersetzung. Das halbiert Auftrag 9.

### 4.7 Profilname

`OpenGL_GLSL450` umbenennen in `OpenGL_GLSL410`. Der Name steht im Shader-Cache-Schlüssel
und in Logs; ein Umbenennen ändert die Cache-Dateinamen, alte Einträge werden einmal neu
erzeugt. Empfehlung: umbenennen, zusammen mit Schritt 5.1, weil dort ohnehin
`kShaderArtifactCacheSchemaVersion` steigt.

## 5. Reihenfolge der Umstellung

Nach jedem Schritt bauen alle drei Backends, und `krom_tests` läuft.

**Bildprüfung unter OpenGL nach jedem Schritt:**
- Die Skripte `tools/shader_cross/capture_window.ps1` und `compare_images.ps1` aus dem
  Versuch nehmen ein Beispiel unter OpenGL einmal mit der alten und einmal mit der neuen
  Fassung auf und vergleichen die Pixel. Ziel ist 0 Abweichung, wie im Versuch.
- Szenen: `ibl_sphere_opengl` für IBL und PBR, `textured_cube_opengl` für Licht und
  Schatten, `load_3d_model_opengl` für Skinning. Letzteres ist animiert und braucht vor
  dem Vergleich einen festen Zeitpunkt.
- Dazu Sichtprüfung im Editor durch dich.

1. **Kernweg einbauen (Variante A) und Profil umbenennen.**
   - Neu: HLSL → GLSL im Shader-Compiler, Namensregel im Backend, Cache-Schema +1.
   - Handgeschriebene Dateien bleiben Standard.
   - Ein Schalter erzwingt je Shader die Übersetzung (Nachfolger der heutigen
     Umgebungsvariable).
   - Prüfung: Tonemap wie im Versuch.
2. **Pass-Shader ohne Varianten:** Tonemap und Fullscreen umstellen, danach die
   handgeschriebenen Dateien löschen.
3. **Gemeinsame Includes:** Schatten-NDC (1), Schattenkarte (2) und Skinning-Puffer (4)
   als HLSL-Include mit `KROM_OPENGL`. Bild unter DX11 und Vulkan unverändert (Vergleich
   gegen vorher), unter OpenGL noch die alten Dateien.
4. **Schatten- und Unlit-Shader:** `shadow*`, `quad_unlit`, `lit` umstellen und je
   vergleichen.
5. **PBR:** `pbr_lit` mit GTAO-Ausschluss (3) und Materialgraph umstellen und vergleichen.
   Danach sind alle 16 Dateien entfernt.
6. **Zeichenketten in AddOns:** Bloom, Himmel, Outline, DebugDraw, Partikel, Text auf
   HLSL umstellen.
7. **Custom-Shader:** Rückfall „HLSL übersetzen, wenn keine GLSL-Datei da ist“ in
   `AssetPipeline` und Anleitung. Die Beispiele unter `assets/examples` und die
   DemoProject-Shader werden dabei HLSL-only.

Die Schritte 1 bis 5 sollten vor den Aufträgen 3, 7 und 8 liegen, sonst werden deren
Änderungen noch doppelt geschrieben. Schritt 6 kann parallel zu Auftrag 3 laufen, weil
Bloom und Tonemap dort ohnehin angefasst werden.

## 6. Offene Fragen an dich

1. **Laufzeit-Übersetzung im Kern (4.1 A):** Darf der Shader-Compiler in `src/renderer`
   den Weg HLSL → GLSL bekommen? Das ist die Kernänderung, die die Empfehlung braucht.
   Ohne sie bleibt nur Variante B mit vielen erzeugten Dateien.
2. **Namensregel im OpenGL-Backend (4.2):** Einverstanden mit `krom_t<N>_s<M>` statt der
   bisherigen Namensliste, oder lieber `GL_ARB_shading_language_420pack` voraussetzen?
3. **SPIRV-Cross als DLL zur Laufzeit (4.5):** Einverstanden, oder soll es ein externes
   Programm wie `dxc.exe` bleiben, mit der Folge, dass die Namen nachträglich im Text
   ersetzt werden müssten?
4. **Profil umbenennen (4.7):** Ja oder nein?
5. **Editor-Anzeige für Funktionen ohne OpenGL (4.4):** Ausgegraut mit Tooltip, oder
   lieber ganz ausblenden?
