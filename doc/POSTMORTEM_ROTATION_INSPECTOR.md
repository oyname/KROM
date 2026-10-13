# Rotation Inspector Postmortem

## Problem

Im Transform-Inspector war das Rotieren von Objekten, besonders auf `Y` und `Z`, instabil:

- eingegebene Rotationswerte sprangen nach der Eingabe wieder auf `0` oder andere Werte
- beim Draggen mit der Maus "huepfte" die Rotation sichtbar
- bei Kombinationen wie `X=90`, `Y=90`, danach `Z=10` war das Verhalten besonders schlecht
- das Problem trat bei Parent- und Child-Objekten auf

Das Ergebnis war, dass sich die Rotation im Editor unzuverlaessig und kaputt anfuehlte.

## Root Cause

Der Hauptfehler lag nicht in der Quaternion-Mathematik der Engine selbst, sondern im Zusammenspiel von:

1. interner Quaternion-Speicherung
2. Euler-Darstellung im Inspector
3. dem UI-State waehrend aktiver Bearbeitung

Konkret:

- Der Inspector nutzte Quaternionen als echte Rotationsdaten.
- Fuer die Anzeige wurden diese Quaternionen in Euler-Werte umgerechnet.
- Waehrend der Benutzer an den Feldern arbeitete, wurde ein temporärer Euler-Entwurf im `EditorState` gehalten.

Der kritische Bug war:

- Beim Aendern von `Y` oder `Z` wurden die neuen Winkelwerte nicht sauber in diesen Editor-Entwurf zurueckgeschrieben.
- Im naechsten Frame konnte der Inspector deshalb wieder einen aelteren Euler-Stand verwenden.
- Dadurch wurden gerade eingegebene Werte scheinbar "zurueckgesetzt", oft auf `0` oder ein alternatives Winkeltripel.

Das fuehrte zu:

- Zurueckspringen der Zahlenfelder
- instabilem Maus-Drag-Verhalten
- sichtbarem "Hüpfen" waehrend der Rotation

## Secondary Confusion

Waehrend der Fehlersuche gab es zusaetzlich Stoergeraesche durch:

- verschiedene Euler-Konventionsversuche
- Reparenting-/Scale-Aenderungen
- lokale vs. Welt-Rotation

Diese Themen haben das Verhalten teilweise verschleiert, waren aber nicht der Kernfehler fuer das akute Zurueckspringen der Rotationswerte im Inspector.

## Fix

Die Korrektur war gezielt im Inspector-State:

### 1. Stabiler Rotations-Edit-State

Im `EditorState` wurde ein stabiler Bearbeitungszustand fuer Rotation gehalten:

- bearbeitete Entity
- bearbeiteter Space (`Local` oder `World`)
- aktueller Euler-Entwurf
- zugehoerige Quaternion
- Aktiv-Zustand des Feldes

### 2. Refresh nur bei externer Aenderung

Die angezeigten Euler-Werte werden nicht mehr in jedem Frame blind aus der Quaternion neu berechnet.

Stattdessen:

- waehrend aktiver Bearbeitung bleiben die vom Benutzer eingegebenen Winkel stehen
- nur wenn sich die Rotation wirklich von aussen geaendert hat, wird der Entwurf neu synchronisiert

### 3. Entscheidender Bugfix

Beim Commit des UI-States werden jetzt immer auch die aktuell eingegebenen Euler-Werte gespeichert, nicht nur die Quaternion.

Vorher:

- Quaternion wurde aktualisiert
- Euler-Entwurf konnte veraltet bleiben

Nachher:

- Quaternion und Euler-Entwurf bleiben synchron
- der naechste Frame zeigt weiter genau den zuletzt eingegebenen Stand

## Betroffene Dateien

- [addons/editor/EditorFeature.hpp](/F:/working/krom%20codex/addons/editor/EditorFeature.hpp)
- [addons/editor/EditorUI.cpp](/F:/working/krom%20codex/addons/editor/EditorUI.cpp)

## Ergebnis nach dem Fix

Nach dem Fix:

- bleiben Rotationswerte im Inspector stabil stehen
- springen `Y`- und `Z`-Werte nicht mehr sofort zurueck
- Maus-Drag fuehlt sich deutlich stabiler an
- Quaternion bleibt intern weiter die robuste Rotationsrepräsentation
- Euler bleibt eine editierbare UI-Darstellung statt eine pro Frame neu schwankende Ableitung

## Lessons Learned

1. Quaternion-Interna und Euler-UI duerfen nicht pro Frame gegeneinander arbeiten.
2. Ein Rotations-Inspector braucht einen expliziten Edit-State, nicht nur On-the-fly-Konvertierung.
3. Bei Rotationsproblemen sollte zuerst zwischen Runtime-Mathematik und Editor-Darstellung getrennt werden.
4. Reparenting-, Scale- und Euler-Themen muessen isoliert untersucht werden, sonst vermischen sich Symptome und Ursachen.

## Follow-up

Sinnvolle naechste Schritte:

- Maus-/Gizmo-Rotationspfad separat gegenpruefen
- langfristig klaren Umgang mit Euler-Konvention dokumentieren
- Transform-Regressionstests fuer Inspector-Eingabe, Reparenting und Child-Rotation aufbauen
