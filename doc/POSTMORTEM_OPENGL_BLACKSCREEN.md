# OpenGL Black Screen – Post Mortem

## Ausgangslage

Nach dem Umbau des Materialsystems renderte:

- DX11 korrekt
- Vulkan korrekt
- OpenGL nur schwarz

Es gab:

- keine OpenGL-Fehlermeldung
- keine Shader-Compilefehler
- gültige Drawcalls
- gültige Programme und VAOs

Dadurch wirkte der Fehler zunächst wie ein klassischer Material-/Shaderbinding-Fehler.

---

# Tatsächliche Ursache

Die eigentliche Ursache lag nicht im PBR-Materialsystem selbst, sondern im Zusammenspiel aus:

- Material Runtime State
- OpenGL Pipeline State
- Fullscreen Tonemap Pass

Der entscheidende Fehler:

## Der Tonemap-Fullscreen-Pass bekam falsche Renderstates

Der Tonemap-Pass sollte eigentlich:

```text
DepthTest = OFF
DepthWrite = OFF
Cull = OFF
```

verwenden.

Nach dem Materialsystem-Umbau wurden diese States jedoch durch den generischen `MaterialRenderPolicy`-Pfad wieder überschrieben.

Dadurch lief der Fullscreen-Triangle-Pass effektiv mit:

```text
DepthTest = ON
DepthWrite = ON
Cull = ON
```

Die Diagnose-Logs zeigten eindeutig:

```text
TONEMAP SetPipeline ...
depthTest=1
depthWrite=1
cull=1
```

Für einen Fullscreen-Pass ist das falsch.

---

# Warum führte das zu einem komplett schwarzen Bild?

Der Tonemap-Pass war der letzte Schritt:

```text
HDR Scene → Tonemap → Backbuffer
```

Der Pass wurde zwar ausgeführt:

- gültiger Shader
- gültiger VAO
- gültiger Drawcall
- gültiges FBO

Aber:

- der Fullscreen-Triangle wurde durch Depth/Culling verworfen
- dadurch wurde niemals sichtbar in den Backbuffer geschrieben
- Ergebnis: schwarzes Bild

---

# Warum funktionierten DX11 und Vulkan trotzdem?

DX11 und Vulkan sind bei Fullscreen-/Postprocess-Pipelines robuster gegen solche Zustandsfehler:

- andere Default-States
- explizitere Pipelineobjekte
- weniger implizite globale Zustände
- andere Behandlung von Cull/Depth beim Fullscreen-Triangle

OpenGL ist hier empfindlicher, weil viel globaler State aktiv bleibt und implizit weiterverwendet wird.

---

# Warum war die Fehlersuche schwierig?

Weil fast alles „gültig“ aussah:

- Shader kompilierten
- Drawcalls liefen
- Programme waren valid
- FBOs existierten
- kein GL-Error
- RenderGraph lief korrekt

Der Fehler war kein offensichtlicher API-Fehler, sondern ein logischer Pipeline-State-Fehler.

---

# Diagnoseweg

Der entscheidende Schritt war nicht weiteres Raten im Materialsystem, sondern harte Isolation:

## 1. Tonemap-Shader auf konstantes Magenta gesetzt

```glsl
fragColor = vec4(1,0,1,1);
```

Ergebnis:

- weiterhin schwarz

Damit war klar:

```text
PBR/Lighting ist NICHT die Hauptursache
```

---

## 2. GLDIAG-Instrumentierung

Es wurden Logs eingebaut für:

- BeginRenderPass
- SetPipeline
- Draw/DrawIndexed
- Tonemap Executor

Dadurch wurde sichtbar:

```text
TONEMAP läuft tatsächlich
Draw(3) wird ausgeführt
FBO=0 (Backbuffer)
```

Aber:

```text
depthTest=1
depthWrite=1
cull=1
```

im Fullscreen-Pass.

Das war der Durchbruch.

---

# Finaler Fix

Der Tonemap-/Fullscreen-Pass bekam wieder explizit korrekte Renderstates:

```text
Depth OFF
DepthWrite OFF
Cull OFF
```

ohne dass diese vom generischen Materialpfad überschrieben werden.

Danach renderte OpenGL wieder korrekt.

---

# Folgeproblem: Bild auf dem Kopf

Nachdem der Black Screen behoben war, erschien das Bild invertiert.

Das lag am unterschiedlichen Ursprung von RenderTargets:

- OpenGL: Bottom-Left Origin
- DX/Vulkan-Pipeline: Top-Left Erwartung

Behoben durch UV-Flip im Fullscreen-Tonemap-Pass:

```glsl
uv.y = 1.0 - uv.y;
```

---

# Wichtigste Erkenntnis

Der Fehler war kein klassischer Shader- oder Materialfehler.

Der eigentliche Bruch entstand dadurch, dass nach dem Materialsystem-Umbau:

```text
generische Material-Policies
postprocess-spezifische Pipeline-States überschrieben
```

Das ist architektonisch wichtig.

---

# Architektur-Lektion

Fullscreen-/Postprocess-Passes dürfen niemals implizit von generischen Material-Defaults abhängig sein.

Diese Passes brauchen:

- explizite Pipeline-States
- explizite Depth/Cull-Regeln
- eigene stabile Runtime-Policies

Sonst entstehen backendabhängige Fehler wie hier.

---

# Ergebnis

Nach dem Fix:

- OpenGL rendert wieder
- Tonemap-Pass funktioniert
- Backbuffer wird korrekt beschrieben
- DX11/Vulkan/OpenGL verhalten sich wieder konsistenter
- Diagnose-Logs konnten entfernt werden
