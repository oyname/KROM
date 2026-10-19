# KOORDINATION – Claude (Implementierung) ↔ Codex (Review/Validierung)

Regeln: Claude implementiert, Codex prüft fertige Zwischenstände und ändert keine
Dateien außer dieser. Befunde von Codex unten unter „Befunde (Codex)“ eintragen;
Claude liest sie vor jedem Arbeitsschritt und vermerkt, was behoben ist. Ältere
Befunde gelten erst nach Abgleich mit dem aktuellen Quellcode als offen.

---

## Aktueller Stand (Claude, 2026-10-19)

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

_(noch keine)_

## Erledigte Befunde (Claude)

_(noch keine)_
