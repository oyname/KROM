# C++-Scripts beim Start nachladen (Windows)

Der Editor laedt Projekt-Scripts aus `krom_scripts_<runtimeTarget>.dll`.
Beim Oeffnen eines Projekts wird eine vorhandene kompatible DLL geladen;
ohne DLL bleiben die Scripts als Platzhalter mit ihren Feldwerten erhalten.

1. C++-Script speichern.
2. In der Editor-Werkzeugleiste **Start** druecken.
3. Falls Quellen, Header oder Bindings geaendert wurden, baut der Editor nur
   das Script-DLL-Ziel im Buildverzeichnis der laufenden Editor-EXE.
   Ein modaler Dialog zeigt Fortschritt und Compilerdatei. **Abbrechen** beendet
   auch die Compilerprozesse. Nach einem erfolgreichen Build startet Play.

Ohne Aenderungen startet Play sofort. Bei Compilerfehlern bleibt Play aus;
die Fehler stehen mit `[Build]` in der automatisch geoeffneten Konsole.
Die zuletzt geladene DLL bleibt verfuegbar. Nach der Korrektur erneut Start druecken.

Der Austausch behaelt Entities, Auswahl, Undo und gespeicherte Script-Felder.
Entfernte oder nicht mehr registrierte Script-Klassen bleiben als Platzhalter
erhalten. Scripts duerfen vor `KromScriptModuleInit` keine Komponenten-IDs
in globalen/statischen Initialisierern abrufen; solche Module werden abgewiesen.

DLL und PDB werden vor dem Laden in ein eigenes Verzeichnis unter
`<Build>/script_modules/` kopiert. Jeder Linkvorgang erzeugt zudem eine PDB mit
einem eindeutigen Namen; eine vom Debugger gesperrte alte PDB blockiert damit
keinen neuen Script-Build. Script-DLLs werden ohne inkrementelles Linken gebaut.
Host und DLL muessen mit derselben Compiler-, CRT- und Debug-Konfiguration
gebaut sein. Wenn Engine-Libraries neuer als der laufende Editor sind, muss
zuerst der Editor neu gebaut und gestartet werden.

Runtime und Export binden Projekt-Scripts weiterhin statisch ein. Externe
Projekte werden beim ersten Script-Build im Hintergrund fuer CMake konfiguriert.

## Automatisierte Pruefung

`krom_tests` enthaelt Resolver-, Lifecycle-, ABI-, Zeitstempel- und echte DLL-Tests.
Die optionalen Ziele `krom_editor_loading_tests`,
`krom_editor_loading_tests_dx11` und `krom_editor_loading_tests_opengl` pruefen mit
`--script-reload <krom-project.json>` eine temporaere Kopie der Projekt-Scripts:
mehrfache Code-Aenderungen, Felder, Auswahl, Undo, Compilerfehler, Abbruch,
sofortigen Start ohne Aenderungen und Projektwechsel. Das erste Ziel nutzt Vulkan
mit aktivierter Validierung. Die temporaeren Projektdateien werden anschliessend entfernt.

`tests/test_script_pdb_lock.ps1 -BuildDir out/build/x64-Debug` prueft zwei echte
DLL-Builds, waehrend die bisherigen PDBs gegen Schreiben gesperrt bleiben.
Der laufende Editor kann geoeffnet bleiben; Projektquellen werden nicht geaendert.

Im Game-Bild erscheinen keine Auswahl-Outlines. Beim Oeffnen des Editors,
bei Stop und bei Pause wird der Viewport aktiv; Play und Fortsetzen aktivieren
Game. Der Inspector zeigt waehrend Play/Pause aktuelle Script-Feldwerte wie
spawnCount. Stop stellt die vorherigen Edit-Werte wieder her.

Die optionalen Editor-Testprogramme pruefen diese Reiterwechsel und Live-Werte
mit --play-ui, einschliesslich eines zuvor auf Game gespeicherten Dock-Layouts.
