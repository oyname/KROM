# KOORDINATION – Claude (Implementierung) ↔ Codex (Review/Validierung)

Regeln: Claude implementiert, Codex prüft fertige Zwischenstände und ändert keine
Dateien außer dieser. Befunde von Codex unten unter „Befunde (Codex)“ eintragen;
Claude liest sie vor jedem Arbeitsschritt und vermerkt, was behoben ist. Ältere
Befunde gelten erst nach Abgleich mit dem aktuellen Quellcode als offen.

Ältere Einträge (bis 2026-11-06, u. a. „Plan: Editor weitergeben“ und
„Punkt 1: Flexible Build-Werkzeuge“) stehen in
`doc/KOORDINATION-archiv-2026-11-06.md`.

---

### Neu (Claude, 2026-11-05): Absolute Editor-Materialpfade in Szenen/Prefabs

Befund: `mask2.prefab` (und die Baseline in `Testraum.json`) enthielten auf Mesh-Records
`"materialAssetPath": "F:/working/krom codex/editor/dpad_gelb.mat"` – das Gizmo-Material.
Ursache: Die Translate/Rotate/Scale-Gizmos merken ihre Entities in Statics
(`Get*GizmoRuntime`) und prueften nur `IsAlive`. Undo/Redo, Szenenwechsel und
Prefab-Modus zerstoeren und erzeugen *alle* Entities neu; die Generation hat nur 8 Bit.
Nach 255 Zyklen passt eine alte Gizmo-ID wieder auf ein Szenen-Mesh, und
`Update*Gizmo` schreibt Gizmo-Material (absolut), Transform und Active-Flag hinein
(inkl. `slotOverrides.clear()`).

Umsetzung:
- `EditorUIViewport.cpp`: `IsLiveGizmoEntity<TGizmoComponent>` prueft zusaetzlich
  `EditorRuntimeGizmoTag` + eigene Gizmo-Komponente; alle Ensure/Update/SetActive-Pfade
  nutzen das. Vor dem Neuerzeugen raeumt `DestroyGizmoEntities` noch lebende echte
  Gizmo-Entities ab (kein Leck bei teilweise zerstoertem Gizmo).
- Speicherregel: `IsPortableAssetPath` (core/PathUtf8.hpp) – nur relativ zu Assets,
  kein Laufwerk/Root, kein Ausbruch per `..`. Scene-Serializer (`MeshRendererSerialization`)
  und Prefab-Writer (`WriteRecord`, auch Baseline) speichern nicht-portable Materialpfade
  (Entity und Slot) nicht, samt zugehoerigem `materialOverrideSource`, mit Warnung.
  Laden unveraendert.

Pruefung: neue Tests „Portable asset path rule“, „Scene drops non-portable material paths“
(Serialization), „JSON drops non-portable material paths“ (Prefab). Gizmo-Fix ohne
Unit-Test (statische Funktionen im Viewport), Editor nicht visuell geprueft.

Pruefauftraege Codex: Gizmo-Highlight/Wechsel Translate/Rotate/Scale nach Undo/Prefab-Modus;
ob weitere statische EntityID-Caches im Editor dasselbe Muster haben.

### Neu (Claude, 2026-11-06): Werkzeugpfade von Hand setzen (Ergaenzung zu Plan Punkt 1)

Ziel: Bei der Weitergabe als EXE soll sich ein Nutzer selbst helfen koennen, wenn die
automatische Suche ein Werkzeug nicht findet oder ein anderes nehmen soll. Die
automatische Erkennung bleibt der Normalfall; die Eingabe ist nur ein Override.
Branch `feature/shadow-caster-culling`, noch nicht committet, zur Pruefung bereit.

- Speicherort: persoenliche `editor-preferences.json` (`%LOCALAPPDATA%\KROM`), Felder
  `toolVisualStudio/toolCMake/toolNinja/toolDxc/toolGlslang` (UTF-8). Bewusst **nicht**
  in `editor.settings` des Projekts (rechnerspezifisch, gehoert nicht ins Repo).
- `app/editor/BuildTools.*`: `BuildToolOverrides` (installation, cmake, ninja).
  Installation gesetzt → kein vswhere, nur deren Toolsets (gleiche Toolset-Auswahl
  und ABI-Regel wie bisher). CMake/Ninja gesetzt → ersetzen Installation und PATH.
  Override, das nicht existiert → Fehler (`build.vs_override_invalid`,
  `build.tool_override_missing`), **kein** stiller Rueckfall.
  `ToolDiscovery::installationOverridden`; `GetBuildTools` cacht je Compiler + Overrides.
  `RuntimeBuildRequest::toolOverrides` wird im Hauptthread aus den Einstellungen kopiert.
- Neu `include/renderer/ShaderToolPaths.hpp` / `src/renderer/ShaderToolPaths.cpp`:
  `FindShaderTool(Dxc|Glslang)` mit Reihenfolge Override → `DXC_PATH` (nur dxc) →
  `VULKAN_SDK/Bin` → PATH, liefert auch die Quelle. Ersetzt `FindDxcExe` und die
  glslang-Suche in ShaderCompilerSPIRV. Umgebungsvariablen jetzt per `_wgetenv`
  (Umlaut-Pfade). Override thread-sicher (Mutex), gesetzt von
  `ApplyEditorToolPreferences` beim Laden und Speichern der Einstellungen.
- UI: Editor-Einstellungen > Externe Tools > „Build- und Shader-Werkzeuge“: je Werkzeug
  Feld (Hinweis „automatisch“) + Durchsuchen, Anfuehrungszeichen/Leerzeichen werden
  entfernt („Als Pfad kopieren“). „Werkzeuge pruefen“ laeuft im Hintergrund-Thread mit
  den noch nicht uebernommenen Werten und zeigt VS (Quelle vswhere/manuell), Toolset,
  `_MSC_FULL_VER` (rot bei Script-ABI-Abweichung), CMake, Ninja, DXC, glslang.
- README-Absatz ergaenzt, Locale-Keys de/en.
- Tests: `krom_build_tools_tests` neue Faelle (Installations-Override ohne vswhere,
  ungueltiger Ordner, fehlendes CMake, CMake/Ninja-Override + Cache) 0 Fehler;
  `krom_tests` neuer Test „Shader tool override“, alle bestanden. Debug x64
  `krom_editor_vulkan`/`krom_editor_dx11` gebaut (`out/claude-tooloverride-*.log`).
- Nicht geprueft: Einstellungsseite optisch/interaktiv im Editor, Durchsuchen-Dialoge,
  echter Rechner nur mit Build Tools.

Pruefauftraege Codex: Ist der fehlende Rueckfall bei ungueltigem Override richtig? Bleibt
die ABI-Pruefung mit Installations-Override unveraendert streng? (Grep fand keine
weiteren Stellen, die `dxc`/`glslang`/`VULKAN_SDK` selbst suchen.)

## Befunde (Codex)

(noch keine)
