# Post-Mortem: Editor Asset Loading, Materialeditor und Auswahl-Lags

## Zusammenfassung

Im Editor traten spuerbare Haenger beim Auswaehlen von Objekten und beim Oeffnen von Materialien auf. Besonders auffaellig war der Fall, dass ein Mesh ausgewaehlt war und parallel im Assets-Fenster ein anderes Material per Doppelklick im Materialeditor geoeffnet wurde.

Die eigentlichen Ursachen waren nicht das Erzeugen von ImGui-Fenstern, sondern:

1. wiederholte synchrone Asset-Reloads auf dem Main-Thread
2. unnnoetige Re-Imports derselben Texturen
3. ein Konflikt zwischen Mesh-Inspektor und globaler Materialauswahl
4. ein materialbezogener Editor-Loadpfad, der vorher komplett synchron war

## Symptome

- Doppelklick auf Material fuehlte sich zoegerlich an
- ausgewaehltes Mesh verlor im Inspektor seine sinnvolle Materialanzeige, wenn im Assets-Fenster ein anderes Material geoeffnet wurde
- Log-Spam durch wiederholte `TextureImporter: imported texture ...` Meldungen
- Auswahlwechsel zwischen Objekten konnte 1-2 Sekunden blockieren, wenn Material-/Texturpfade betroffen waren

## Root Cause

### 1. Materialauswahl und Materialeditor teilten sich einen globalen Zustand

Der Inspektor fuer ein ausgewaehltes Mesh war an denselben globalen Materialzustand gekoppelt wie das separate Materialeditor-Fenster.

Folge:

- Oeffnete man im Assets-Fenster ein anderes Material, wechselte der globale Zustand
- der Mesh-Inspektor verlor dadurch seinen Bezug auf das am Mesh gebundene Material

### 2. Doppelklick auf Material loeste mehrfaches Laden aus

Der alte Pfad war im Kern:

1. erster Klick: `SelectMaterialAsset(...)`
2. zweiter Klick: erneut `SelectMaterialAsset(...)`
3. danach `QueueOpenMaterialAsset(...)`
4. spaeter `OpenMaterialAsset(...)`
5. darin erneut `SelectMaterialAsset(...)`

Damit wurde dasselbe Material beim Doppelklick mehrfach synchron geladen und geparst.

### 3. `LoadTexture()` und `LoadMaterial()` luden unveraenderte Assets erneut

In der `AssetPipeline` wurde bei jedem Zugriff wieder ein Reload angestossen, auch wenn Asset und Datei unveraendert waren.

Folge:

- dieselbe Textur wurde immer wieder importiert
- dieselbe Materialdatei wurde immer wieder geparst
- entsprechender Log-Spam und unnnoetige CPU-/I/O-Kosten

### 4. Materialeditor-Load war komplett synchron

Der Editor las Materialdateien vorher direkt auf dem Hauptthread ein. Wenn Materialauswahl oder Materialeditor auf einen langsameren Pfad trafen, blockierte das unmittelbar das UI.

## Was wir geaendert haben

### A. Materialeditor und Mesh-Auswahl konfliktfreier gemacht

- Der Inspektor behaelt sein Mesh-Material stabil.
- Wenn Materialeditor und Inspektor dasselbe Material zeigen, bleiben beide weiterhin synchron.
- Beim Doppelklick auf ein Material im Assets-Fenster wird die aktuelle Entity-Auswahl aufgehoben, bevor der Materialeditor geoeffnet wird.

Relevante Stellen:

- [EditorAssetBrowser.cpp](/F:/working/krom%20codex/addons/editor/EditorAssetBrowser.cpp:576)
- [EditorUI.cpp](/F:/working/krom%20codex/addons/editor/EditorUI.cpp:1035)
- [EditorMaterialLibrary.cpp](/F:/working/krom%20codex/addons/editor/EditorMaterialLibrary.cpp:1777)

### B. Doppelklick-Pfad verschlankt

- Beim echten Doppelklick wird nicht mehr vorher unnnoetig selektiert und geladen.
- Wenn ein Material bereits geladen ist, wird es beim Oeffnen nicht erneut synchron geladen.

Relevante Stellen:

- [EditorAssetBrowser.cpp](/F:/working/krom%20codex/addons/editor/EditorAssetBrowser.cpp:564)
- [EditorMaterialLibrary.cpp](/F:/working/krom%20codex/addons/editor/EditorMaterialLibrary.cpp:1604)
- [EditorMaterialLibrary.cpp](/F:/working/krom%20codex/addons/editor/EditorMaterialLibrary.cpp:1620)

### C. Cache-Fast-Path fuer unveraenderte Texturen und Materialien

`AssetPipeline::LoadTexture()` und `AssetPipeline::LoadMaterial()` geben bereits geladene, unveraenderte Assets direkt zurueck.

Dadurch:

- verschwindet der wiederholte `TextureImporter`-Spam weitgehend
- fallen Dateilesen, Parsing und Import fuer unveraenderte Assets weg

Relevante Stellen:

- [AssetPipeline.cpp](/F:/working/krom%20codex/src/assets/AssetPipeline.cpp:190)
- [AssetPipeline.cpp](/F:/working/krom%20codex/src/assets/AssetPipeline.cpp:220)

### D. Erster echter Async-Schritt ueber das vorhandene Job-System

Der Materialeditor laedt Materialdateien jetzt asynchron ueber das bestehende Engine-`JobSystem`. Das Ergebnis wird spaeter im Hauptthread uebernommen.

Wichtig:

- async ist derzeit der editorbezogene Materialdatei-Load
- GPU-Upload und allgemeine Runtime-Loads bleiben weiterhin auf dem bisherigen Pfad

Relevante Stellen:

- [EditorFeature.hpp](/F:/working/krom%20codex/addons/editor/EditorFeature.hpp:44)
- [RenderSystem.hpp](/F:/working/krom%20codex/include/renderer/RenderSystem.hpp:71)
- [ExampleApp.cpp](/F:/working/krom%20codex/examples/framework/ExampleApp.cpp:410)
- [EditorMaterialLibrary.cpp](/F:/working/krom%20codex/addons/editor/EditorMaterialLibrary.cpp:400)

## Was nicht funktioniert hat

Eine Zwischenloesung mit "Loading..."-Overlay und um einen Frame verzoegerter UI-Aktion wurde getestet und wieder entfernt.

Grund:

- fuehlte sich kuenstlich an
- loeste nicht die eigentliche Ursache
- war UX-seitig schlechter als ein echter technischer Fix

## Warum das System jetzt deutlich schneller ist

Der groesste Gewinn kam aus drei Punkten:

1. keine mehrfachen Reloads mehr beim Material-Doppelklick
2. keine dauernden Re-Imports identischer Texturen
3. asynchrones Laden der Materialdatei fuer den Materialeditor

Der Flaschenhals war also ueberwiegend CPU/I/O im Asset-Pfad, nicht das Fenster-Rendering.

## Restrisiken

Die aktuelle Verbesserung ist bewusst nur ein erster Async-Schritt.

Offen bleibt insbesondere:

- Texturdecode/-import ist noch nicht allgemein asynchron
- Mesh-Import ist weiterhin synchron
- Registry-Commit und GPU-Upload sind nicht als vollstaendige 2-Phasen-Async-Pipeline ausgebaut
- Auswahlwechsel kann noch blockieren, wenn er auf andere synchrone Asset-Arbeit trifft

## Wenn es spaeter wieder zu langsam wird

Die sinnvollen Eskalationsstufen sind:

### 1. Textur-Import ebenfalls ueber Job-System vorbereiten

Wahrscheinlich der naechste groesste Hebel.

Ziel:

- Dateilesen
- Bilddecode
- Mip-Generierung
- Metadatenableitung

im Worker ausfuehren und erst danach das Ergebnis im Main-Thread committen.

### 2. Material-Load zu echter 2-Phasen-Async-Pipeline ausbauen

Aktuell wird die Materialdatei async geladen und geparst, aber noch nicht als vollstaendige generische Asset-Pipeline modelliert.

Ziel:

1. Worker: Datei lesen, parsen, CPU-Struktur bauen
2. Main-Thread: Registry aktualisieren, Handles setzen, Runtime-Material markieren

### 3. Mesh-Import entkoppeln

Wenn grosse Meshes oder Prefabs Haenger verursachen:

- Import/Decode/Tangentenerzeugung in Worker
- Entity-/Registry-Commit spaeter im Main-Thread

### 4. AssetRegistry und AssetPipeline thread-ready machen

Fuer groessere Async-Ausbaustufen wird irgendwann eine sauber definierte Trennung noetig:

- Worker duerfen vorbereitete Ergebnisse erzeugen
- Main-Thread commitet atomar in Registry/Runtime
- GPU-Upload bleibt renderthread-sicher

Das ist der groessere strukturelle Schritt.

### 5. Sichtbares Profiling einbauen

Falls erneut Unklarheit entsteht, wo Zeit verloren geht:

- Zeitmessung fuer `LoadMaterial`, `LoadTexture`, `ReloadTexture`, `UploadPendingGpuAssets`
- Editor-seitige Messung fuer Auswahlwechsel und Materialeditor-Oeffnen
- optional Statistikfenster im Editor

Das wuerde zukuenftige Entscheidungen deutlich zielgerichteter machen.

## Empfehlung fuer die Zukunft

Wenn erneut editorbezogene Lags auftreten, zuerst in dieser Reihenfolge vorgehen:

1. messen, welcher Asset-Typ blockiert
2. Cache-Hit-Rate pruefen
3. CPU-Vorbereitung in Worker verschieben
4. Commit/GPU-Upload auf Main-/Render-Thread belassen

Kurz gesagt:

Nicht mehr mit UI-Workarounds anfangen, sondern direkt den jeweiligen Asset-Pfad in CPU-Async plus Main-Thread-Commit aufteilen.

## Fazit

Das Problem war im Kern ein synchroner Asset-Pfad mit zu vielen Wiederholungen und einer zu stark gekoppelten Editorzustandslogik.

Die wirksamsten Verbesserungen waren:

- Entkopplung der Editorinteraktionen
- Vermeidung redundanter Reloads
- erster echter Async-Load ueber das vorhandene Job-System

Damit ist der Editor jetzt deutlich reaktiver, ohne dass dafuer das komplette Asset-System auf einmal umgebaut werden musste.
