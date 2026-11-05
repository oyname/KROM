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

## Build-Werkzeuge

Der Editor sucht Visual Studio einschliesslich der eigenstaendigen Build Tools
ueber `vswhere`. Verwendet wird die neueste Installation mit den x64-C++-Tools;
Edition und Installationslaufwerk sind nicht festgelegt. CMake und Ninja werden
zuerst dort und anschliessend im `PATH` gesucht. Die Windows-SDK-Komponente
muss fuer C++-Builds ebenfalls installiert sein.

Die Visual-Studio-Entwicklungsumgebung wird einmal geladen und zusammen mit
den gefundenen Werkzeugen fuer die Editor-Sitzung gespeichert. CMake und Ninja
starten direkt mit dieser Umgebung, Argumenten und Arbeitsverzeichnis;
es werden keine `.cmd`-Build-Skripte mehr erzeugt.

Vor dem Build wird die tatsaechliche MSVC-Version abgefragt. Fuer Script-DLLs
gilt weiterhin die exakte Compiler-Version der laufenden Editor-EXE.
Ein neuerer Compiler lockert diese ABI-Pruefung noch nicht. Runtime-Builds
akzeptieren dieselbe oder eine neuere Version innerhalb derselben MSVC-Familie.
Verweist ein vorhandener CMake-Cache auf andere Compiler-/Ninja-Dateien oder
einen anderen Generator, wird er mit einer Meldung abgewiesen. Der Editor
wechselt die Werkzeuge eines vorhandenen Build-Ordners nicht stillschweigend.

## Automatisierte Pruefung

`krom_build_tools_tests` prueft Werkzeugsuche, PATH-Fallback, Compiler-Abgleich,
Cache-Konflikte, native Prozessargumente, Unicode-Umgebung, Abbruch und Timeout.
Auf Windows konfiguriert und baut es ausserdem ein kleines C++-Projekt in einem
Pfad mit Leerzeichen und Umlaut mit den wirklich gefundenen Werkzeugen.

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
