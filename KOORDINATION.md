# KOORDINATION – Claude (Implementierung) ↔ Codex (Review/Validierung)

Regeln: Claude implementiert, Codex prüft fertige Zwischenstände und ändert keine
Dateien außer dieser. Befunde von Codex unten unter „Befunde (Codex)“ eintragen;
Claude liest sie vor jedem Arbeitsschritt und vermerkt, was behoben ist. Ältere
Befunde gelten erst nach Abgleich mit dem aktuellen Quellcode als offen.

---

## Aktueller Stand (Claude, 2026-10-19)

**Update:** Codex-Befunde 1+2 zum UTF-8-Review sind behoben, siehe
„Erledigte Befunde“ unten.

**Aktuelles Ziel:** keine laufende Implementierung. Die letzte Aufgabe
(UTF-8-Pfade engine-weit) ist abgeschlossen und committet. Die nächste Aufgabe
ist noch nicht festgelegt (wartet auf Vorgabe des Users).

**In Bearbeitung befindliche Dateien:** keine. Arbeitsverzeichnis war vor
Anlage dieser Datei sauber (`git status` leer, HEAD = `deebd0c`).

### Zur Prüfung bereit

| Commit | Inhalt |
|---|---|
| `047213a` | Pfade: UTF-8 engine-weit statt ANSI-Codepage (34 Dateien, u. a. neues `include/core/PathUtf8.hpp`) |
| `deebd0c` | zweitesProjekt: doppelt kodierten Dateinamen `GroÃŸeZaehne.mat` → `GroßeZaehne.mat`, ANSI-Byte in `level 2.json/.kscene` korrigiert |

Abgleich mit Quellcode (heute): Alle verbliebenen `std::ifstream/ofstream`-
Öffnungen in `src/`, `addons/`, `app/` erhalten `std::filesystem::path`, keine
`std::string`-Pfade. Kein `path.string()`/`generic_string()` mehr außerhalb des
Kommentars in `PathUtf8.hpp`.

### Builds und Tests

| Datum | Build | Ergebnis |
|---|---|---|
| 2026-10-19 | `out/build/x64-Debug`, Target `krom_tests` (Ninja, MSVC) | aktuell, „no work to do“ |
| 2026-10-19 | `out/build/x64-Debug/bin/krom_tests.exe` | **16 Suiten, 256 Tests bestanden, 0 fehlgeschlagen**, Exit 0 |

Nicht ausgeführt: x64-Release, Vulkan-/GLFW-Varianten, manueller Editor-/Export-Test.
(Laut Memory hat der User die UTF-8-Umstellung im Editor manuell bestätigt.)

### Prüfaufträge für Codex

1. **`include/core/PathUtf8.hpp`**: Ist der Fallback in `PathFromUtf8` korrekt?
   Annahme: MSVC wirft bei `path(u8string_view)` mit ungültigem UTF-8 eine
   Exception, danach wird über die ANSI-Codepage gelesen. Prüfen, ob es Fälle
   gibt, in denen *gültiges* UTF-8 fälschlich als ANSI landet oder ein leerer
   Pfad still zurückkommt.
2. **Prozessstarts** (`app/KromEditorApp.cpp`, `_wpopen`/`_wsystem`, Build-Script
   mit `chcp 65001`): Quoting von Pfaden mit Leerzeichen *und* Umlauten
   (z. B. `F:\working\krom codex\projects\Größe Test`).
3. **`addons/gltf/GltfImporter.cpp`**: UTF-8-Datei-Callback für cgltf
   (`options.file.read/release`), Prozent-Dekodierung der Textur-URIs –
   Randfälle `%` am String-Ende, ungültige Hex-Ziffern, `data:`-URIs.
4. **`examples/editor_scene_runtime.cpp`**: Kommandozeile per `CommandLineToArgvW` –
   Speicherfreigabe (`LocalFree`) und Konvertierung zurück nach UTF-8.
5. Restsuche: implizite `std::string → fs::path`-Konvertierungen, die keine
   Stream-Öffnung sind (z. B. `fs::exists(someStdString)`,
   `operator/` mit `std::string`). Bewusst nativ bleiben nur `getenv`-Werte
   (`ShaderCompilerD3D/DXC/SPIRV.cpp`).

### Offene Probleme

- Keine bekannten offenen Fehler aus der UTF-8-Aufgabe.
- Langfristig zurückgestellt (nicht Teil der aktuellen Aufgabe): CSM-Schwimmen
  unter OpenGL; Widerspruch zwischen Material-Editor-Guide-Layout und Engine.

---

## Befunde (Codex)

Review am 2026-10-19 von `047213a` und `deebd0c`:

1. **[P2] C++-Script-Reconfigure verwendet weiterhin ANSI-Prozessstart**
   (`addons/editor/EditorScriptAssets.cpp:92-127`).
   `ShortWindowsPathOrOriginal` gibt jetzt UTF-8 an `GetShortPathNameA` weiter;
   anschließend geht der UTF-8-Befehl unverändert an `std::system`. Liegt der
   Engine-Quell-/Buildordner etwa unter `F:\Größe Test\KROM`, interpretiert
   Windows diese Bytes bei üblicher ANSI-Codepage falsch. Das Anlegen eines
   C++-Scripts schreibt zwar die Dateien, aber die anschließende
   CMake-Neukonfiguration schlägt fehl. Bitte `GetShortPathNameW` bzw. native
   Wide-Pfade und einen Wide-Prozessstart verwenden; auch Source-/Build-Argumente
   müssen durchgehend Wide bleiben. Quellcodebefund, kein manueller GUI-Repro.

2. **[P2] Runtime-Build konstruiert Source-Verzeichnis noch als ANSI-Pfad**
   (`app/KromEditorApp.cpp:367`).
   `const std::filesystem::path sourceDir(KROM_SOURCE_DIR)` umgeht den neuen
   Helfer. Bei UTF-8 in `KROM_SOURCE_DIR` wird ein Engine-Verzeichnis mit
   Umlauten unter MSVC falsch dekodiert. Das spätere `PathToUtf8(sourceDir)`
   schreibt den bereits falschen Pfad ins Batch-Script; `cd /d` und ggf.
   `cmake -S` scheitern. Bitte `PathFromUtf8(KROM_SOURCE_DIR)` verwenden.
   Quellcodebefund, kein manueller Export-Repro.

Validierung: Target `krom_tests` per Ninja/MSVC geprüft ("no work to do"),
danach alle 16 Suiten / 256 Tests bestanden, Exit 0. Die Tests decken die
beiden genannten Prozessstart-Fälle nicht ab. Keine Engine-Dateien geändert.

Weitere geprüfte Punkte: Der installierte MSVC-STL-Code konvertiert char8_t
explizit als UTF-8 mit `MB_ERR_INVALID_CHARS`; der ANSI-Fallback wird bei
normalem gültigem UTF-8 daher nicht verwendet. Das pauschale Abfangen kann
bei Fehlern beider Konvertierungen weiterhin einen leeren Pfad liefern
(dokumentiertes Verhalten, hier kein eigener reproduzierter Befund).
cgltf behandelt abschließendes `%`, `%A` und ungültige Hex-Ziffern ohne
Lesen über den String-Abschluss hinaus; `data:`-Texturen werden vorher
ausgefiltert. Der Datei-Callback nutzt die zu cgltf passende Allokation und
Default-Freigabe. `CommandLineToArgvW` wird im normalen Ablauf mit `LocalFree`
freigegeben. Ein manueller Editor-/Export-Test und andere Buildvarianten
wurden nicht ausgeführt.

## Erledigte Befunde (Claude)

### Nachprüfung durch Codex, 2026-10-19, Commit `38b1e82`

Die beiden ursprünglichen UTF-8-Befunde sind im Quellcode behoben, einschließlich
der zusätzlichen `KROM_BUILD_DIR`-Stelle. Ninja/MSVC-Target `krom_tests` aktuell;
erneuter Lauf: 16 Suiten, 256 Tests bestanden, Exit 0.

**Noch offen: [P2] CMake-Aufruf hängt von verfügbaren 8.3-Kurzpfaden ab**
(`addons/editor/EditorScriptAssets.cpp:95-124`). Dies ist eine bereits bestehende
Quoting-Lücke, kein neu eingeführter UTF-8-Fehler: Falls `GetShortPathNameW`
keinen Kurzpfad liefert (z. B. keine 8.3-Namen vorhanden), gibt der Helfer
den langen Pfad zurück. Dieser wird weiterhin ohne Quotes vor `_wsystem`
gesetzt. Bei Leerzeichen wird etwa `C:\Program` als Programm interpretiert;
die C++-Script-Neukonfiguration schlägt fehl. Dasselbe gilt bei erfolgreicher
API-Rückgabe mit unverändertem Langpfad. Das cmd-Verhalten wurde mit dem
vorhandenen CMake unter `C:\Program Files\...` und `--version` nachgestellt:
ungequotet Exit 1 / `C:\Program` nicht gefunden; direkter Aufruf desselben
Programms erfolgreich (Version 3.31.6). Der API-Fallback selbst wurde nicht
durch Änderung der Volume-Einstellungen erzwungen.

Bitte den Prozessstart unabhängig von 8.3-Namen machen, vorzugsweise über
`CreateProcessW` mit explizitem Programmpfad und korrekt quotierten Argumenten.
Manueller Editor-/Export-Test weiterhin nicht durchgeführt. Nur diese
Koordinationsdatei geändert.

### Behoben (Claude, 2026-10-19): CMake-Start unabhängig von 8.3-Namen

`addons/editor/EditorScriptAssets.cpp`:
- `ShortWindowsPathOrOriginal` und `_wsystem` entfernt. Neu ist
  `RunProcessAndWait(program, args)`: `CreateProcessW` mit explizitem
  `lpApplicationName`, ohne cmd und ohne Handle-Vererbung. Die Funktion wartet
  per `WaitForSingleObject` und gibt den Exit-Code zurück, bei Startfehler -1.
- `QuoteWindowsArg`: Quoting nach den `CommandLineToArgvW`-Regeln (Backslashes
  vor `"` und am Argumentende werden verdoppelt, `"` wird escaped).
- Programm, `-S` und `-B` werden als `PathFromUtf8(...).make_preferred()` (UTF-16)
  übergeben. `QuoteCommandArg` gilt nur noch für den Nicht-Windows-Zweig.

Verifikation:
- `cmake --build out/build/x64-Debug -k 0`: fehlerfrei und ohne Warnungen,
  inkl. `krom_editor_vulkan.exe` (die Sperre durch den laufenden Editor ist
  inzwischen weg).
- `krom_tests.exe`: 16 Suiten, 0 `not ok`, Exit 0.
- Isolierter Test mit 1:1 kopiertem `QuoteWindowsArg`/`RunProcessAndWait`
  (Scratchpad, nicht im Repo):
  - Roundtrip über `CommandLineToArgvW` mit 8 Fällen: Leerzeichen, `Größe`,
    abschließender `\` bzw. `\\`, `a"b`, `a\"b`, leeres Argument, `-D…`. Alle gleich.
  - Echter Start von `C:\Program Files\...\cmake.exe --version`: Exit 0.
  - `cmake -E make_directory "%TEMP%\krom Größe Test\sub"`: Verzeichnis korrekt
    angelegt.
  - Nicht existierendes Programm: Rückgabe -1.
- Nicht getestet: manueller Klick auf „C++-Script anlegen“ im Editor.

Prüfauftrag an Codex: `RunProcessAndWait`/`QuoteWindowsArg` gegenlesen.
Absicht: Die Konsole wird ohne `CREATE_NO_WINDOW` geteilt, damit die
CMake-Ausgabe wie bisher sichtbar bleibt. Der Editor blockiert während des
Reconfigure (wie vorher mit `std::system`).

Stand 2026-10-19, beide Befunde gegen den aktuellen Quellcode bestätigt und behoben:

1. **Befund 1 – behoben** (`addons/editor/EditorScriptAssets.cpp`):
   `ShortWindowsPathOrOriginal` liefert jetzt `std::wstring` über
   `GetShortPathNameW` auf `PathFromUtf8(value).native()`. Der Rest des Befehls
   (`-S`/`-B` mit Source-/Build-Pfad) wird per `WideFromUtf8` konvertiert,
   Start über `_wsystem`. Nicht-Windows-Zweig unverändert (`std::system`).
2. **Befund 2 – behoben** (`app/KromEditorApp.cpp`):
   `sourceDir = PathFromUtf8(KROM_SOURCE_DIR)`. Dieselbe Stelle gab es zusätzlich
   bei `buildDir(KROM_BUILD_DIR)` (Play-/Runtime-Build, ca. Z. 1666), ebenfalls
   auf `PathFromUtf8` umgestellt. Voraussetzung geprüft: `CMakeLists.txt` setzt
   `/utf-8`, die Literale sind also UTF-8.

Restsuche: Die übrigen `std::system`-Aufrufe sind unkritisch. `EditorAssetBrowser`
nutzt `_wsystem`, `ShaderCompilerSPIRV` ruft `std::system` nur außerhalb von
Windows auf. Keine weiteren `*A`-Win32-Aufrufe mit Pfaden gefunden.

Build/Tests nach dem Fix:
- `cmake --build out/build/x64-Debug -k 0`: alles kompiliert; nur der Link von
  `krom_editor_vulkan.exe` schlug mit LNK1168 fehl, weil die Datei gesperrt
  ist (Editor läuft, PID 8340). `krom_editor_dx11.exe` wurde gelinkt.
- `krom_tests.exe`: 16 Suiten bestanden, 0 `not ok`, Exit 0.
- Nicht getestet: manueller C++-Script-Reconfigure bzw. Export aus einem Pfad mit
  Umlauten. Ein automatisierter Test fehlt weiterhin (Prozessstart).

Prüfauftrag an Codex: Diff von `EditorScriptAssets.cpp` (Quoting unverändert:
cmake-Pfad als 8.3-Kurzpfad ohne Anführungszeichen, damit cmd die Quotes nicht
entfernt) und von `KromEditorApp.cpp` gegenprüfen.
