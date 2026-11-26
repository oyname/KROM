# Plan: Surface Cache GI (dynamische diffuse GI ohne Bake)

Stand: 2026-11-26, Paket 3 (Radiance-Cache) abgenommen auf `feature/gi-cache`; Paket 0 (Vorprüfung) auf `feature/gi-vorpruefung`, abgezweigt von
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
| 1 BVH auf der CPU | erledigt (2026-11-25), siehe unten |
| 2 Trace-Kern | erledigt und im Editor abgenommen (2026-11-25, DX11 und Vulkan); Empfehlung: Paket 3 bauen |
| 3 Radiance-Cache | erledigt und im Editor abgenommen (2026-11-26, DX11 und Vulkan); Empfehlung: Paket 4 bauen |
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

### Paket 1 (2026-11-25, `feature/gi-bvh`, abgezweigt von `feature/gi-vorpruefung`)

Neu:

- `include/collision/MeshBvh.hpp`, `src/collision/MeshBvh.cpp` (in `engine_core`): BLAS pro
  Submesh, Binned SAH mit 12 Bins, Blätter mit höchstens 4 Dreiecken, Dreiecke in
  Blattreihenfolge. `BvhNode` 32 Byte (min, `a`, max, `count`; `count == 0` = innerer Knoten,
  rechtes Kind = `a + 1`), `BvhTriangle` 48 Byte (drei Positionen, im freien Wort hinter `v0`
  der Index des Quelldreiecks). Traversierung `BvhTraverseClosest`/`BvhTraverseAny` über
  gepackte Arrays mit Offsets – genau die Form, die Paket 2 in HLSL braucht.
- `include/collision/SceneBvh.hpp`, `src/collision/SceneBvh.cpp` (in `engine_core`): TLAS,
  Instanztabelle `GpuBvhInstance` (144 Byte), BLAS-Pool, Änderungsliste, Versionsnummern,
  `TraceClosest`/`TraceAny`. Kein ECS.
- `addons/mesh_renderer/MeshSceneBvh.{hpp,cpp}`: sammelt die Instanzen aus dem ECS, BLAS-Cache
  pro (Mesh-Handle, Submesh, `geometryRevision`), Aufbau über das Job-System.
- `tests/test_gi.cpp`: 12 neue Tests (Suite „GI“), `krom_tests --gi` führt nur diese Suite aus.
- `SceneQueries::Raycast` ist unverändert; die exakte Variante ist `SceneBvh::TraceClosest`.
- Kein Schreiben in `.kmesh`. Niemand ruft `MeshSceneBvh::Update` bisher auf: ohne GI kostet
  das nichts.

Entscheidungen:

- **Ort:** BLAS/TLAS/Traversierung im Kern (`include/collision`), wie vorgeschlagen. Das
  Einsammeln aus dem ECS liegt in `addons/mesh_renderer`, weil `MeshComponent` dort definiert
  ist (Befund 6) – wie `MeshSceneQueries`.
- **TLAS-Blätter mit genau einer Instanz** (`a` = Instanzindex, `count = 1`). Spart auf der GPU
  eine Indirektion, TLAS hat immer `2N - 1` Knoten.
- **Instanz-AABB** = BLAS-Wurzelbox durch die Welt-Matrix (Arvo), pro Submesh. Enger als
  `BoundsComponent` (die gilt für das ganze Mesh). Änderungen erkennt die TLAS über
  `TransformComponent::worldVersion` (wie Befund 9): gleiche Version = unverändert ohne
  Matrixvergleich; neue Version = Matrix vergleichen (nur echte Bewegung zählt). Ohne
  `TransformComponent` wird jedes Update die Matrix verglichen.
- **Schwellen** als Konstanten in `SceneBvh`: Refit bis 10 % bewegte Instanzen
  (`kRefitMaxMovedFraction`), Neubau wenn die SAH-Kosten nach Refit 1,3-mal so hoch wie nach dem
  letzten Neubau sind (`kRefitMaxSahGrowth`) oder nach 240 Refits in Folge (`kRefitMaxCount`).
  Kommen oder gehen Instanzen, oder wechselt die BLAS einer Instanz: immer Neubau.
- **Ruhend-Bit** (`kSceneBvhInstanceResting`) nach 30 unbewegten Updates (`kRestingFrames`),
  automatisch, keine Komponente, keine Serialisierung (Befund 10). Wechselt das Bit, steigt
  `Version()` einmal (die Instanztabelle hat sich geändert). Eine neue Szene ist deshalb nach
  30 Frames einmal „geändert“, danach still.
- **Versionen:** `Version()` steigt bei jeder Änderung an TLAS oder Instanztabelle (Bewegung,
  Kommen/Gehen, Ruhend-Bit, Material-Kennung). `BlasPoolVersion()` steigt nur, wenn die
  gepackten BLAS-Arrays sich ändern (neues Mesh, Verdichtung).
- **BLAS-Pool:** jede BLAS liegt einmal im gemeinsamen Array, egal wie viele Instanzen sie
  nutzen. Wird eine BLAS nicht mehr gebraucht, bleibt ihr Bereich zunächst als Loch liegen;
  ist mehr als die Hälfte ungenutzt (`kBlasPoolMaxWaste`), wird verdichtet (beide Versionen
  steigen). Abweichung vom Auftrag Paket 2 („Löcher dürfen liegen bleiben“): Verdichten ist
  schon drin, weil es auf der CPU fast nichts kostet.
- **Änderungsliste:** `Moved` (Transform oder Geometrie der Instanz geändert), `Added`,
  `Removed`, jeweils mit alter und neuer Welt-AABB (ungültig = `min > max`).
- **Traversierung:** ein gemeinsamer Stack mit 32 Einträgen (`kBvhTraversalStackSize`) für
  TLAS und BLAS: die BLAS-Schleife eines Blatts nutzt die Einträge oberhalb der TLAS-Einträge.
  Näheres Kind zuerst. Ist der Stack voll, wird der fernere Teilbaum übersprungen und
  `stackOverflows` gezählt (kein undefiniertes Verhalten). In allen Tests: 0 Überläufe,
  höchste Tiefe 12.
- **Dreiecke:** beidseitig (kein Backface-Culling). Entartete Dreiecke (Fläche 0, NaN/Inf,
  Index außerhalb) werden beim Aufbau verworfen und gezählt; Treffer melden den Index im
  Quell-Indexbuffer. Die Treffer-Normale ist die geometrische Welt-Normale in
  Wicklungsrichtung (nicht zum Strahl gedreht), aus den in den Weltraum transformierten
  Ecken – stimmt auch bei Spiegelung.
- **Ausgeschlossen:** Entities mit `SkinComponent` oder Meshes mit `HasSkinning()`, inaktive
  und unsichtbare Meshes (gleiche Regel wie die Mesh-Extraktion), Assets, die nicht `Loaded`
  sind.
- **Job-System:** BLAS werden mit `JobPriority::Background` gebaut, auf Kopien der Positionen
  und Indizes (das Asset darf sich derweil ändern). Fertige Ergebnisse holt `Update()` ab; bis
  dahin fehlt die Instanz. Ohne Job-System wird sofort gebaut. Keine Device-Aufrufe in Jobs.
  Unbenutzte BLAS fliegen nach 600 Updates aus dem Cache (`kBlasEvictAfterUpdates`).

Messwerte (`krom_tests --gi`, x64-Release = RelWithDebInfo `/O2`, AMD Ryzen 5 2600, ein
Kern; Debug ist etwa zehnmal langsamer):

| Messung | Wert |
|---|---|
| BLAS Drache (`assets/Dragon/drache.glb`) | 592 Dreiecke, 401 Knoten, 201 Blätter, Tiefe 11, **0,39 ms** |
| BLAS dichte Kugel 120 × 240 (Ersatz, weil der Drache sehr grob ist) | 57 352 Dreiecke (248 entartete an den Polen verworfen), 34 475 Knoten, Tiefe 19, **41,8 ms** |
| CPU-Strahlen/s Drache | nächster Treffer **3,6 Mio.**, Any-Hit 4,6 Mio.; 8,4 Knoten und 3,5 Dreieckstests pro Strahl |
| CPU-Strahlen/s Kugel | nächster Treffer **2,2 Mio.**, Any-Hit 2,6 Mio.; 16,1 Knoten und 3,9 Dreieckstests pro Strahl |
| TLAS 1 000 Instanzen | erster Aufbau 0,65 ms, Neubau **0,38 ms**, Refit bei 1 % bewegt **0,05 ms** (ganzes Update 0,07 ms), unveränderte Szene 0,02 ms |
| TLAS 10 000 Instanzen | erster Aufbau 8,0 ms, Neubau **5,3 ms**, Refit bei 1 % bewegt **0,25 ms** (ganzes Update 0,73 ms), unveränderte Szene 0,26 ms |
| Höchste Stack-Tiefe | 12 von 32, keine Überläufe |

Einordnung: Refit und „nichts geändert“ sind pro Frame vernachlässigbar. Ein TLAS-Neubau bei
10 000 Instanzen (5 ms) fällt nur an, wenn Instanzen kommen/gehen oder mehr als 10 % sich
bewegen; das ist ein einzelner Ruckler, kein Dauerzustand. Falls das in Paket 2/3 stört: den
Neubau in einen Job verlagern (die alte TLAS bleibt bis dahin gültig) oder ein billigeres
Verfahren (Morton-Sortierung) für die TLAS nehmen. Der Aufbau liegt bei etwa 2 Mio.
Instanzen bzw. 1,4 Mio. Dreiecken pro Sekunde und Kern.

Prüfung (2026-11-25):

- `krom_tests` x64-Debug: „ALL TESTS PASSED“, 23 Suiten, davon „GI“ 14/14 (2 aus Paket 0,
  12 neu). `krom_tests --gi` auch in x64-Release grün.
- Gesamter x64-Debug-Build (Editor DX11 und Vulkan, Runtimes, Beispiele) ohne Fehler; nur
  bekannte Warnungen in Dateien, die dieses Paket nicht anfasst.
- Editor-Start: `krom_editor_loading_tests_dx11` und `krom_editor_loading_tests` (Vulkan)
  laufen durch („editor loop completed without crash“, Exit 0).
- Nicht geprüft: Editor von Hand gestartet; da noch niemand `MeshSceneBvh` aufruft, ändert
  sich am laufenden Editor nichts.

Offen für Paket 2:

- Wer `MeshSceneBvh::Update` aufruft (das GI-Addon, nach `TransformSystem`/`BoundsSystem`)
  und welches Job-System es bekommt (Erinnerung: der Job-Pool gehört den Hosts).
- Material-Kennung ist der `MaterialHandle`-Wert so, wie die Extraktion ihn wählt; Registry-
  oder Runtime-Raum wird erst in Paket 3 (Albedo pro Material) relevant.
- Upload: `Version()` für TLAS-Knoten + Instanzen, `BlasPoolVersion()` für BLAS-Knoten +
  Dreiecke.

### Paket 2 (2026-11-25, `feature/gi-trace`, abgezweigt von `feature/gi-bvh`)

Neu:

- `shaders/surface_cache_trace.hlsl`: Traversierung als HLSL-Baustein, `TraceClosest` und
  `TraceAny`, Zeile für Zeile nach `SceneBvh`/`BvhTraverse*` (Paket 1). Ein Stack mit
  32 Einträgen für TLAS und BLAS, Überlauf wird gezählt. **Eigene Datei statt eingebettetem
  String**, weil Paket 3 sie wieder einbindet und der WARP-Test sie direkt nutzt; eingebettete
  Compute-Shader dürfen schon heute aus `shaders/` einbinden (`ForwardPlusLighting.cpp:52`).
- `addons/surface_cache/` (`engine_surface_cache`): `SurfaceCacheTypes.hpp`,
  `SurfaceCachePass.{hpp,cpp}` (Compute-Kernel, Overlay, Passes), `SurfaceCacheFeature.{hpp,cpp}`
  (Extraktion, Upload, Messmodus, GPU/CPU-Prüfung).
- Editor: Einstellungen → Viewport, unter der Probe-Debug-Ansicht: „Surface Cache GI (Test)“,
  Debug-Ansicht (Normale, Distanz, Farbe pro Instanz, besuchte Knoten), Überblenden,
  Knöpfe „Messen“ und „GPU gegen CPU prüfen“, Statuszeile. Nicht gespeichert, Standard aus.
  `KromEditorAppConfig::enableSurfaceCache` (in `editor_main.cpp` an = registriert, aber aus).
  Texte in `locales/de.json` und `en.json`.
- `krom_editor_loading_tests[_dx11] --gi-bench <Projekt> [Bildordner]`: automatischer Messlauf
  (siehe unten).
- `SceneBvh::GeometryVersion()` (Paket-1-Code): zählt nur echte Geometrieänderungen; das
  Ruhend-Bit hatte die GPU/CPU-Prüfung sonst als „Szene geändert“ verworfen.

Aufbau:

- Extraktion (Hauptthread, nur Ansichten mit `enableDepthPrepass`, also Szene und Spiel,
  keine Vorschauen): `MeshSceneBvh` pro Welt, Upload nur bei geänderten Versionen
  (`BlasPoolVersion` → BLAS-Knoten und Dreiecke; `Version` → TLAS und Instanzen). Puffer wachsen
  mit 50 % Reserve; bei unbewegter Szene kein Upload pro Frame (Statuszeile „Uploads“).
- Passes: Compute in `BeforeOpaque` (Debug-Trace der Ansicht, im Messmodus 14 Mess-Passes,
  ggf. Prüfung), Overlay in `AfterPostProcess` auf das fertige Bild (wie die Editor-Gizmos).
  Ergebnisse in Structured Buffern (Befund 2: Vulkan kann keine UAV-Texturen); die
  Compute-Passes schreiben eine Kette 1×1-Platzhalter, die das Overlay liest, sonst würde der
  RenderGraph sie wegkürzen.
- Bindungen ohne Änderung am Bindungsmodell: Buffer-SRV 0–3 (Szene), 4 (Overlay), UAV 0–3,
  `PerFrame` b0 (Kamera), `PerPass` b3 (Dispatch-Konstanten, eigener Ring aus 96 Puffern).
- Aus: Extraktion gibt alle Shader, Pipelines und Puffer frei, keine Passes im Graph
  (`GetStructureRevision` 0). Es bleibt ein Frame-Daten-Eintrag pro Ansicht.

Prüfung GPU gegen CPU:

- `krom_tests --gi`: Trace-Kernel auf WARP gegen `SceneBvh` (512 Strahlen, Drehung,
  ungleichmäßige und gespiegelte Skalierung, gekürzte Strahlen): 0 Abweichungen bei nächstem
  Treffer und Any-Hit. Shader übersetzen über FXC und DXC.
- Echte GPU (GTX 1660), DX11 und Vulkan: 256 Kamerastrahlen, 0 Abweichungen, größte
  Distanzdifferenz 0,00003 m; nach Bewegen, Drehen, Skalieren einer Instanz, Entfernen eines
  Meshes und Hinzufügen eines Prefabs erneut 0 Abweichungen (Upload folgt der Änderung).
- Deckungsgleichheit: Bilder des Viewports mit 0 %, 50 % und 100 % Überblendung (Normale,
  Instanzfarbe, Knoten, Distanz) für Hubschrauber- und Drachen-Szene angesehen; die Ansicht
  liegt pixelgenau auf dem Bild.

Messwerte (GTX 1660, x64-Release, Editor mit 1920×1080-Fenster, je 200 Frames nach 30 Frames
Anlauf, GPU-Zeit des RenderGraph-Passes über `GpuPassProfiler`). Strahlen pro Frame bei
Sekundärstrahlen = Primärtreffer (nur dort startet ein Strahl). Szene „car“: DemoProject,
42 Instanzen, 108 645 Dreiecke. Szene „Drachen“: 64 Drachen-Prefabs im 26-m-Raster, übrige
Meshes ausgeblendet, 256 Instanzen (4 Submeshes je Drache), 423 424 Dreiecke.

Strahlen pro Millisekunde, 1920×1080:

| Szene, Backend | primär | diffus ∞ | diffus 15 m | diffus 5 m | Verdeckung ∞ | 15 m | 5 m |
|---|---|---|---|---|---|---|---|
| car, DX11 | 1 420 k | 384 k | 519 k | 554 k | 451 k | 585 k | 689 k |
| car, Vulkan | 1 708 k | 523 k | 599 k | 693 k | 577 k | 743 k | 986 k |
| Drachen, DX11 | 609 k | 87 k | 128 k | 136 k | 99 k | 136 k | 146 k |
| Drachen, Vulkan | 1 018 k | 143 k | 206 k | 239 k | 161 k | 224 k | 254 k |

Dazu GPU-Zeit in ms (1920×1080 / 960×540):

| Szene, Backend | primär | diffus ∞ | diffus 15 m | diffus 5 m | Verdeckung ∞ | 15 m | 5 m |
|---|---|---|---|---|---|---|---|
| car, DX11 (767 k / 192 k Sek.-Strahlen) | 1,46 / 0,54 | 2,00 / 0,68 | 1,48 / 0,71 | 1,38 / 0,69 | 1,70 / 0,56 | 1,31 / 0,66 | 1,11 / 0,46 |
| car, Vulkan | 1,21 / 0,58 | 1,47 / 0,52 | 1,28 / 0,52 | 1,11 / 0,63 | 1,33 / 0,41 | 1,03 / 0,48 | 0,78 / 0,40 |
| Drachen, DX11 (324 k / 81 k Sek.-Strahlen) | 3,41 / 1,19 | 3,73 / 1,09 | 2,52 / 0,84 | 2,38 / 0,72 | 3,27 / 1,06 | 2,38 / 0,91 | 2,22 / 0,72 |
| Drachen, Vulkan | 2,04 / 0,69 | 2,27 / 0,71 | 1,57 / 0,51 | 1,36 / 0,46 | 2,01 / 0,62 | 1,45 / 0,47 | 1,27 / 0,42 |

Höchste Stack-Tiefe 16 (car) bzw. 15 (Drachen) von 32, keine Überläufe. Trefferquote der
Primärstrahlen: car 37 %, Drachen 15,6 % (viel Himmel); diffuse Strahlen treffen in car 7,4 %
(offener Himmel über dem Boden), bei den Drachen 15,6 %.

Hinweise zur Genauigkeit: Die Halbauflösung hat einen festen Sockel von etwa 0,3–0,4 ms pro
Pass (Barrieren und Leerlauf zwischen abhängigen Dispatches); ihre Strahlen/ms sind deshalb
zu niedrig und für die Hochrechnung nicht geeignet. Die Werte sind Mittel der geglätteten
Profiler-Zeiten; ein zweiter Lauf schwankt um einige Prozent (car Vulkan primär: 1,02 ms im
Debug-Lauf mit Validierung, 1,21 ms in Release mit Bildaufnahmen davor).

Einschätzung: Wie viele diffuse Strahlen passen in 2 ms auf der 1660?

- Offene Szene wie „car“: Vulkan 1,0–1,4 Mio. (unbegrenzt bis 5 m), DX11 0,8–1,1 Mio.
- Dichte Szene wie „Drachen“ (viele sich überlappende Instanz-Boxen): Vulkan 0,3–0,5 Mio.,
  DX11 0,2–0,3 Mio.
- Für den Cache heißt das: ein festes Budget von 128 k–256 k diffusen Strahlen pro Frame
  passt auch im ungünstigen Fall (DX11, dichte Szene) in etwa 1–2 ms; 1 Strahl pro Pixel in
  halber Auflösung (≈ 520 k) nur unter Vulkan in offenen Szenen. Kürzen auf 15 m spart 25–35 %,
  auf 5 m 30–45 %. Vulkan ist durchgehend 1,3–1,7-mal schneller als DX11 (DXC gegenüber FXC).
- **Empfehlung: Paket 3 bauen**, mit einem Strahlenbudget pro Frame (Start: 128 k, Regler)
  statt fester Strahlen pro Pixel, Strahllänge 15 m mit Probe/IBL-Rest.

Wirksamste Maßnahmen, falls nötig (nicht umgesetzt):

1. **Eine BLAS pro Mesh statt pro Submesh** (Materialkennung pro Dreieck): Der Drache besteht
   aus 4 Submeshes mit fast gleichen Boxen, also 4 überlappende TLAS-Blätter pro Objekt; das
   verdoppelt ungefähr die Arbeit pro Sekundärstrahl in dichten Szenen.
2. **Breitere Knoten (BVH4) mit komprimierten Boxen**: weniger Stack-Arbeit und
   Speicherzugriffe pro Strahl; typischer Gewinn 1,5–2× bei inkohärenten Strahlen.
3. **Kürzere Strahlen** (schon gemessen: 15 m spart 25–35 %) und Kohärenz durch Sortieren der
   Cache-Strahlen nach Richtung.

Offen / Auffälligkeiten:

- Behoben (gleicher Branch, `src/renderer/FrameExecutionStage.cpp`): Die Vulkan-Validierung
  meldete `VUID-vkQueueSubmit-pSignalSemaphores-00067`. Erzeuger einer Queue-Abhängigkeit
  signalisierten jeden Frame ihr Semaphor (`exportsQueueSignal`), obwohl auf dem Fence-Weg
  niemand darauf wartet. Jetzt wird nur noch signalisiert, wenn eine andere Submission wartet.
  Der GI-Messlauf mit Prefab-Instanziierung läuft unter Validierung ohne Meldung.
- Behoben nach der Abnahme im Editor: `MeshSceneBvh` nahm die Editor-Gizmos (Licht- und
  Kamerasymbole, Transform-Pfeile) mit in die BVH. Sie skalieren jeden Frame mit dem
  Kameraabstand, dadurch änderte sich die BVH ständig (Statuszeile: Version und Uploads stiegen
  jeden Frame). Jetzt sind alle Editor-Layer ausgeschlossen (`MeshSceneBvh::kExcludedLayers`);
  der Messlauf prüft, dass eine ruhende Szene keinen Upload auslöst. Die GPU-Zeiten oben sind
  davon nicht betroffen (Uploads laufen außerhalb der gemessenen Passes).
- Upload unter Vulkan geht über `ImmediateSubmit` mit `vkQueueWaitIdle`: bei jeder Bewegung
  ein kurzer Stillstand. Bei unbewegter Szene kein Upload. Für Paket 3 Upload über
  Staging-Ring im Frame erwägen.
- Freie Bereiche im BLAS-Pool werden ab 50 % Verschnitt verdichtet (Paket 1).
- Die Runtime (`KromWrapper`) registriert das Addon nicht; nur der Editor.
- Messlauf neu ausführen:
  `krom_editor_loading_tests_dx11.exe --gi-bench <abs. Pfad>\krom-project.json [Bildordner]`,
  `KROM_PERF_SCENE=<Szene>`, `KROM_GI_BENCH_DRAGONS=64`, `KROM_GI_BENCH_SPACING=26`.

### Paket 3 (2026-11-25, `feature/gi-cache`, abgezweigt von `feature/gi-trace`)

Neu:

- `shaders/surface_cache_common.hlsl`: PerFrame-Block (gleiches Layout wie `pbr_lit.ps.hlsl`),
  Dispatch-Konstanten `SurfaceCacheParams` (b3), Hash-Grid (Schlüssel, Suche, Stufe nach
  Kameradistanz) und das Fernfeld `ScFarFieldIrradiance` (lokale Probes per SH aus t19, sonst
  IBL-Irradiance aus t5, ohne IBL die Umgebungsfarbe × π).
- `shaders/surface_cache_update.hlsl`: die Kernel des Caches, je einer pro `SC_KERNEL_*`
  (Begin, Register, Trace, Resolve, Age, Material), eingebettet über
  `kCacheKernelHlsl` (`SurfaceCachePass.cpp`).
- `shaders/shadow_csm_lookup.hlsl`: Kaskadenwahl und Ausblendung (`ChooseCascadeIndexFromViewDepth`,
  `KromShadowDistanceFade`), aus `pbr_lit.ps.hlsl` und `pbr_forward_plus.ps.hlsl` herausgezogen
  (beide Fassungen waren zeichengleich, beide binden jetzt die Datei ein). Mit
  `KROM_SHADOW_CSM_LOAD` zusätzlich `KromCsmVisibilityLoad`: Sonnenschatten ohne Ableitungen
  und ohne Sampler (Shadow Map per `Load`, ein bilinearer Vergleich, fester Zusatz-Bias von
  1,5 Texel Normalen-Versatz und doppeltem Tiefen-Bias).
- `ShaderBindingModel.hpp`: Sampler-Range auch für Compute sichtbar (Befund 11, eine Zeile).
- `addons/surface_cache`: Hash-Grid pro Welt (`SurfaceCacheGrid`), Materialtabelle
  (`SurfaceCacheMaterialTable`), drei neue Passes, Zähler-Readback, Messlauf für den Cache,
  `RequestSurfaceCacheReset`, `SurfaceCacheLastCounters`.
- Editor (Einstellungen → Viewport, unter „Surface Cache GI (Test)“): Debug-Ansichten
  „Cache: Zellen“, „Cache: Irradiance“, „Cache: Samples“; Regler Strahlen pro Frame
  (Tausend, Start 128), Strahllänge (15 m), Zellgröße (0,5 m), Verlauf (256 Samples); Knopf
  „Cache leeren“; zweite Statuszeile mit belegten/sichtbaren Zellen, Strahlen, Änderung pro
  Frame, Anmeldungen ohne Platz. Nicht gespeichert.
- Tests: `krom_tests --gi` zwei Furnace-Tests auf WARP (siehe Prüfung), Kernel-Übersetzung für
  FXC und DXC; `--gi-bench` misst zusätzlich den Cache, vergleicht Kaskaden mit
  Verdeckungsstrahlen und prüft, dass Ausschalten alles freigibt.

Ablauf pro Frame (Phase `AfterDepthPrepass`, drei RenderGraph-Passes, danach wie bisher
Debug-Trace, Messpasses, Prüfung):

1. `SurfaceCacheRegister`: neue oder geänderte Materialien mitteln (höchstens 64 pro Frame),
   Zähler sichern und leeren, dann je 4 × 4 Pixel ein Kamerastrahl auf ein pro Frame
   wechselndes Pixel. Der Treffer meldet seine Zelle an (`InterlockedCompareExchange` auf den
   Schlüssel, `InterlockedExchange` auf den Frame); wer die Zelle als Erster in diesem Frame
   sieht, hängt sie an die Liste der sichtbaren Zellen und schreibt Position und Normale. Neue
   Zellen starten mit dem Fernfeld (Probe/IBL, Befund 13) und dem Gewicht 4.
2. `SurfaceCacheRayTrace`: `min(Budget, sichtbare Zellen × 32)` Kosinus-Strahlen aus den
   Zellen, gekürzt auf 15 m. Gibt es mehr Zellen als Strahlen, kommt jedes Frame ein anderer
   Teil der Zellen dran. Am Treffer: `Emission + Albedo / π × (Direkt + Cache der Trefferzelle)`;
   Direkt = jede gerichtete Lichtquelle (Kaskaden, außerhalb ein Verdeckungsstrahl) plus eine
   zufällig gewählte lokale Lichtquelle mit Verdeckungsstrahl (gewichtet mit der Anzahl);
   fehlt die Trefferzelle, das Fernfeld. Fehlschuss oder Ende bei 15 m: Fernfeld in
   Strahlrichtung / π. Summen als Festkomma-Atomics pro Zelle.
3. `SurfaceCacheResolve`: pro sichtbarer Zelle Mittelwert der Strahlen dieses Frames ×π,
   gleitend eingemischt mit Gewicht `n / min(Gewicht + n, Verlauf)`. Danach „Age“ über alle
   Plätze: Zellen ohne Anmeldung seit 300 Frames werden frei; Zellen in geänderten Bereichen
   (Änderungsliste aus Paket 1, alte und neue Box, vergrößert) fallen auf Gewicht 2 zurück und
   schwingen schnell neu ein. Alle 16 Frames (im Messlauf alle 4) werden die Zähler kopiert.

Entscheidungen und Abweichungen:

- **Phase `AfterDepthPrepass` statt `BeforeOpaque`.** Die Passes von `BeforeOpaque` stehen im
  Rezept vor dem Shadow-Pass (`StandardFramePipeline.hpp`, `BuildRecipe`); ein Lesen der Shadow
  Map dort ergab einen Zyklus im RenderGraph. In `AfterDepthPrepass` ist sie geschrieben (wie in
  Befund 5 empfohlen). Ein Depth-Prepass wird dadurch nicht erzwungen (keine Opaque-Eingabe).
- **Der Cache speichert nur indirekte Irradiance.** Das direkte Licht einer Zelle rechnet in
  Paket 4 weiterhin der PBR-Shader. Deshalb ist die Irradiance-Ansicht in offenen Szenen fast
  gleichförmig (überwiegend schwacher Himmel; car: IBL-Intensität 0,05) und an Wänden neben
  besonntem Boden heller. Mehrfachreflexion entsteht über die Cache-Abfrage am Treffpunkt
  (Wert des Vorframes).
- **Anmelden über eigene Primärstrahlen** in 1/16 der Auflösung statt über die Tiefe (wie
  vorgeschlagen): exakte Normale, kein Prepass nötig.
- **Lichter aus `PerFrame.lights[7]`**, nicht aus dem LightBuffer (der existiert nur mit
  Forward+). Mehr als 7 Lichter fehlen im Cache.
- **Schlüssel:** Startplatz aus einem Hash, gespeichert wird ein zweiter 32-Bit-Hash (nie 0);
  Kollisionen zweier Zellen mit gleichem Startplatz und gleichem Zweithash werden in Kauf
  genommen. Normale auf 6 Achsenrichtungen quantisiert. 2^18 Plätze, 16 Sondierschritte,
  15 MB pro Welt (Schlüssel, zwei uint4, Frame, Akkumulator, Liste). Freigegebene Plätze
  unterbrechen Sondierketten; dahinter liegende Zellen werden neu angemeldet (Startwert) oder
  altern aus. Bisher in keinem Lauf eine Anmeldung ohne Platz.
- **Zellgröße:** 0,5 m bis 8 m Kameraabstand, danach Verdopplung je Verdopplung des
  Abstands (Stufe im Schlüssel). Die Demo-Szenen haben damit nur 570–890 sichtbare Zellen;
  ein Budget von 128 k wird nie erreicht (höchstens 32 Strahlen pro Zelle und Frame).
- **Materialtabelle:** Die hochgeladene Instanztabelle trägt statt des Material-Handles den
  Index in die Tabelle (`GpuBvhInstance::materialId`, nur in der GPU-Kopie);
  `albedo`/`emission` in `GpuBvhInstance` bleiben ungenutzt. Die Werte kommen aus dem
  `MaterialSystem` des Frames (Runtime-Handles wie in der Extraktion, Erinnerung
  „Material-Handle-Räume“): `baseColorFactor` × Mittel der Albedo-Textur, `emissiveFactor` ×
  Mittel der Emissions-Textur (Mip mit höchstens 8 × 8 Texeln per `Load`). Abweichung von
  Befund 7: nicht über `MaterialAsset::WithGraphTextures()` (Asset-Seite, anderer
  Handle-Raum); Graph-Materialien bekommen damit nur Faktor und ggf. Albedo-Slot.
  Neuberechnung bei neuer Revision.
- **Verlauf über ein Sample-Gewicht** (Standard 256, Regler), nicht über Frames: gleich
  starkes Rauschen unabhängig vom Budget. 256 gibt ein Rauschniveau von 1,2–2,9 %
  Änderung pro Frame bei Einschwingen in etwa 8 Frames; 1024 senkt es auf 0,7 % bei etwa
  30 Frames (Tabelle unten).
- Nicht umgesetzt: eine BLAS pro Mesh (das Budget passt, Paket 2) und Upload über einen
  Staging-Ring. Änderungsboxen gehen über einen `FrameRingBuffer`.
- Messlauf: `--gi-bench` speichert jetzt die Szenen und `editor.settings` des Projekts vor dem
  Lauf und schreibt sie danach zurück. Vorher hat der Editor beim Szenenwechsel und beim
  Schließen die Testbearbeitungen (Drachen, verschobene und gelöschte Meshes) in `car.json`
  bzw. die geöffnete Szene gespeichert.

Prüfung:

- Furnace auf WARP (`krom_tests --gi`): geschlossene Box, Albedo 0,5, Emission 1, erwartet
  E = 2π = 6,2832 in jeder Zelle. 16 Strahlen pro Zelle: 768 Zellen, Mittel 6,271, Spanne
  6,149–6,279, 60 Frames vorher 6,261 (noch leicht steigend, die Kamera dreht durch sechs
  Richtungen). 256 Strahlen für alle 768 Zellen (Verlauf 8): Mittel 6,280, Spanne
  6,188–6,280.
- Kaskaden gegen Verdeckungsstrahlen (Messlauf, Sonnenschatten am Treffpunkt): mittlere
  Irradiance gleich bis auf 0,0–0,9 %; besonnter Anteil der beleuchteten Treffer car
  0,73/0,72, Room 0 0,76/0,74, 64 Drachen aus 140 m Abstand 0,82/0,75 (grobe ferne Kaskaden
  verlieren feine Selbstverschattung, wie im PBR-Shader).
- Ausschalten gibt alles frei (Messlauf: keine Welt, keine Programme, kein Puffer).
- Bildvergleich DX11/Vulkan (Zellen, Irradiance, Samples): gleich.
- `krom_tests` x64-Debug: „ALL TESTS PASSED“, Suite „GI“ 18/18. Gesamter x64-Debug-Build ohne
  Fehler (nur bekannte Warnungen in nicht berührten Zeilen). `krom_editor_loading_tests_dx11`
  und `krom_editor_loading_tests` (Vulkan, `KROM_VALIDATION=1`): „editor loop completed without
  crash“; `--gi-bench` unter Vulkan-Validierung ohne Meldung. Nach allen Messläufen keine
  Änderung unter `projects/`.
- Im Editor vom Nutzer abgenommen (2026-11-26, Release-Editor). In der Samples-Ansicht zeigen sich beim Bewegen kurz frische Zellen (Wechsel der Zellstufe, neu sichtbare Flächen); sie sind nach etwa 8 Frames voll.

Messwerte (GTX 1660, x64-Release, Editor 1920×1080, GPU-Zeit der RenderGraph-Passes, Mittel
über 200 Frames nach 30 Frames Anlauf; Budget 128 k, Strahllänge 15 m, Verlauf 256):

| Szene, Backend | Register | Trace | Resolve + Age | Summe | sichtbare / belegte Zellen | Strahlen | Rauschen | eingeschwungen |
|---|---|---|---|---|---|---|---|---|
| car, DX11 | 0,33 | 0,40 | 0,02 | **0,76 ms** | 571 / 702 | 18 272 | 1,16 % | Frame 8 |
| car, Vulkan | 0,40 | 0,25 | 0,02 | **0,67 ms** | 571 / 702 | 18 272 | 1,18 % | Frame 8 |
| 64 Drachen, DX11 | 0,23 | 0,61 | 0,03 | **0,87 ms** | 834 / 1 422 | 26 688 | 1,46 % | Frame 12 |
| 64 Drachen, Vulkan | 0,29 | 0,34 | 0,02 | **0,65 ms** | 834 / 1 422 | 26 688 | 1,46 % | Frame 12 |
| Room 0, DX11 | 0,19 | 0,51 | 0,02 | **0,73 ms** | 892 / 1 017 | 28 544 | 2,89 % | Frame 8 |
| Room 0, Vulkan | 0,28 | 0,26 | 0,02 | **0,56 ms** | 893 / 1 017 | 28 576 | 2,88 % | Frame 8 |
| Room 0, DX11, Verlauf 1024 | 0,19 | 0,50 | 0,02 | 0,71 ms | 892 / 1 017 | 28 544 | 0,74 % | Frame 32 |
| Room 0, Vulkan, Verlauf 1024 | 0,28 | 0,26 | 0,02 | 0,56 ms | 893 / 1 017 | 28 576 | 0,74 % | Frame 28 |

Rauschen = Median der mittleren relativen Änderung pro Frame in der zweiten Hälfte des Laufs;
eingeschwungen = erster Frame nach dem Leeren, ab dem die Änderung unter dem 1,5-Fachen davon
bleibt (Zähler alle 4 Frames gelesen). Register enthält ca. 125 000 Primärstrahlen
(1920 × 1040 / 16) und die Materialmittelung (nur am Anfang).

Einordnung: Mit 0,6–0,9 ms liegt der Cache gut unter den 2 ms aus Paket 2. Der Trace-Pass ist
für 18–28 k Strahlen teuer (DX11 0,4–0,6 ms): er startet immer `Budget / 64` Gruppen, von denen
die meisten sofort enden, und jeder Treffer kostet Sonnen- und Cache-Abfrage. Die
Anmeldung ist bei 1/16 der Pixel ein fester Sockel von 0,2–0,4 ms.

Offen / für Paket 4 und 5:

- Zellen außerhalb des Bildes werden nicht aufgefrischt; Strahlen, die dort treffen, nehmen
  den letzten Wert oder das Fernfeld. Abhilfe in Paket 5: Zellen auch an Treffpunkten anmelden
  (wie SHaRC), mit eigener Liste für den nächsten Frame.
- Dispatch-Größe des Trace-Passes an die gelesene Zellzahl anpassen (spart die leeren
  Gruppen), Anmeldung bei Bedarf in 1/64 der Pixel.
- Zellgröße/Stufen-Abstand für die Demo-Szenen abstimmen (wenige, große Zellen in der Ferne).
- Neue Zelle beim Wechsel der Zellstufe mit dem Wert der alten Stufe starten statt mit Probe/IBL (falls frische Zellen nach Paket 4 sichtbar flackern).
- Mehr als 7 Lichter, Punktlicht-Schatten über die Shadow Map (heute nur Verdeckungsstrahl).
- Graph-Materialien mit prozeduraler Basisfarbe: nur der Faktor (Befund 7).
- Kollisionen des 32-Bit-Zweithashs sind möglich, aber nicht gezählt.
- Runtime (`KromWrapper`) registriert das Addon weiterhin nicht.
- Messlauf neu ausführen wie Paket 2; zusätzlich `KROM_GI_BUDGET_K=<Tausend>` und
  `KROM_GI_HISTORY=<Samples>`.
