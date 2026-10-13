# Pass-Lock-Architektur – Post Mortem

## Ausgangslage

Nach dem OpenGL-Black-Screen-Incident (siehe `POSTMORTEM_OPENGL_BLACKSCREEN.md`) war der
unmittelbare Renderingfehler durch explizite Policy-Setzung am Tonemap-Material behoben:

```cpp
tonemapDesc.renderPolicy.depth.test  = false;
tonemapDesc.renderPolicy.depth.write = false;
tonemapDesc.renderPolicy.cull.mode   = MaterialCullMode::None;
```

Das System renderte wieder korrekt. Der eigentliche Architekturbruch blieb jedoch bestehen:

**Kein struktureller Mechanismus verhinderte, dass ein Material die kritischen States eines
Fullscreen- oder Postprocess-Passes überschreiben konnte.**

---

## Das strukturelle Problem

Der Fehlerklasse lag folgendes Ownership-Problem zugrunde:

```
Vorher (falsch):
  Material beschreibt gewünschte Render-States
  BuildPipelineDescForPass übernimmt diese States ungefiltert
  Pass hat keine Autorität über pass-kritische States
  → Material kann Fullscreen-Pass korrumpieren

Ziel:
  Pass hat Autorität über pass-kritische States (Locks)
  Material hat Autorität nur über material-eigene States
  BuildPipelineDescForPass merged beides mit klarer Priorität
```

Konkret: `ShaderRuntime::BuildPipelineDescForPass` hatte nur eine pass-spezifische Sonderbehandlung
— für den Shadow-Pass (VertexLayout-Reduktion, Depth-Format). Für Postprocess-/Fullscreen-Passes
existierte kein solcher Schutzmechanismus.

Ein Entwickler, der ein Tonemap-Material ohne korrekte `renderPolicy.depth.test = false` registriert,
hätte denselben Black-Screen-Fehler reproduziert. Auf DX11 und Vulkan wäre der Fehler wahrscheinlich
nicht aufgefallen.

---

## Risikobewertung vor der Sofortmaßnahme

| Szenario | Wahrscheinlichkeit | Impact |
|---|---|---|
| Neues Fullscreen-Material ohne explizite Depth-Policy | Hoch | Hoch (schwarzes Bild, OpenGL) |
| Refactor entfernt bestehende explizite Policy-Setzung | Mittel | Hoch |
| DX11/Vulkan verdeckt denselben Fehler dauerhaft | Hoch | Hoch (Fehler wird erst auf OpenGL sichtbar) |
| Shadow-Pass ohne DepthWrite durch Material-Override | Mittel | Hoch (Shadow Maps leer) |

Die alte Lösung war fragil, weil sie auf Konvention basierte (Entwickler setzt Policy korrekt) statt
auf Mechanismus (System erzwingt korrekte Policy).

---

## Implementierte Sofortmaßnahmen

### 1. PassLocks in RenderPassRegistry

**Datei:** `include/renderer/RenderPassRegistry.hpp`

Neuer Struct `PassLocks` als Teil von `RenderPassDesc`:

```cpp
struct PassLocks {
    bool depthTestLocked  = false;
    bool depthWriteLocked = false;
    bool cullModeLocked   = false;

    bool     depthTestValue  = false;
    bool     depthWriteValue = false;
    CullMode cullModeValue   = CullMode::None;
};
```

Pass-Locks beschreiben, welche States ein Pass absolut erzwingt. Kein Material kann diese
überschreiben. Die Locks sind Daten — keine Logik im Backend, keine implizite Konvention.

---

### 2. Vorbesetzung aller Standard-Passes

**Datei:** `src/renderer/RenderPassRegistry.cpp`

```
Postprocess: DepthTest=OFF, DepthWrite=OFF, Cull=None  (locked)
UI:          DepthTest=OFF, DepthWrite=OFF, Cull=None  (locked)
Shadow:      DepthTest=ON,  DepthWrite=ON              (locked)
Opaque:      keine Locks (Material hat volle Freiheit)
Transparent: keine Locks
AlphaCutout: keine Locks
```

Shadow-DepthTest/DepthWrite wird ebenfalls gelockt, weil ein Shadow-Pass ohne DepthWrite
strukturell falsch ist — analog zum Postprocess-Problem, nur mit umgekehrten Werten.

---

### 3. ApplyPassLocks in BuildPipelineDescForPass

**Datei:** `src/renderer/ShaderRuntimeMaterials.cpp`

Am Ende von `ShaderRuntime::BuildPipelineDescForPass` wird `ApplyPassLocks()` aufgerufen.
Die Funktion korrigiert pass-kritische States nach dem Material-Policy-Merge:

```cpp
static bool ApplyPassLocks(PipelineDesc& pd, RenderPassID pass) noexcept
{
    bool corrected = false;
    if (pass == Postprocess() || pass == UI()) {
        if (pd.depthStencil.depthEnable)             { pd.depthStencil.depthEnable = false; corrected = true; }
        if (pd.depthStencil.depthWrite)               { pd.depthStencil.depthWrite  = false; corrected = true; }
        if (pd.rasterizer.cullMode != CullMode::None) { pd.rasterizer.cullMode = CullMode::None; corrected = true; }
    }
    else if (pass == Shadow()) {
        if (!pd.depthStencil.depthEnable) { pd.depthStencil.depthEnable = true; corrected = true; }
        if (!pd.depthStencil.depthWrite)  { pd.depthStencil.depthWrite  = true; corrected = true; }
    }
    return corrected;
}
```

In Debug-Builds: Wenn `corrected == true`, wird eine Warnung ausgegeben mit dem Materialnamen und
der Pass-ID. Der Fehler ist damit sichtbar ohne den Betrieb zu unterbrechen.

Dasselbe Muster wurde parallel in `src/renderer/MaterialRuntimeBridge.cpp` implementiert (alternativer
Pipeline-Pfad).

---

### 4. Expliziter Pass in ForwardFeature.cpp

**Datei:** `addons/forward/ForwardFeature.cpp`

Vorher:
```cpp
shaderRuntime->BindMaterial(*execCtx.cmd, tonemapMaterialSource, material,
    BufferHandle::Invalid(), BufferHandle::Invalid(), BufferHandle::Invalid())
// passOverride default = StandardRenderPasses::Opaque()
```

Nachher:
```cpp
shaderRuntime->BindMaterial(*execCtx.cmd, tonemapMaterialSource, material,
    BufferHandle::Invalid(), BufferHandle::Invalid(), BufferHandle::Invalid(),
    StandardRenderPasses::Postprocess())
```

Der Tonemap-Pass lief bisher unter dem falschen Pass-ID `Opaque`. Das war die eigentliche Ursache,
warum der erste Workaround (explizite Policy) funktionierte, aber die Pass-Locks nicht gegriffen
hätten: `ApplyPassLocks` für `Postprocess` wurde nie erreicht, weil der Pass `Opaque` war.

Mit dem expliziten `Postprocess`-Pass:
- greifen die Pass-Locks strukturell
- ist der Pipeline-Cache-Key semantisch korrekt
- ist der `renderPass`-Metadatenwert konsistent mit der tatsächlichen Nutzung

---

### 5. Konsistente renderPass-Metadaten

**Dateien:** `ExampleApp.cpp`, `PbrShadowScene.cpp`, alle `alt/`-Beispiele

```cpp
// Vorher (inkonsistent):
tonemapDescRuntime.renderPass = renderer::StandardRenderPasses::Opaque();

// Nachher (korrekt):
tonemapDescRuntime.renderPass = renderer::StandardRenderPasses::Postprocess();
```

Der `renderPass`-Wert in `MaterialRuntimeDesc` war ursprünglich `Opaque`, weil der `BindMaterial`-
Default-Parameter ebenfalls `Opaque` war. Beides war falsch — der Tonemap-Pass ist semantisch ein
Postprocess-Pass.

---

## Warum OpenGL weiterhin funktioniert

Eine berechtigte Frage bei Architekturänderungen am Rendering-State-Pfad: Ändert sich etwas am
tatsächlichen GL-State?

Nein — und zwar aus einem spezifischen Grund:

`OpenGLCommandList::SetPipeline` setzt **alle** relevanten States explizit in beiden Zweigen:

```cpp
// Depth — BEIDE Zweige explizit
if (p->depthTest) { glEnable(GL_DEPTH_TEST); }
else              { glDisable(GL_DEPTH_TEST); }
glDepthMask(p->depthWrite ? GL_TRUE : GL_FALSE);

// Blend — BEIDE Zweige explizit
if (p->blendEnable) { glEnable(GL_BLEND); ... }
else                { glDisable(GL_BLEND); }

// Cull — BEIDE Zweige explizit
if (p->cullEnable) { glEnable(GL_CULL_FACE); glCullFace(...); }
else               { glDisable(GL_CULL_FACE); }
```

Der OpenGL-State-Automat wird pro Pipeline-Bind vollständig und deterministisch gesetzt.
Es gibt keinen impliziten Zustand der überlebt.

Der ursprüngliche Fehler entstand nicht in `SetPipeline`, sondern **davor** — im `OGLPipelineState`
der mit `depthTest=true` gebaked wurde, weil `BuildPipelineDescForPass` falsche States durchließ.

Nach der Sofortmaßnahme:
- `BuildPipelineDescForPass` erzwingt für den Postprocess-Pass `depthEnable=false`
- `CreatePipeline` baked `depthTest=false` in den `OGLPipelineState`
- `SetPipeline` ruft `glDisable(GL_DEPTH_TEST)` auf
- OpenGL rendert korrekt

Die Änderungskette ist sauber, deterministisch und backend-neutral.

---

## Was diese Maßnahmen NICHT lösen

Ehrliche Einschätzung dessen, was die Sofortmaßnahmen bewusst offen lassen:

### Pass-Locks sind noch nicht data-driven für neue Passes

`ApplyPassLocks` prüft aktuell hardcodiert gegen `StandardRenderPasses`. Ein neuer Custom-Pass
mit `PassLocks`-Daten in der Registry wird von `ApplyPassLocks` noch nicht automatisch
berücksichtigt. `ShaderRuntime` hat keinen direkten Zugang zur `RenderPassRegistry`.

Langfristiger Pfad: `ShaderRuntime` erhält einen Registry-Pointer. `ApplyPassLocks` iteriert
über die `PassLocks`-Daten statt hardcoded IDs zu prüfen.

### MaterialDomain-Konzept fehlt noch

Materialien haben noch kein `domain`-Feld (Mesh, Fullscreen, Shadow, UI). Eine Domain würde
ermöglichen:
- Compile-Time-Constraint: Fullscreen-Material kann strukturell keine Shadow-Variante haben
- Frühere Validation beim `RegisterMaterial`-Aufruf statt erst beim Pipeline-Build
- Klare API für Material-Editoren

### Keine Prüfung bei RegisterMaterial

Die Validation greift heute erst in `BuildPipelineDescForPass` — also beim ersten Rendering.
Ein Fullscreen-Material mit `depth.test=true` wird beim Registrieren nicht abgelehnt.
Die Debug-Warnung erscheint erst beim ersten Frame.

---

## Wichtigste Erkenntnis

Der Fehler war kein Implementierungsfehler sondern ein Ownership-Fehler.

```
Alte Regel (implizit, verletzbar):
  "Fullscreen-Materialien müssen manuell korrekte Depth/Cull-Policies setzen."

Neue Regel (strukturell, unverletzbar):
  "Postprocess- und UI-Passes erzwingen ihre States.
   Materialien dürfen pass-kritische States nicht überschreiben."
```

Die entscheidende Verschiebung: von Konvention zu Mechanismus.

---

## Ergebnis

Nach der Sofortmaßnahme:

- Fullscreen-/Tonemap-Passes können strukturell nicht mehr falsche Depth/Cull-States bekommen
- Shadow-Passes können strukturell nicht mehr fälschlicherweise DepthWrite=OFF erhalten
- Debug-Builds warnen automatisch bei pass-material-Konflikten mit Materialname und Pass-ID
- OpenGL rendert weiterhin korrekt — `SetPipeline` war nie das Problem
- DX11 und Vulkan sind unverändert korrekt
- Der Pipeline-Cache-Key für Tonemap ist jetzt semantisch korrekt (`Postprocess` statt `Opaque`)
- Alle Tonemap-Registrierungen sind konsistent auf `Postprocess` gesetzt

Der Fehlerklasse "Material überschreibt pass-kritische States" ist durch `ApplyPassLocks`
strukturell ausgeschlossen — nicht durch Konvention.
