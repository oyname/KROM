# Vulkan Shadow Post-Mortem

## Problem

Im Vulkan-Backend traten im Mehrfach-Schattenpfad sichtbare falsche Schattenartefakte auf:

- kleine, abgesetzte runde Schattenflecken
- rechteckige bzw. atlasartige Spot-Shadow-Artefakte
- Unterschiede zwischen `R` (nur Spot-Light) und `T` (alle Lichter)
- Verhalten war nicht konsistent zu `DX11` und `OpenGL`

Das Problem trat vor allem dann sichtbar auf, wenn mehrere 2D-Schatten (`Directional` + `Spot`) gleichzeitig aktiv waren.

## Symptome

- `R` funktionierte relativ plausibel, weil effektiv nur ein Shadow-Tile aktiv war.
- `T` zeigte in Vulkan zusätzliche falsche Schatten, die in `DX11` und `OpenGL` nicht sichtbar waren.
- Die Artefakte wirkten zunächst wie Demo- oder Shaderfehler, waren aber im Kern ein Vulkan-spezifisches Engine-Problem im Mehrfach-Shadow-Pfad.

## Was NICHT die eigentliche Ursache war

Im Verlauf wurden mehrere Hypothesen geprüft, die nicht die eigentliche Hauptursache waren:

- Demo-Geometrie oder Szeneaufbau
- OpenGL-/DX11-spezifische Shaderpfade
- allgemeines Material-/PBR-Verhalten
- reine Kugel-Tessellation
- ein Vulkan-`frontFace`-Umbau
- ein Vulkan-Atlas-`Y`-Flip-Hack

Einige dieser Änderungen waren Fehlschläge oder Regressionen und wurden wieder zurückgenommen.

## Echte technische Ursache

Es gab zwei relevante Engine-Ursachen im Vulkan-Mehrfach-Shadow-Pfad.

### 1. Derselbe `PerFrame`-UBO wurde pro Shadow-Tile überschrieben

Im Shadow-Pass wurde für jedes Shadow-Tile dieselbe `PerFrame`-Buffer-Instanz erneut beschrieben und anschließend gezeichnet.

Das ist auf `DX11` und `OpenGL` oft weniger auffällig, ist aber für Vulkan im Multipass-/Atlas-Fall nicht robust:

- mehrere Shadow-Draws verwendeten effektiv dieselbe UBO-Ressource
- pro Tile wurde `shadowViewProj` überschrieben
- dadurch konnten Draws mit falschen bzw. zuletzt geschriebenen Shadow-Daten laufen

Das erklärt genau das Muster:

- `R` ok-ish, weil nur ein Shadow-Tile aktiv
- `T` kaputt, weil mehrere Shadow-Tiles hintereinander denselben UBO-Speicher überschrieben

### 2. Vulkan-Hardware-Depth-Bias war zu aggressiv

Der Shadow-Pass verwendete einen globalen Hardware-`slope bias`, der für Vulkan zu stark war.

Folge:

- sichtbares `Peter Panning`
- kleine abgelöste Spot-Schatten
- stärkere Backend-Abweichung gegenüber `DX11` und `OpenGL`

## Finaler Fix

### Fix 1: Per-Tile `PerFrame`-Constant-Ranges

Der Shadow-Pass verwendet jetzt für jedes Shadow-Tile eine eigene `PerFrame`-Constant-Range aus einer frame-lokalen Constant-Arena.

Statt:

- ein UBO
- mehrfach überschreiben
- mehrfach drawen

jetzt:

- eigene `FrameConstants` pro Shadow-Tile
- eigener Range-Offset pro Tile
- sauberes Binding über `SetConstantBufferRange(...)`

Damit bekommt jeder Vulkan-Shadow-Draw die korrekten Shadow-Matrizen und Atlas-Daten.

### Fix 2: Vulkan-spezifische Bias-Kalibrierung

Der Hardware-`slope bias` im Shadow-Pass wurde für Vulkan reduziert.

Ziel:

- kein zu aggressives Ablösen von Spot-Schatten
- näher an das visuelle Verhalten von `DX11`/`OpenGL`
- ohne globale Änderungen an den anderen Backends

## Warum das Industriestandard-konform ist

Der finale Fix folgt einem sauberen Backend-Design:

- keine Demo-Sonderlogik für Vulkan
- keine shaderseitigen Workarounds nur für einzelne Bilder
- keine globalen Front-Face-/Viewport-Hacks zur Maskierung
- stattdessen:
  - korrekte pro-Draw-Konstantendaten
  - saubere per-backend Bias-Kalibrierung dort, wo die Hardwaresemantik tatsächlich abweicht

Das entspricht dem üblichen Vorgehen in 3D-Engines:

- Shadow-Pass-Daten pro Draw/Pipeline sauber materialisieren
- Backend-Unterschiede im Backend-/Rasterizerpfad behandeln
- nicht die Szene oder das Materialsystem verbiegen

## Lessons Learned

1. Ein Mehrfach-Shadow-Atlas-Pfad darf nicht dieselbe `PerFrame`-Buffer-Instanz pro Tile blind überschreiben.
2. Vulkan reagiert empfindlicher auf unsaubere Multipass-UBO-Nutzung als `DX11`/`OpenGL`.
3. Shadow-Artefakte dürfen nicht vorschnell der Demo oder dem Materialsystem zugeschrieben werden.
4. Front-Face-, Viewport- und Atlas-Hacks ohne klaren Beweis erzeugen schnell Regressionen.
5. Für Shadow-Bugs zuerst den Datenpfad prüfen:
   - welche Matrix
   - welcher Atlas-Rect
   - welcher Buffer
   - welcher Draw

## Offene Themen

Die folgenden Themen sind davon getrennt und waren nicht Kern dieses Vulkan-Bugs:

- `Point-Light`-Schattenpfad (`ShadowMapCube`) im aktuellen sichtbaren Mehrfach-Shadow-Renderpfad
- weiterer Shadow-System-Ausbau wie Cascades und Atlas-Management
- spätere Entkopplung von altem `PerFrame`-Lichtlayout hin zu buffer-basiertem Forward/Forward+

## Betroffene Dateien im finalen Fix

- `addons/forward/ForwardFeature.cpp`
- `src/renderer/ShaderRuntimeMaterials.cpp`
- `include/renderer/ShaderRuntime.hpp`
- `include/renderer/FeatureRegistry.hpp`
- `src/renderer/FrameGraphStage.cpp`
- `addons/shadow/ShadowFeature.cpp`
