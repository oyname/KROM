# Post Mortem – Zittern bewegter Objekte bei VSync

24. November 2026

## Kurzfassung

Im Editor-Play zitterte der Hubschrauber `apache15` (DemoProject, Szene `car`)
beim Drehen: Das Heck mit dem hinteren Flügel sprang seitlich hin und her. Im
Stand war das Bild ruhig. Hinter dem Symptom lagen drei getrennte Fehler, die
nacheinander gefunden wurden. Der Hauptfehler war allgemein und betraf jedes
bewegte Objekt: Bei VSync schwankte die gemessene Frame-Zeit pro Bild zwischen
etwa 2,8 ms und 14,5 ms, obwohl die Bilder gleichmäßig alle 6,94 ms (144 Hz) auf
dem Monitor erschienen. Die Spiellogik bewegte Objekte um `Geschwindigkeit ×
Delta` und damit abwechselnd zu weit und zu kurz.

Die beiden Nebenfehler:

1. Die Physik setzte beim Teleport eines dynamischen Körpers die Interpolation
   der Position zurück, auch wenn ein Script nur die Drehung gesetzt hatte.
2. Die Kamera lief in `OnUpdate` vor der Physik und richtete sich nach der
   Position des Vorbilds aus.

Behoben wurde alles in der Engine. Am Hubschrauber-Script war keine
Sonderlösung nötig; nur die Kamera nutzt jetzt den neuen `OnLateUpdate`.

## Symptome

- Beim Drehen nach links oder rechts sprang das Heck sichtbar hin und her.
  Je weiter ein Teil von der Drehachse entfernt war, desto stärker.
- Im Stand und bei ruhiger Kamera war kein Zittern zu sehen.
- Der Effekt wirkte wie eine Unschärfe am hinteren Flügel, bei genauem Hinsehen
  wie ein Doppelbild.

## Vorgeschichte

Der Hubschrauber hatte kurz zuvor einen dynamischen Rigidbody mit einer
Kapsel-Kollision bekommen. Steigen und Sinken laufen seitdem über eine
Auftriebskraft in `OnFixedUpdate`, Vorwärts und Seitwärts über die
Körpergeschwindigkeit. Die Drehung (Kurs, Neigen in Kurven) setzt das Script
weiterhin jedes Frame direkt.
[HelicopterControl](../projects/DemoProject/Assets/Scripts/Helicopter/HelicopterControl.cpp)

## Fehler 1: Interpolation beim Setzen der Drehung zurückgesetzt

`PhysicsWorld::Sync` erkennt, wenn ein Script die Transform eines dynamischen
Körpers von außen ändert, und teleportiert den Körper. Dabei wurden
`prevPosition` und `prevRotation` immer auf die neue Pose gesetzt, damit der
Teleport nicht interpoliert wird. Setzte ein Script jedes Frame nur die
Drehung, ging so auch die Interpolation der Position verloren. In Frames ohne
festen Physikschritt (bei 144 Hz und 60-Hz-Physik die Mehrheit) sprang die
Position deshalb nach vorn und im nächsten Frame mit Schritt wieder zurück.

**Fix:** Nur der tatsächlich gesetzte Teil setzt seine Interpolation zurück:
Position und Drehung werden getrennt geprüft.
[PhysicsWorld::Sync](../addons/physics/PhysicsWorld.cpp)

**Test:** `Rotation every frame stays smooth` setzt bei 144 FPS jedes Frame die
Drehung und prüft, dass die Position jedes Frame gleichmäßig weiterläuft und
nie zurückspringt. Gegenprobe ohne Fix: 24 Frames Stillstand, Rücksprünge.
[test_physics.cpp](../tests/test_physics.cpp)

## Fehler 2: Kamera vor der Physik

`HelicopterCamera` lief in `OnUpdate`. In `GameSimulation::RunFrame` laufen die
Scripts vor den festen Schritten und dem Physik-WriteBack. Die Kamera richtete
sich deshalb nach der Pose des Vorbilds aus, während der Hubschrauber an der
neuen Pose gezeichnet wurde. Der Versatz schwankte mit der Frame-Zeit.

**Fix:** Neuer Script-Aufruf `OnLateUpdate`, einmal pro Frame nach allen
`OnUpdate`, der Physik (inklusive interpolierter Pose) und der Animation, vor
dem TransformSystem. Das entspricht `LateUpdate` in Unity. Die Dienste
(Physik, Audio, Partikel …) baut `GameSimulation::WithScriptServices` für
`OnUpdate` und `OnLateUpdate` gemeinsam auf. Weil sich die vtable von
`ComponentScript` ändert, steigt `ScriptModuleAbiVersion` von 7 auf 8.
[ComponentScript](../addons/script/ComponentScript.hpp),
[GameSimulation](../addons/gameplay/GameSimulation.cpp),
[ScriptModuleApi](../addons/script/ScriptModuleApi.hpp)

**Test:** `Game frame: late update sees physics pose` prüft die Reihenfolge
`OnUpdate` → `OnFixedUpdate` → `OnLateUpdate` und dass ein fallender Körper in
`OnLateUpdate` schon die Höhe dieses Frames hat.
[test_script.cpp](../tests/test_script.cpp)

Nach dem ersten Start des neu gebauten Editors war noch kein Unterschied zu
sehen. Der Editor baute die Script-Module wegen der neuen ABI-Version erst im
Hintergrund neu; danach wurde es ruhiger, aber nicht gut.

## Fehler 3 (Hauptursache): Schwankende Frame-Zeit bei VSync

### Eingrenzen

Ein Nachbau im Test (Körper mit derselben versetzten Kapsel, Drehen wie im
Script, Kamera in `OnLateUpdate`, 144 FPS mit absichtlich schwankender
Frame-Zeit) lief vollständig glatt. Die gesetzte Drehung kam exakt im Bild an,
das Heck blieb auf dem Bildschirm auf 0,00001 genau ruhig. Physik, Script und
Kamera waren damit entlastet. Der Nachbau hatte aber eine stille Annahme: Die
Frame-Zeit, die er der Simulation gab, war zugleich die Zeit, die das Bild
angezeigt wurde.

Eine temporäre Messung im Release-Editor (Statistik der Frame-Zeit pro Sekunde
während Play) zeigte das Gegenteil:

```text
frames=144 mean=6.95ms min=2.81ms max=14.58ms sd=5.01ms jumps>25%=130
```

Im Mittel genau 144 Bilder pro Sekunde, aber die einzelnen Messwerte wechselten
ständig zwischen etwa 2,8 ms und 14,5 ms. In rund 90 % der Frames sprang der
Wert um mehr als 25 %. Der Monitor zeigte die Bilder trotzdem gleichmäßig an.
Das Script drehte im einen Bild um 14,5 ms weiter, im nächsten um 2,8 ms.

### Ursache

`StdTiming::BeginFrame` misst die CPU-Zeit zwischen zwei Frame-Anfängen. Mit
VSync und mehreren Bildern im Flug blockiert die CPU nicht jedes Frame an
derselben Stelle; die gemessene Zeit beschreibt dann nicht, wann ein Bild
angezeigt wird. Das Problem ist bekannt (A. Ladavac, „The Elusive Frame
Timing“, GDC 2018). Es betrifft jedes Objekt, das sich pro Frame um
`Geschwindigkeit × Delta` bewegt, nicht nur den Hubschrauber.

### Fix

Die Spielzeit folgt jetzt der Bildanzeige. `GameSimulation::BeginFrame` gibt
das gemessene Delta durch eine `FrameDeltaClock`; Editor-Play und die Runtime
(`krom.h`) melden ihr vor jedem Frame die Bildwiederholrate und den
VSync-Status.
[FrameDeltaSmoother.hpp](../include/core/FrameDeltaSmoother.hpp),
[GameSimulation](../addons/gameplay/GameSimulation.cpp),
[Editor](../app/KromEditorApp.cpp), [Runtime](../src/app/KromWrapper.cpp)

- **Bildwiederholrate:** `IWindow::GetRefreshRateHz` liefert die Rate des
  Monitors unter dem Fenster. Win32 liest sie exakt (rational, z. B. 143,998
  oder 59,94 Hz) über `QueryDisplayConfig` aus dem aktiven Anzeigepfad, mit
  `EnumDisplaySettings` (ganze Hertz) als Rückfall; der Wert wird pro Monitor
  zwischengespeichert und einmal pro Sekunde erneuert. GLFW (Linux) liefert
  ganze Hertz aus dem Videomodus.
  [Win32Window](../src/platform/win32/Win32Window.cpp),
  [GLFWWindow](../src/platform/glfw/GLFWWindow.cpp)
- **Einrasten:** Liegt die über 8 Frames gemittelte Frame-Zeit 8 Frames lang auf
  einem Vielfachen der Bildperiode, rastet die Uhr ein. Jedes Frame bekommt
  dann eine ganze Zahl von Perioden: eine normal, zwei nach einem verpassten
  Bildwechsel. Zwei gibt es erst ab 1,65 Perioden echter Zeit, damit
  Messrauschen keinen falschen Doppelschritt erzeugt.
- **Im Takt bleiben:** Die Anzahl der Perioden folgt der aufsummierten echten
  Zeit. Eine kleine Rückführung hält die Frames mittig zwischen zwei
  Bildwechseln. Ab vier Perioden Abstand wird der Rest hingenommen, statt ihn
  in einem sichtbaren Sprung aufzuholen.
- **Periode messen:** Solange eingerastet, misst die Uhr die tatsächliche
  Periode aus der gemittelten Frame-Zeit nach (höchstens 3 % neben der
  gemeldeten). Damit stimmt sie auch, wenn die Plattform nur ganze Hertz meldet
  (59 oder 60 für 59,94 Hz).
- **Rückfall:** Ohne VSync, bei unbekannter Rate, bei variabler Bildrate
  (G-Sync/FreeSync) oder wenn die Frames 8 Frames lang deutlich neben einem
  Vielfachen liegen, wird das Delta nur über 8 Frames gemittelt. Ein einzelnes
  langes Frame (ab 0,25 s) beendet das Einrasten.

**Tests:** `Game frame: delta smoothing` (Mittelung, Spielzeit läuft nicht
weg) und `Game frame: delta locks to display` (Einrasten bei schwankender
Messung, verpasster Bildwechsel, gemeldete 60 bzw. 59 Hz bei echten 59,94 Hz
ohne Doppelschritt, kein VSync, variable Bildrate).
[test_script.cpp](../tests/test_script.cpp)

Die Regeln für das Einrasten wurden vor dem Einbau in einer Simulation mit
Messrauschen auf den Zeitstempeln und eingestreuten verpassten Bildwechseln
abgestimmt. Zwei frühere Varianten fielen dabei durch: Ohne Trägheit beim
Ausrasten fiel die Uhr genau beim verpassten Bildwechsel aus dem Takt (der
8-Frame-Mittelwert springt dort um 20 %). Eine Periodenmessung aus „echte Zeit
÷ vergebene Perioden“ koppelte sich mit falschen Doppelschritten zurück und
schaukelte sich auf.

## Falsche Fährten

- **Rotor statt Flügel.** „Unschärfe am hinteren Flügel“ wurde zunächst als
  Heckrotor gedeutet, der mit 1080 °/s dreht. Gemeint war das feste
  Höhenleitwerk. Die Rückfrage „Doppelbild oder Verwischen?“ hätte am Anfang
  stehen sollen.
- **Kantenglättung und Nacheffekte.** Der Renderer mischt keine Bilder
  (SMAA statt TAA, keine Bewegungsunschärfe, GTAO und SSR ohne Verlauf). Eine
  echte Render-Unschärfe war damit ausgeschlossen.
- **Kräfte außerhalb des Schwerpunkts.** Ein Drehmoment durch den Auftrieb
  hätte ein Wippen erklärt; `AddForce` greift aber im Schwerpunkt an.

## Verifikation

- Alle Tests von `krom_tests` im Debug-Build x64 bestanden, einschließlich der
  fünf neuen Tests oben.
- Im Release-Editor (Vulkan) bestätigt: Das Hin und Her am Heck beim Drehen ist
  verschwunden, das Bild läuft sauber.
- Die temporäre Frame-Zeit-Messung und der Test-Nachbau wurden wieder entfernt.
- Unter Linux ist der GLFW-Weg nicht getestet; dort gibt es derzeit keinen
  Build.

## Erkenntnisse

- **Erst messen.** Drei Annahmen (Physik, Kamera, Rotor) waren plausibel, aber
  erst die Messung der echten Frame-Zeiten zeigte die Hauptursache. Eine kleine
  temporäre Messung im echten Programm hätte früher Klarheit gebracht.
- **Nachbauten prüfen ihre Annahmen nicht.** Ein Test, der die Frame-Zeit
  vorgibt, kann einen Fehler in der Frame-Zeit nicht finden. Der Nachbau war
  trotzdem wertvoll, weil er Physik, Script und Kamera ausschloss.
- **Frame-Zeit ist Anzeigezeit.** Für Spiellogik zählt, wann ein Bild auf dem
  Monitor erscheint, nicht wie lange die CPU dafür gebraucht hat.
- **Kameras gehören hinter die Physik.** Alles, was einem physikalisch oder
  animiert bewegten Objekt folgt, gehört in `OnLateUpdate`.
- **Teilweise Teleports.** Wer nur einen Teil der Pose von außen setzt, darf
  nur diesen Teil aus der Interpolation nehmen.
