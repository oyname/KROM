# PostMortem: Austauschbarkeit von Render-Pipelines in KROM

**Datum:** 2026-05-05  
**Kontext:** Bewertung Forward → Forward+ Migrationsstrategie

---

## Ausgangsfrage

Soll Forward+ als komplett neues Addon (`krom-forward-plus`) oder als Erweiterung des
bestehenden `krom-forward`-Addons gebaut werden?

---

## Befund: Was die Architektur bereits leistet

### 1. Feature ≠ Pipeline

Das Addon-System trennt sauber zwischen zwei Konzepten:

| Konzept | Interface | Verantwortung |
|---|---|---|
| **Feature** | `IEngineFeature` | Lebenszyklusverwaltung, Registrierung |
| **Pipeline** | `IRenderPipeline` | Frame-Aufbau, Pass-Sequenz, Draw-Ausführung |

Ein Feature *hostet* eine oder mehrere Pipelines. Es registriert sie über
`FeatureRegistrationContext::RegisterRenderPipeline(pipeline, makeDefault)`.

```cpp
// ForwardFeature.cpp – Zeilen 707-710
void Register(FeatureRegistrationContext& context) override
{
    context.RegisterRenderPipeline(m_pipeline, true);
}
```

### 2. FeatureRegistry unterstützt mehrere Pipelines

Die `FeatureRegistry` verwaltet intern eine Liste aller registrierten Pipelines und einen
aktiven Index (`m_activeRenderPipelineIndex`). Das letzte `makeDefault = true` gewinnt,
aber die Registry kann jederzeit auf eine andere Pipeline umschalten.

```cpp
// FeatureRegistry.cpp – Zeilen 295-298
m_registeredRenderPipelines.push_back({...});
if (makeDefault || m_activeRenderPipelineIndex == kInvalidRegistrationIndex)
    m_activeRenderPipelineIndex = m_registeredRenderPipelines.size() - 1u;
```

### 3. ForwardFeature ist bereits nur ein Pipeline-Host

`ForwardFeature` selbst enthält keine Render-Logik. Es erstellt intern eine
`ForwardRenderPipeline` und übergibt sie der Registry. Die gesamte Frame-Logik
liegt in der Pipeline-Implementierung.

```cpp
// ForwardFeature.cpp – Zeile 700
m_pipeline(std::make_shared<ForwardRenderPipeline>(config, m_skyResources))
```

---

## Entscheidung: Erweiterung statt neues Addon

### Warum kein neues Addon

Ein neues Addon (`krom-forward-plus`) lohnt sich erst, wenn mindestens eines dieser
Kriterien zutrifft:

- andere Feature-Abhängigkeiten (andere `GetDependencies()`-Liste)
- eigenständiger Framegraph-Aufbau ohne Überschneidung
- anderer Ressourcenlebenszyklus (eigene Texturen, Buffer-Layouts)
- bewusst getrenntes Versionierungs- und Aktivierungsmodell

Für Forward+ trifft **keines** davon zu: gleiche Materialsysteme, gleiche DrawLists,
gleiche RenderWorld-/Lighting-/Shadow-Daten, gleiche Präsentationskette.

### Warum Erweiterung im selben Addon

Forward+ ist eine **Evolution** des gleichen Renderers, nicht ein anderes Produkt:

- **Gemeinsame Infrastruktur:** Pass-Scaffold, Opaque-/Transparent-Draw-Ausführung,
  Tonemap, Sky, Present-Integration, Shadow-Sampling
- **Nur dieser Teil ist neu:** Light-Culling-Pass (Compute), Tile-Buffer-Bindung,
  angepasster Shader-Lichtloop

Das rechtfertigt zwei Pipeline-Klassen *innerhalb* desselben Addons, nicht zwei Addons.

---

## Zielarchitektur

```
addons/forward/
├── ForwardFeature.cpp          ← Feature-Host, unverändert
├── ForwardRenderPipeline.*     ← bestehende Pipeline
├── ForwardPlusRenderPipeline.* ← neue Pipeline (Light Culling + Tile-Buffer)
└── ForwardShared.*             ← extrahierte gemeinsame Basis (Pass-Scaffold, Draw)
```

### Konfiguration

```cpp
enum class ForwardPath { Forward, ForwardPlus };

struct ForwardFeatureConfig
{
    ForwardPath path = ForwardPath::Forward;
    // ... bestehende Felder
};
```

`CreateForwardFeature(config)` instanziiert je nach `config.path` die passende Pipeline:

```cpp
if (config.path == ForwardPath::ForwardPlus)
    m_pipeline = std::make_shared<ForwardPlusRenderPipeline>(config, m_skyResources);
else
    m_pipeline = std::make_shared<ForwardRenderPipeline>(config, m_skyResources);
```

---

## Was Forward+ im Vergleich zu Forward neu braucht

| Komponente | Forward | Forward+ |
|---|---|---|
| Light-Culling-Pass | — | Compute-Pass (Tile 16×16) |
| Tile-Light-Buffer | — | Structured Buffer (GPU) |
| Lichtloop im Shader | globale Liste (≤7) | Tile-lokale Liste |
| Compute-Unterstützung | nicht benötigt | DX11 FL 11.0+ / Vulkan |
| Draw-Ausführung | identisch | identisch |
| Shadow-Sampling | identisch | identisch |
| Tonemap/Present | identisch | identisch |

---

## Wann doch ein neues Addon sinnvoll wäre

Nur wenn die Entscheidung bewusst fällt:

> „`krom-forward` bleibt minimal und stabil.  
> `krom-forward-plus` ist ein eigenständiger High-End-Renderer mit eigener Roadmap."

Dann wären zwei Addons organisatorisch gerechtfertigt — technisch ist es möglich, weil
die `FeatureRegistry` mehrere unabhängig registrierte Pipelines verwalten kann.

---

## Fazit

Die KROM-Architektur ist für diesen Schritt bereits vorbereitet. `IRenderPipeline` ist
der richtige Erweiterungspunkt. Das Addon bleibt `krom-forward`, bekommt aber intern
eine zweite Pipeline-Implementierung. Die gemeinsame Basis wird dabei explizit extrahiert,
sodass beide Pfade wartbar bleiben.
