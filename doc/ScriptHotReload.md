# C++-Scripts beim Start nachladen (Windows)

Der Editor laedt Projekt-Scripts aus `krom_scripts_<runtimeTarget>.dll`.
Beim Oeffnen eines Projekts wird eine vorhandene kompatible DLL geladen, wenn
ihre eingebettete Projektkennung zum geoeffneten Projektordner gehoert;
ohne DLL bleiben die Scripts als Platzhalter mit ihren Feldwerten erhalten.
Alte DLLs ohne Projektkennung und DLLs anderer Projekte werden abgewiesen.
Ein erneuter Script-Build erstellt eine passende DLL, auch nach einem Umzug
des Projektordners.

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

Runtime und Export binden Projekt-Scripts weiterhin statisch ein.

## Projekte an beliebigen Speicherorten

Projekte koennen ausserhalb des Engine-Quellbaums liegen, auch in Pfaden mit
Leerzeichen und Umlauten. Script-Build, Play, Testbuild und Export verwenden
den aktiven Projektordner. `projects/` bleibt der vorgeschlagene Ablageort.

Beim ersten Build und beim Wechsel der Projektauswahl im jeweiligen
Buildverzeichnis konfiguriert der Editor CMake im Hintergrund neu. Das gilt
auch beim Wechsel zurueck zu einem Projekt unter `projects/`. Die Konfiguration
dauert normalerweise Sekunden und behaelt bestehende Engine-Objektdateien;
ein erstmals verwendetes Buildprofil muss seine Engine-Libraries jedoch einmal
bauen. Ohne Projektwechsel wird kein zusaetzlicher CMake-Aufruf eingeplant.
Ninja kann CMake weiterhin bei geaenderten Builddateien oder Scriptlisten starten.

Fuer manuelle Builds waehlt `-DKROM_PROJECT_ROOT=<Projektordner>` ein zusaetzliches
Projekt neben allen Projekten unter `projects/`. `KROM_PROJECT_SCRIPT_ROOT`
bleibt als Uebergangsname fuer bestehende Konfigurationen erhalten; der neue
Name hat Vorrang, auch bei einem ausdruecklich leeren Wert. Zum Entfernen der
externen Auswahl `-DKROM_PROJECT_ROOT=` uebergeben.
Ein nachtraeglicher, abweichender Wert unter dem alten Namen wird mit einer
Warnung ignoriert. Normale Projektwechsel ueber den neuen Namen erzeugen keine
solche Warnung.

Wurde ein ausgewaehltes Projekt verschoben, umbenannt oder geloescht und fehlt
seine `krom-project.json`, warnt CMake und leert beide Cache-Namen. Der Editor
kann danach auch aus Visual Studio weiter gebaut werden, ohne den alten Pfad
von Hand zu entfernen. Ein weiterhin vorhandenes Projekt am neuen Ort muss
erneut geoeffnet bzw. mit `KROM_PROJECT_ROOT` ausgewaehlt werden.

Die Engine-Wrapper-Beispiele (etwa `load_3d_model_*`) binden keine Scripts des
aktiven Projekts ein. Nur das generische `editor_scene_runtime_*` behaelt den
festen DemoProject-Standard aus dem Quellbaum. Fuer eigene Projekte sind die
projektspezifischen `krom_runtime_<runtimeTarget>_<backend>`-Targets vorgesehen.

Jeder Projektordner wird ueber seinen echten Pfad nur einmal eingebunden.
Verschiedene Projekte brauchen eindeutige `runtimeTarget`-Werte; auch Namen
wie `A-B` und `A_B` ergeben dasselbe Target. Bei einer Kollision bricht CMake
mit beiden Projektpfaden und dem Target-Namen ab. In einer der Projektdateien
einen eindeutigen `runtimeTarget` setzen oder zu einer konfliktfreien Auswahl
wechseln und erneut bauen. Der sichtbare Projektname und Ordner koennen bleiben.

Diese Unterstuetzung setzt aktuell einen vorhandenen Engine-Quellbaum mit
Buildumgebung voraus. Ein weitergebbares Editor-SDK mit vorgebauten Libraries,
Headern, Ressourcen und Projektvorlagen ist der folgende Paketierungsschritt.

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
Es prueft auch, dass DLLs mit falscher oder fehlender Projektkennung die
bereits geladene passende DLL nicht ersetzen.
Die optionalen Ziele `krom_editor_loading_tests` und
`krom_editor_loading_tests_dx11` pruefen mit
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

`--external-script-reload <krom-project.json>` fuehrt denselben Script-/Play-Test
in einem temporaeren externen Ordner mit Leerzeichen und Umlaut aus und stellt
vor dem Aufraeumen die urspruengliche CMake-Projektauswahl wieder her.
`--external-project-export <krom-project.json>` prueft danach auch Testbuild,
Runtime-Start, Release-Export mit Shader-Warm-up und die Rueckkehr zum DemoProject.
`--external-export-only` mit demselben Argument beginnt direkt mit dem Runtime-Build,
damit auch der Ablauf ohne einen vorherigen Script-DLL-Build geprueft wird.
Fuer den Exporttest muss der Release-Buildordner entweder vorbereitet
sein oder seine optionalen Engine-Abhaengigkeiten selbst beziehen koennen.
`python tests/test_project_roots.py --cmake <cmake.exe> --ninja <ninja.exe>`
prueft CMake-Projektauswahl, Pfad-Deduplizierung, Kollisionen, Fehlerbehebung
und Migration ohne Engine-Build.
