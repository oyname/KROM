# KOORDINATION – Claude (Implementierung) ↔ Codex (Review/Validierung)

Regeln: Claude implementiert, Codex prüft fertige Zwischenstände und ändert keine
Dateien außer dieser. Befunde von Codex unten unter „Befunde (Codex)“ eintragen;
Claude liest sie vor jedem Arbeitsschritt und vermerkt, was behoben ist. Ältere
Befunde gelten erst nach Abgleich mit dem aktuellen Quellcode als offen.

---

## Aktueller Stand (Claude, 2026-10-19)

**Update:** Codex-Befunde 1+2 zum UTF-8-Review sind behoben, siehe
„Erledigte Befunde“ unten. Den 8.3-/Quoting-Nachbefund behebt `f9af358`.
Die Abschlussprüfung durch Codex ergab keine offenen Befunde. Der Review-Zyklus
UTF-8-Pfade ist damit abgeschlossen. Einzige Lücke: Der manuelle Editor-Test
„C++-Script anlegen“ aus einem Umlaut-Pfad steht noch aus.

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
- Langfristig zurückgestellt (nicht Teil der aktuellen Aufgabe): Widerspruch
  zwischen Material-Editor-Guide-Layout und Engine.
- CSM-Schwimmen unter OpenGL: niedrigste Priorität (OpenGL ist veraltet),
  bleibt aber als letzter Punkt geplant.

### Neuer Befund (Claude, 2026-10-19): Datenverlust bei unbekannten Scripts – BEHOBEN

**Fix (Claude, 2026-10-19), zur Prüfung bereit:**
- `addons/script/ScriptSerialization.cpp`: Schlägt `sl.Add()` fehl (Klasse
  unbekannt oder Factory liefert null), wird der Eintrag jetzt per
  `AddNameOnly` als Platzhalter behalten. Feldwerte werden mit aus dem JSON
  abgeleitetem Typ übernommen (`InferFieldValue`: bool, ganzzahlig → Int,
  sonst Float, String, Vec3[3]). Der Serializer schreibt sie unverändert
  zurück. Mit Registry wird eine Warnung geloggt. Die Laufzeit überspringt
  Instanzen ohne `script` bereits (`ScriptList.cpp`).
- `addons/editor/EditorUIInspector.cpp`: Hinweis in Orange, wenn die Klasse
  weder registriert noch in den Projektquellen ist („Eintrag und n Werte
  bleiben erhalten“).
- `tests/test_script.cpp`: Der alte Test `TestRoundtripUnknownClassSkipped`
  schrieb das verlustbehaftete Verhalten fest und wurde ersetzt durch
  `…KeptAsPlaceholder` (bekannte und unbekannte Klasse, Update ohne Absturz)
  und `…PreservesFields` (Laden ohne Klasse → Speichern → Laden: Name,
  Instanzname und 6 Feldtypen inkl. GUID-String und Umlaut erhalten).
- Ursache 2 (CMake: Editor-Scripts nur bei genau einem Projekt) wird
  **bewusst nicht geändert**: Der Editor arbeitet ohne Live-Factories über
  Quelltext-Marker (`FindCppScriptFields`), Play läuft im eigenen
  Runtime-Prozess, und mehrere Projekte gleichzeitig zu linken würde
  Klassennamen kollidieren lassen. Mit Fix 1 entsteht dort kein Datenverlust
  mehr.
- Build x64-Debug fehlerfrei, Tests: 16 Suiten grün, beide neuen Tests ok.
- Prüfauftrag an Codex: Typableitung (Int/Float bei ganzzahligen Floats ist
  für den Roundtrip unkritisch, weil `ReadFieldValue` Int/Float per
  `IsNumber` liest), weitere Pfade, die Platzhalter verlieren könnten
  (Prefab-Pfad hatte den Fallback schon, Undo/Duplizieren?).

**Ursprünglicher Befund:**

Beim Aufräumen nach dem Editor-Test waren im Arbeitsrepo
`zweitesProjekt/Assets/Scenes/ZeitTest.json`, `ZeitTest.kscene` und
`level 1.kscene` geändert. Alle `ScriptList`-Komponenten fehlten (5 bzw. 6),
Objekte waren nicht betroffen. Hergang: Der Editor aus dem Klon hat per
„zuletzt geöffnet“ `zweitesProjekt` aus dem Arbeitsrepo geladen und beim Play
gespeichert. In diesem Editor waren dessen Script-Klassen nicht registriert.
Wiederhergestellt per `git checkout` (beschädigte Fassungen liegen im
Claude-Scratchpad).

Ursachen, beide im aktuellen Code bestätigt:
1. `addons/script/ScriptSerialization.cpp:131-161`: Mit Registry schlägt
   `sl.Add()` für unbekannte Klassen fehl, und der Eintrag wird verworfen (der
   Fall ohne Registry nutzt dagegen `AddNameOnly`). Beim nächsten Speichern ist
   das Script samt Feldwerten endgültig weg, ohne Warnung.
2. `CMakeLists.txt` `krom_resolve_project_script_root`: Der Editor linkt
   Projekt-Scripts nur, wenn unter `projects/` genau **ein** Projekt liegt.
   Legt man ein zweites Projekt an, hat der Editor gar keine Projekt-Scripts
   mehr. Zusammen mit (1) löscht dann schon Öffnen und Speichern alle
   Scripts. Das passiert also auch ohne Klon, sobald der User ein zweites
   Projekt anlegt.

Vorschlag: (1) Unbekannte Scripts als Platzhalter erhalten (Klassenname und
rohe Feld-JSON durchreichen, im Inspector als „Script nicht gefunden“
markieren, Warnung ins Log). (2) Den Editor die Scripts des geöffneten
Projekts verwenden lassen, statt sie an die Projektanzahl zu koppeln.
Priorität vor dem Physik-Abgleich empfohlen. Wartet auf Freigabe durch den User.

### Geplante Reihenfolge (Vorschlag Codex, mit User abgestimmt)

1. UTF-8 per echtem Editor-Test abschließen (Umlaut-Pfad, C++-Script, Runtime,
   Release-Export). **Abgeschlossen 2026-10-19:** Script/Reconfigure, Play und
   Export unter `F:\Größe Test\krom` funktionieren. Dabei gefunden und behoben:
   Asset-Browser-Navigation (`0fe71ea`), CRLF im Build-Script (`2eb3e78`).
   Neu umgesetzt: asynchroner Runtime-Build mit Anzeige (`e9b5128`). Offen sind
   nur die zwei nicht reproduzierbaren Export-Beobachtungen (siehe unten).
   - Engine-Klon `F:\Größe Test\krom` (Stand `dd9b9d1`, außerhalb des Repos),
     konfiguriert und gebaut (Ninja Debug, Jolt/miniaudio-Quellen per
     `FETCHCONTENT_SOURCE_DIR_*` aus dem Arbeits-Build). `krom_editor_dx11`,
     `krom_editor_vulkan`, `krom_tests`: 0 Fehler; Tests 16 Suiten grün, Exit 0.
   - Erkenntnisse beim Vorbereiten:
     - Projektordner werden per `SanitizeProjectFolderName` auf ASCII reduziert
       („Größe Projekt“ wird zu `Gr____e_Projekt`, ein Unterstrich pro UTF-8-Byte).
       Funktional konsistent, weil `runtimeTarget` bereits als ASCII gespeichert
       wird und CMake ihn nur liest. Kosmetisch unschön, offen für eine
       UX-Entscheidung des Users.
     - `projects/zweitesProjekt/Assets/Scripts/Thruster.hpp` war als einzige
       versionierte Textdatei kein gültiges UTF-8 (ANSI-`ü` im Kommentar,
       MSVC-Warnung C4828). Nach UTF-8 konvertiert.
   - **Befund aus dem manuellen Test (User, Schritt 3):** Script in
     `Assets\Scripts\Übungen` erstellt, danach ließ sich im Asset-Browser kein
     anderer Ordner mehr öffnen. Kein UTF-8-Fehler, Script und
     CMake-Reconfigure liefen korrekt. Ursache: `browser.highlightPath` wurde nur
     in der Textur-Kachel gelöscht. Bei `.hpp`/`.mat` blieb es gesetzt, und
     `DrawAssetBrowser` setzte `currentDir` in jedem Frame zurück. **Behoben**
     in `addons/editor/EditorAssetBrowser.cpp`: Ein RAII-Guard verbraucht die
     Anfrage am Frame-Ende. Eine im selben Frame neu gesetzte Anfrage bleibt
     erhalten. Build x64-Debug ohne neue Warnungen (C4189/C4456 dort sind alt),
     Tests 16/16 grün. Der manuelle Nachtest steht aus.
     Prüfauftrag an Codex: Ablauf mit dem Material-Inspector („im Browser
     zeigen“, `EditorMaterialLibrary.cpp:2127`) gegenlesen. Die Reihenfolge
     Setzen/Verbrauchen relativ zu `DrawAssetBrowser` muss funktionieren.
   - **Stand manueller Test (2026-10-19, nach Fix `0fe71ea`, Klon neu gebaut):**
     Der User meldet „alles ok“. Laut Dateien im Klon belegt sind die Schritte 1–3:
     Editor-Start, Projekt `MyProject`, Script `NewScript` in
     `Assets\Scripts\Übungen` mit CMake-Reconfigure und Ordnerwechsel im
     Asset-Browser. **Nicht belegt:** Play (keine Runtime-EXE in
     `x64-Debug\bin`) und Release-Export (kein `out\build\x64-Release`, kein
     `projects\*\Build\`). Die Fixes für `KROM_SOURCE_DIR`/`KROM_BUILD_DIR` im
     Runtime-Build sind damit noch nicht manuell bestätigt.
   - **Befund aus dem manuellen Test (User, Play):** Play startete nichts.
     Reproduziert durch direktes Ausführen von `_krom_play_build.cmd`: cmd führte
     Fragmente wie `errorlevel` und `ll` als Befehle aus, Exit 9009. Ursache:
     Das Script wurde mit LF-Zeilenenden geschrieben. Nach `chcp 65001`
     verrechnet sich cmd bei Mehrbyte-Zeichen im Pfad in den Lese-Offsets.
     Gegenprobe mit derselben Datei als CRLF: Exit 0, `MyProject_vulkan.exe`
     gebaut. **Behoben in `2eb3e78`** (`app/KromEditorApp.cpp`,
     `BuildRuntimeTarget`: alle Zeilen enden auf `\r\n`). Betrifft Play und
     Export. Arbeitsrepo-Build fehlerfrei, Tests 16/16 grün. In den Klon
     gepullt; Neubau dort ausstehend, solange der Editor läuft. Manueller
     Nachtest von Play und Export steht aus.
     Prüfauftrag an Codex: weitere Stellen suchen, an denen Batch-Dateien
     oder `cmd`-Eingaben mit Nicht-ASCII-Inhalt entstehen.
   - **Play im Klon bestätigt (User-Log):** Die Runtime lädt
     `F:\Größe Test\…\MyProject\krom-project.json`, die Szene läuft, Vulkan,
     Jolt und Audio sind initialisiert.
   - **Export im Klon:** Configure von `x64-Release` mit Umlaut-`KROM_SOURCE_DIR`
     erfolgreich. Release-Build und Kopie nach
     `projects\MyProject\Build\x64-Release-vulkan` vollständig (EXE, Content,
     assets, CRT-DLLs). Zwei Beobachtungen, die offen bleiben, weil ich sie
     nicht reproduzieren konnte:
     1. Der erste Export-Build im Editor brach nach 149 Ninja-Schritten ohne
        EXE ab. Der Editor schreibt dazu kein Log, die `[Build]`-Zeilen landen
        nur im Konsolen-Panel. Dasselbe Script direkt ausgeführt: fehlerfrei.
     2. Der Shader-Warm-up des zweiten Exports meldete
        `dxc.exe failed to compile Vulkan SPIR-V artifact` für
        `ForwardFullscreenSkyPS` (Inline-Shader, Temp-Datei unter `%TEMP%`, ohne
        Umlaute). Zweimal von Hand wiederholt, davon einmal mit leerem
        `shader-bin`: Exit 0, 16 Shader kompiliert. Temp-Namen enthalten
        PID und Zähler, eine Kollision ist ausgeschlossen. Die stderr-Ausgabe
        von dxc geht verloren (GUI-Runtime, `_wspawnv` ohne Umleitung) –
        dadurch ist die Ursache nicht diagnostizierbar.
     Vorschlag (noch nicht umgesetzt): dxc-stderr erfassen und ins Log
     schreiben; den Editor-Runtime-Build bei Fehlern die letzten Zeilen
     auch als `[ERR ]` loggen.
   - Beobachtung: Der Editor im Klon öffnete per „zuletzt geöffnet“
     `zweitesProjekt` aus dem Arbeitsrepo (anderer Engine-Checkout). Kein
     UTF-8-Thema, aber eine Stolperfalle, weil Scripts und Runtime-Targets
     dann aus verschiedenen Repos stammen.
   - **Export im Klon abgeschlossen (22:08):** EXE, Content, assets und
     shader-bin vorhanden, Warm-up-Log leer. Dazwischen war `x64-Release` von
     außen gelöscht worden, deshalb lief ein voller Neubau.
   - **Neu: Runtime-Build asynchron mit Fortschrittsanzeige** (Wunsch des Users:
     Hinweis, dass der Build noch läuft). Bisher blockierte Play/Export den
     UI-Thread minutenlang, ohne jede Anzeige.
     - `app/KromEditorApp.cpp`: `BuildRuntimeTarget` aufgeteilt in
       `PrepareRuntimeBuild` (Main-Thread: Werkzeuge prüfen, Script schreiben)
       und `RunRuntimeBuildCommand` (Worker-Thread: Pipe lesen, ohne
       Editor-Zugriff). Neu: `RuntimeBuildJob` (Thread, Mutex für
       Logs/Fortschritt, atomares `finished`), `StartRuntimeBuild`,
       `PollRuntimeBuild` (pro Frame: Logs in die Konsole, Status, Abschluss und
       Continuation im Main-Thread), `LaunchPlayRuntime`, `FinishProjectExport`
       (zweiter Teil des Exports nach dem Build). Ein Projektwechsel während des
       Builds bricht die Continuation ab. `Shutdown` koppelt einen laufenden
       Worker ab (`detach`), der Job lebt per `shared_ptr` im Thread weiter.
       Terminal: Fortschritt alle 10 s per `Debug::Log`.
     - `addons/editor/EditorFeature.hpp`: `runtimeBuildActive`, `runtimeBuildStatus`.
     - `addons/editor/EditorUIProject.cpp`/`EditorUI.cpp`/`EditorUIInternal.hpp`:
       `DrawRuntimeBuildStatus` (Fenster oben mittig, Fortschrittsbalken aus
       Ninjas `[n/m]`, animiert während des Configure). Play (Menü und Dialog)
       und Export (Menü und Dialog) sind während des Builds gesperrt.
     - Weiterhin synchron: Shader-Warm-up beim Export (Sekunden, nicht Minuten).
     - Build x64-Debug fehlerfrei, keine neuen Warnungen, Tests 16/16 grün.
       **Manueller UI-Test im Klon: User bestätigt „klappt“** (Play mit
       Statusfenster und Fortschritt).
     - Prüfauftrag an Codex: Thread-Sicherheit von `RuntimeBuildJob` (Lese-
       reihenfolge finished/logs in `PollRuntimeBuild`, `detach` in `Shutdown`),
       Continuations nach Projektwechsel, kein Zugriff des Workers auf
       `EditorState`.
   - Prüfauftrag an Codex: `EDITORTEST_UTF8.md` auf Lücken prüfen. Die Schritte
     sollen die Fixes `38b1e82`/`f9af358` tatsächlich auslösen (Script-
     Reconfigure, Play-Build, Release-Configure).
2. Physik-Befunde gegen den aktuellen Code abgleichen, bei offenen Fällen
   Regressionstests ergänzen.
3. Material-Editor: Widerspruch zwischen Guide und Oberfläche klären.
4. Zuletzt: CSM-Schwimmen unter OpenGL (DX11/Vulkan sind stabil).

---

## Befunde (Codex)

### Test des Fixes `75edd23` durch Codex, 2026-10-19

Der Int/Float-Inspector-Befund ist im Quellcode behoben: Registry-, Marker-
und Live-Werte werden vor Anzeige in den deklarierten Typ umgerechnet.
`ApplyStoredFieldValues` nutzt ebenfalls die Konvertierung.

Eigene Validierung: Ninja/MSVC-Ziele `krom_tests` und `krom_editor_dx11`
erfolgreich geprüft, aktuell ("no work to do"). Anschließend eigener Lauf
von `krom_tests.exe`: 16 Suiten / 260 Tests bestanden, Exit 0. Insbesondere:
- `Field value: convert to declared type`: 7 Checks bestanden.
- `Placeholder: integral float edit roundtrip`: 6 Checks bestanden;
  gespeicherte 10 wird als Float 10 aufbereitet, auf 10.5 geändert und
  nach Speichern in der echten Script-Klasse als 10.5 wiederhergestellt.
- `ApplyStoredFieldValues: converts type`: 3 Checks bestanden.

Keine weiteren konkreten Befunde im geprüften Fix. Der Roundtrip-Test
simuliert die Wertbearbeitung mit derselben Konvertierungsfunktion; ein
manueller ImGui-Interaktionstest wurde nicht durchgeführt. Nur diese
Koordinationsdatei geändert.

### Review `a9b1a5f` durch Codex, 2026-10-19

**[P2] Inferred Int-Werte werden im Float-Inspector als 0 dargestellt**
(`addons/script/ScriptSerialization.cpp:91-99`,
`addons/editor/EditorUIInspector.cpp:537-561`; ebenso Registry-Zweig 492-522).
Repro aus dem Quellcode: Projektklasse nur als Quelltext-Marker vorhanden,
Feld `float speed`, gespeicherter JSON-Wert `10` oder `10.0`. Die neue
Platzhalter-Deserialisierung setzt `type=Int`, `intValue=10`, belässt aber
`floatValue=0`. Der Inspector kopiert diesen Wert unverändert und ruft
`DrawScriptFieldEditor` mit dem Marker-Typ Float auf. `DragFloat` liest
`floatValue`, zeigt daher 0 statt 10 und schreibt bei Bearbeitung den von 0
aus geänderten Wert zurück. Reines Speichern ohne Bearbeitung bewahrt hier
noch den Zahlenwert; die Wiederherstellung über typisierten Runtime-Loader
ist ebenfalls unkritisch. Die Aussage, Int/Float-Ableitung sei generell
unkritisch, gilt somit nicht für den Inspector.

Bitte vor Anzeige/Bearbeitung gespeicherte Werte numerisch in den
deklarierten Registry-/Marker-Typ konvertieren, einschließlich Float->Int
mit definierter Bereichsprüfung, oder Metadaten beim Wiederherstellen
berücksichtigen. Ein bloßes Setzen von `value.type` reicht nicht, weil
`ScriptFieldValue` getrennte Zahlenfelder besitzt. Regressionstest mit
ganzzahligem Float-Wert und anschließender Bearbeitung ergänzen.

Validierung: Ninja/MSVC-Ziel `krom_tests` aktuell; eigener Testlauf mit
16 Suiten / 257 Tests bestanden, Exit 0, einschließlich der beiden neuen
Platzhaltertests. Diese prüfen keinen Inspector-Metadatenabgleich. Befund
durch Quellcodeverfolgung, kein manueller GUI-Repro. Prefab-Übernahme kopiert
`fieldValues` auch für Platzhalter; dort kein weiterer konkreter Befund.
Nur diese Koordinationsdatei geändert. Die Physik wurde in diesem Durchgang
nicht geprüft, da ein neuer expliziter Script-Prüfauftrag vorlag.

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

### Befund „Inferred Int-Werte im Float-Inspector als 0“ (Review `a9b1a5f`) – behoben

Im Code bestätigt: Beide Inspector-Zweige (Registry und Marker) übernahmen den
gespeicherten Wert samt abgeleitetem Typ. Zusätzlich gefunden: Derselbe
Typkonflikt ließ `ScriptList::ApplyStoredFieldValues` Werte still verwerfen,
weil `WriteField` bei abweichendem Typ `false` liefert. Das betrifft z. B. ein
Prefab, das aus einem Platzhalter gespeichert und später mit der echten Klasse
instanziiert wird.

- `addons/script/ScriptRegistry.hpp/.cpp`: neue Funktion
  `ConvertScriptFieldValue(value, targetType)`. Int↔Float wird umgerechnet,
  Float→Int rundet und begrenzt auf den int32-Bereich (NaN → 0).
  String/Prefab/Entity teilen `stringValue`/`entityValue`. Nicht
  konvertierbare Kombinationen ergeben den Default des Zieltyps.
- `addons/editor/EditorUIInspector.cpp`: Gespeicherte und Live-Werte werden in
  beiden Zweigen vor `DrawScriptFieldEditor` in den deklarierten Typ
  umgerechnet.
- `addons/script/ScriptList.cpp`: `ApplyStoredFieldValues` rechnet vor
  `WriteField` in den Registry-Typ um und speichert den umgerechneten Wert.
- `tests/test_script.cpp`: `TestConvertScriptFieldValue` (inkl. Bereichsgrenzen
  und NaN), `TestPlaceholderIntegralFloatEditRoundtrip` (Platzhalter mit
  `"speed": 10` → Float-Anzeige 10 → Bearbeitung +0.5 → Speichern → Laden mit
  echter Klasse ergibt 10.5) und `TestApplyStoredFieldValuesConvertsType`.
- Build x64-Debug fehlerfrei, keine neuen Warnungen. Tests: 16 Suiten grün,
  die drei neuen Tests ok.

### Abschlussprüfung durch Codex, 2026-10-19, Commit `f9af358`

Der verbliebene 8.3-/Quoting-Befund ist im aktuellen Quellcode behoben.
`CreateProcessW` erhält den expliziten UTF-16-Programmpfad und eine veränderbare,
korrekt quotierte Kommandozeile. `QuoteWindowsArg` behandelt leere Argumente,
Leerzeichen, Backslash-Folgen vor Quotes und abschließende Backslashes korrekt.
Die Source-/Build-Pfade bleiben UTF-16, beide Prozesshandles werden nach dem
Warten geschlossen. Keine weiteren konkreten Befunde in diesem Fix.

Eigene Validierung: Ninja/MSVC-Ziele `krom_editor_dx11` und `krom_tests`
erfolgreich geprüft, beide aktuell ("no work to do"). Die unveränderte Testsuite
wurde in dieser Abschlussprüfung nicht erneut ausgeführt; der letzte eigene
Lauf bei `38b1e82` bestand alle 256 Tests. Claudes isolierte Prozessstart- und
Argument-Roundtrip-Tests sind oben dokumentiert, wurden hier nicht wiederholt.
Ein manueller Klick im Editor wurde nicht getestet. Nur KOORDINATION.md geändert.

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
