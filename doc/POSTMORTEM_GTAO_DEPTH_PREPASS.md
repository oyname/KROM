# Post Mortem – GTAO aus dem aktuellen Frame

Datum: 2026-11-15

## Kurzfassung

Die Forward-Beleuchtung las die Ambient Occlusion (AO) des vorherigen
Frames. Bei Kamerabewegung oder bewegter Geometrie konnte die Abdunklung
dadurch noch an der alten Bildposition sichtbar sein. Ein Tiefen-Prepass
liefert jetzt die Tiefe für GTAO, bevor `MainOpaque` die Beleuchtung berechnet.
Die AO bleibt eine transiente Ressource des aktuellen Render-Graphs.

## Symptome

- Beim Drehen oder Verschieben der Kamera zog die AO an Objektkanten nach.
- Auch ein bewegtes Objekt erzeugte Abweichungen zum Standbild an derselben
  Position. Kamerareprojektion konnte die Bewegung des Objekts und neu
  sichtbare Flächen nicht aus der vorhandenen History rekonstruieren.
- In Room 2 machte die GTAO-Ansicht (F3) den Unterschied zwischen dem
  ersten Bewegungsbild und den folgenden Standbildern messbar.

## Ursache

`MainOpaque` erzeugte die Szenentiefe und brauchte gleichzeitig das Ergebnis
von GTAO. Das Rezept löste diese Abhängigkeit über zwei Frames. GTAO lief
nach Opaque, der Blur schrieb in ein persistentes Ziel und der nächste
Opaque-Pass las daraus. Die Reprojektion korrigierte die Position des
Zugriffs anhand der vorherigen Kamera. Die gespeicherte AO beschrieb
weiterhin die Geometrie und Sichtbarkeit des vorherigen Frames.

## Fix

```text
Shadows → Sky (optional) → DepthPrepass
        → GTAO (Linearize → Main → Blur) → MainOpaque
        → MSAA-Resolve (optional) → SSR (optional)
        → Transparent → Overlays → Post/Tonemap
```

- **Tiefe und Geometrie.** Der Prepass löscht und beschreibt den Tiefenanteil
  von `HDRSceneColor` beziehungsweise `HDRSceneColorMS`. Er verwendet die
  Opaque-Liste mit denselben Vertex-Shadern, Instanzdaten und
  Knochenpaletten. Die Maskenvarianten der Standardshader verwenden die
  vorhandene Opacity-Berechnung bis einschließlich `clip()`.
  `MainOpaque` testet mit `LESS_EQUAL` bei deaktiviertem Depth-Write.
  Es löscht die SSR-MRTs und übernimmt den Clear der Szenenfarbe, wenn
  kein Sky-Pass läuft.
  [Forward-Rezept](../addons/forward/StandardFramePipeline.hpp),
  [Material-Pipelines](../src/renderer/ShaderRuntimeMaterials.cpp)
- **Render-Graph und MSAA.** `OpaqueDepth` verweist auf die tatsächlich
  gebundene HDR-Tiefentextur. Damit plant der Graph den Übergang vom
  Schreibzugriff zum Lesen. Der Linearize-Shader nimmt bei MSAA das
  Minimum der Samples. `MainOpaque` deklariert `ReadTexture` auf dem
  gefilterten `GTAOOutput` in `R16_FLOAT` und liest ihn per
  `Load` an `SV_Position`.
  [GTAO-Pässe](../addons/gtao/GtaoPass.cpp)
- **Weiterer Farbpfad.** SSR arbeitet auf der aufgelösten Opaque-Szene.
  Transparente Geometrie rendert danach in `GTAOComposite` mit der
  aufgelösten Szenentiefe. Bloom und Tonemap verwenden dieses Composite,
  wenn es vorhanden ist.
  [Forward-Rezept](../addons/forward/StandardFramePipeline.hpp)
- **Aktivierung.** Editor und Runtime setzen `RenderView::enableDepthPrepass`
  für GTAO-Views. Probe-Bake, Vorschauen, Thumbnails und OpenGL verwenden
  den bisherigen Pfad. Die History-Bindung und die
  Reprojektionsparameter wurden zusammen mit dem UI-Schalter entfernt.
  Alte Projektdateien dürfen den Schlüssel `gtaoReprojection` weiterhin
  enthalten.

## Verifikation

- **Room 2, GPU-Readback der F3-Ansicht.** Jeweils den ersten Frame nach
  Kameradrehung, Kameraverschiebung und Verschiebung des Spielerkörpers mit
  einem Standbild in derselben Zielpose verglichen. Die AO-Pixel waren auf
  DX11 und Vulkan bei MSAA 1× und 2× mit SSR an und aus identisch. Die
  übrigen Differenzen lagen im Bereich der animierten Tür.
- **Stationärer Vergleich mit `4913fa2`.** Auf Vulkan war die AO bei
  MSAA 1× und SSR aus identisch. Bei MSAA 2× und SSR an betrug die mittlere
  absolute Abweichung auf beiden Backends 0,016 von 255 je AO-Pixel.
- **Sichttest.** Beim 35°-Schwenk um den Spielerkörper im Vulkan-Editor
  stimmten das erste Lit-Bild nach dem Schwenk und das Standbild überein.
- **Debug x64.** Alle `krom_tests` bestanden. Die neuen Tests decken
  Passreihenfolge, Tiefenzustände und Masken-Shader ab. Die Editoren für
  DX11, Vulkan und OpenGL wurden gebaut. OpenGL startete und die vier
  Vulkan-Readback-Läufe mit `KROM_VALIDATION=1` blieben ohne Meldung.

## Frame-Kosten

`--perf` auf einer DemoProject-Testkopie mit GTX 1660, Debug x64,
1920×1080 Editorfenster, MSAA 2× und SSR an. Je Zustand wurden nach
vollständiger Shader-Kompilierung und 60 Warm-up-Frames 300 Frames
gemessen. Alle Werte sind ms pro Frame.

| Backend | Szene | `4913fa2` mit History | Prepass + GTAO | GTAO aus |
| --- | --- | ---: | ---: | ---: |
| DX11 | Room 2 | 14,89 | 16,15 | 16,31 |
| Vulkan | Room 2 | 21,14 | 22,76 | 20,48 |
| DX11 | car | 10,05 | 10,36 | 10,44 |
| Vulkan | car | 13,52 | 13,85 | 13,00 |

Gegenüber der History-Referenz stieg die Frame-Zeit in Room 2 um 1,26 ms
auf DX11 und um 1,62 ms auf Vulkan. In `car` lag der Anstieg bei 0,31 ms
bzw. 0,33 ms. „GTAO aus“ misst den aktuellen Pfad bei deaktivierter
GTAO-Kette samt Prepass.

## Erkenntnisse aus der Prüfung

- **Ein Nullunterschied braucht sichtbare Geometrie.** Die ersten
  DX11-Aufnahmen zeigten bei kaltem Shader-Cache nur Himmel und Raster.
  Der Test wartete anschließend auf fertige Shader und tatsächlich
  ausgeführte Opaque-Draws. Erst diese Aufnahmen gingen in den Vergleich ein.
- **Die geladene Szene gehört ins Messprotokoll.** Der Editor bevorzugt
  `lastScene` und danach die Szenenreihenfolge. Die ersten als `car`
  gestarteten Messungen liefen deshalb in Room 2. Die Tabelle verwendet
  die Wiederholung mit bestätigtem `scene=car`.

Die Befehle für Readback und Performance-Messung stehen in der
[Koordination](../KOORDINATION.md#gtao-aus-der-tiefe-desselben-frames-2026-11-15).
