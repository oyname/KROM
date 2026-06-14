# PostMortem: Job-System-Integration in den Engine-Core

**Datum:** 2026-05-06  
**Kontext:** Parallelisierung der KROM Engine — Stand nach erster produktiver Integration

---

## Ausgangslage

Das Job-System (`jobs/JobSystem`) existierte vor dieser Phase bereits als isoliertes Subsystem,
wurde jedoch nur intern in der Renderer-Schicht und für Hintergrundaufgaben (Asset-Loading)
genutzt. Der Engine-Core — insbesondere Scene Traversal, Transform-Hierarchie und
Renderable-Extraktion — lief vollständig seriell auf dem Main-Thread.

Das Ziel: Job-System produktiv in den Core-Datenpfad bringen, ohne die ECS-Semantik
zu brechen und ohne Lock-Contention auf dem kritischen Frame-Pfad einzuführen.

---

## Was wurde umgesetzt

### 1. Job-System-Infrastruktur (vollständig)

`include/jobs/JobSystem.hpp` / `src/jobs/JobSystem.cpp` bieten heute:

| Feature | Status |
|---|---|
| Thread-Pool (N Worker, konfigurierbar) | ✅ |
| Zwei Prioritäts-Queues (Frame / Background) | ✅ |
| `ParallelFor<F>(itemCount, fn, minBatch)` | ✅ |
| `Dispatch` / `DispatchBackground` | ✅ |
| `DispatchResult` (Future + TaskResult) | ✅ |
| `DispatchReturn<T>` (Future + ValueResult<T>) | ✅ |
| `WaitIdle` (Main-Thread-Barrier) | ✅ |
| `IsWorkerThread` / `IsAnyWorkerThread` | ✅ |
| `PeakActiveWorkers`-Metrik | ✅ |

Das System ist pool-aware: Worker von Pool A gelten nicht als Worker von Pool B,
was mehrere gleichzeitige Pools ohne falsche Assert-Auslöser erlaubt.

### 2. Renderable-Extraktion parallelisiert

`addons/mesh_renderer/MeshRendererExtraction.cpp` implementiert ein Zwei-Pass-Modell:

```
Pass 1 (seriell):  world.View() → EntityID-Liste filtern
Pass 2 (parallel): ParallelFor → RenderProxy-Slots befüllen
```

Der serielle Pass ist notwendig, weil `world.View()` keine nebenläufigen strukturellen
Zugriffe unterstützt. Danach schreibt jeder Worker in einen disjunkten Proxy-Slot —
keine Locks, kein Contention.

```cpp
// MeshRendererExtraction.cpp:85-116
const auto result = js.ParallelFor(entities.size(), [&](size_t begin, size_t end)
{
    for (size_t i = begin; i < end; ++i)
    {
        // world.Get<T>() — lesend, concurrent-safe
        // proxies[i] — disjunkter Slot, kein Sharing
    }
});
```

Der Aufrufer wählt den Pfad per optionalem `jobs::JobSystem*`-Parameter —
Fallback auf seriellen Pfad wenn `nullptr`.

---

## Was noch seriell läuft (offene Arbeit)

### 1. Transform-Hierarchie — strukturell seriell

**Datei:** `src/scene/TransformSystem.cpp`

`TransformSystem::Update()` iteriert über `m_sortedEntities` in BFS-topologischer Reihenfolge.
Das ist korrekt und notwendig: ein Kind darf erst berechnet werden, nachdem sein Elternteil
fertig ist. Diese Abhängigkeitskette macht eine naïve Parallelisierung ungültig.

**Lösungsansatz (nicht umgesetzt):**
- Level-Parallelismus: alle Entities auf Ebene N können parallel berechnet werden,
  bevor Ebene N+1 beginnt. BFS liefert die Ebenen bereits implizit.
- Alternativ: Dirty-Propagation als separater Job-Graph mit Dependency-Countern.

**Risiko:** Hohe Implementierungskomplexität für Szenen mit flachen Hierarchien
(< 3 Ebenen) — dort ist der Serialisierungsoverhead ohnehin minimal.

---

### 2. Scene Traversal / `world.View()` — nicht concurrent-safe

`world.View<C1, C2, ...>(fn)` ist aktuell nicht thread-sicher für nebenläufige
Aufrufe: die Archetype-Iteration ist nicht durch Locks geschützt, und strukturelle
Mutationen (Entity-Erstellung, Komponenten-Attachment) dürfen nicht gleichzeitig
mit Lesezugriffen stattfinden.

**Lösungsansatz (nicht umgesetzt):**
- Read-Epoch / Snapshot-Mechanismus: Strukturänderungen nur zwischen Frames erlauben,
  Lesephase für Jobs freigeben.
- Alternativ: `EntityCommandBuffer` (bereits vorhanden, `include/ecs/EntityCommandBuffer.hpp`)
  konsequenter einsetzen, sodass Mutationen in die Post-Frame-Phase verschoben werden.

---

### 3. Lighting- und Shadow-Extraktion — seriell

**Dateien:** `addons/lighting/LightingExtraction.cpp`, `addons/shadow/`

Die Licht- und Shadow-Extraktion folgt demselben `world.View()`-Muster wie die
Mesh-Extraktion, nutzt aber noch keinen parallelen Pfad. Extraktion aller
Render-relevanten Daten (Meshes, Lights, Shadows) könnte als drei unabhängige
Jobs gleichzeitig laufen — sie schreiben in getrennte Puffer.

**Lösungsansatz (nicht umgesetzt):**
- Dispatch der drei Extraktionsphasen als parallele Frame-Jobs.
- `WaitIdle()` erst nach allen drei Jobs — dann erst Render-Übergabe.

---

### 4. Render-Stages im ForwardFeature — seriell

**Datei:** `addons/forward/ForwardFeature.cpp`

Die Pass-Sequenz (Shadow → Opaque → Transparent → Tonemap → Present) ist
sequenziell implementiert. GPU-Commands werden in Blöcken aufgebaut und
am Ende submitted.

**Lösungsansatz (nicht umgesetzt):**
- Paralleles Command-List-Recording: jeder Pass baut seine Command-List auf
  einem eigenen Worker-Thread. GPU-Submission erfolgt seriell aus der fertigen
  Liste.
- Voraussetzung: Backend-seitige Multi-Thread-Unterstützung (DX11 deferred context,
  Vulkan secondary command buffers).

---

### 5. Material- und Ressourcenzugriffe — ungeschützt für Parallelzugriff

Material-Lookups (`MaterialSystem`, `PipelineCache`) sind aktuell nicht für
gleichzeitige Lesezugriffe aus Job-Threads abgesichert. Solange Render-Stages
seriell laufen, ist das kein Problem. Sobald Command-List-Recording parallelisiert
wird, muss hier ein `shared_mutex` oder eine Read-Only-Snapshot-Strategie ergänzt werden.

---

## Fazit: Was der aktuelle Stand leisten kann

```
Frame-Datenpfad heute:

Main-Thread:
  TransformSystem::Update()           ← seriell (Hierarchie-Abhängigkeit)
  world.View() → EntityID-Filter      ← seriell (ECS-Constraint)

Job-Threads (ParallelFor):
  RenderProxy-Befüllung               ← ✅ parallel

Main-Thread:
  LightingExtraction                  ← seriell
  ShadowExtraction                    ← seriell
  ForwardFeature Render-Passes        ← seriell
  GPU Submit                          ← seriell (must)
```

Der wichtigste Fortschritt: Das Job-System ist kein Satelliten-Feature mehr,
sondern ein dokumentierter Erweiterungspunkt im Core-Datenpfad. Die Zwei-Pass-
Trennung in der Mesh-Extraktion ist das Muster, das für Lighting und Shadow
direkt repliziert werden kann.

---

## Priorisierte nächste Schritte

| Priorität | Aufgabe | Aufwand |
|---|---|---|
| 1 | Lighting- + Shadow-Extraktion parallelisieren (Zwei-Pass-Muster übertragen) | niedrig |
| 2 | Alle drei Extraktionen als parallele Frame-Jobs dispatchen | mittel |
| 3 | ECS Read-Epoch / Frame-Freeze für concurrent `world.View()` | hoch |
| 4 | TransformSystem: Level-Parallelismus (BFS-Ebenen als Batches) | mittel |
| 5 | Command-List-Recording in separate Worker-Threads (Backend-Voraussetzungen prüfen) | hoch |
| 6 | MaterialSystem / PipelineCache: `shared_mutex` für parallele Lesezugriffe | niedrig |
