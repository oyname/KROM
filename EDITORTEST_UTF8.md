# Editor-Test UTF-8-Pfade – Checkliste

Ziel: Den Engine-Pfad mit Umlauten in den Abläufen prüfen, die kein
automatisierter Test abdeckt: C++-Script mit CMake-Reconfigure, Play
(Runtime-Build) und Release-Export.

**Testumgebung (von Claude vorbereitet):**
- Engine-Klon: `F:\Größe Test\krom` (Stand `dd9b9d1`, lokaler Klon von `F:\working\krom codex`)
- Debug-Build: `F:\Größe Test\krom\out\build\x64-Debug`
  (`krom_editor_dx11`, `krom_editor_vulkan`, `krom_tests` gebaut)
- Editor starten: `F:\Größe Test\krom\out\build\x64-Debug\bin\krom_editor_vulkan.exe`
  (oder `_dx11.exe`). Das Konsolenfenster offen lassen, dort erscheinen CMake-
  und Build-Ausgaben.

Hinweise vorab:
- `projects/*/Assets/Textures/` ist per `.gitignore` ausgeschlossen und fehlt im
  Klon. Für `zweitesProjekt` wurde es nachträglich per robocopy ergänzt (34 Dateien).
  Fehlermeldungen „failed to read texture file“ aus der Zeit davor sind kein
  UTF-8-Fehler.
- In der Konsole erscheinen Umlaute als `Gr├Â├ƒe`. Das ist korrektes UTF-8,
  angezeigt in der OEM-Codepage der Konsole. Maßgeblich ist, ob die Datei
  gefunden wird.
- Neue Projekte bekommen nur unter `F:\Größe Test\krom\projects\` eine Runtime.
  Der Editor schlägt diesen Ordner vor, bitte nicht ändern.
- Projektordner werden auf ASCII bereinigt: Der Name „Größe Projekt“ ergibt den
  Ordner `Gr____e_Projekt`. Die Umlaute im Pfad kommen deshalb aus dem
  Engine-Pfad und aus einem Asset-Unterordner (Schritt 3).
- Der erste Export konfiguriert `out\build\x64-Release` neu und lädt dabei Jolt
  und miniaudio aus dem Internet. Das dauert einige Minuten.

---

## 1. Editor starten
- [ ] Editor startet ohne Fehlermeldung in der Konsole.
- [ ] Unter **New Project** steht als Zielordner `F:\Größe Test\krom\projects`.
      Die Umlaute müssen korrekt erscheinen, nicht als `GrÃ¶ÃŸe`.

## 2. Projekt anlegen
- [ ] **New Project**, Name `Größe Projekt`, Backend Vulkan (oder DX11), erstellen.
- [ ] Kein orangefarbener Hinweis „Nur Projekte in … bekommen eine Runtime“.
- [ ] Ordner `F:\Größe Test\krom\projects\Gr____e_Projekt\` existiert mit
      `krom-project.json` und `Assets\Scenes\`.

## 3. Asset-Unterordner mit Umlaut und C++-Script (Fix aus 38b1e82/f9af358)
- [ ] Im Asset-Browser in `Assets` einen Ordner `Scripts` und darin `Übung` anlegen.
- [ ] In `Assets\Scripts\Übung` per Rechtsklick **C++ Script erstellen**,
      Klassenname `Drehen`.
- [ ] Die Konsole zeigt einen CMake-Lauf (`-- Configuring done` /
      `-- Generating done`) ohne Fehler.
- [ ] Im Editor keine Meldung „CMake-Reconfigure fehlgeschlagen (Exit …)“.
- [ ] `Drehen.hpp`/`Drehen.cpp` liegen in `…\Assets\Scripts\Übung\`, der
      Dateiname ist im Explorer korrekt.

## 4. Szene und Script verwenden
- [ ] Szene anlegen, z. B. `Szene Ä`, mit Würfel oder Mesh und Licht. Dem Objekt
      das Script `Drehen` als Komponente geben, speichern.
- [ ] `Assets\Scenes\Szene Ä.json` bzw. `.kscene` existiert. Der Dateiname ist
      korrekt, nicht `Szene Ã„`.
- [ ] Editor schließen, neu starten, Projekt öffnen: Die Szene lädt mit
      Objekt und Script-Komponente.

## 5. Play (Runtime-Build im Debug-Buildordner, Fix aus 38b1e82)
- [ ] Im Projektfenster **Play**.
- [ ] Die Konsole zeigt den Ninja-Build von `krom_runtime_Gr____e_Projekt_<backend>`
      ohne Fehler.
- [ ] Keine Meldung „Play fehlgeschlagen: Runtime-Build fehlgeschlagen.“
- [ ] Das Spielfenster öffnet sich, die Szene ist sichtbar und das Script läuft
      (Objekt reagiert).

## 6. Release-Export (Configure von x64-Release mit KROM_SOURCE_DIR, Fix aus 38b1e82)
- [ ] Im Projektfenster **Projekt exportieren**. Beim ersten Mal dauert das
      einige Minuten.
- [ ] Die Konsole zeigt `[Build] Konfiguriere F:\Größe Test\krom\out\build\x64-Release (Release)`
      mit korrekten Umlauten, danach CMake und Ninja ohne Fehler.
- [ ] Meldung „Exportiert: …“, keine „Export fehlgeschlagen: …“.
- [ ] Ordner `…\projects\Gr____e_Projekt\Build\x64-Release-<backend>\` enthält
      die EXE, `Content\` und `assets\`.
- [ ] Die exportierte EXE per Doppelklick starten: Die Szene läuft und das
      Script ist aktiv.
- [ ] Optional: Den Export-Ordner nach `F:\Größe Test\Kopie Ü\` kopieren und
      von dort starten (ortsunabhängig).

## Bei Fehlern bitte festhalten
- Nummer des fehlgeschlagenen Schritts.
- Die letzten ~30 Zeilen der Editor-Konsole.
- Falls vorhanden: `krom.log` neben der gestarteten EXE und
  `out\build\x64-Release\_krom_play_build.cmd`.

Danach in `KOORDINATION.md` eintragen oder an Claude geben.
Aufräumen nach dem Test: Den Ordner `F:\Größe Test` komplett löschen. Er ist ein
eigenständiger Klon, das Arbeitsrepo bleibt unberührt.
