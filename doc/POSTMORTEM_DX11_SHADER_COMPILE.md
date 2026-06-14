# DX11 Shader Compile Postmortem

## Zusammenfassung

Beim ersten Laden eines 3D-Modells unter `DX11` kam es zu extrem langen Ladezeiten, sobald der Ordner `shader_artifacts` leer war. Der Editor wirkte dabei so, als ob der Modellimport selbst langsam sei. Die eigentliche Ursache war jedoch die Laufzeit-Kompilierung eines einzelnen Pixelshaders.

Das Problem war nicht allgemein "DX11 ist langsam", sondern ein konkreter Compile-Path:

- `pbr_lit.ps.hlsl`
- `D3DCompile(...)`
- kalter Cache (`shader_artifacts` leer)

Die finale Lösung war kein Editor-Sonderweg und kein Feature-Cut, sondern eine strukturelle Entschärfung des echten Runtime-Shaders.

## Symptom

Beobachtung:

- Wenn `shader_artifacts` gefüllt war: Ladezeit im Sekundenbereich
- Wenn `shader_artifacts` leer war: Ladezeit im Minutenbereich
- Besonders sichtbar beim ersten Laden eines Modells oder Materials unter `DX11`

Wichtig:

- `Vulkan` und `OpenGL` waren deutlich schneller
- Das Problem war reproduzierbar und an den DX11-Cold-Compile gebunden

## Messung

Zur Diagnose wurde Timing-Logging in den Shader-Compile- und Cache-Pfad eingebaut:

- [ShaderCompilerShared.cpp](/F:/working/krom%20codex/src/renderer/ShaderCompilerShared.cpp)
- [ShaderCompilerD3D.cpp](/F:/working/krom%20codex/src/renderer/ShaderCompilerD3D.cpp)

Dadurch wurde sichtbar:

```text
ShaderCompile DX11: OK shader='pbr_lit.vs.hlsl' ... time=15.92 ms
ShaderCompile DX11: OK shader='pbr_lit.ps.hlsl' ... time=167833.42 ms
ShaderCompile DX11: OK shader='pbr_lit.ps.hlsl' ... time=172005.05 ms
```

Das heißt:

- Vertexshader: unkritisch
- Shadow-VS: unkritisch
- `pbr_lit.ps.hlsl`: eigentlicher Ausreißer

## Root Cause

Der alte Full-PBR-Pixelshader war für den `DX11`-Compiler strukturell zu teuer.

Der kritische Teil war nicht die Dateigröße allein, sondern die Kombination aus:

- großem Monolith-Shadermodell
- komplexem Shadow-Code
- aggressivem `[unroll]` im Shadow-Bereich
- `D3DCompile(...)` auf dem DX11/DXBC-Pfad

Besonders teuer waren:

- PCF-Schleifen
- Shadow-Light-Suche
- Point-Shadow-Blending über mehrere Faces

Das führte dazu, dass `D3DCompile(...)` in eine extreme Optimizer-/Compile-Kostenfalle lief.

## Was nicht die eigentliche Ursache war

Folgende Erklärungen wurden geprüft, waren aber nicht die Hauptursache:

- `Vulkan` sei nur schneller wegen `-Od`
- `OpenGL` sei generell besser für Shader
- Modellimport sei das Problem
- Asset-Laden sei das Problem

Ein Gegentest mit absichtlich teurerem Vulkan-Debug-Compile zeigte, dass Vulkan trotzdem schnell blieb. Damit war klar: der Hauptunterschied lag nicht nur in der Optimierungsstufe, sondern speziell im DX11-Compilepfad und im Aufbau des Pixelshaders.

## Zwischenversuch

Es wurde testweise ein abgespeckter Preview-Shader ohne Schatten verwendet. Damit sank die Compilezeit auf ca. `70-118 ms`.

Das war als Diagnose nützlich, aber keine akzeptable Endlösung, weil:

- Schatten fehlten
- es ein Sonderweg nur für den Editor gewesen wäre
- es nicht dem späteren Engine-Runtime-Pfad entsprach

Dieser Sonderweg wurde wieder entfernt.

## Finale Lösung

Die eigentliche Lösung wurde im echten Runtime-Shader umgesetzt:

- [pbr_lit.ps.hlsl](/F:/working/krom%20codex/assets/pbr_lit.ps.hlsl)

Geändert wurde vor allem der Shadow-Bereich:

- erzwungene `[unroll]`-Schleifen wurden durch normale bzw. `[loop]`-Schleifen ersetzt
- betroffen waren:
  - PCF 2x2
  - PCF 3x3
  - Shadow-Light-Suche
  - Point-Shadow-Face-Blending

Wichtig:

- kein Feature wurde entfernt
- Schatten blieben erhalten
- der gleiche Vollshader wird weiter von der Engine benutzt
- kein Editor-Sondermaterial blieb aktiv

## Ergebnis

Nach der Änderung lagen die gemessenen Compilezeiten für denselben Vollshader bei:

```text
ShaderCompile DX11: OK shader='pbr_lit.ps.hlsl' ... time=628.54 ms
ShaderCompile DX11: OK shader='pbr_lit.ps.hlsl' ... time=645.02 ms
ShaderCompile DX11: OK shader='pbr_lit.ps.hlsl' ... time=637.40 ms
```

Vorher:

- ca. `167000-172000 ms`

Nachher:

- ca. `628-645 ms`

Das entspricht einer Verbesserung von mehreren Größenordnungen, ohne den Runtime-Pfad funktional zu verändern.

## Warum die Lösung funktioniert

`D3DCompile(...)` reagiert sehr empfindlich auf bestimmte Shader-Strukturen. Starkes manuelles oder erzwungenes Unrolling in komplexen Shadow-Pfaden kann Compilekosten massiv explodieren lassen.

Durch die Umstellung auf echte Schleifen konnte der Compiler:

- weniger Code vervielfältigen
- weniger aggressive Optimizer-Arbeit leisten
- den Shader viel stabiler und schneller übersetzen

Die Laufzeitfunktion blieb dabei gleich genug, dass die Engine weiter denselben vollständigen Shaderpfad mit Schatten verwenden kann.

## Lessons Learned

1. Lange DX11-Ladezeiten beim ersten Material-/Modellkontakt waren in diesem Fall kein Asset-Problem, sondern Shader-Compile-Kosten.
2. Der wichtigste Hebel war hier nicht Caching allein, sondern die Struktur des Shaders selbst.
3. Große Monolith-Shader mit aggressivem Shadow-Unrolling sind auf dem DX11/DXBC-Pfad riskant.
4. Ein schneller Preview-Shader kann als Diagnose nützlich sein, sollte aber kein Ersatz für einen sauberen Runtime-Pfad sein.
5. Timing-Logging im Compilepfad war entscheidend, um die echte Ursache zu isolieren.

## Aktueller Status

Der bewährte Stand ist:

- voller Runtime-Shader aktiv
- Schatten aktiv
- kein Editor-Sonderweg
- deutlich schnellere DX11-Cold-Compiles

Spätere mögliche Verbesserungen:

- weitere modulare Aufteilung des Full-Shaders nach echten Runtime-Anforderungen
- noch gezieltere Variantentrennung für Shadow-Arten
- persistenter und vorgebauter Shader-Cache für Release/Distribution

