# Plan: Surface Cache GI (dynamische diffuse GI ohne Bake)

Stand: 2026-11-25, Paket 0 (Vorprüfung) auf `feature/gi-vorpruefung`, abgezweigt von
`feature/multi-select-editing` (`ebcd932`). Jede Sitzung der GI-Reihe liest diese Datei zuerst
und trägt am Ende ein, was erledigt ist, was offen bleibt und was vom Plan abweicht.

## Ziel

Dynamische diffuse Global Illumination für **DX11 und Vulkan**, Zielkarte GTX 1660, ohne
Hardware-Raytracing: Strahlen per Compute-Shader gegen eine eigene BVH, Ergebnis in einem
Radiance-Cache im Weltraum (Spatial Hash über Position und Normale), Resolve in halber
Auflösung als eine Textur in den Opaque-Pass (wie GTAO und Kontaktschatten). Specular bleibt
bei Probes und SSR, GTAO bleibt für die Feinverdeckung.

Pakete: 0 Vorprüfung (diese Datei), 1 BVH auf der CPU, 2 Trace-Kern auf der GPU mit
Debug-Ansicht und Messung (Ausstiegspunkt), 3 Radiance-Cache, 4 Resolve und Anbindung,
5 Abstimmung.

## Abweichungen vom Auftrag (mit dem Nutzer abgestimmt am 2026-11-25)

- **OpenGL gibt es nicht mehr.** Der Auftrag spricht von „OpenGL eingefroren, muss weiter
  bauen“. Das Backend ist seit PR #49 (`bd15d0f`, „Remove the OpenGL backend“) aus `main`
  entfernt. Alle OpenGL-Punkte entfallen; geprüft wird nur DX11 und Vulkan.
- **Kein Eintrag in `KOORDINATION.md`.** Die Datei wird seit 2026-11-22 nicht mehr gepflegt
  (nur noch Claude Code, kein Codex-Review). Ergebnisse, Messwerte und Entscheidungen stehen
  hier unter „Stand der Pakete“.
- Branch-Basis ist `feature/multi-select-editing` statt `main` (Wunsch des Nutzers). Die
  fremde Änderung an `projects/DemoProject/Assets/Scenes/car.json` bleibt unangetastet.

## Kurzfassung

| Frage | Ergebnis | Folge |
|---|---|---|
| 1 Atomics | `InterlockedCompareExchange` auf `RWStructuredBuffer<uint>` geht über FXC (DX11, läuft auf WARP korrekt) und DXC (Vulkan, `OpAtomicCompareExchange` im SPIR-V). Test `tests/test_gi.cpp`. | Keine Ausweichlösung nötig. |
| 2 Bindungen Compute | 4 lesende Buffer + bis 3 UAV-**Buffer** passen in die vorhandenen Slots. **Aber:** Vulkan kann keine UAV-Texturen (`SetUnorderedAccess(TextureHandle)` ist ein No-op). | Paket 2 schreibt die Debug-Ausgabe in einen `RWStructuredBuffer`, ein Fullscreen-Pixel-Shader zeigt ihn an. Kein Umbau am Bindungsmodell. |
| 3 Textur-Slot | Neuer `TexSlots::GlobalIllumination = 21`, `COUNT = 22`. Passt in die 24 reservierten Vulkan-Bindings vor `SamplerBase = 40`. | Erst in Paket 4. |
| 4 Normalen | Der Prepass schreibt nur Tiefe. Echte Normalen gibt es nur im Opaque-MRT von SSR (nach Opaque). | Für den Cache-Schlüssel reicht die rekonstruierte Normale (grob quantisiert). Keine Prepass-Ausgabe jetzt. |
| 5 Ort im Frame | Shadow → Sky → DepthPrepass → `AfterDepthPrepass` (GTAO, Kontaktschatten) → Opaque. Dauerhafte Buffer hält das Addon selbst (Vorbild Auto-Exposure), der RenderGraph kennt keine importierten Buffer. | Cache-Update und Resolve in `AfterDepthPrepass`. |
| 6 Mesh-Daten | Positionen und Indizes bleiben nach dem Upload auf der CPU. Entity → `MeshComponent` → `MeshAsset::submeshes`, Welt-Matrix aus `WorldTransformComponent`, geskinnt = `SkinComponent` oder `MeshAsset::HasSkinning()`. | `MeshComponent` liegt im Addon `mesh_renderer`, nicht im Kern (siehe Paket 1, Ort). |
| 7 Materialwerte | `MaterialAsset::WithGraphTextures()` liefert die Graph-Näherung schon heute (für Custom-Shader). | Mittelwert per kleinem GPU-Pass aus dem kleinsten Mip (BC-Formate!), siehe unten. |
| 8 Addon-Muster | 11 Stellen außerhalb des Addons, Liste unten. | Paket 2. |
| 9 Änderungen | Pro Entity `TransformComponent::worldVersion`, global `World::StructureVersion()`. Keine fertige Änderungsliste. | TLAS merkt sich pro Instanz die zuletzt gesehene `worldVersion` und Mesh-Handle; Vergleich O(N) pro Frame. |
| 10 Statisch | Es gibt kein Kennzeichen. | Automatisch (N Frames unbewegt), wie vom Nutzer bevorzugt. |
| 11 Schatten/Lichter | Shadow Map und `PerFrame`-CB sind im Compute lesbar; die CSM-Daten liegen im `PerFrame`-CB, nicht in `ShadowLights`/`ShadowViews`. **Sampler** sind unter Vulkan nur im Fragment-Stage sichtbar, und die Schattenfunktionen hängen an `ddx`/`ddy`. | Für Paket 3: Sampler-Range auch für Compute freigeben (1 Zeile) und die CSM-Abfrage in eine eigene Include-Datei ohne Ableitungen ziehen. |
| 12 Letzter Frame | Keine Kopie des letzten Frames, keine Motion Vectors, keine alte View-Projection. `GTAOComposite` ist eine Kopie **desselben** Frames. | Das GI-Addon muss Kopie und alte Matrix selbst halten (Paket 5). |
| 13 Probes/IBL | Lokale Probes: Irradiance als SH über `Load` aus `t19` – im Compute ohne Sampler lesbar. IBL-Irradiance ist eine Cubemap und braucht einen Sampler. | Mit der Sampler-Freigabe aus Frage 11 beides lesbar (Paket 3). |

**Empfehlung:** Paket 1 und Paket 2 können **ohne Änderung am Bindungsmodell** starten, mit
einer Auflage für Paket 2: Ausgaben von Compute-Passes nur als `RWStructuredBuffer`, nicht als
`RWTexture2D` (Vulkan-Backend unterstützt keine Storage-Images). Änderungen am Bindungsmodell
werden erst in Paket 3 (Sampler für Compute) und Paket 4 (Textur-Slot 21) nötig, beide klein.

## Befunde im Detail

### 1. Shader-Sprache und Atomics

- Übersetzungspfade: `src/renderer/ShaderCompilerShared.cpp:895-925` (`CompileBackendArtifact`).
  - DX11: `CompileToD3DBytecode` (`src/renderer/ShaderCompilerD3D.cpp:205`), Profil `cs_5_0`
    (`ShaderCompilerD3D.cpp:171`), FXC über `D3DCompile`.
  - Vulkan: HLSL geht **nicht** über shaderc/glslangValidator, sondern über
    `dxc.exe -spirv` (`ShaderCompilerShared.cpp:907-908`,
    `src/renderer/ShaderCompilerDXC.cpp:167-276`, Profil `cs_6_0`, `-fvk-use-dx-layout`,
    Register-Verschiebungen aus `BindingRegisterRanges`). shaderc/glslang ist nur der Pfad für
    GLSL-Quellen (`ShaderCompilerSPIRV.cpp`). Der Kommentar in `ShaderCompilerDXC.cpp:162-165`
    nennt veraltete Verschiebungen (SMP 32); maßgeblich ist der Code, der
    `BindingRegisterRanges` liest (`ShaderCompilerDXC.cpp:229-245`).
- Test: `tests/test_gi.cpp`, Suite „GI“ in `krom_tests`.
  - Shader: offene Adressierung mit linearem Sondieren, 256 Threads fügen 37 verschiedene
    Schlüssel über `InterlockedCompareExchange` ein und zählen mit `InterlockedAdd`.
  - DX11: übersetzt mit FXC, **ausgeführt auf WARP**; jeder Schlüssel steht genau einmal in
    der Tabelle, die Zähler stimmen (7 bzw. 6 pro Schlüssel, Summe 256).
  - Vulkan: übersetzt mit DXC; das SPIR-V enthält `OpAtomicCompareExchange` (Opcode 230) und
    `OpAtomicIAdd` (234). Ohne `dxc.exe` wird der Teil übersprungen (wie die vorhandenen
    Shader-Tests).
  - Nicht geprüft: Ausführung des SPIR-V auf einer echten Vulkan-GPU (der Testlauf hat kein
    Vulkan-Device). Das holt Paket 2 mit dem GPU-gegen-CPU-Vergleich nach.
- Ergebnis Testlauf: siehe „Stand der Pakete“.
- Ausweichlösung nicht nötig. Falls ein Treiber Probleme macht: Einfügen in zwei Schritten
  (`InterlockedMax` auf einen Schlüssel-Buffer, dann Prüfen) oder Einfügen auf der CPU aus der
  Readback-Liste des letzten Frames.

### 2. Bindungen im Compute-Pass

- Layout: `BuildEnginePipelineBindingLayout` (`include/renderer/ShaderBindingModel.hpp:796-815`).
  Ein einziges Descriptor-Set für alle Pipelines; Range 5 (UAV, 8 Slots, Compute) und Range 6
  (Buffer-SRV, 16 Slots, Vertex|Fragment|Compute).
- Die fachlichen Namen in `BufSRVSlots` (9-15) sind nur Konvention der Szenen-Passes. In einem
  reinen Compute-Pass ist jeder Slot frei. Vorschlag für Paket 2: Buffer-SRV 0..3 für
  TLAS-Knoten, BLAS-Knoten, Dreiecke, Instanzen; UAV 0..2 für Ausgaben.
- **DX11:** Buffer-SRV und Textur-SRV teilen sich den `t`-Registerraum
  (`addons/dx11/DX11CommandList.cpp:303,316`, beide `CSSetShaderResources(slot)`). Liest ein
  Pass zusätzlich Texturen (Tiefe, Shadow Map), dürfen sie nicht auf `t0..t3` liegen – z. B.
  Tiefe auf `PassSRV0` (t8), Shadow Map auf `ShadowMap` (t4).
- **Vulkan:** Buffer-SRVs liegen auf Binding `64 + slot` und müssen im Shader mit
  `KROM_BUFFER_SRV(type, name, slot)` aus `shaders/krom_platform.hlsl:8` deklariert werden
  (sonst landet `register(tN)` auf dem Textur-Binding `16 + N` mit falschem Typ). Vorbild:
  `addons/forward/ForwardPlusLighting.cpp:52,70`.
- **Vulkan, UAV:** Range 5 wird als `VK_DESCRIPTOR_TYPE_STORAGE_BUFFER` angelegt
  (`addons/vulkan/VulkanDevice.cpp:64`, `VulkanCommandList.cpp:84`) und nur mit Buffern
  beschrieben (`VulkanCommandList.cpp:1593-1610`). `SetUnorderedAccess(slot, TextureHandle)`
  ist ein No-op (`VulkanCommandList.cpp:1348-1353`). Unter DX11 geht beides
  (`DX11CommandList.cpp:322-346`). **Folge:** Compute-Passes können unter Vulkan keine Textur
  beschreiben. Für Paket 2 (Debug-Ansicht) und Paket 4 (Resolve) heißt das: Ergebnis als
  `RWStructuredBuffer`, Darstellung/Weitergabe über einen Fullscreen-Pixel-Shader, der den
  Buffer liest (Buffer-SRVs sind im Fragment-Stage sichtbar), oder den Resolve gleich als
  Fullscreen-Pixel-Pass bauen (so machen es GTAO und Kontaktschatten).
- Storage-Images in Vulkan nachzurüsten hieße: eigene Binding-Range für UAV-Texturen
  (ein Binding kann nicht zugleich `STORAGE_BUFFER` und `STORAGE_IMAGE` sein), Layout-Übergang
  nach `GENERAL`, Fallback-Image. Das ist eine Änderung am Bindungsmodell und am
  DescriptorSetLayout; für dieses Vorhaben nicht nötig.
- Barrieren: Vulkan setzt nach jedem `Dispatch` eine globale Speicherbarriere
  Compute → Compute|Fragment (`VulkanCommandList.cpp:1742-1764`); DX11 braucht keine.
- DescriptorSetLayout: unverändert, solange keine Slots dazukommen. Alle Buffer-SRV- und
  UAV-Bindings sind `PARTIALLY_BOUND` (`VulkanDevice.cpp:713-722`), ungebundene Slots stören
  also nicht.

### 3. Textur-Slot für das GI-Ergebnis (Paket 4)

- `TexSlots` endet bei `ContactShadow = 20`, `COUNT = 21`
  (`include/renderer/ShaderBindingModel.hpp:94-96`). t14/t15 sind in Forward+ durch
  `ShadowLights`/`ShadowViews` belegt (DX11 teilt den `t`-Raum), t16/t17 durch den
  Materialgraph, t18/t19 durch die Probes. **Richtig ist `GlobalIllumination = 21`,
  `COUNT = 22`.**
- Vulkan: Textur-Bindings liegen auf `16 + slot`; `SamplerBase = 40` lässt Platz für
  24 Texturen (`ShaderBindingModel.hpp:162-163`). Slot 21 → Binding 37, passt. Für Slot 23
  wäre Schluss, danach müsste `SamplerBase` wandern.
- Zu ändern (Vorlage Kontaktschatten):
  1. `include/renderer/ShaderBindingModel.hpp`: Slot und `COUNT`. Alle Arrays und Schleifen
     über `TexSlots::COUNT` wachsen mit (`addons/dx11/DX11CommandList.cpp:92-95`,
     `addons/vulkan/VulkanCommandList.cpp:173,610,657,1414,1509`,
     `src/renderer/ShaderCompilerShared.cpp:210`, `src/renderer/ShaderRuntimeMaterials.cpp:634`).
  2. Addon: `ctx.recipe.opaqueInputs.push_back({ "<Name>", TexSlots::GlobalIllumination })`
     (Vorbild `addons/contact_shadow/ContactShadowPass.cpp:412`).
  3. Ersatztextur: macht `ForwardFeature` schon generisch. Fehlt die Ressource, wird die
     Weiß-Textur gebunden (`addons/forward/ForwardFeature.cpp:1486-1499`), im Transparent-Pass
     immer Weiß (`ForwardFeature.cpp:1675-1682`). Vulkan ersetzt ungebundene 2D-Slots durch
     seine 2D-Ersatztextur (`VulkanCommandList.cpp:114-128,657-661`). **Weiß ist für
     Irradiance falsch**, also braucht der Shader ein Freigabe-Bit im Frame-Parameterblock
     (wie `lightIndexPlusOne` beim Kontaktschatten, `ContactShadowFeature.cpp:56-61`), sonst
     die heutige Probe/IBL-Irradiance.
  4. Shader: `shaders/pbr_lit.ps.hlsl:16` und `shaders/pbr_forward_plus.ps.hlsl:38`
     (`Texture2D ... : register(t21)`), diffuse Irradiance dort ersetzen.
  5. Depth-Prepass: läuft nur, wenn ein `AfterDepthPrepass`-Contributor eine Opaque-Eingabe
     anmeldet (`ForwardFeature.cpp:2132`). Mit der GI-Opaque-Eingabe ist das automatisch so.
- OpenGL: entfällt (siehe Abweichungen).

### 4. Tiefe und Normalen

- Der Depth-Prepass ist ein reiner Tiefen-Pass: die PBR-Shader haben in der
  `KROM_DEPTH_PREPASS`-Variante `void main(PSInput IN)` ohne Farbausgabe
  (`shaders/pbr_lit.ps.hlsl:633-637`), der Pass ist `depthOnly`
  (`addons/forward/StandardFramePipeline.hpp:609-617`).
- Echte Normalen gibt es nur als MRT-Ausgabe des **Opaque**-Passes, wenn SSR an ist
  (`SsrNormalRoughness`, `shaders/pbr_ssr_output.hlsl`, `addons/ssr/SsrPass.cpp:630-641`).
  Das liegt nach dem Opaque-Pass, also zu spät für einen Resolve vor Opaque, wäre aber für
  den Cache-Update des **nächsten** Frames nutzbar.
- GTAO rekonstruiert die Normale aus der Tiefe (`addons/gtao/GtaoPass.cpp:176`,
  `ReconstructNormal`), mit Kantenbehandlung über kleinere Tiefendifferenz.
- Bewertung: Der Cache-Schlüssel braucht nur eine grob quantisierte Normale (z. B. 6 Richtungen
  oder Oktaeder mit 3 bis 4 Bit pro Achse). Dafür reicht die rekonstruierte Normale. Fehler
  entstehen an Silhouetten und auf Normal-Maps; beides ist für diffuse GI unkritisch, weil der
  Resolve ohnehin über mehrere Pixel filtert. **Empfehlung: keine Normalen-Ausgabe im
  Prepass**, vorerst rekonstruieren wie GTAO.
- Falls sich Paket 4 als zu unruhig erweist: Normalen-Ausgabe im Prepass (eine RG16-Oktaeder-
  Textur). Kosten: ein zusätzliches Ziel im Prepass, ~4 MB bei 1080p, Bandbreite ~8 MB/Frame;
  die Prepass-Variante muss Normal-Maps auswerten (Pixel-Shader wird teurer, bisher
  `void main`). Betroffen: `shaders/pbr_lit.ps.hlsl`, `shaders/pbr_forward_plus.ps.hlsl`,
  `shaders/lit.ps.hlsl`, `shaders/quad_unlit.ps.hlsl` (alle mit `KROM_DEPTH_PREPASS`),
  `addons/forward/StandardFramePipeline.hpp` (Prepass mit Farbziel statt `depthOnly`),
  `addons/forward/ForwardFeature.cpp` (Pipeline-Formate des Prepass),
  `src/renderer/ShaderCompiler.cpp` (ggf. neue Variante), Materialgraph-Shader.

### 5. Ort im Frame und im RenderGraph

- Reihenfolge (`addons/forward/StandardFramePipeline.hpp:300-340` und `588-619`): Shadow →
  Sky → DepthPrepass → Passes der Phase `AfterDepthPrepass` → Opaque → Scene-Outputs (SSR-MRT)
  → `BeforeTransparent` → Transparent/Partikel/Debug → `AfterOpaque` → Postprocess.
- GTAO hängt in `AfterDepthPrepass` (`addons/gtao/GtaoFeature.cpp:51`) und meldet sein
  Ergebnis als Opaque-Eingabe auf `PassSRV0` an (`addons/gtao/GtaoPass.cpp:560`); sein
  Composite liegt in `BeforeTransparent` (`GtaoFeature.cpp:52`). Kontaktschatten:
  `AfterDepthPrepass` (`addons/contact_shadow/ContactShadowPass.hpp:66`), Eingabe auf t20.
- `src/renderer/FrameGraphStage.cpp` sammelt nur die Contributors und bildet den
  Topologie-Schlüssel (`FrameGraphStage.cpp:52-54,120`); die Phasen ordnet `ForwardFeature`
  (`addons/forward/ForwardFeature.cpp:2106-2118`).
- Neue Passes: Cache-Update (Compute) und Resolve (Fullscreen-PS, halbe Auflösung) beide in
  `AfterDepthPrepass`. Die Shadow Map ist dann geschrieben (Shadow-Pass steht vorne). Die
  Reihenfolge innerhalb einer Phase folgt der Registrierung der Features; GI braucht nichts
  von GTAO oder Kontaktschatten.
- Dauerhafte Ressourcen:
  - Der RenderGraph kennt `ImportRenderTarget`, `ImportTexture`, `ImportHistoryPair`
    (Texturen/RTs mit Ping-Pong) (`include/rendergraph/RenderGraph.hpp:189-233`), aber **keinen
    Import für Buffer**.
  - Vorbild für dauerhafte Buffer ist die Auto-Exposure: Das Addon legt die Buffer selbst über
    `IDevice` an, hält sie über Frames und bindet sie im Executor direkt
    (`addons/forward/ForwardFeature.cpp:467-621,1036-1049`). Der RenderGraph sieht sie nicht;
    Barrieren sind dank der Dispatch-Barriere (Frage 2) unter Vulkan abgedeckt, DX11 braucht
    keine. **So soll es der Cache auch machen.**
  - Uploads mit `IDevice::UploadBufferData` (`include/renderer/IDevice.hpp:159`). Ein Buffer,
    den die GPU im vorigen Frame noch liest, darf nicht ohne Weiteres überschrieben werden
    (siehe Erinnerung FrameRingBuffer, 2026-11-18). Für TLAS/Instanzen in Paket 2 prüfen.

### 6. Mesh-Daten für die BVH

- `SubMeshData` (`include/assets/AssetRegistry.hpp:45-61`): `positions` (3 float/Vertex),
  `indices` (uint32), `materialIndex`, optional `boneWeights`/`boneIndices`.
- Die CPU-Daten bleiben nach dem GPU-Upload erhalten. Der `.kmesh`-Lader füllt neben
  `rawInterleavedBytes` auch `positions` usw. (`src/assets/KMeshSerializer.cpp:398-420`), der
  Upload liest nur (`src/renderer/GpuResourceRuntime.cpp:1132-1144`), nirgends werden
  `positions` gelöscht. Auch `MeshSceneQueries` verlässt sich darauf
  (`addons/mesh_renderer/MeshSceneQueries.hpp:43-46`).
- Weg von der Entity:
  - `MeshComponent::mesh` (`addons/mesh_renderer/MeshRendererComponents.hpp:12-25`) →
    `AssetRegistry::meshes.Get(handle)` → `MeshAsset::submeshes` (`AssetRegistry.hpp:63-66`),
    nur wenn `state == AssetState::Loaded`.
  - Welt-Matrix: `WorldTransformComponent::matrix` und `inverse`
    (`include/ecs/Components.hpp:56-63`).
  - Welt-AABB: `BoundsComponent::centerWorld/extentsWorld` (`Components.hpp:111-120`).
  - Material: `MaterialComponent::FindSlotOverride(submesh)` bzw. `material`, sonst
    `MeshAsset::materialHandles[sub.materialIndex]`. Achtung: Handles liegen mal im
    Registry-, mal im Runtime-Raum (Erinnerung „Material-Handle-Räume“); für Paket 1 reicht
    eine Kennung pro Submesh, Paket 3 löst sie wie die Extraktion auf
    (`addons/mesh_renderer/MeshRendererExtraction.*`).
  - Sichtbarkeit: `MeshComponent::visible` (und vererbte Aktivierung).
- Geskinnt: `SkinComponent` an der Entity (`addons/animation/AnimationComponents.hpp:25`)
  oder `MeshAsset::HasSkinning()` (`AssetRegistry.hpp:75-81`). Beides prüfen; das zweite geht
  ohne Abhängigkeit vom Animations-Addon.
- **Folge für Paket 1:** `MeshComponent` liegt im Addon `mesh_renderer`, nicht im Kern. Der
  Kern (`include/collision`) darf ihn nicht kennen. Die BVH selbst (BLAS, TLAS, Traversierung)
  gehört in den Kern; das Einsammeln der Instanzen aus dem ECS gehört nach
  `addons/mesh_renderer` (neben `MeshSceneQueries`).

### 7. Materialwerte am Treffpunkt (nur Vorschlag)

- Am Treffpunkt reicht eine mittlere Albedo und Emission pro Material-Slot.
- Grundlage gibt es schon: `MaterialAsset::WithGraphTextures()`
  (`include/assets/AssetRegistry.hpp:343-353`) löst einen verlinkten Graph auf seine Texturen
  auf – genau die Näherung, die Custom-Shader heute bekommen. `RuntimeEmissive()`
  (`AssetRegistry.hpp:335-340`) liefert die Emission.
- Vorschlag: albedo = `baseColorFactor` × Mittelwert der Basisfarb-Textur, emission =
  `RuntimeEmissive()` × Mittelwert der Emissions-Textur.
- Mittelwert **auf der GPU** bestimmen, nicht aus `TextureAsset::pixelData`: die Texturen
  liegen oft BC1/BC3/BC7-komprimiert vor (`AssetRegistry.hpp:104-112`), BC7 auf der CPU zu
  dekodieren lohnt nicht. Ein winziger Compute-Pass liest pro Material den kleinsten Mip mit
  `Load` (kein Sampler nötig) und schreibt in die Materialtabelle. Nur bei neuem Material oder
  geänderter Material-Revision.
- Vermutung, nicht geprüft: Prozedurale Graph-Knoten ohne Textur (Rauschen, Verläufe) fallen
  dabei auf den Basisfarb-Faktor zurück. Für diffuse GI akzeptabel.

### 8. Addon-Muster (Vorlage GTAO / Kontaktschatten)

Neues Addon `addons/surface_cache`:

1. Dateien: `CMakeLists.txt`, `SurfaceCacheFeature.{hpp,cpp}`, `SurfaceCachePass.{hpp,cpp}`
   (wie `addons/gtao`, `addons/contact_shadow`). Bibliothek `engine_surface_cache`, Abhängigkeiten
   wie `addons/gtao/CMakeLists.txt` (engine_core, engine_rendergraph_layer,
   engine_renderer_runtime_layer, engine_shader_layer, engine_forward) plus
   `engine_mesh_renderer` für die Instanzen.
2. Root-`CMakeLists.txt`: `add_subdirectory` (Kontaktschatten: Zeile 412), Linken in den
   Editor-/Runtime-Zielen (455, 508, 671), Include-Pfad (692), `krom_tests` (922).
3. Feature: `IEngineFeature` mit `RegisterPassContributor` und ggf.
   `RegisterFrameParameterBlock` + `RegisterFrameConstantsContributor`
   (`addons/contact_shadow/ContactShadowFeature.cpp:85-90`); `Create...Feature(&resources)`
   gibt die geteilten Einstellungen an den Host.
4. Registrierung: Editor `app/KromEditorApp.cpp:1919` (Schalter `enableContactShadows` in
   `KromEditorApp.hpp:65`), Runtime `src/app/KromWrapper.cpp:912` (Schalter in
   `include/krom.h:160`).
5. Projekt-Einstellungen (`rendering`-Block): `include/core/ProjectSettings.hpp:51-55`
   (Felder), `229-234` (Lesen mit Grenzen), `396-400` (Schreiben).
6. Runtime-Anwendung: `src/app/KromRenderSettings.cpp:84-91` (`KromRenderSettings`),
   Editor `app/KromEditorApp.cpp:6223-6230`.
7. Editor-Oberfläche: Projekteinstellungen → Rendering, eigener Abschnitt
   (`addons/editor/EditorUIProject.cpp:1596-1614`), Verfügbarkeit über
   `EditorFrameContext` (`KromEditorApp.cpp:3814`).
8. Script-Schalter (optional): `addons/script/IRenderSettingsService.hpp:62`,
   `include/krom.h:389-391`.
9. Debug-Ansicht: Kontaktschatten nutzt Bits in `debugFlags` (`pbr_lit.ps.hlsl:143-144`) und
   Tasten in `KromEditorApp.cpp:3226-3227`; die Probes ein Feld `debugView` in ihren
   Einstellungen (`addons/probes/ProbeFeature.hpp:42,68`), das über den Frame-Parameterblock
   in den Shader geht. Für die GI-Debug-Ansicht (Paket 2) passt das Probe-Muster.
10. Nicht verfügbar: früher OpenGL-Prüfung; heute über `IDevice::GetComputeRuntime()`
    (`computePipelinesSupported`, Vorbild `ForwardFeature.cpp:564-566`).

### 9. Änderungsverfolgung

- `TransformComponent` hat `dirty`, `localVersion`, `worldVersion`, `parentWorldVersion`
  (`include/ecs/Components.hpp:31-34`). `TransformSystem::Update` erhöht `worldVersion`, wenn
  die Entity selbst oder ein Elternteil sich geändert hat (`src/scene/TransformSystem.cpp:41-49`).
  `UpdateCount()` zählt die geänderten Transforms des letzten Updates – aller Entities, auch
  Kamera und Lichter, also als „hat sich etwas bewegt“-Signal zu grob.
- `BoundsComponent` merkt sich `lastTransformVersion` und `localDirty`
  (`Components.hpp:111-120`, `src/scene/BoundsSystem.cpp:41-52`).
- `World::StructureVersion()` steigt bei jedem `CreateEntity`, `DestroyEntity`, `Add`,
  `Remove` (`include/ecs/World.hpp:85,117`, `src/ecs/World.cpp:57,86,131`) – auch für
  Komponenten, die nichts mit Meshes zu tun haben.
- Es gibt **keine fertige Änderungsliste** und keine alte AABB. Ableitbar ist sie aber billig:
  Die TLAS hält pro Instanz (Entity + Submesh) die zuletzt gesehene `worldVersion`, das
  Mesh-Handle, `geometryRevision` des Meshes und die alte Welt-AABB. Pro Frame:
  - `StructureVersion` unverändert und keine `worldVersion` anders → nichts zu tun. Der
    Vergleich ist ein Lauf über die Instanzen (O(N), bei 10 000 Instanzen einige µs).
  - Bei geänderter `StructureVersion` zusätzlich Abgleich der Entity-Menge (neu/entfernt).
  - Mesh-Tausch an einer vorhandenen Entity ändert die `StructureVersion` **nicht**, deshalb
    auch das Mesh-Handle vergleichen.
- Damit sind (a) „überhaupt etwas geändert“ und (b) „welche Instanzen, alte und neue AABB“
  ohne Eingriff in Transform- oder Bounds-System lieferbar.

### 10. Statisch oder beweglich

- Kein Kennzeichen vorhanden: weder an `MeshComponent`
  (`addons/mesh_renderer/MeshRendererComponents.hpp:12-25`) noch an Entities (Suche nach
  `isStatic`/`StaticComponent` ohne Treffer).
- Automatisch (N Frames unbewegt = ruhend): kein Eingriff in Datenmodell, Serialisierung,
  Editor; funktioniert auch für Objekte, die Scripts gelegentlich bewegen. Nachteile: am Anfang
  ist alles „bewegt“ (N Frames Anlauf), und ein Objekt, das nach langer Ruhe einmal bewegt
  wird, kostet einen Neubau/Refit – beides harmlos. Bewertung: **passt**, so umsetzen.
- Ausdrückliches Kennzeichen: würde Editor-Feld, Szenen-Serialisierung, Prefab-Overrides und
  Script-API berühren, schützt aber nicht vor Fehlbedienung (statisch markiert und trotzdem
  bewegt). Nur sinnvoll, falls Paket 5 eine getrennte BVH für ruhende Instanzen braucht und
  der Automatismus dort flattert.

### 11. Schatten und Lichter im Compute-Pass

- Wo die Daten liegen:
  - CSM-Daten (`shadowViewProjArray`, `shadowViewRect`, `shadowLightMeta`, Kaskaden)
    stehen im `PerFrame`-CB `b0` (`shaders/pbr_lit.ps.hlsl:31-60`), nicht in den
    Buffer-SRVs. `ShadowLights`/`ShadowViews` (BufSRV 14/15) sind die Forward+-Variante.
  - Lichter: `lights[7]` im `PerFrame`-CB, für Forward+ zusätzlich `LightBuffer` (BufSRV 10),
    den schon der Forward+-Culling-Compute-Shader liest
    (`addons/forward/ForwardPlusLighting.cpp:70`).
  - Die Shadow Map ist eine Textur auf t4, nach dem Shadow-Pass im Zustand
    Tiefe-Lesen; Vulkan-Layout `DEPTH_STENCIL_READ_ONLY_OPTIMAL` ist zum Lesen im Shader
    gültig.
- Bindbarkeit: `PerFrame`- und `PerPass`-CB sind für Compute sichtbar
  (`ShaderBindingModel.hpp:800,803`), Texturen auch (`:805`), Buffer-SRVs auch (`:808`).
  **Sampler nicht:** Range 7 hat nur `ShaderStageMask::Fragment` (`:810`). Unter DX11 bindet
  `SetSampler(..., Compute)` trotzdem (`DX11CommandList.cpp:355`), unter Vulkan fehlt
  `VK_SHADER_STAGE_COMPUTE_BIT` am Sampler-Binding (`VulkanDevice.cpp:700-708`) – ein
  Compute-Shader mit Sampler wäre ein Validierungsfehler.
- Die Schattenfunktionen sind nicht für Compute geschrieben: `KromShadowDepthGradient` nutzt
  `ddx`/`ddy` (`shaders/shadow_receiver.hlsl:2-13`, in `cs_5_0` nicht erlaubt), die
  CSM-Auswahl und Filter stehen **im** PBR-Shader (`shaders/pbr_lit.ps.hlsl:356-572`), nicht in
  einer Include-Datei, und brauchen `sPoint` bzw. `shadowSampler`.
- Für Paket 3 nötig (klein, beides kein Umbau):
  1. Sampler-Range auch für Compute sichtbar machen (`ShaderBindingModel.hpp:810`,
     `| ShaderStageMask::Compute`). Das ändert den Hash des Layouts, sonst nichts.
     Alternative ohne Änderung: Shadow Map mit `Load` statt `SampleLevel` lesen (bilinear von
     Hand, wie `CompareShadowManualBilinear` es ohnehin tut).
  2. Kaskadenwahl + einfacher bilinearer Vergleich in eine eigene Datei
     (`shaders/shadow_csm_lookup.hlsl` o. ä.) ziehen, ohne Ableitungen (Gradient = 0, etwas
     mehr konstanter Bias). Die PBR-Shader können sie mitbenutzen; deren Filter (Poisson,
     PCSS, Tent) bleiben, wo sie sind.

### 12. Szenenfarbe des letzten Frames

- `GTAOComposite` (`addons/ssr/SsrPass.hpp:47-50`) ist eine Kopie von `HDRSceneColor`
  **desselben** Frames nach Opaque (für Overlays und Tonemap), eine transiente Ressource des
  RenderGraph. GTAO legt sie an, sonst SSR (`addons/ssr/SsrPass.cpp:642-757`). Ohne GTAO und
  SSR gibt es sie nicht, und über das Frame-Ende hinaus lebt sie nie.
- Keine Motion Vectors, keine View-Projection des letzten Frames (Suche nach
  `prevViewProj`/`previousViewProj` ohne Treffer). `PerFrame` hat nur die aktuelle
  (`pbr_lit.ps.hlsl:33-36`).
- Folge: Für die Bildsuche (Paket 5) muss das GI-Addon selbst eine dauerhafte Kopie der
  Szenenfarbe (halbe Auflösung reicht) und der Tiefe am Ende des Frames anlegen, z. B. über
  `ImportHistoryPair` (Texturen gehen, Frage 5), und die alte View-Projection im eigenen
  Frame-Parameterblock mitführen.

### 13. Startwert aus Probes und IBL

- Lokale Probes: Irradiance steht als SH (9 Koeffizienten) in `tFeatureTexture` (t19) und wird
  mit `Load` gelesen (`shaders/pbr_local_probe.hlsl:43-60`) – kein Sampler, im Compute
  lesbar. Die Cubemap-Array t18 (Specular) braucht `SampleLevel` und einen Sampler.
- Die Probe-Texturen kommen nicht über einen festen Slot, sondern über die Frame-Daten
  `ForwardSceneBindings` (`addons/probes/ProbeFeature.cpp:451-509`); ein Compute-Pass muss sie
  dort abholen und selbst binden. Probe-Anzahl/Flags im Frame-Parameterblock 1
  (`pbr_local_probe.hlsl:17-19`).
- IBL-Irradiance ist eine `TextureCube` auf t5 und braucht einen Sampler. Mit der
  Sampler-Freigabe aus Frage 11 lesbar; ohne sie nicht (für `TextureCube` gibt es kein `Load`).
  Alternative: einmal pro Umgebungswechsel SH aus der Irradiance-Cubemap bestimmen.
- Folge: Probes als Startwert neuer Cache-Zellen gehen sofort, IBL nach der kleinen
  Sampler-Änderung (Paket 3).

## Risiken

| Risiko | Einschätzung | Gegenmittel |
|---|---|---|
| Strahlenbudget auf der 1660 reicht nicht | offen, entscheidet Paket 2 | Ausstiegspunkt nach Paket 2; kürzere Strahlen, breitere Knoten, kompaktere Dreiecke |
| Vulkan ohne UAV-Texturen | sicher (Befund 2) | Ausgaben als Buffer, Resolve als Pixel-Pass |
| DX11: Buffer- und Textur-SRVs teilen `t0..t20` | sicher | Slots pro Pass planen (Befund 2) |
| Uploads in Buffer, die die GPU noch liest (Vulkan) | mittel | Ring/Doppelpuffer für TLAS und Instanzen (Paket 2) |
| BVH-Speicher bei großen Szenen | mittel | Nur BLAS geladener, nicht geskinnter Meshes; Speicher im Messlog ausweisen |
| Rekonstruierte Normale zu unruhig | gering | Normalen-Ausgabe im Prepass (Befund 4) |
| Weiß-Fallback für die GI-Textur | sicher, wenn übersehen | Freigabe-Bit im Parameterblock (Befund 3) |
| Doppelte Abdunklung GI + GTAO | mittel | Paket 5 (GTAO-Stärke anpassen) |
| Materialgraph-Näherung (prozedural) | gering | Mittelwert per GPU, Rückfall Basisfarbe |

## Ort der BVH (für Paket 1)

- `include/collision/MeshBvh.hpp`, `src/collision/MeshBvh.cpp`: BLAS aus Positionen/Indizes,
  ohne ECS.
- `include/collision/SceneBvh.hpp`, `src/collision/SceneBvh.cpp`: TLAS über eine Liste von
  Instanzen (Kennung, BLAS, Matrix, AABB, Version), Änderungsliste, Versionsnummer,
  CPU-Traversierung. Kein Wissen über `MeshComponent`.
- `addons/mesh_renderer/MeshSceneBvh.{hpp,cpp}`: sammelt pro Frame die Instanzen aus dem ECS
  (MeshComponent, WorldTransform, Bounds, Skinning-Ausschluss), verwaltet den BLAS-Cache pro
  Mesh-Asset und die Hintergrund-Jobs. Grund: `MeshComponent` liegt im Addon (Befund 6), der
  Kern darf es nicht kennen; `MeshSceneQueries` liegt aus demselben Grund dort.

## Stand der Pakete

| Paket | Stand |
|---|---|
| 0 Vorprüfung | erledigt (2026-11-25), Test siehe unten |
| 1 BVH auf der CPU | offen |
| 2 Trace-Kern | offen |
| 3 Radiance-Cache | offen |
| 4 Resolve und Anbindung | offen |
| 5 Abstimmung | offen |

### Paket 0 (2026-11-25)

- Neu: `tests/test_gi.cpp` (Suite „GI“), eingetragen in `CMakeLists.txt` (`krom_tests`) und
  `tests/main.cpp`. Kein Engine-Code geändert.
- Testlauf 2026-11-25, `krom_tests` x64-Debug: „ALL TESTS PASSED“, 23 Suiten. Suite „GI“:
  WARP-Lauf 145 Prüfungen grün (jeder der 37 Schlüssel genau einmal, Zähler korrekt), Vulkan
  übersetzt mit DXC, `OpAtomicCompareExchange` und `OpAtomicIAdd` im SPIR-V.
- Nicht geprüft: Ausführung unter Vulkan auf echter GPU (Paket 2), Editor-Start (kein
  Engine-Code geändert).

