# Post Mortem – Falsche Speicherabfrage beim Schließen des Editors

16. November 2026

## Kurzfassung

Nach Start und Stopp von Play konnte der Editor beim Schließen ungespeicherte
Szenenänderungen melden, obwohl die Szene im Editor nicht bearbeitet worden war.
Beim Wiederherstellen der Szene wurden identische Prefab-Overrides in anderer
Reihenfolge serialisiert. In einer vollständigen lokalen Kopie des DemoProject
änderte sich auch die Reihenfolge von Script-Feldern. Der Vergleich der
Szenensignaturen wertete beide Abweichungen als Änderungen.

## Reproduktion vor dem Fix

1. `Room 2` im DemoProject öffnen.
2. Play starten und stoppen, ohne Änderungen im Editor vorzunehmen.
3. Den Editor schließen. Die Speicherabfrage erscheint.

Im DX11-Reproduktionslauf unterschieden sich acht Prefab-Instanzen zwischen dem
Speicherstand beim Start von Play und der wiederhergestellten Szene. Die Pfade der
Overrides waren jeweils dieselben. Nachdem die Override-Pfade sortiert wurden,
zeigte der Lauf mit der vollständigen lokalen Kopie noch Unterschiede bei den
Script-Feldern von `Kamera`, `Spielablauf` und `Bodenschalter`. Die Feldwerte
waren jeweils identisch. Beim direkten Öffnen und Schließen ohne Play blieb die
Abfrage in den Kontrollläufen auf DX11 und Vulkan aus.

## Ursache

Beim Stoppen von Play stellt `EndPlaySession` den Snapshot der Editor-Szene
wieder her und synchronisiert die Prefab-Instanzen erneut. Die Synchronisierung
sammelt die Pfade der Overrides in einem `std::unordered_set` und übernimmt dessen
Iterationsreihenfolge in die Bindings. Der Serializer schrieb diese Reihenfolge
direkt in das JSON-Array `overrides`.
[Play-Wiederherstellung](../addons/editor/EditorCommands.cpp),
[Prefab-Synchronisierung](../addons/prefab/PrefabSynchronization.cpp)

`IsEditorSceneModified` vergleicht die gespeicherte Szenensignatur mit der
aktuellen. Die Signatur berücksichtigte die Reihenfolge von JSON-Arrays und
Objektschlüsseln. Script-Felder werden in einer `std::unordered_map` gehalten
und als JSON-Objekt geschrieben. Nach dem Wiederherstellen konnten dieselben
Schlüssel in anderer Reihenfolge erscheinen.
[Szenenvergleich](../app/KromEditorApp.cpp),
[Script-Serializer](../addons/script/ScriptSerialization.cpp),
[Schließdialog](../addons/editor/EditorUIProject.cpp)

## Fix

`WritePrefabInstanceFields` sortiert eine Kopie der Override-Pfade für jedes
Binding vor der Ausgabe. Die sortierte Darstellung gilt für Szenendateien,
Play-Snapshots und serialisierte verschachtelte Prefab-Instanzen. Vorhandene
Dateien mit anders sortierten Pfaden bleiben lesbar. Ein hinzugefügter oder
entfernter Override ändert weiterhin die Szenensignatur.
[Prefab-Serializer](../addons/prefab/PrefabSerialization.cpp)

`AppendCanonicalJson` verarbeitet Objektschlüssel für die Szenensignatur jetzt
in sortierter Reihenfolge. Die Reihenfolge von Arrays fließt weiterhin in den
Vergleich ein, etwa bei der Liste der Script-Instanzen.
[Szenenvergleich](../app/KromEditorApp.cpp)

## Verifikation

Der Regressionstest `--scene-close` startet und stoppt Play und prüft die
Speicherabfrage mit der wiederhergestellten Szene, mit vertauschten
Override-Pfaden und mit einem hinzugefügten Override. Der Test bestand mit
`krom_editor_loading_tests_dx11` und `krom_editor_loading_tests` (Vulkan) im
Debug-Build x64.
[Editor-Regressionstest](../tests/test_editor_loading_runtime.cpp)

- **Room 2, DX11.** Auf einer Kopie der Szene blieb die Speicherabfrage nach
  Start und Stopp von Play aus.
- **Testsuite.** `krom_tests` bestand vollständig im Debug-Build x64.
