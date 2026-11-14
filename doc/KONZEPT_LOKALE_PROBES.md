# Konzept: Lokale Probes (Auftrag 6, Reihe „Bildqualität“)

Stand 2026-11-12, Branch `feature/bildqualitaet`, nach Commit `917bde1` (Auftrag 5).
Nur Konzept, kein Engine-Code geändert. Umsetzung ist Auftrag 7. Prüfszene:
`projects/DemoProject/Assets/Scenes/Room 2.kscene` (früher „Testraum“).

Ziel: Innenräume holen indirektes Licht und Reflexionen aus der eigenen Umgebung statt aus
dem globalen Himmel. Dazu kommt Specular Occlusion. OpenGL ist für diese Reihe eingefroren
(klassisches Forward, kein Forward+, kein GTAO): dort bleibt es beim globalen IBL.

---

## 0. Ausgangslage, am Code geprüft

| Punkt | Befund | Stelle |
|---|---|---|
| Globales IBL | Ein aktiver Eintrag; Irradiance an t5, Prefilter an t6, BRDF-LUT an t7, alle als `TextureCube`/`Texture2D`. | `EnvironmentSystem.cpp`, `ShaderBindingModel.hpp` (`TexSlots`) |
| Baker | Rein CPU, kein `IDevice`. Eingaben nur Equirect-Textur oder prozedurale Himmelsbeschreibung (`BakeFromTexture`, `BakeFromProceduralSky`). Ausgabe `IBLBakedData` RGBA16F: Environment 512, Irradiance 64, Prefilter 128 mit 6 Mips. Parallel über `JobSystem`. | `IBLBaker.hpp/.cpp`, `IBLTypes.hpp` |
| Cache | `.iblcache` mit Magic, `kFileVersion`, `kBakeVersion = 6`, Größen. Schlüssel ist ein Quell-Hash. Ablage `.krom/ibl` (Cache, nicht Projekt-Asset). | `IBLCacheSerializer.hpp`, `EnvironmentSystem.cpp:305` |
| Shader-IBL | `SamplePrefilteredIBL` (LOD aus Roughness + Varianz-Bias), Irradiance über `N`; danach `ambientIBL = (iblDiffuse + iblSpecular) * ao`, dann `*= gtaoFactor`. | `pbr_forward_plus.ps.hlsl:496`, `:589–622` |
| Forward+ | Tile 16×16, Culling im Normalfall auf der CPU. Puffer `LightBuffer` t10, `TileHeaders` t11 (`uint4`), `LightIndices` t12; gebunden vom Forward-AddOn, das die Frame-Daten von Lighting und Shadow per Typ abfragt. | `ForwardFeature.cpp:1133–1190`, `ForwardPlusLighting.cpp` |
| Freie Slots | `TexSlots`: 0–12 und 16/17 belegt, **18 und 19 frei**, `COUNT = 20`. `BufSRVSlots`: 9–15 belegt, `COUNT = 16`; 0–8 sind in HLSL dieselben `t`-Register wie die Texturen. Vulkan: SRV ab Binding 16 (20 Plätze + 4 Reserve bis 40), Buffer-SRV ab 64. | `ShaderBindingModel.hpp` |
| Cubemap-Array | **Gibt es nicht.** `TextureDimension` kennt `Cubemap` und `Tex2DArray`; alle drei Backends setzen bei `Cubemap` fest 6 Ebenen. Vulkan schaltet das Feature `imageCubeArray` nicht ein. | `RendererTypes.hpp:173`, `DX11Resources.cpp:233`, `VulkanDevice.cpp:1497`, `:641` |
| Rücklesen | `IDevice::ReadTexturePixels` liest nur RGBA8/BGRA8, Mip 0. HDR-Rücklesen fehlt. | `IDevice.hpp:173` |
| Parameterblock | 4 Plätze à 16 Byte, GTAO belegt Platz 0. Für Probe-Listen zu klein. | `KROM_ENGINE_ARCHITECTURE.md`, „Add-on parameter blocks“ |
| DX11 | Feature Level 11_1 bis 10_0 angefragt. `TextureCubeArray` braucht 10_1; die GTX 1660 liefert 11_1. | `DX11Device.cpp:19` |
| OpenGL | Kontext 4.1 core, Shader `#version 410`. Cubemap-Arrays sind seit GL 4.0 Kern, also **vorhanden**; wegen des Einfrierens aber nicht genutzt. | `IDevice.hpp:88`, `GLShaderReflector.cpp:154` |
| „Statisch“-Kennzeichen | Kein Feld für statische Objekte gefunden. | — |

Folge: Der Probe-Pfad braucht einige **allgemeine** Kernerweiterungen (Abschnitt 9), aber
kein Probe-Feld im Kern.

---

## 1. Platzierung

**Komponente** `ReflectionProbeComponent` im neuen AddOn `addons/probes`:

| Feld | Bedeutung | Vorgabe |
|---|---|---|
| `extents` (Vec3) | halbe Kantenlängen des Einflussvolumens, lokal zur Entity | 5, 3, 5 |
| `boxOffset` (Vec3) | Mitte des Volumens relativ zur Entity | 0 |
| `blendDistance` (float) | Breite des Überblendbereichs innen am Rand, m | 1,0 |
| `priority` (int) | höher gewinnt bei Überlappung | 0 |
| `intensity` (float) | Multiplikator | 1,0 |
| `boxProjection` (bool) | Parallax-Korrektur an | an |
| `resolution` (uint) | Kantenlänge Prefilter-Mip 0: 64/128/256 | 128 |
| `bakedDataPath` | Pfad der gebackenen Datei (relativ zu Assets) | leer |

Aufnahmepunkt ist die Entity-Position. Die Box dreht sich mit der Entity (OBB): Der Shader
rechnet ohnehin im lokalen Raum der Box, eine Drehung kostet nur eine 3×4-Matrix je Probe.
Skalierung der Entity wird ignoriert, die Größe kommt nur aus `extents` (eindeutig, keine
Verwechslung mit verzerrter Projektion).

**Überlappung:** Sortierung nach `priority` absteigend, bei Gleichstand das kleinere Volumen
zuerst (kleine Probe in großem Raum gewinnt, ohne dass der Nutzer Prioritäten pflegen muss).

**Editor** (nach `feedback_reuse_editor_concepts`: Hierarchie und Inspector, keine eigene UI):
- Hierarchie › Erstellen › „Reflection Probe“ legt Entity + Komponente an.
- Inspector-Karte „Reflection Probe“ mit den Feldern oben, Zustand („gebacken“, „veraltet“,
  „nicht gebacken“) und Knopf „Backen“. Szene-Menü: „Alle Probes backen“.
- Anzeige im Viewport bei Auswahl (DebugDraw): Einflussbox als Drahtgitter, innere Box
  (`extents − blendDistance`) gestrichelt bzw. heller, am Aufnahmepunkt eine kleine
  verspiegelte Kugel, die die gebackene Cubemap zeigt. Nicht ausgewählt: nur ein Symbol.
- Box-Größe per Zahlenfeld; Griffe zum Ziehen der Box sind ein späterer Schritt.

**Ohne Editor:** Komponente aus C++ anlegen wie jede andere; `probes::BakeProbe(...)` und
`probes::BakeAllProbes(...)` sind Engine-Funktionen (Abschnitt 2).

**Speichern:** Die Komponente geht wie andere AddOn-Komponenten über den Scene-Serializer in
`.json`/`.kscene`. Alte Szenen haben keine Probes und laden unverändert.

---

## 2. Backen

### 2.1 Aufnahme der sechs Ansichten

Sechs Ansichten mit 90° Öffnung, quadratisch, vom Aufnahmepunkt aus, über den **normalen
Render-Weg** (`RenderSystem` mit eigener `RenderView` und Offscreen-Ziel RGBA16F, Größe
`2 × resolution`, z. B. 256 für 128er Probe). Für die Aufnahme abgeschaltet: Bloom, Tonemap,
GTAO, MSAA, Partikel, Editor-Overlays und Gizmos, UI. Belichtung neutral (Werte bleiben linear
HDR). Die Ansicht muss den Sky-Pass enthalten, damit Fenster den Himmel zeigen.

Danach werden die sechs Bilder als RGBA16F zur CPU zurückgelesen und als Cube an den Baker
gegeben.

**Weg A: GPU-Aufnahme, CPU-Faltung (empfohlen).** Neuer allgemeiner Einstieg im Kern-Baker
`IBLBaker::BakeFromCubeFaces(faces[6], size, params)`: überspringt Equirect→Cube und rechnet
mit dem vorhandenen Code Irradiance und Prefilter. Gleiche Mathematik wie beim globalen IBL,
also gleiche Optik; alle drei Backends liefern über dieselbe CPU-Rechnung identische Daten.
Kosten: Faltung 128er Prefilter mit 128 Samples dauert pro Probe etwa so lange wie das globale
IBL heute (parallel über `JobSystem`); bei 16 Probes Sekunden bis wenige zehn Sekunden.
Editor-Backen ist ein bewusster Schritt, das ist hinnehmbar.

**Weg B: Faltung per Compute auf der GPU.** Schneller (Millisekunden), aber neuer Code
je Backend, abweichende Ergebnisse zwischen Backends möglich, und OpenGL 4.1 hat kein Compute.
Verworfen für jetzt, später möglich, ohne das Dateiformat zu ändern.

### 2.2 Was beim Backen einfließt

- **Globales IBL und Himmel:** ja. Der Himmel ist durch Öffnungen zu sehen, und Oberflächen im
  Raum bekommen ihr Umgebungslicht im ersten Durchgang aus dem globalen IBL.
- **Andere Probes, mehrere Durchgänge:**
  - Durchgang 1: alle Probes aus, Szene mit globalem IBL. Ergebnis: direktes Licht korrekt,
    indirektes Licht im Raum aber noch „Himmel ohne Decke“ (zu hell, bläulich).
  - Durchgang 2: Probes aus Durchgang 1 aktiv. Die Raumflächen holen ihr Umgebungslicht jetzt
    aus den Raum-Probes; der Himmelsanteil wird durch die zweite Reflexion ersetzt.
  - Weitere Durchgänge ändern wenig.
  - **Empfehlung: 2 Durchgänge als Vorgabe**, einstellbar 1–3 (`bounceCount` beim Aufruf, nicht
    in der Szene). Ein Durchgang lässt genau das Himmelslicht im Raum, das wir loswerden wollen.
- **Spiegelnde Anteile in der Aufnahme:** Spekulares IBL der Oberflächen bleibt in der Aufnahme
  an (sonst wirken Metalle in Reflexionen schwarz). Die aufzunehmende Probe ist in Durchgang 2
  zwar aktiv, das ist gewollt (Mehrfachreflexion), konvergiert aber wegen Roughness/Fresnel.
- **Welche Objekte:** Es gibt kein Kennzeichen „statisch“. Vorschlag: Aufgenommen wird alles,
  was beim Backen sichtbar ist, außer Entities mit Probe-Ausschluss über Render-Layer
  (`RenderLayers.hpp` hat Layer). Offene Frage 3.

### 2.3 Ablage im Projekt

Gebackene Daten sind Projekt-Assets, nicht Cache (sie sollen mit dem Spiel exportiert werden
und sind nicht billig neu zu erzeugen).

**Weg A: eine Datei je Probe (empfohlen):** `Assets/Baked/<Szenenname>/<probeId>.kprobe`.
Einzelne Probes neu backen, ohne den Rest anzufassen; kleine Diffs.
**Weg B: eine Datei je Szene** (`<Szene>.kprobes`). Ein Ladevorgang, aber jedes Neu-Backen
schreibt alles. Verworfen.

Format `.kprobe` (binär, little endian), angelehnt an `.iblcache`:
- Magic `KPRB`, `formatVersion` (Start 1), `bakeVersion` (Algorithmus, wie `kBakeVersion`)
- `inputHash` (64 bit, siehe 2.4), Aufnahmeposition, `extents`, `resolution`, `mipCount`
- Irradiance als SH L2: 9 × RGB float32 (108 Byte), siehe Abschnitt 3.3
- Prefilter-Cube RGBA16F, mip-major wie `IBLBakedData`

`probeId` ist eine stabile ID der Komponente (beim Anlegen vergeben, mit der Szene gespeichert),
nicht die Entity-ID (die ändert sich beim Laden). Szene umbenannt → Ordner bleibt; die
Komponente hält den Pfad (`bakedDataPath`).

### 2.4 „Veraltet“ erkennen

Beim Backen wird ein `inputHash` gespeichert aus:
- Probe: Transform, `extents`, `boxOffset`, `resolution`, `bounceCount`, `bakeVersion`
- Szene: Transforms, Meshes und Material-Hashes aller aufgenommenen Renderables, Lichter
  (Typ, Farbe, Stärke, Transform, Schatten), Environment-Hash (vorhandener Quell-Hash des IBL)

Der Editor rechnet den Hash nach Änderungen (Dirty-Flag aus Undo/Inspector) höchstens alle
0,5 s neu und zeigt „veraltet“ an der Karte und am Symbol im Viewport. Er backt nicht von
selbst.

**Weg A: ein Szenen-Hash für alle Probes (empfohlen für den Start):** einfach, eine Änderung
irgendwo markiert alle Probes. **Weg B:** nur Objekte, deren Bounds die Einflussbox (plus
Rand) schneiden. Genauer, aber falsch bei Licht, das von außen durch ein Fenster fällt. Weg B
später als Verfeinerung.

Laufzeit: Fehlt die Datei oder passt `formatVersion`/`bakeVersion` nicht, wird die Probe
ignoriert (globales IBL), mit Log `ProbeFeature.cpp: ...`. Ein abweichender `inputHash` lädt
trotzdem (Spiel ohne Editor rechnet ihn nicht).

---

## 3. Daten im Shader

### 3.1 Cubemap-Array oder Atlas

**Weg A: `TextureCubeArray` (empfohlen).** Ein Array mit `N × 6` Ebenen, alle Probes gleiche
Auflösung, Hardware-Filterung über Mips und Würfelkanten.
- DX11: `TextureCubeArray` ab Feature Level 10.1 (GTX 1660: 11_1). Bis 2048 Ebenen.
- Vulkan: `VK_IMAGE_VIEW_TYPE_CUBE_ARRAY`, braucht das Device-Feature `imageCubeArray`
  (heute nicht eingeschaltet; praktisch überall vorhanden).
- OpenGL 4.1: `GL_TEXTURE_CUBE_MAP_ARRAY` ist seit 4.0 Kern, also verfügbar. Wegen des
  Einfrierens bekommt OpenGL keine Probes; das Backend sollte die neue Dimension trotzdem
  anlegen können oder sauber „nicht unterstützt“ melden.
- Nachteil: eine Auflösung für alle Probes im Array. Unterschiedliche Auflösungen bräuchten
  mehrere Arrays; Empfehlung: eine Auflösung pro Szene (128), `resolution` an der Probe wird
  dann zur Szeneneinstellung (Offene Frage 2).

**Weg B: Oktaeder-Atlas in einer 2D-Textur.** Läuft überall ohne neue Texturdimension, Probes
dürfen verschiedene Größen haben. Aber: Filterung über Atlas-Kanten braucht Ränder je Mip,
Mip-Auswahl manuell, mehr Shader-Code, sichtbare Nähte bei hoher Roughness. Verworfen.

### 3.2 Probe-Puffer

`StructuredBuffer<GpuProbeData>`, 96 Byte je Probe:
```
struct GpuProbeData
{
    float4 worldToBox0, worldToBox1, worldToBox2; // 3x4: Welt -> lokaler Box-Raum
    float4 extentsBlend;   // xyz = extents, w = blendDistance
    float4 capturePosWS;   // xyz = Aufnahmepunkt, w = intensity
    float4 params;         // x = array slice, y = boxProjection, z = mipCount, w = frei
};
```
Plus SH-Koeffizienten (9 × float4 = 144 Byte) im selben Puffer oder einem zweiten. Bei 32
Probes zusammen unter 8 KB.

Globale Werte (Probe-Anzahl, an/aus, globale Stärke, Debug-Ansicht) passen in **einen Platz des
Parameterblocks** aus Auftrag 5 (`RegisterFrameParameterBlock`, Platz 1).

### 3.3 Irradiance: Cubemap oder Spherical Harmonics

**SH L2 (9 Koeffizienten, empfohlen).** 108 Byte statt 49 KB (32er Cube) je Probe, keine zweite
Array-Textur, Auswertung sind ~30 MADs, glatt und ohne Mip-Fragen. Projektion aus der
gefalteten Irradiance-Cube oder direkt aus der Aufnahme auf der CPU (neue allgemeine Funktion
im Kern-Baker). Schwäche: L2 verschmiert sehr gerichtetes indirektes Licht (heller Fleck
an der Wand) und kann bei starkem Kontrast leicht negativ werden (Ringing); dagegen hilft
eine Fensterung der Koeffizienten (Hanning o. ä.) und `max(0, ...)`.

**Irradiance-Cube-Array (32²).** Genauer bei harten Kontrasten, aber ein zweites Array und zwei
Texturzugriffe je Probe mehr. Für diffuse Raumbeleuchtung ist der Unterschied klein.

Das globale IBL bleibt unverändert als Cube (t5); nur lokale Probes nutzen SH.

---

## 4. Auswahl pro Pixel

**Weg A: Tile-Bitmaske (empfohlen).** Höchstens **32 Probes** pro Szene → eine `uint` je
Forward+-Tile reicht als Liste: Bit i = Probe i (nach Priorität sortiert). Das Probes-AddOn
projiziert jede Box auf der CPU auf den Bildschirm (Rechteck der 8 Ecken, Box hinter der
Kamera oder Kamera in der Box → alle Tiles) und setzt die Bits. 1080p: 120 × 68 Tiles = 8160
`uint` = 32 KB, je Frame hochgeladen. Kein Compute, keine Abhängigkeit vom Lichter-Culling, und
die Bit-Reihenfolge ist schon die Prioritätsreihenfolge.

**Weg B: Probes als zusätzliche Einträge in den Licht-Tile-Listen.** Spart einen Puffer, aber
vermischt Typen in der Liste des Forward-AddOns (Index-Raum, Obergrenzen, Compute-Shader) und
braucht eine Sortierung pro Tile. Verworfen.

**Überblendung im Shader:**
```
remaining = 1
for each bit in tileMask (Priorität absteigend), höchstens 2 Treffer:
    p = mul(worldToBox, positionWS)                  // lokaler Box-Raum
    d = extents - abs(p)                             // Abstand zur Wand, je Achse
    if (min(d) <= 0) continue                        // außerhalb
    w = saturate(min(d.x, min(d.y, d.z)) / blendDistance)
    w = w * w * (3 - 2w)                             // weicher Rand
    accumulate(probe, w * remaining); remaining *= (1 - w)
    if (remaining < 0.01) break
global IBL * remaining
```
Höhere Priorität deckt also ab und lässt nur ihren Rand durch.

**Obergrenze: 2 Probes pro Pixel**, danach globales IBL für den Rest. Damit höchstens 2
Prefilter-Zugriffe und 2 SH-Auswertungen zusätzlich. Die Schleife über die Bitmaske prüft
bis zu 32 Boxen, die Prüfung ist billig (3 Dot-Produkte).

**Pfade ohne Forward+:** `pbr_lit.ps.hlsl` (DX11/Vulkan im klassischen Forward-Modus) hat keine
Tiles. Vorschlag: dort dieselbe Schleife über alle Probes ohne Maske (≤ 32), weil die Datenlage
gleich ist. OpenGL (`pbr_lit.opengl.fs.glsl`): eingefroren, nur globales IBL (Offene Frage 5).

---

## 5. Reflexionen: Box-Projektion und Übergang

Box-Projektion (Parallax-Korrektur) im lokalen Box-Raum:
```
Rl   = mul((float3x3)worldToBox, R)                  // Richtung lokal
Pl   = mul(worldToBox, positionWS)
t1   = ( extents - Pl) / Rl
t2   = (-extents - Pl) / Rl
tMax = max(t1, t2); dist = min(tMax.x, min(tMax.y, tMax.z))
hitL = Pl + Rl * dist
Rcorr = mul(boxToWorld3x3, hitL - captureL)         // Richtung vom Aufnahmepunkt zum Treffer
```
Der Prefilter wird mit `Rcorr` und dem LOD aus `SamplePrefilteredIBL` gesampelt
(`iblPrefilterLevels` gilt auch für die Probes, da gleiche Mip-Zahl).

Bei hoher Roughness stimmt die Projektion schlechter (der Lobe ist breit). Übliche Abschwächung:
`Rcorr = lerp(Rcorr, R, roughness²)` – bei rauen Flächen zurück zur unkorrigierten Richtung.
Empfohlen.

**Übergang zum globalen IBL:** außerhalb aller Volumen `remaining = 1` → reines globales IBL,
wie heute. Innerhalb der Blendzone mischt `remaining` linear zwischen Probe und global. Für
Innenräume, deren Blendzone an einer Außenwand liegt, sollte `blendDistance` klein sein; sonst
leckt Himmel an der Wandinnenseite. Die Vorgabe 1 m ist für Räume ab ~4 m Kantenlänge gedacht.

---

## 6. Specular Occlusion

Heute: `ambientIBL = (iblDiffuse + iblSpecular) * ao`, dann `*= gtaoFactor`.

Neu:
```
aoTotal  = ao * gtaoFactor                           // Material-AO × GTAO (wie bisher kombiniert)
iblDiffuse  *= aoTotal
iblSpecular *= ComputeSpecularOcclusion(NoV, aoTotal, roughness)

// Lagarde & de Rousiers, "Moving Frostbite to PBR" (2014)
float ComputeSpecularOcclusion(float NoV, float ao, float roughness)
{
    return saturate(pow(NoV + ao, exp2(-16.0 * roughness - 1.0)) - 1.0 + ao);
}
```
Eigenschaften: bei `ao = 1` immer 1; glatte Flächen in Blickrichtung (NoV ≈ 1) werden kaum
abgedunkelt, streifende Blicke und raue Flächen fast wie diffus. Kosten: ein `pow`.

Die Debug-Schalter `DBG_DISABLE_AO`/`DBG_DISABLE_GTAO` wirken weiter auf `aoTotal`.

Alternative: GTSO (Jimenez 2016) mit Bent Normals und Kegelschnitt. Genauer, braucht aber
Bent Normals aus GTAO (zweiter Kanal, Änderung am GTAO-AddOn) – das passt zu Auftrag 8, nicht
hierher. Verworfen für Auftrag 7.

Gilt in `pbr_forward_plus.ps.hlsl` und `pbr_lit.ps.hlsl` (HLSL). OpenGL eingefroren.

---

## 7. Dynamische Objekte

Weil die Auswahl **pro Pixel** über die Weltposition läuft, braucht es keine Zuordnung pro
Objekt: Ein Objekt, das durch eine Tür läuft, ist auf beiden Seiten richtig beleuchtet, und in
der Blendzone mischt es weich. Das gilt für Skinned Meshes und Partikel-Materialien mit PBR
ebenso.

Alternative: Probe pro Objekt (CPU wählt nach Bounds-Mittelpunkt, 1–2 Probes je Draw). Billiger
im Shader, aber sichtbares Umspringen beim Durchqueren und falsch für große Objekte über zwei
Räume. Verworfen.

Grenzen:
- Dynamische Objekte sind nicht in den gebackenen Probes (sie spiegeln sich nicht, werfen kein
  indirektes Licht). Das ist bei gebackenen Probes üblich.
- Wenn beim Backen bewegliche Objekte im Raum stehen, werden sie eingebacken. Daher der
  Ausschluss über Render-Layer (Offene Frage 3).
- Echtzeit-Probes (jeden Frame neu) sind nicht vorgesehen; 6 Ansichten × Szene pro Frame
  sprengen das Budget.

---

## 8. Kosten

### 8.1 Speicher (Prefilter RGBA16F, 6 Mips, SH im Puffer)

Je Probe bei 128er Basis: Σ (128² + 64² + 32² + 16² + 8² + 4²) × 6 Seiten × 8 Byte =
21 840 × 48 = **1,05 MB**. Bei 256er Basis ×4 (4,2 MB), bei 64er ÷4 (0,26 MB).

| Probes | 64 | 128 (empfohlen) | 256 |
|---|---|---|---|
| 8 | 2,1 MB | 8,4 MB | 33,5 MB |
| 16 | 4,2 MB | 16,8 MB | 67 MB |
| 32 | 8,4 MB | 33,5 MB | 134 MB |

Dazu: Probe-Puffer < 8 KB, Tile-Maske 32 KB. Mit Irradiance-Cube statt SH (32er, RGBA16F)
kämen je Probe 49 KB dazu (32 Probes: 1,6 MB). Gegen 6 GB der GTX 1660 sind 16,8 MB (16 × 128)
unkritisch. BC6H-Kompression würde ÷8 bringen, ist aber für Auftrag 7 nicht nötig.

Das gesamte Array wird für die maximale Probe-Zahl der Szene angelegt (gerundet auf 8er
Schritte), nicht fest für 32.

### 8.2 GPU-Zeit pro Frame (geschätzt, nicht gemessen)

Budget nach der Plan-Datei: 16,7 ms bei 60 fps; gemessen im Editor (Auftrag 2) Summe aller
Pässe 1,4–1,8 ms (Vulkan/OpenGL), MainOpaque 0,8–1,0 ms.

Zusatz im Opaque-Pixelshader bei 1080p (~2,1 Mio. Pixel), je Pixel: 1 Masken-Lesezugriff,
bis zu 32 Box-Tests (meist 1–3 gesetzte Bits), bis zu 2 Box-Projektionen + 2 Prefilter-Samples
aus dem Array + 2 SH-Auswertungen, Specular Occlusion (1 `pow`).
- Schätzung: **+0,2 bis 0,5 ms** im MainOpaque-Pass, je nach Überlappung. Der größte Posten
  sind die Prefilter-Zugriffe mit hohen Mips (Cache-freundlich) und die Divergenz an
  Box-Rändern.
- CPU: Masken-Projektion für ≤ 32 Boxen < 0,05 ms; Upload 32 KB.
- Backen: kein Frame-Kosten-Thema, läuft auf Knopfdruck.

Das liegt deutlich im Rahmen. Messen in Auftrag 7 mit dem Fenster „GPU-Zeit pro Pass“ in
„Room 2“.

---

## 9. Schnitt Kern / AddOn

### Neues AddOn `addons/probes` (`engine_probes`)
- `ReflectionProbeComponent`, Serialisierung, stabile `probeId`.
- Feature `ProbeFeature`: Extraktion, Sortierung, Tile-Maske (CPU), `GpuProbeData`-Puffer,
  `TextureCubeArray` anlegen und aus `.kprobe` füllen, Parameterblock (Platz 1) über
  `IFrameConstantsContributor`.
- Format `.kprobe` (Lesen/Schreiben, Versionen).
- Backen: `probes::BakeProbe(RenderSystem&, Scene&, entity, const ProbeBakeOptions&)` und
  `BakeAllProbes(...)`, `ComputeProbeInputHash(...)`. Ruft den Kern-Baker auf.
- Shader-Teil `assets/probes_sampling.hlsli` (Auswahl, Box-Projektion, SH), eingebunden von
  `pbr_forward_plus.ps.hlsl` und `pbr_lit.ps.hlsl` unter `#ifdef KROM_LOCAL_PROBES`.

### Editor (`addons/editor`)
- Inspector-Karte, Hierarchie-Eintrag, „Backen“/„Alle backen“ (ruft nur die Engine-Funktionen),
  Viewport-Anzeige über DebugDraw, Anzeige „veraltet“, Locale-Keys de/en.

### Allgemeine Kerndienste (keine Probe-Namen)
1. **`TextureDimension::CubemapArray`** in `RendererTypes.hpp`, mit `arraySize` = Anzahl
   Würfel. Backends: DX11 SRV `TEXTURECUBEARRAY`, Vulkan View `CUBE_ARRAY` + Feature
   `imageCubeArray`, OpenGL `GL_TEXTURE_CUBE_MAP_ARRAY` (oder „nicht unterstützt“).
   `UploadTextureData` mit `arraySlice` gibt es schon.
2. **Shader-Slots für AddOns.** Freie HLSL-Register sind nur t18 und t19. Vorschlag: zwei
   allgemein benannte Plätze `TexSlots::FeatureSRV0 = 18`, `FeatureSRV1 = 19` und
   `BufSRVSlots::COUNT` auf 20 (Buffer-SRV 18/19 teilen sich in HLSL das Register mit
   18/19 der Texturen – also je Platz entweder Textur oder Puffer). Dazu eine Anmeldung analog
   `RegisterFrameParameterBlock` (`RegisterShaderResourceSlot`), damit zwei AddOns nicht
   denselben Platz nehmen. Probes brauchen: t18 = Cube-Array, t19 = Probe-Puffer; die
   Tile-Maske wird an den Probe-Puffer angehängt (ein `StructuredBuffer<uint4>`-Bereich oder
   `ByteAddressBuffer`), damit zwei Plätze reichen. Vulkan: Bindings 34/35 bzw. 64+18/19,
   vorhandene Reserve; Layout-Erweiterung in `VulkanCommandList` zu prüfen.
   Alternative: `TexSlots::COUNT` auf 24 erhöhen (die 4 Reserve-Bindings bis 40 sind schon
   da). Mehr Platz, aber mehr Bindearbeit in drei Backends. Erst bei Bedarf.
3. **Bindung an den Opaque-Pass.** Heute bindet das Forward-AddOn Lighting- und Shadow-Puffer
   selbst, weil es deren Frame-Daten-Typen kennt.
   - **Weg A (empfohlen):** Das Forward-AddOn bietet in seinem öffentlichen Header eine Liste
     zusätzlicher Bindungen für die Szenenpässe (`ForwardExtraSceneBindings` in den
     Frame-Daten: Slot, Handle, Typ). Das Probes-AddOn füllt sie; Forward bindet sie ohne den
     Typ zu kennen. Kern unverändert, Abhängigkeit nur probes → forward (natürlich, weil die
     Tile-Größe von Forward kommt).
   - Weg B: allgemeine Schnittstelle im Kern („Feature trägt Ressourcen zu Material-Pässen
     bei“). Sauberer für künftige AddOns, aber Kernänderung ohne zweiten Nutzer. Später.
4. **HDR-Rücklesen:** `IDevice::ReadTextureData(handle, mip, slice, outBytes)` für beliebige
   Formate, insbesondere RGBA16F, synchron, nur für Werkzeuge. Drei Backends.
5. **Baker:** `IBLBaker::BakeFromCubeFaces(...)` und `IBLBaker::ProjectSH9(...)`. Der Baker ist
   schon Kern und allgemein.
6. **Offscreen-Ansicht mit eigener Kamera und abgeschalteten Pässen.** Prüfen in Auftrag 7, ob
   die vorhandenen Editor-Vorschauen/`RenderView` das ohne Erweiterung können; der offene Punkt
   aus Auftrag 3 („Pass je Ansicht abschalten“ statt `enableBloom`) würde hier passen.

Shader-Code im Kern-Asset `pbr_forward_plus.ps.hlsl` bekommt nur ein `#include` hinter
`#ifdef KROM_LOCAL_PROBES`; das Define setzt das Probes-AddOn wie GTAO seine Defines.

---

## 10. Reihenfolge der Umsetzung (Auftrag 7)

1. **Specular Occlusion + Kernvorarbeit.** SO in beiden HLSL-PBR-Shadern (sofort im Editor
   sichtbar: Kanten und Ecken mit AO verlieren den spekularen Glanz; Vergleich über
   `DBG_DISABLE_AO`). Dazu `CubemapArray` in den Backends, Slot-Anmeldung, HDR-Rücklesen –
   mit Tests, noch ohne Nutzer.
2. **Komponente, Editor-Anzeige, Laufzeit mit Test-Daten.** `ReflectionProbeComponent`,
   Serialisierung, Inspector-Karte, Box im Viewport. Shader-Auswahl, Überblendung und
   Box-Projektion mit einem **Platzhalter-Inhalt** (Kopie des globalen IBL, eingefärbt per
   Debug-Ansicht „Probe-Gewicht“). Sichtbar: Boxen, Blendzonen, Priorität.
3. **Backen.** Sechs Ansichten, Rücklesen, `BakeFromCubeFaces`, SH, `.kprobe`, 2 Durchgänge,
   „veraltet“-Anzeige, „Alle backen“. Sichtbar: Room 2 ohne Himmelslicht im Inneren.
4. **Messen und Feinschliff.** GPU-Zeit in Room 2 bei 1080p, Speicher, Vorgaben
   (`blendDistance`, Auflösung), Export der `.kprobe` im Game-Build prüfen.

---

## 11. Lightmaps (nicht Teil dieser Reihe)

Was später fehlt:
- **Zweiter UV-Satz:** `VertexSemantic::TexCoord1` gibt es in Kern und Backends, aber keine
  Daten: `SubMeshData` hat nur `uvs`, `GpuResourceRuntime.cpp` füllt `TexCoord1` mit `uvs`.
  Fehlt: Feld im Mesh-Asset, Import aus glTF/FBX (`TEXCOORD_1`), Mesh-Cache-Version.
- **UV-Atlas:** Wer keinen zweiten Satz liefert, braucht automatisches Entfalten
  (z. B. xatlas) und Packen aller statischen Meshes einer Szene in Atlas-Seiten, mit Rand.
- **Baker:** Pfadverfolgung auf der CPU (Embree o. ä.) oder Rasterisierung + Hemisphären-
  Rendering; ohne RT-Cores eher CPU. Dazu Denoising, Dilatation der Ränder.
- **Laufzeit:** Lightmap-Textur je Objekt (Slot, Skala/Versatz pro Objekt in PerObject),
  Kennzeichen „statisch“, Zusammenspiel mit Probes (Lightmap ersetzt diffuse Irradiance,
  Probes liefern weiter Spekular). Format mit Versionsnummer.
- Bis dahin übernehmen die SH der Probes das diffuse indirekte Licht.

---

## 12. Offene Fragen an den Nutzer

1. **Durchgänge:** Vorgabe 2 Durchgänge beim Backen (empfohlen) – einverstanden?
2. **Auflösung:** eine Probe-Auflösung je Szene (128, wählbar 64/128/256) statt je Probe,
   wegen des Cube-Arrays – einverstanden?
3. **Was wird eingebacken:** alles Sichtbare außer einem Render-Layer „nicht in Probes“, oder
   lieber ein neues Kennzeichen „statisch“ an der Entity (Kern-nah, auch für Lightmaps später
   nützlich)?
4. **Obergrenzen:** 32 Probes je Szene, 2 je Pixel – reicht das?
5. **Klassisches Forward unter DX11/Vulkan** (`pbr_lit.ps.hlsl`): Probes dort per Schleife ohne
   Tiles mitnehmen (Vorschlag), oder nur Forward+? OpenGL bleibt beim globalen IBL.
6. **Slots (Abschnitt 9, Punkt 2):** zwei allgemeine Plätze t18/t19 mit Anmeldung – oder gleich
   `TexSlots::COUNT` auf 24?
7. **Ablage:** `Assets/Baked/<Szene>/<probeId>.kprobe` als Projekt-Asset (mit Export) – passt
   der Ordner?
8. **Specular Occlusion** schon im ersten Schritt von Auftrag 7, unabhängig von Probes – oder
   als eigener kleiner Auftrag vorziehen?
