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

### Neu (Claude, 2026-11-06): Frame-Reihenfolge an einer Stelle (GameSimulation::RunFrame)

Befund: Runtime (`KromWrapper.cpp`) und Editor-Play (`KromEditorApp`) schrieben die
Frame-Reihenfolge je selbst aus und wichen ab: Editor-Play rief kein AnimationSystem auf.
Branch `feature/shadow-caster-culling`, noch nicht committet, zur Pruefung bereit.

- `GameSimulation::RunFrame(world, unscaledDt, input, hooks, GameFrameSteps)` legt die
  Reihenfolge fest: BeginFrame -> `beforeScripts(dt)` -> UpdateScripts (Scripts, Physik)
  -> AnimationSystem (optional) -> `beforeTransforms()` -> TransformSystem -> BoundsSystem
  -> UpdateLate (Audio, Partikel). BeginFrame/UpdateScripts/UpdateLate bleiben oeffentlich.
  `engine_gameplay` linkt jetzt `engine_animation` (nur core + renderer_api-Header).
- Runtime: Tick-Callback und User-Systeme in `beforeScripts`, LocalBounds in
  `beforeTransforms`. SkinningSystem::Upload und Live-State-Snapshot jetzt nach RunFrame
  (vorher vor Transform bzw. vor Animation; beides ohne Einfluss auf Transforms).
- Editor: `TickEditorPlayScripts/Late` ersetzt durch `TickEditorPlay(dt, editorSync)`.
  Editor-Sync (LocalBounds, Gizmos, Vorschau, Thumbnails, Konsole) laeuft im Play als
  `beforeTransforms`, sonst wie bisher vor Transform/Bounds. Neu: Editor-Play hat ein
  eigenes AnimationSystem (`m_playAnimation`, lazy) und SkinningSystem (`m_playSkinning`,
  lazy mit Device): nach RunFrame `Upload` + `GarbageCollect` (Buffer von per Destroy/
  LoadScene entfernten Entities). Freigabe bei StopEditorPlay und Shutdown (vor dem Device).
  Der Renderer liest den Bone-Buffer ueber `SkinComponent::gpuBuffer` (MeshRendererExtraction);
  Vulkan gibt zerstoerte Buffer erst nach der Fence frei.
- Test: `krom_tests` „Game frame: RunFrame order“ (Hook-Reihenfolge, skaliertes Delta,
  Transform sieht Hook-Schreibzugriff); alle Tests bestanden. Gebaut: Debug x64
  `krom_editor_vulkan`, `krom_editor_dx11`, `engine_krom_wrapper_win32`
  (`out/claude-runframe-*.log`). GLFW-Wrapper nicht gebaut.
- Laufzeit: `krom_editor_loading_tests_dx11 --play-ui` 25 PASS (Play/Pause/Stop, Scripts,
  Partikel); `krom_scene_lifetime_tests` (Wrapper-Frame) 11 PASS, Exit 0.
  Nach Skinning-Nachruestung erneut: krom_tests alle bestanden, --play-ui 25 PASS.
- Nicht geprueft: Animation sichtbar - im DemoProject hat kein GLB Animationen/Skins;
  braucht ein Testmodell (z. B. Khronos AnimatedCube, RiggedSimple). Export-Build nicht gebaut.

Pruefauftraege Codex: Verhaltensunterschied durch spaeteren Skinning-Upload/Snapshot?
Darf Editor-Sync (Gizmos, Thumbnails) im Play nach der Animation laufen (vorher davor)?
Ist die Skinning-Freigabe bei Stop sicher (DX11/OpenGL: Buffer noch im Flug)?

### Neu (Claude, 2026-11-06): Skinning fuer PBR-Materialien

Befund (CesiumMan im Editor-Play): Animation und Bone-Upload laufen, die Figur bleibt aber in
T-Pose. Editor-Material (Default bzw. glb-Material ueber das Prefab) ist PBR, und PBR hatte
keinen Skinning-Vertex-Shader. Zusaetzlich fehlte in `skinned_lit.vs.hlsl` das Vulkan-Binding:
Buffer-SRVs liegen bei 64 + Slot (`BufferSRVBase`), BonePalette (Slot 13) also bei 77; ohne
`[[vk::binding(77)]]` landete er bei 16 + 13 = 29.

- Neu `assets/skinned_pbr_lit.vs.hlsl` (aus pbr_lit.vs.hlsl erzeugt, gleiche Ausgaben, passt zu
  pbr_lit.ps / pbr_forward_plus.ps) und `assets/skinned_shadow_pbr.vs.hlsl` (Schatten folgt der
  Pose). Beide und `skinned_lit.vs.hlsl` mit `[[vk::binding(77)]]` unter `__spirv__`.
- `MeshAsset::HasSkinning()` (AssetRegistry.hpp); Wrapper-`MeshHasSkinning` nutzt es.
- Wahl bei Skinning (nur HLSL; OpenGL bleibt Bind-Pose, letzte Prioritaet): Layout
  `VertexContracts::SkinnedLit()`, VS `skinned_pbr_lit`, Schatten `skinned_shadow_pbr` in
  `CreatePbrMaterialFromAsset`/`LoadPbrShaders` (EditorMaterialLibrary), im PBR-Zweig von
  `CreateRuntimeMaterialForMesh` (EditorAssetBrowser, vorher fuer Skinning gesperrt) und in
  `CreateRuntimePbrMaterialFromAssets` (KromWrapper, Export).
- Pruefung: alle drei Skinning-Shader mit dxc (Engine-Optionen, SPIR-V, Binding 77, Locations
  7/8 = VertexSemantic) und fxc vs_5_0 kompiliert; krom_tests alle bestanden, --play-ui 25 PASS.
- Nicht geprueft: CesiumMan sichtbar animiert (Vulkan/DX11), Schatten, Export.

Pruefauftraege Codex: Weitere Materialwege ohne Skinning (Lit-Pfad Z. ~1375 in
EditorMaterialLibrary, Shader-Templates)? Bindet der Schattenpass den Bone-Buffer
(gemeinsamer executeDrawList)?

### Neu (Claude, 2026-11-06): Portable Modellpfade + Skelett/Animation beim Prefab-Laden

Befund: `CesiumMan.prefab` speicherte mesh/skeleton/animationAssetPath absolut
(`F:/.../CesiumMan.glb#skeleton/0`): `SpawnModel`/`OpenModelView` importieren mit absolutem
Pfad, `BuildPrefabFromImportedBundle` nimmt `meshes[0].path` als Basis. Zweitens luedt
niemand `#skeleton/N`/`#animation/N` nach (nur Meshes via ResolveMeshAssetBindings) ->
Animation ging nach Editor-Neustart und im Export verloren.

- `SetPrefabAssetPipeline(AssetPipeline*)` (Prefab.hpp), gesetzt von Editor und Wrapper nach
  Erzeugen der Pipeline, vor deren Freigabe auf nullptr. Beim Lesen jedes Records
  (Dateien, eingebettete Baselines): mesh/skeleton/animation-Pfade unter dem Asset-Root ->
  relativ (`ToAssetRelative`, fragment-erhaltend); fehlende Skelette/Animationen ->
  `ImportBundle(base)`, alle unter `base#skeleton/i` / `base#animation/i` registriert
  (gleiche Schluessel wie BuildPrefabFromImportedBundle). Ohne Pipeline altes Verhalten.
- Editor-Import (`UseAssetRelativeBundlePaths` in SpawnModel/OpenModelView): Bundle-Pfade
  relativ, damit Registry-Schluessel und Prefabs von Anfang an portabel sind.
  Thumbnail-Import bleibt absolut (eigene Schluessel).
- Prefab-Writer warnt bei nicht-portablen mesh/skeleton/animation-Pfaden, schreibt sie aber
  (Weglassen wuerde das Mesh verlieren).
- Test „Model paths: relative + animation import“ (selbst erzeugtes Rig-glTF, frische
  Registry): ohne Pipeline unveraendert; mit Pipeline relativ, Handles gueltig, Speichern
  ohne absoluten Pfad, Instanz hat Skin + AnimationPlayer. krom_tests alle bestanden,
  --play-ui 25 PASS, krom_scene_lifetime_tests 11 PASS.
- Nicht geprueft: CesiumMan nach Editor-Neustart im Editor, Export.

Pruefauftraege Codex: Globaler Pipeline-Zeiger ok (ein Editor/Runtime pro Prozess)? Kollision
relativer Mesh-Schluessel aus SpawnModel (ReplaceOrAddMesh) mit per Szene geladenen Meshes?

### Neu (Claude, 2026-11-06): VK_ERROR_DEVICE_LOST durch Skinning im Edit-Modus

Befund (User, Vulkan-Editor): `vkQueueSubmit failed (-4)` in Serie. Ursache aus dem
PBR-Skinning-Eintrag: Skinned-Material liest Binding 77, im Edit-Modus gab es aber keinen
Bone-Buffer (Animation/Upload nur im Play). Der Vulkan-Pfad schreibt ungebundene Buffer-SRVs
nicht (`continue`) -> Shader liest unbeschriebenen Deskriptor -> Device lost.

- `SkinningSystem(device, registry)`: leere Palette + gueltiges Skelett -> Einheitsmatrizen je
  Knochen (= Bind-Pose) hochladen. Wrapper und Editor uebergeben ihre Registry.
- Editor: `UploadSkinning()` jeden Frame (Edit und Play) nach Transform/Bounds; Upload aus
  `TickEditorPlay` entfernt, `m_playSkinning` -> `m_skinning`, Freigabe nur noch im Shutdown
  (GarbageCollect raeumt nach Stop/LoadScene).
- Vulkan-Absicherung: `m_fallbackBufferSRV` (256 Einheitsmatrizen, Structured) wird an jeden
  Buffer-SRV-Slot gebunden, den niemand gesetzt hat (`BuildDescriptorMaterializationState`).
  Layout hat alle 16 Slots (partially bound). Unbound Bone-Palette -> Bind-Pose statt Absturz.
- Pruefung: krom_tests alle bestanden; --play-ui Vulkan 25 PASS und DX11 25 PASS, je ohne
  Submit-Fehler; krom_scene_lifetime_tests 11 PASS. Kein Test mit Skinned-Mesh unter Vulkan.
- Nicht geprueft: CesiumMan im Vulkan-Editor (Edit und Play).

Nachtrag (User meldete weiter -4): mit `KROM_VALIDATION=1` reproduziert
(`krom_editor_loading_tests --perf DemoProject`, Szene Testraum mit CesiumMan). Eigentliche
Ursachen:
1. `ToVkFormat` kannte `RGBA32_UINT` (BoneIndex) nicht -> VK_FORMAT_UNDEFINED im Vertex-Input.
   Ergaenzt: R16/RG16/RGBA16_UINT, R32/RG32/RGB32/RGBA32_UINT, R32_SINT, RG16_FLOAT.
   Betraf jedes Skinning unter Vulkan, auch skinned_lit.
2. `BuildShadowVertexLayout` (ShaderRuntimeMaterials.cpp) behielt nur Position/UV ->
   skinned_shadow_pbr ohne Locations 7/8. Behaelt jetzt BoneWeight/BoneIndex.
- Danach: Perf-Lauf 360 Frames, 0 Submit-Fehler, 0 Vulkan-Validierungsfehler (2 Warnungen
  „location 2 not consumed“). krom_tests alle bestanden, --play-ui Vulkan/DX11 je 25 PASS,
  krom_scene_lifetime_tests 11 PASS. Test-Perf-Modus: Validierung per KROM_VALIDATION.
- Play mit Animation unter Vulkan weiterhin nicht automatisch geprueft.

Pruefauftraege Codex: Fallback-Buffer-SRV fuer alle Slots unproblematisch (Descriptor-Cache-
Schluessel, Performance)? DX11 liest ungebundene SRVs als 0 -> Mesh kollabiert, kein Absturz;
dort ebenfalls Fallback?

### Neu (Claude, 2026-11-06): Prefab-Modus waehrend Play gesperrt

Befund (User): Play -> Doppelklick auf Prefab -> Stop: ganze Szene im Prefab. Play stasht die
Edit-Szene, der Prefab-Modus stasht darueber die Spielwelt; `EndPlaySession` stellt die
Edit-Szene in den noch offenen Prefab-Modus zurueck, `EnforcePrefabModeSingleRoot` haengt alle
Wurzeln unter die Prefab-Wurzel (Log `[Reparent] ... newParent=...`). Beim Verlassen haette
SavePrefabMode die Szene ins Prefab geschrieben. Gespeichert wurde nichts (Szene/Prefab mit
Sicherung verglichen).

- `EnterPrefabMode`/`EnterModelView`: bei aktiver Play-Session abgelehnt, Statuszeile
  `prefab.blocked_during_play` (de/en).
- `EndPlaySession`: falls doch ein Prefab-Modus offen ist, wird er ohne Speichern verworfen
  (Kamera zurueck) bevor die Edit-Szene wiederhergestellt wird.
- Test „Prefab mode blocked during Play“ (EditorCommands, 12 Checks); krom_tests alle
  bestanden, --play-ui DX11 25 PASS.

Pruefauftrag Codex: Weitere Wege, waehrend Play die Welt auszutauschen (Szene wechseln,
Projekt oeffnen, Undo der Edit-Historie)?

### Neu (Claude, 2026-11-06): Antwort auf Codex-Review `6145576..ac82658` – Paket 1 (P1)

Codex-Befunde stehen im Codex-Worktree (6953), hier nur der Stand je Befund.

- **P1-1 Skin/Player in Szenen – behoben.** `SkinComponent::skeletonAssetPath`,
  `AnimationPlayerComponent::clipAssetPath`. Handler (Pfad, playing, looping, speed) in
  `RegisterMeshRendererSerializationHandlers`/`...Deserialization...` (dort schon an allen Stellen
  registriert: Snapshot, WriteEditorScene, Export, Runtime). `ResolveMeshAssetBindings` ruft am
  Ende `ResolveAnimationAssetBindings` (Pfad relativ, Lookup ueber Pfad-Index
  `Find*ByPath`, sonst `ImportModelAnimationAssets`). Prefab nutzt dieselben Funktionen;
  Instanziierung/Sync setzen die Pfade und legen Komponenten auch bei fehlendem Handle an.
  Tests: „Scene round trip keeps skinned animation“ (Serializer -> frische Registry ->
  Resolve -> AnimationSystem-Pose), „Play snapshot keeps skin and animation“.
- **P1-2 Runtime-Animationssysteme – behoben.** `EnsureAnimationSystems` vor der Frame-
  Schleife in `Run()`; damit auch Script-Spawns, Szenen und der Ein-Frame-Versatz abgedeckt.
- **P1-3 Neues Projekt / Neue Szene waehrend Play – behoben.** `CreateEditorProject` (vor dem
  Leeren der Welt) und `NewEditorScene` rufen `StopEditorPlay()` wie `LoadProjectFile`.
- Pruefung: krom_tests alle bestanden; --play-ui Vulkan/DX11 je 25 PASS; scene_lifetime 11 PASS;
  DemoProject Testraum Vulkan mit Validierung: 0 Submit-, 0 Validierungsfehler.
- Paket 2 siehe naechster Eintrag. Nicht praktisch geprueft: Rig im Export.

### Neu (Claude, 2026-11-06): Antwort auf Codex-Review – Paket 2 (P2)

- **P2-5 Bone-Slot / Bereich – behoben.** ForwardFeature setzt Slot 13 bei jedem Wechsel, auch
  auf ungueltig (DX11 null-SRV, Vulkan Fallback). Alle Skinning-VS pruefen per
  `KromBonesValid` (GetDimensions) die vier Indizes gegen die gebundene Palette; ausserhalb ->
  Bind-Pose fuer Position/Normal/Tangente (deckt DX11 ohne Palette und >256 Knochen ab).
- **P2-6 Upload jedes Frame – behoben.** `BoneBufferEntry::uploaded`: Upload nur bei geaenderter
  Palette (memcmp); Bind-Pose und Pause schreiben nicht mehr. Test „Skinning uploads only changed
  palettes“ (RecordingComputeDevice zaehlt Uploads).
- **P2-4 Pfade aus vorigem Projekt – behoben.** `ApplyEditorProjectPaths` leert nach
  `ClearPathCaches` die projektrelativen `path`-Felder von Mesh/Material/Skelett/Clip im Store
  (Handles bleiben gueltig). Absolute Pfade (Editor/Engine) und Wrapper-Laufzeit-Meshes
  (kein Projektwechsel) unveraendert. Prefab-Store-Suche trifft damit nur das aktive Projekt.
- **P2-7 Lit/Unlit – behoben bis auf Custom.** Neue `skinned_shadow.vs.hlsl`,
  `skinned_unlit.vs.hlsl`, Vertragsvariante `VertexContracts::SkinnedUnlit()`. LegacyLit und
  DefaultWhite: Tangenten erzeugen + `SkinnedLit` + skinned VS/Schatten; Unlit: `SkinnedUnlit` +
  skinned VS; Modellimport-Lit-Fallback und Runtime-Lit nutzen den skinned Schatten.
  Bewusst offen: Custom-Shader (vom Nutzer) bleiben statisch; OpenGL bleibt Bind-Pose.
- Pruefung: alle neuen Shader mit dxc (SPIR-V, mit/ohne KROM_ALPHA_TEST) und fxc vs_5_0;
  krom_tests alle bestanden; --play-ui Vulkan/DX11 je 25 PASS; scene_lifetime 11 PASS;
  DemoProject Testraum Vulkan + Validierung: 0 Submit-, 0 Validierungsfehler (2 Warnungen
  „location 2 not consumed“, vorbestehend).
- Nicht praktisch geprueft: sichtbare Animation mit Lit/Unlit-Material, Rig im Export.

### Neu (Claude, 2026-11-06): Alte Szenen – Prefab-Sync traegt Skin/Player nach

Befund (User): Play in der Szene animiert CesiumMan nicht (im Prefab-Modus schon).
`Testraum.json` wurde vor der Skin/Player-Serialisierung gespeichert; die verknuepfte Instanz
hat keine Komponenten. `ApplyChanges` kopiert nur geaenderte Felder, und das Override-Diff liest
die fehlenden Komponenten als lokale Entfernung.

- `RestoreSkinnedAnimation` (PrefabSynchronization.cpp) nach `ApplyChanges`: fehlen Skin bzw.
  Player an einer verknuepften Instanz, werden sie aus dem geerbten Prefab-Record ergaenzt.
  Entscheidung: Skin/Player sind nicht lokal entfernbar (kommen aus dem Modell); lokale
  playAnimation/loopAnimation gelten weiter, solange die Komponente existiert.
- Test „Prefab sync restores missing animation“; krom_tests alle bestanden, --play-ui
  Vulkan/DX11 je 25 PASS, scene_lifetime 11 PASS.
- Runtime: Instanzen werden dort nicht synchronisiert; alte Szenen einmal im Editor oeffnen
  und speichern, dann enthalten sie Skin/Player.

Pruefauftrag Codex: Ist „nicht lokal entfernbar“ vertretbar oder braucht es eine Markierung
fuer bewusst entfernte Animation?

### Neu (Claude, 2026-11-07): Hierarchie-Drag&Drop, Prefab per Drag, Startszene

Branch `feature/animation-skinning`, nicht committet, zur Pruefung bereit.

- Inspector: Buttons "Save as Prefab" und "Delete" entfernt. Loeschen nur noch ueber das
  Hierarchie-Kontextmenue (gleicher Bestaetigungsdialog). "Als Prefab speichern" auch aus
  den Kontextmenues entfernt; im Modell-Ansichtsmodus bleibt der Button in der Hierarchie.
- Prefab erstellen: Entity aus der Hierarchie ins Asset-Fenster ziehen. Inhaltsflaeche =
  aktueller Ordner, Ordner-Kachel/Ordnerbaum = dieser Ordner (`AcceptEntityToPrefabPayload`,
  `SaveEntityAsPrefab(ctx, entity, targetDir)`). Die Szenen-Entity wird wie bisher zur Instanz.
- Hierarchie: neuer Befehl `MoveEntityInHierarchy(entity, newParent, anchor, insertAfter)`
  (ein Undo-Schritt, Weltpose bleibt, No-op ohne Undo-Eintrag, Zyklen abgelehnt).
  Zeile oberes/unteres Viertel = davor/danach als Geschwister (Linie), Mitte = letztes Kind.
  Ablage ausserhalb der Zeilen (leere Flaeche, Kopf) = Szenenwurzel, ans Ende; ausgeschaltet,
  solange die Maus auf einer Zeile liegt (sonst wuerde Loslassen auf der eigenen Zeile
  entparenten). Auto-Scroll am oberen/unteren Rand waehrend des Ziehens.
- Startszene: `editor.settings` speichert `lastScene` (bei Schliessen/Projektwechsel/
  Einstellungen); beim Projekt-Laden zuerst diese, sonst wie bisher erste aus sceneOrder.

Pruefung: alle krom_tests gruen; neuer Test "Hierarchy drop reorders and unparents";
"Camera deletion requires confirmation" laeuft jetzt ueber das Flag statt den Inspector-Button.
Editor nicht visuell geprueft (Ziehlinie, Auto-Scroll, Drop ins Asset-Fenster).

Nachtrag (gleicher Tag):
- Kontextmenue "Prefab entkoppeln" (`UnpackPrefabInstance`, Undo): nimmt die aeusserste
  Instanz (`FindOutermostPrefabInstanceRoot`), entfernt `PrefabInstanceComponent` am Root und
  `PrefabSourceComponent` im Teilbaum; verschachtelte Instanzen bleiben verknuepft.
  Test "Unpack prefab instance keeps nested instances".
- Flackern beim Ziehen: Zeilen-Dropflaeche = volle Panelbreite + halber Zeilenabstand, damit
  Luecken/Einrueckung nicht auf das Wurzel-Ziel fallen; Wurzel-Ziel zeichnet keinen Rahmen
  mehr, sondern eine Linie am Listenende. (Ziehen nach links in die Einrueckung entparentet
  damit nicht mehr; dafuer unter die Liste ziehen.)

### Neu (Claude, 2026-11-07): Materialeditor als eigenes OS-Fenster (ImGui-Multi-Viewports)

Branch `feature/animation-skinning`, nicht committet, zur Pruefung bereit.

- `ImGuiConfigFlags_ViewportsEnable` fuer DX11 und Vulkan (Win32 und GLFW). OpenGL bleibt
  ohne (braucht WGL-Kontext-Hooks; letzte Prioritaet).
- Zusatzfenster werden **nach** `RenderLoop::Tick` gezeichnet (`UpdatePlatformWindows` +
  `RenderPlatformWindowsDefault`), also nach Submit/Present des Engine-Frames; Material-Vorschau
  und Scene-View sind dann fertig, die Engine-Commandlist ist zu. Nur wenn der UI-Pass in diesem
  Tick `ImGui::Render()` aufgerufen hat (`m_imguiFrameRendered`).
- DX11: Swapchains der Zusatzfenster im Format der Editor-Swapchain (sRGB, Bitblt-Swap-Effect),
  sonst waeren die linearen Theme-Farben zu dunkel. `ImGui_ImplDX11_SetSwapChainDescs` kopiert
  im Backend nur die erste Vorlage, daher genau eine.
- Vulkan/Win32: eigener `Platform_CreateVkSurface` (`vkCreateWin32SurfaceKHR`), vorher war
  `PlatformHasViewports` deshalb abgeschaltet. `PipelineInfoForViewports` mit dem Swapchain-Format.
- Editor: `detail::KeepNextWindowInMainViewport(name)` vor allen Fenstern ausser dem
  Materialeditor (Viewport, Game, Hierarchie, Inspector, Assets, Console, Kamera-Vorschau,
  Einstellungen, Layer, Overlays/Toolbar/Startscreen). Angedockte Fenster werden nicht
  gepinnt (folgen ihrem Dock-Host). `EditorWindowPos(x, y)` fuer Startpositionen relativ zum
  Editorfenster; Rueckfallpfade (`renderViewX/Y` ohne Panel, `BuildEditorMouseRay`,
  Perf-Overlay, `UpdateEditorLayout`) rechnen ab `GetMainViewport()->Pos` statt (0,0),
  da ImGui-Koordinaten mit Viewports Desktop-Koordinaten sind.

Pruefung: `krom_tests` gruen. Neuer Laufzeittest `krom_editor_loading_tests[_dx11] --detached-window`
(Materialeditor oeffnet im Editorfenster, per Maus-Drag rechts heraus -> eigener Viewport mit
OS-Fenster + Swapchain, Hierarchie bleibt im Hauptfenster, weitere Frames ok, Schliessen entfernt
das OS-Fenster) auf DX11 und Vulkan je 2x gruen, Vulkan mit Validierung ohne Fehler.
`--dock-drag`, `--headless`, `--play-ui`, `--settings-ui`, `--console-ui` auf beiden Backends gruen.
Nicht geprueft: echter zweiter Monitor (anderes DPI), Optik/sRGB der Zusatzfenster mit eigenen Augen.

Bekannte Grenze: Zwei gepinnte Panels, die zu einer schwebenden Dock-Gruppe zusammengedockt und
dann aus dem Fenster gezogen werden, koennen ebenfalls ein OS-Fenster bilden (Dock-Host wird
nicht gepinnt). Funktioniert, Overlays ueber dem Scene-View waeren dort aber unsichtbar.

Pruefauftraege Codex: Reihenfolge Zusatzfenster nach Present (Vulkan: gleiche Queue, Barrieren
der Engine im naechsten Frame decken Lesezugriffe der Zusatzfenster ab?); Pinning-Liste
vollstaendig?

### Neu (Claude, 2026-11-09): Node-Materialeditor – drei Fehler behoben

Branch `feature/animation-skinning`, nicht committet, zur Pruefung bereit. Der Graph selbst
(`MaterialGraph.hpp`, `PbrGraph.hpp`, `EditorMaterialGraph.*`, `pbr_material_graph.*`) ist
noch im Umbau und unversioniert.

- Alpha: Graph rechnet jetzt mit vec4. Konstanten haben Alpha 1, Add/Multiply multiplizieren
  das Alpha ihrer Eingaenge. Ist Base Color verbunden, gilt Graph-Alpha * `baseColorFactor.a`;
  eine verdeckte direkte Textur schneidet nicht mehr. Gilt auch im Schattenpass
  (`shadow_pbr.ps.hlsl`, `shadow_pbr.opengl.fs.glsl`).
- Editor: Bei verbundener Base Color ersetzt ein Alpha-Regler das Farbfeld; bei verbundenem
  Emissive entfaellt das Farbfeld.
- Compiler: Texturknoten ohne Textur sind gueltig und wirken als Weiss, belegen keinen Slot.

Pruefauftraege Codex: Mask-Material mit Graph-Textur sichtbar auf DX11/Vulkan/OpenGL, auch
der Schatten; ist die Alpha-Regel fuer Add sinnvoll?

Nachtrag (gleicher Tag): Knoten fuer alle Materialtypen.
- Neu `include/renderer/MaterialGraphBinding.hpp` (aus PbrGraph.hpp, das nicht mehr benutzt wird);
  Lit und Unlit haengen die Graph-Parameter hinter PerMaterial (Offset 64).
- `lit`, `quad_unlit`, `shadow` (HLSL/GLSL) werten den Graph aus. Legacy nutzt verbundene Werte
  und die Normal-Textur. `shadow.ps.hlsl` hatte ein zu Lit falsches PerMaterial-Layout (behoben);
  der Runtime-Lit-Pfad setzt jetzt auch den Schatten-PS.
- Custom: nur Texturknoten, die die Textur-Slots ersetzen (`MaterialAsset::WithGraphTextures`).

Nachtrag 2 (gleicher Tag): Mix, Kanal, Metallic/Roughness pro Pixel.
- Neue Knoten `Mix` (A, B, Faktor/Maske; `MaterialGraphNode::c`) und `Separate` (R/G/B/A);
  `.mat`-Format um ein optionales letztes Feld erweitert, alte Dateien laden weiter.
- Op-Codes 4/5; Mix-Faktor steckt im Nachkomma von `instruction.w`. `outputs.w` = Metallic | Roughness << 4.
  Ein Value-Knoten an Metallic/Roughness bleibt konstanter Parameter. Graph ersetzt ORM und Faktor.
- Texturen, die nur Metallic/Roughness speisen, laden linear (`textureLinear`).
- Math/Mix-Eingaenge bleiben verbunden bearbeitbar: Farbe am Knoten, Textur/Math als Staerke.

Pruefauftraege Codex: Mix-Faktor-Kodierung; sichtbar Roughness aus Textur auf allen Backends.

Nachtrag 3 (gleicher Tag): Undo und UV im Materialeditor.
- Undo: Zustand vor der Bearbeitung in `materialEditBaseline`; `RecordEditorValueChange` beim Ende der
  Geste (kein aktives Item, keine Maustaste, kein Drag). Ein Ziehen = ein Schritt. Inspector-Overrides
  laufen weiter ueber `ExecuteSceneMutation`.
- UV: Skala/Versatz am Texturknoten, `graphUV[2]` hinter `graphNodes` (je Textur-Slot); gleiche Datei
  mit anderen UVs belegt einen eigenen Slot. An Normal/ORM wirkungslos (Felder gesperrt), Custom ohne UV.

Nachtrag 4 (gleicher Tag): 16 Knoten, 4 Texturen, Math-Ops, Zeit.
- Achtung Binding-Modell: `TexSlots::COUNT` 16 -> 20, `SamplerBase` 32 -> 40 (Vulkan). Graph-Texturen: t13, t9
  (GL 14), t16, t17. DX11 setzt Null-SRVs jetzt fuer COUNT Slots.
- Neue Knotentypen Subtract, Power, Invert, Clamp, Remap, Sine (0..1), Time; Eingaenge 5 Bit je Index.
- `graphUV[8]` = je Textur scale/offset und scroll (nutzt `timeData.x`). Shader ohne PerFrame deklarieren
  den Anfang davon ueber `KROM_GRAPH_DECLARE_TIME`.

Pruefauftraege Codex: Vulkan-Layout nach Sampler-Verschiebung auf echter Hardware mit Validierung; GL-Units 16/17.

Nachtrag 5 (gleicher Tag): Script-Parameter pro Objekt.
- Knoten `Parameter` (Name in `texturePath`), Script: `ctx.SetMaterialParameter("Glow", 2.f)` oder Vec3.
- `MaterialSystem::SetParameterAlias` bildet den Namen auf `graphNodeN` ab. `gameplay::MaterialParameterInstances`
  legt beim ersten Setzen eine Kopie je Entity an, traegt sie in `MaterialComponent` ein und gibt sie bei
  Zerstoerung, Szenenwechsel und Shutdown frei. Neuer virtueller Aufruf in `IScriptContext`: Script-Module neu bauen.

Pruefauftraege Codex: Ueberschreibt der Editor waehrend Play die `MaterialComponent`-Handles? Freigabe bei Play-Stop.

### Neu (Claude, 2026-11-09): Inspector-Layout aufgeraeumt

Branch `feature/animation-skinning`, nicht committet, zur Pruefung bereit.

- `value_ui::InlineLabelScope`: solange aktiv, zeichnen die Wrapper Label links und Widget
  rechts in einer gemeinsamen Spalte (zu lange Labels mit Ellipse und Tooltip). Gilt nur im
  Scope-Fenster; Popups, Child-Fenster, Widgets hinter `SameLine` und der Materialeditor
  bleiben im ImGui-Standard. Die Widget-ID ist der versteckte Teil des Labels.
- `DragFloat3` im Scope: `DragAxes3` mit X/Y/Z-Plaketten, gruppiert wie `ImGui::DragFloat3`.
- Kopf: Icon (Typ/Komponenten als Tooltip), Name, Aktiv-Haekchen in einer Zeile; darunter
  Tag/Layer, dann Visible/Persistent.
- Komponenten-Kopfzeile: Farbstreifen entfernt, "..." oeffnet das bestehende Kontextmenue,
  Umsortieren per Drag auf der ganzen Kopfzeile.

Pruefung: `krom_tests` und `--play-ui` (DX11) gruen. Aussehen nicht visuell geprueft.

Pruefauftraege Codex: Undo und Wert-Kopieren/Einfuegen in X/Y/Z-Feldern; Rechtsklick aufs
Label; Klick aufs Transform-Label (Werkzeugwechsel); "..."-Menue und Drag auf der Kopfzeile
(Auf-/Zuklappen darf beim Drag nicht ausloesen).

### Neu (Claude, 2026-11-09): Materialien nur noch im Materialeditor bearbeiten

Branch `feature/animation-skinning`, nicht committet, zur Pruefung bereit.

- Inspector: keine Materialwerte mehr. Slots als Rasterzeilen (Name = Drop-Ziel, "Oeffnen",
  "Eigene Kopie"); bei mehreren Slots Zeile "Alle Slots". Bereich "Material" nur noch ohne
  Mesh-Slots. `DrawSelectedMaterialInspector`, `DrawMaterialProperties`, der Inline-Zweig von
  `DrawSelectedMaterialEditor` und `inspectorMaterial*` sind entfernt.
- Bestehende `materialOverrideSource` werden weiter geladen und gerendert; neu entstehen
  keine. Zeile "Lokale Werte" mit `ResetEntityMaterialOverride` / `CreateEntityMaterialCopy`.
  `SetEntityMaterialOverride` bleibt nur fuer Tests/Altdaten-Pfade bestehen.
- `AssignTexture`: ins Asset nur bei offenem Materialeditor; sonst werden Entities mit
  Material-Asset uebersprungen (Warnung im Log).

Pruefung: `krom_tests` (inkl. Reset-Check) und `--play-ui` (DX11) gruen.
`CreateEntityMaterialCopy` ohne Test (braucht Asset-Pfade), Aussehen nicht visuell geprueft.

Pruefauftraege Codex: "Eigene Kopie" an Slot und Entity; Undo von Reset/Zuweisung; Prefab-
Instanzen mit lokalen Werten; Textur-Doppelklick bei offenem/geschlossenem Materialeditor.

### Neu (Claude, 2026-11-09): Skinning im Export (Runtime-Materialpfad)

Branch `feature/animation-skinning`, nicht committet, zur Pruefung bereit.

Der Export nutzt die Materialerzeugung in `KromWrapper.cpp`, die vom Editor abwich:
- Unlit: immer statisches Layout + `quad_unlit` -> Figur starr. Jetzt `SkinnedUnlit` +
  `skinned_unlit.vs.hlsl` (ohne Custom-Vertex-Shader).
- Legacy Lit: `SkinnedLit` verlangt Tangenten, die Runtime erzeugte sie nicht -> Material
  fehlgeschlagen. `CreateRuntimeLitMaterialFromAssets` ruft jetzt `EnsureTangentsForPbr`.

Pruefung: Runtime-Release (Vulkan) gebaut, `krom_tests` gruen, Export startet und laeuft 12 s
ohne Eintrag in `krom.log` (vorher Tangent-Fehler). Bewegung selbst nicht visuell geprueft.

### Neu (Claude, 2026-11-09): Animationseditor Stufe 1

Branch `feature/animation-skinning`, nicht committet, zur Pruefung bereit.

- `EditorAnimationWindow.*` (Menue Fenster > Animation): Zeitleiste, Spuren je Objekt/Knochen,
  REC (Abweichung von der angezeigten Pose wird Keyframe), Keys ziehen/loeschen, Linear/Stufe,
  Undo ueber `RecordEditorValueChange`, Autosave als `.anim`.
- `animation/AnimationClipFile.hpp` (Format), `animation/AnimationClipEditing.hpp` (Keys).
- `.anim` wird ueber `ImportModelAnimationAssets` geladen (Editor-Play, Export, Prefab).
- Skin-Clips starten mit einem Key je Knochen (T/R/S), weil der Sampler Knochen ohne Channel
  als Identitaet behandelt.
- Vorschau pose't die Szene ohne Play; `SuspendAnimationPreview` in `SaveEditorScene` stellt
  vorher die Ausgangspose her, damit sie nicht gespeichert wird.
- `RebuildBindings` bevorzugt Ziele in der eigenen Hierarchie (Prefab-Instanzen, gleiche Namen).

Pruefung: `krom_tests` und neuer Lauf `--animation-ui` (DX11, starres Objekt) sowie `--play-ui`
gruen. Nicht automatisch geprueft: Knochen/Skin-Aufnahme, Maus-Bedienung der Zeitleiste, Export.

Pruefauftraege Codex: Scene-Undo waehrend aktiver Vorschau; Autosave-Backup waehrend Vorschau.

Nachtrag: Animator-Komponente im Inspector (`DrawAnimatorSection`, oeffnet den Editor). 3D-Vorschau
im Fenster: eigenes RT, `LAYER_EDITOR_ANIMATION_PREVIEW` wird dem Ziel nur in der Sitzung gesetzt
(`NormalizeSingleUserLayerMask` erhaelt Vorschau-Bits), eigene Licht-Rig, Knochen-Punkte und
Dreh-/Verschiebe-Gizmo per ImDrawList. `--animation-ui` gruen auf DX11 und Vulkan; Bildinhalt und
Gizmo-Bedienung nicht automatisch geprueft.

Nachtrag 2: Bearbeitung auf versteckter Kopie (`EnsureCopy`, `EditorRuntimeGizmoTag`, nur Vorschau-Layer);
die Szene bleibt unberuehrt, `SuspendAnimationPreview` entfaellt. `.anim` v2 = mehrere Clips, Referenz
`Datei.anim#Clip` (v1 weiter lesbar). Speichern explizit, weist dem Objekt zu, Rueckfrage bei
ungespeicherten Aenderungen. Gitter, Knochenbaum, Asset-Browser-Kachel, Start-Clip im Animator.
Vorschau haelt hinter dem Clip-Ende die letzte Pose (Sampler lief mit looping um).
Pruefauftraege Codex: Kopie bei Skin-Rigs, Prefab-Instanzen, Clip-Umbenennung gegen zugewiesene Pfade.

### Neu (Claude, 2026-11-12): Reihe „Bildqualitaet“ – Auftrag 1, Bestandsaufnahme

Neue Auftragsreihe (neun Auftraege) auf Branch `feature/bildqualitaet` (von `origin/main`
`287a721`). Gemeinsamer Stand: `doc/PLAN_BILDQUALITAET.md`. In Auftrag 1 kein Engine-Code
geaendert, nur die Plan-Datei angelegt.

- Neun Befunde zur Ausgangslage am Code geprueft, Ergebnis mit Fundstellen in der Plan-Datei.
  Sieben stimmen. Zwei liegen anders: `TexCoord1` existiert als Semantik, wird aber mit den
  Daten von `TexCoord0` gefuellt (kein zweiter UV-Kanal im Mesh); Forward+-Culling laeuft
  per Voreinstellung auf der CPU (Compute-Pfad vorhanden, von niemandem eingeschaltet), und
  OpenGL faellt auf klassisches Forward zurueck und hat kein GTAO.
- Ausgangszustand: Debug x64 `krom_editor_vulkan/dx11/opengl` und `krom_tests` gebaut,
  `krom_tests` 419 bestanden, 0 fehlgeschlagen (`out/claude-bildqualitaet-baseline-*.log`).
  Gebaut mit den fremden, nicht committeten Aenderungen im Arbeitsordner (Animationseditor/
  Gizmo); diese sind nicht Teil des Branches.
- Nicht geprueft: Laufzeit, Bild, Release, GLFW.

Pruefauftraege Codex: Befunde 3 (UV-Satz) und 7 (Culling-Weg, OpenGL-Rueckfall) in
`doc/PLAN_BILDQUALITAET.md` gegenlesen; ob es ausser `KromWrapper.cpp` und
`KromEditorApp.cpp` weitere Stellen gibt, die das Tonemap-Material bauen.

## Befunde (Codex)

### Neu (Claude, 2026-11-10): Animationsfenster-Layout nach Materialeditor-Regeln

- Datei-Dropdown entfernt. Oben wie im Materialeditor: Button „Speichern“ (gleicher
  Stil, ohne Icon), daneben „Ungespeicherte Änderungen“, Trennlinie, Zeile „Datei: …“.
- Eine linke Spalte (`AnimationColumnWidth`): oben Keyframe-Panel, unten Transport +
  Spurnamen; Vorschau und Keys beginnen auf derselben x-Kante. Panel dafür nach links.
- Unter den Play-Buttons: Zeile „Clip“ (Combo + „...“), Zeile „Länge“ + „Schleife“.
  Label-Spalte über `InlineLabelScope(float column)` (neu, EditorValueClipboard.hpp)
  in Panel und Kopf identisch; Zeitfeld/Combo/Loop enden auf derselben rechten Kante.
- Asset-Browser-Doppelklick auf .anim: Ziel = Nutzer der Datei, sonst Animator der
  Auswahl, sonst Animator mit passendem Kanal-Zielnamen; Fenster wird fokussiert.

Prüfung: Build x64-Debug, `krom_editor_loading_tests_dx11 --animation-ui` 23 PASS.
Nicht visuell geprüft.

### Neu (Claude, 2026-11-10, Nachtrag): Inspector-Karten in Animations- und Materialeditor

- Neu `addons/editor/EditorCard.hpp`: `card::Begin/End` (Kopf mit Dreieck, Icon, Titel,
  Karte mit Hintergrund/Rahmen wie im Inspector) und `card::SaveButton` (Diskette + Text).
- Animationsfenster: Datei-Zeile und linkes Panel entfernt; Vorschau und Zeitleiste ueber
  die volle Breite. Die Werte des gewaehlten Keyframes zeigt der Inspector als Karte
  „Keyframe“ ueber dem Objekt (`DrawAnimationKeyCard`).
- Materialeditor: Speichern mit Diskette; Name/Datei als Label-links-Zeilen; UV,
  Render-Optionen, Shader-Parameter im Material-Knoten als Karten (standardmaessig zu).
- Neuer Locale-Schluessel `common.file`.

Pruefung: Build x64-Debug; `krom_editor_loading_tests_dx11 --animation-ui` 23 PASS,
Standardlauf 18 PASS. Nicht visuell geprueft (Kartenbreite im Material-Knoten pruefen).

### Neu (Claude, 2026-11-10, Nachtrag 2): Schalter „Automatisch speichern“, Einstellungen als Karten

- `EditorPreferences::autoSaveEditors` (Standard an, persoenliche Einstellungen), UI in
  Einstellungen > Arbeitsablauf > Karte „Speichern“ neben dem Szenen-Backup.
  `SetAutoSaveEditorsForSession` nur fuer Tests (schreibt keine Datei).
- Animationseditor: an = nach jeder abgeschlossenen Aenderung `SaveAnimation` (TickSession),
  `RunAfterUnsavedCheck(ctx, …)` speichert dann statt zu fragen.
- Materialeditor: aus = Aenderungen/Undo/Paste/Textur-Drop bleiben im Speicher (dirty),
  Vorschau auf Szenenobjekten laeuft weiter; Materialwechsel und Fenster-Schliessen fragen
  Speichern/Verwerfen/Abbrechen; Verwerfen liest die .mat von der Platte und zeigt sie an.
  Bei geschlossenem Fenster wird wie bisher gespeichert.
- Materialeditor: Buttons „Auswahl zuweisen“ und „Auf gebundene Meshes anwenden“ entfernt.
- Einstellungsfenster: alle Seiten als `card::`-Karten mit `value_ui`-Zeilen in einem Raster.

Nachtrag: `SaveEditorScene` (Szenenwechsel, Strg+S, Play, Export) und `Shutdown` speichern
offene Material-/Animationsaenderungen still mit (`Save…IfChanged`), wie die Szene selbst.
Nicht visuell geprueft.
Pruefung: `--animation-ui` 24 PASS (neu: Auto-Save-Fall), `--settings-ui` 14 PASS, Standard 18 PASS.

### Neu (Claude, 2026-11-10): Animations-Workflow Schritt 1 – Clips aus Blender

Branch `feature/animation-skinning`, nicht committet, zur Pruefung bereit. Format mit dem Nutzer
abgestimmt: Quelle (.glb/.gltf) bleibt fuehrend, Zusatzdaten in einer Sidecar-Datei daneben.

- Neu `include/animation/ModelClipSettings.hpp`: `<model>.clips` (JSON, `"kromclips": 1`, `fps`,
  Eintraege `id`/`name`/`source`/optional `start`+`end` in Frames/`loop`/`rootMotion`).
  Referenz `"<model>#clip/<id>"` (stabile 8-Hex-ID, Umbenennen bricht nichts). Dazu
  `SyncModelClipSettings` (ein ganzer Clip je Blender-Animation), `SliceAnimationClip`
  (Raender gesampelt, CubicSpline wird linear), `BuildModelClip`, `FindSkeletonMismatch`.
- `AnimationClip::rootMotion` (nur Flag, Laufzeit folgt in Schritt 6).
- `MeshRendererSerialization`: `ImportModelAnimationAssets` liest die Sidecar-Datei und registriert
  `#clip/<id>`; `#animation/<n>` bleibt unveraendert (alte Szenen/Prefabs laden ohne Migration).
  Neu `ReadModelClipSettings`, `RegisterModelClips` (ersetzt in place, Handles bleiben),
  Parameter `replaceExisting` (Neuimport nach Blender-Export). Fehlende Quelle/fehlender Clip: Warnung
  (je Fall einmal).
- Neu `addons/editor/EditorModelClips.*`: Animationsnamen direkt aus dem glTF-JSON (bei .glb nur der
  JSON-Chunk), Cache nach Zeitstempel. Die Sidecar-Datei entsteht erst, wenn eine Clip-ID benutzt wird
  (`PersistModelClips`: Zuweisen, Ziehen, Oeffnen), nicht beim blossen Anzeigen.
- Asset-Browser: Modell-Kachel mit Animationen zeigt ein Badge (Anzahl), Klick/Kontextmenue klappt
  Clip-Kacheln auf; ziehbar in den Animator, Doppelklick oeffnet den Clip im Animationsfenster.
- Animator (Inspector): Start-Clip-Auswahl auch fuer Modelle (Blender-Namen), Drop von Modell-Clips,
  Loop kommt beim Zuweisen aus dem Clip. Rot „Clip fehlt“, gelb „passt nicht zum Skelett“.
- Animationsfenster, Modell-Modus: Clips des Modells schreibgeschuetzt (kein REC/K/Key-Edit), bearbeitet
  werden Name, Bereich (Frames), Loop, Root Motion, FPS; „Clip aus Bereich schneiden“, Loeschen nur
  fuer geschnittene Clips oder solche mit fehlender Quelle. Undo je abgeschlossener Aenderung.
  Speichern schreibt `.clips` und stellt den Animator von `#animation/<n>` auf `#clip/<id>` um.
  Objekte ohne Clip, deren Rig aus einem Modell mit Animationen stammt, oeffnen den Modell-Modus statt
  einer .anim-Kopie; „Neue eigene Animation (.anim)“ steht im Clip-Menue.

Pruefung: Build x64-Debug (`krom_tests`, `krom_editor_dx11`, `krom_editor_vulkan`,
`krom_editor_loading_tests_dx11`). `krom_tests` alle gruen (neu: „Model clip settings round trip“,
„Slice animation clip“, „Clip skeleton mismatch“, „Model clip settings import“). UI: Standard 18 PASS,
`--animation-ui` 34 PASS (neu 10: Namen aus Datei, Sidecar, Vorschau, Speichern migriert Referenz,
Schnitt ab Haelfte, Oeffnen eines Schnitts), `--play-ui` 33 PASS.
Nicht geprueft: Bedienung mit der Maus (Badge, Bereichsfelder), echte Blender-Datei mit mehreren
Actions, DemoProject visuell (Referenzen dort sind `#animation/0`, Pfad unveraendert).

Pruefauftraege Codex: Sidecar mit Hand-Fehlern (kaputtes JSON wird nie ueberschrieben?); Neuimport nach
geaenderter .glb mit weniger Animationen (alte `#animation/<n>` bleiben registriert); Undo der
Modell-Einstellungen nach Fenster-Schliessen.

### Neu (Claude, 2026-11-10): Zeitleiste als eigenes Fenster, Speichern-Rueckfrage beim Szenenwechsel

Branch `feature/animation-skinning`, nicht committet. Wunsch des Nutzers nach Schritt 1.

Zeitleiste:
- `EditorAnimationWindow.cpp`: Transport, Clip-Zeilen und Spuren stehen im neuen Fenster
  „Zeitleiste“ (`Timeline##editor`, Locale `animation.timeline`). Beim ersten Oeffnen dockt es in
  den Bereich von Assets/Console und kommt nach vorn; im Standard-Layout (`EditorUI.cpp`) ebenfalls
  unten. Es oeffnet und schliesst mit dem Animationsfenster (kein eigenes X). Das Animationsfenster
  zeigt nur noch Speichern-Zeile und 3D-Vorschau ueber die volle Hoehe. Entf/K wirken in beiden.
- Behoben: Kopfbereich der Zeitleiste hatte feste 3 Zeilen; im Modell-Modus sind es 6.

Szenenwechsel:
- `KromEditorApp`: Inhaltsstand nach Laden/Speichern (`RememberSavedSceneContent`), Vergleich ohne
  Laufzeit-IDs (Entities nach GUID sortiert, Eltern per GUID, Hierarchie-Reihenfolge als Rang unter
  Geschwistern) plus Umgebung, ohne Editor-Kamera. `isEditorSceneModified` im Kontext.
- `RunAfterSceneSaveCheck`/`DrawSceneSavePrompt` (EditorUI.cpp): Dialog „Speichern / Nicht speichern /
  Abbrechen“ vor Szenenwechsel (Asset-Browser Doppelklick/Laden, Menue Szene wechseln) und vor
  „Neue Szene“. Additives Laden und „Neue Szene in Ordner“ (wechselt nicht) fragen nicht.
- „Nicht speichern“ setzt `discardSceneChangesOnce`; `SaveSceneBeforeLeaving` verwirft dann nur die
  Szene, offene Material-/Animationsaenderungen werden wie bisher gespeichert. Abgelehnter Wechsel
  setzt das Flag zurueck. Ohne Aenderungen kein Dialog; Prefab-Modus und Play gelten als unveraendert.

Pruefung: Build x64-Debug (krom_tests, Editor DX11/Vulkan, Loading-Tests). `krom_tests` gruen; UI
Standard 18, `--animation-ui` 39 (neu: Aenderung erkannt, Speichern setzt zurueck, neues Objekt,
Undo zurueck auf Speicherstand = keine Aenderung), `--play-ui` 33, `--settings-ui` 15 PASS.
Nicht geprueft: Dialog per Maus, Docking-Verhalten mit der persoenlichen imgui.ini des Nutzers.

Pruefauftraege Codex: Faelle mit falscher „geaendert“-Meldung (z. B. Normalisierungen beim Laden);
Szenenwechsel aus dem Projektfenster/anderen Wegen, die `switchEditorScene` direkt rufen.

### Neu (Claude, 2026-11-11): Animations-Workflow Schritt 2 – Controller-Laufzeit (.animctrl)

Branch `feature/animation-skinning`, nicht committet. Format und Komponentenfelder vom Nutzer
freigegeben. Entwurf aus `claude/animation-controller-761052` uebernommen und umgebaut (IDs statt
Namen/Indizes, Prioritaet, interruptible, Int-Parameter, Operatoren, Parameter per Index).

- Neu `addons/animation/AnimationController.*`:
  - Asset (`kromanimctrl 1`): Parameter/States/Transitionen mit stabilen IDs, Clips als Asset-Pfad,
    `kind: "clip"` (Variante fuer BlendSpace spaeter), `editor`-Positionen.
  - `CompileAnimationController`: Indizes, Transitionen je State und Any State nach Prioritaet
    (Gleichstand: Dateireihenfolge), Bedingungen per Parameterindex, `problems` (Clip fehlt, geloeschter
    Parameter -> Transition nie gueltig, Entry fehlt -> erster State, State ohne Ausgang).
  - `StepAnimationController`: Zeit, Cross-Fade, Exit Time (normiert >= Wert, auch ueber Loops und
    grosse Schritte), Any State nie in den eigenen State, Trigger beim Feuern verbraucht, nicht
    unterbrechbare Fades blockieren, Unterbrechung friert die gemischte Pose ein. `CrossFadeToState`
    fuer Scripts (Schritt 3).
  - `AnimationControllerLibrary`: shared_ptr je Pfad, Dateipruefung hoechstens alle 500 ms.
- `AnimationComponents.hpp`: `controllerAssetPath`, `AnimationControllerRuntime` (Instanz, Posen-
  Puffer), `SetParameter/GetParameter` per Index.
- `AnimationSystem`: Controller-Pfad; Puffer werden beim Binden einmal angelegt, im Frame keine
  Allokation (`SkeletonPoseEvaluator::EvaluateInto`, `SampleClipInto`). Skin am Objekt oder darunter;
  ohne Skelett je animiertem Knoten ein Slot (Einzelobjekt = Skelett mit einem Bone). Player ohne
  Controller unter einem mit Controller werden uebersprungen. `SetAssetSources` (Asset-Wurzel +
  Clip-Import) in Editor-Play (`TickEditorPlay`) und `KromWrapper::EnsureAnimationSystems`.
- Szene: `controllerAssetPath`, Prefab: `animationControllerPath` (inkl. Instanziierung, Record aus
  Entity, Prefab-Abgleich). Nur geschrieben, wenn gesetzt; alte Dateien unveraendert.
- Editor: `EditorAnimationControllers.*` (Anlegen, gecachte kompilierte Ansicht). Asset-Browser:
  `.animctrl`-Kacheln, Erstellen-Menue „Animation Controller“, Drag `KROM_ASSET_ANIMCTRL`.
  Animator: Zeile „Controller“ (Drop, X), Fehler/Probleme farbig, States/Parameter nur lesend.
- Bekannte Grenze (mit Nutzer abgestimmt): States/Transitionen anlegen erst mit dem Graph-Editor
  (Schritt 4); bis dahin entstehen Controller nur in Tests.

Pruefung: Build x64-Debug (krom_tests, Editor DX11/Vulkan, Loading-Tests). `krom_tests` gruen, neu:
„Controller file round trip“, „Controller compile problems“, „Controller transitions, triggers, blend“,
„Controller priority and interrupt“, „Controller drives objects“ (Cross-Fade 0 -> 5 -> 10, keine
Neuallokation des Ausgabepuffers, Einzel-Clip unveraendert). UI: Standard 18, `--animation-ui` 39,
`--play-ui` 33, `--settings-ui` 15 PASS.
Nicht geprueft: Controller an einem Skin-Rig im laufenden Spiel, Editor-Bedienung per Maus.

Pruefauftraege Codex: Exit-Time-Semantik bei Loops (>= statt Kreuzen); Verhalten bei Controller-Wechsel
waehrend Play; Prefab-Overrides des Controller-Pfads.

### Neu (Claude, 2026-11-11): Animations-Workflow Schritt 3 – Script-API fuer Controller

Branch `feature/animation-skinning`, nicht committet. Keine Formataenderung.

- Neu `addons/script/IAnimationService.hpp` (Muster wie Physik/Audio/Partikel/Material):
  FindParameter, GetParameterKind, Set/GetParameter per Index, GetCurrentState, GetNormalizedTime,
  IsInTransition, CrossFade.
- Neu `addons/animation/AnimationScriptService.*` (engine_animation): bindet den Controller bei Bedarf
  (`AnimationSystem::EnsureControllerBound`, aus UpdateController herausgeloest) — Scripts koennen schon
  in OnStart Parameter setzen. Fehler einmal im Log: kein Animator mit Controller, unbekannter
  Parameter/State.
- `IScriptContext`: FindAnimationParameter, SetAnimationFloat/Int/Bool/Trigger, ResetAnimationTrigger,
  SetAnimationParameter(index), GetAnimationFloat/Int/Bool, GetAnimationState, IsAnimationState,
  GetAnimationNormalizedTime, IsAnimationInTransition, CrossFadeAnimation. Typpruefung (Float<->Int
  erlaubt, Bool/Trigger nur passend; sonst false + Warnung). `virtual GetAnimation()` steht am Ende der
  Klasse (vtable nur hinten erweitert).
- `ScriptServices::animation`, `ScriptFrameContext::GetAnimation`; `GameSimulation` merkt sich
  `steps.animation` in RunFrame und stellt den Dienst in UpdateScripts bereit.
- Behoben: Ein Controller ohne Pose-Slots (Clips ohne Kanaele) lief nicht weiter; die Zustandsmaschine
  laeuft jetzt immer, nur das Posieren entfaellt.

Pruefung: Build x64-Debug (krom_tests, Editor DX11/Vulkan, Loading-Tests). `krom_tests` gruen, neu
„Controller script API“ (vor erstem Update nutzbar, Typpruefung, Trigger setzen/zuruecksetzen, State,
Transition, normierte Zeit, CrossFade, Entity ohne Animator, Kontext ohne Dienst). UI: Standard 18,
`--animation-ui` 39, `--play-ui` 33, `--settings-ui` 15 PASS.
Nicht geprueft: echtes Projekt-Script (DLL) gegen die neue API gebaut.

Pruefauftraege Codex: ABI alter Script-DLLs (nur angehaengte virtuelle Funktion); Warnungen bei
falschem Typ jeden Frame (aktuell ohne Drossel).

### Neu (Claude, 2026-11-11): Animations-Workflow Schritt 4a – Controller-Graph-Editor

Branch `feature/animation-skinning`, nicht committet. Bedienablauf vom Nutzer freigegeben, Format unveraendert.

- Neu `addons/editor/EditorControllerGraph.*`: andockbares Fenster „Controller“ (Standard: Tab neben dem
  Viewport; Menue Fenster > Controller). Kopfzeile wie Materialeditor (Speichern, Status, Pfad).
  - Links Parameterliste: „+“ mit Typwahl, Name direkt (Uebernahme beim Verlassen), Standardwert,
    Rechtsklick Loeschen (Bedingungen bleiben und werden rot).
  - Rechts Flaeche: Raster, Pan mittlere Maustaste, Zoom Mausrad, Knoten rasten ein (Alt = frei).
    Entry gruen mit Pfeil auf Start-State, Any State tuerkis, Transitionen als Pfeile mit Kurzform der
    Bedingung und „P<n>“, mehrere zwischen zwei States versetzt, Selbst-Transition als Schleife.
  - Anlegen: Rechtsklick Flaeche „Neuer State“, Clip aus dem Asset-Browser auf die Flaeche ziehen,
    Rechtsklick State: Transition erstellen (dann Ziel klicken), Als Start-State, Duplizieren, Loeschen; Entf.
  - Fehler: roter Rahmen bei fehlendem Clip, gestrichelter roter Pfeil bei geloeschtem Parameter,
    gelbes „!“ bei State ohne Ausgang (mit Tooltip).
- Inspector-Karten (`DrawControllerGraphCard`, ueber der Objekt-Ansicht wie die Keyframe-Karte):
  State (Name, Clip per Drop/Auswahl aus derselben Datei, Tempo, Start-State, ausgehende Transitionen in
  Pruefreihenfolge, anklickbar) und Transition (Von/Nach, Prioritaet, Ueberblenden, Exit Time, darf
  unterbrechen, Bedingungen Parameter–Operator (nach Typ gefiltert)–Wert, „+ Bedingung“).
- Undo: eine Stufe je abgeschlossener Aenderung (`history.edit_controller`), Ziehen als Geste.
  Auto-Speichern wie die anderen Editoren; aus: Rueckfrage beim Schliessen. Szene speichern, „Nicht
  speichern“ beim Szenenwechsel und Beenden speichern offene Controller-Aenderungen mit.
- Einstiege: „Oeffnen“ am Animator (Controller-Zeile, auch Klick auf den Namen), Doppelklick/Kontextmenue
  der `.animctrl`-Kachel. `EditorAnimationControllers`: `MakeEditorClipResolver`, `EditorClipLabel`.

Pruefung: Build x64-Debug (krom_tests, Editor DX11/Vulkan, Loading-Tests). `krom_tests` gruen; UI Standard 18,
`--animation-ui` 48 (neu 9: Anlegen/Oeffnen, States mit Clip-Namen, Start-State, Undo/Redo als ein Schritt,
Speichern, geladene Datei ohne Probleme, Parameter loeschen; Karten werden gezeichnet), `--play-ui` 33,
`--settings-ui` 15 PASS.
Nicht geprueft: Maus-Bedienung (Ziehen, Kontextmenues, Transition per Klick), Darstellung bei vielen Pfeilen.
Offen (4b): Live-Vorschau im Animationsfenster, Live-Anzeige und Regler im Play.

Pruefauftraege Codex: Undo nach Wechsel auf einen anderen Controller; Duplizieren/Loeschen bei Entry;
Bedienung bei Zoom-Grenzen.

### Neu (Claude, 2026-11-11, Nachtrag): Entf nur im fokussierten Fenster, Graph-Bedienung

Nutzerbefund: Entf im Controller-/Animationsfenster loeschte (nach Rueckfrage) das in der Hierarchie
ausgewaehlte Objekt; Transitionen liessen sich per Klick/Entf nicht entfernen.

- `EditorUIProject.cpp`: globale Entf-Abfrage „Objekt loeschen“ nur noch, wenn Hierarchie oder Viewport
  den Fokus haben (`SceneSelectionHasFocus`, NavWindow->RootWindow). Graph, Animationsfenster, Zeitleiste,
  Inspector loesen sie nicht mehr aus.
- Graph: Entf wirkt bei Fokus auf das Controller-Fenster (statt nur bei Maus ueber der Flaeche). Pfeile
  werden erst vermessen, dann gezeichnet; der Pfeil unter der Maus wird hervorgehoben, Trefferbreite 7 px.
  Der Entry-Pfeil (keine Transition) erklaert sich per Tooltip. Farben auf Theme-Linearfarben umgestellt
  (Raster dunkler, Entry/Any dunkel mit farbigem Rand).
- `ControllerGraphScreenPosition` / `ControllerGraphSelection` fuer Tests (inkl. "#canvasMin/Max").

Pruefung: `--animation-ui` 53 PASS (neu: Pfeil auf der Flaeche, Rechtsklick waehlt + Menue, Linksklick
waehlt, Entf loescht die Transition, ausgewaehltes Szenenobjekt bleibt, keine Loeschabfrage) – mit echten
ImGui-Maus-/Tastenereignissen. `krom_tests` gruen; „Delete shortcut ...“ prueft jetzt beide Faelle
(anderes Fenster fokussiert: keine Abfrage; Hierarchie fokussiert: Abfrage + Loeschen). Standard 18,
`--play-ui` 33, `--settings-ui` 15.

### Neu (Claude, 2026-11-11): Animations-Workflow Schritt 4b – Live-Vorschau und Live-Anzeige fuer Controller

Branch `feature/animation-skinning`, nicht committet. Ablauf vom Nutzer freigegeben, Format unveraendert.
Rueckfragen beantwortet: Zeitleiste zeigt im Controller-Modus Statuszeile + Transport; Start ueber Knopf
„Vorschau“ im Graph (oeffnet das Animationsfenster).

- Laufzeit: `AnimationSystem::UpdatePlayer(world, entity, dt)` wertet einen Animator mit derselben
  `UpdateController`-Logik aus, auch wenn er nicht spielt (kein zweiter Code-Pfad).
  `AnimationControllerInstance::blendTransition` merkt sich die Transition des laufenden Fades
  (-1 bei `CrossFadeToState`/Reset). `engine_editor` linkt jetzt `engine_animation` ausdruecklich.
- Animationsfenster (`EditorAnimationWindow.cpp`), Controller-Modus: Ist ausserhalb von Play ein Objekt
  gewaehlt, dessen Animator (selbst oder oberster Rig-Animator) den im Graph offenen Controller nutzt,
  startet eine Session auf der versteckten Kopie (`EnsureCopy`). Die Kopie bekommt einen eigenen
  `AnimationPlayerComponent` (playing = false, damit ein laufendes Spiel ihn nicht anfasst) und wird
  von einer editor-eigenen `AnimationSystem`-Instanz mit echtem Frame-Delta gestept. Die Arbeitskopie des
  Graphen wird per `Library::Set` mit `MakeEditorClipResolver` uebersetzt; neu gebunden wird nur bei
  Logik-Aenderungen (Knoten verschieben laeuft weiter), vorher wird die Kopie auf die Ruhepose gesetzt.
  Bei Rigs folgen die Knochen-Objekte der Pose (Gizmos). Kein Speichern, kein REC/K, keine Auto-Speicherung.
  Ende: Graph zu/anderer Controller, Auswahl weg, Play-Start, Fenster zu. Unsaved-Rueckfrage wie bisher,
  „Abbrechen“ fragt nicht jeden Frame neu. `FollowSelection` merkt sich die Auswahl jetzt im WindowState.
- Zeitleiste im Controller-Modus: Pause/Weiter, Neustart (Parameter auf Standard, Start-State), aktiver
  State mit Clip und normierter Zeit als Balken, waehrend eines Fades Fortschritt und auslaufender State.
- Graph (`EditorControllerGraph.cpp`): Knopf „Vorschau“ in der Kopfzeile (gesperrt ohne passendes Objekt,
  markiert solange sie laeuft; im Play Hinweis „Live im Spiel“). Parameterliste: unter jedem Standardwert
  eine Live-Zeile (Float/Int ziehen, Bool Schalter, Trigger „Ausloesen“, gesetzt = hervorgehoben). Sie
  setzt nur die Vorschau-Instanz bzw. im Play die echten Werte des ausgewaehlten Objekts. Flaeche: aktiver
  State mit Akzentrahmen und Zeitbalken, auslaufender State mit grauem Balken, laufende Transition fuellt
  ihren Pfeil (Schleife als Bogen). Zuordnung Laufzeit -> Graph ueber State-Namen/Transition-Ziel, damit
  ein im Play aelterer gespeicherter Stand nichts Falsches markiert. Farben ueber `theme::`.
- API: `ControllerLivePlayer`, `ControllerPreviewCandidate`, `ControllerPreviewRunning`,
  `RestartControllerPreview`, `Set/ControllerPreviewPaused` (Animationsfenster), `ControllerGraphVersion`,
  `ControllerGraphEdit` (eine Undo-Stufe, fuer Tests). Texte de/en.

Pruefung: Build x64-Debug (krom_tests, Editor DX11/Vulkan, Loading-Tests). `krom_tests` gruen, neu: blendTransition
(Fade/CrossFade), `UpdatePlayer` an pausiertem Animator. UI: Standard 18, `--animation-ui` 80 (neu 27: Vorschau startet
auf der Kopie im Start-State, Zeitbalken nur am aktiven State, Kopie bewegt sich mit echtem Delta, Szenenobjekt bleibt,
Trigger feuert und wird verbraucht, Pfeil-Fuellstand, beide Balken im Fade, Standardwert bleibt, Verschieben startet
nicht neu, Logik-Aenderung bindet ohne Speichern neu, Pause haelt die Pose, Neustart, Abwaehlen beendet, Play zeigt
den echten Animator, Regler setzt den echten Parameter, Transition im Spiel mit Fuellstand), `--play-ui` 33,
`--settings-ui` 15 PASS.
Nicht geprueft: Maus-Bedienung der Live-Regler und des Vorschau-Knopfs (Test setzt die Werte per API), Skin-Rig in der
Vorschau (nur Knoten-Clip getestet), Vulkan-Editor nur gebaut. Im Play wirken ungespeicherte Graph-Aenderungen nicht
(Spiel liest die Datei).

Pruefauftraege Codex: Vorschau an Skin-Rig mit Animator am Root und Mesh-Kind; Auswahl eines Knochens/Mesh-Kinds
(AnimatorTarget-Pfad); Undo/Redo waehrend laufender Vorschau; Wechsel Clip-Bearbeitung <-> Controller-Vorschau mit
ungespeicherten Clip-Aenderungen.

### Neu (Claude, 2026-11-11, Nachtrag): 4b nach Nutzertest

Nutzerbefunde und Korrekturen:
- Gizmo des Viewports erschien gross in der Animationsvorschau: Die Vorschau rendert `LAYER_EDITOR_GIZMO` nur noch,
  wenn sie die Gizmos besitzt (`BuildAnimationPreviewRenderView`); im Controller-Modus besitzt sie nie welche.
  Gilt auch fuer die Clip-Bearbeitung (Kopie steht am Ort des Originals).
- Vorschau kam nach einmaligem Ende nicht wieder („Select an object with an Animator“): die Sperre „schon gefragt“
  blieb stehen. Sie gilt jetzt nur noch nach „Abbrechen“ in der Unsaved-Rueckfrage. Ausserdem laeuft die Vorschau
  weiter, wenn die Auswahl verloren geht (z. B. Clip im Asset-Browser angeklickt); sie endet erst bei Auswahl eines
  anderen Animators, Graph zu/anderer Controller, Play oder Fenster zu.
- Zeitleiste zeigt im Controller-Modus nichts mehr (Statuszeile/Balken entfernt). Pause/Weiter und Neustart stehen
  neben dem Knopf „Vorschau“ im Graph-Kopf.
- Parameter: nur noch ein Wertfeld je Zeile. Laeuft Vorschau oder Play, zeigt und setzt es den Live-Wert (Akzentfarbe),
  sonst den Standardwert der Datei. Ueberschrift „Vorschau-Werte“/„Live im Spiel“ mit Tooltip.

Pruefung: Build x64-Debug, `krom_tests` gruen, UI Standard 18, `--animation-ui` 84 (neu: keine Gizmos im Controller-
Modus, Auswahlverlust haelt die Vorschau, anderer Animator beendet sie, erneute Auswahl startet sie wieder, Fenster zu
beendet sie), `--play-ui` 33, `--settings-ui` 15 PASS. Nicht geprueft: Darstellung im echten Editor per Maus.
Nachtrag: Der Knopf „Vorschau“ schaltet bei laufender Vorschau selbst Pause/Weiter (kein zweiter Play-Knopf mehr),
daneben nur Neustart.
Nachtrag 2: Inspector „Animation“ (und Oeffnen aus dem Asset-Browser/Neue Animation) oeffnet die Clips auch bei
Animator mit Controller; die Controller-Vorschau uebernimmt das Fenster erst wieder bei anderer Auswahl oder ueber
den Knopf „Vorschau“ (`StartControllerPreview`). Test-Helfer `FindPlayUiEntity` ueberspringt die versteckte Kopie.
`--animation-ui` 86 PASS, uebrige Suiten unveraendert gruen.
Nachtrag 3: „Vorschau“ im Graph oeffnet das Animationsfenster ohne Fokuswechsel (NoFocusOnAppearing fuer Animation
und Zeitleiste, Fokus zurueck auf „Controller“). `--animation-ui` 87 PASS.

### Neu (Claude, 2026-11-11): Animations-Workflow Schritt 5 – Clip-Events

Branch `feature/animation-skinning`, nicht committet. Entwurf vom Nutzer freigegeben (Events am Clip, Formate erweitert,
rueckwaertskompatibel).

- Daten: `assets::AnimationEvent {time, name, value, text}`, `AnimationClip::events` (nach Zeit sortiert).
- Formate: `.anim` Zeile `event <zeit> <wert> <name> [text]` (Name ein Wort, Leerzeichen -> `_`, `AnimationEventName`);
  beim Laden sortiert. `<model>.clips`: optional `"events": [{frame, name, value, text}]` je Clip-Eintrag, Frames der
  Quellanimation wie start/end. `BuildModelClip` rechnet in Sekunden ab Clip-Start um und verwirft Events ausserhalb
  des Bereichs. Alte Dateien laden unveraendert; aeltere Engine-Staende lehnen `.anim` mit `event`-Zeilen ab.
- Laufzeit (`AnimationSystem`): `ForEachPassedEvent` feuert beim Ueberschreiten (Schleifen je Durchlauf, grosser Schritt
  ueber >= 8 Durchlaeufe je Event einmal, rueckwaerts nichts). Controller: nur der State, der laeuft bzw. in den geblendet
  wird; der auslaufende State bleibt stumm; der Rest bis zu einer Transition feuert noch. Einzel-Clip-Modus ebenso.
  `FiredEvents()` (Puffer wiederverwendet), auch fuer `UpdatePlayer`.
- Zustellung: `GameSimulation::RunFrame` legt die Events nach dem Animationsschritt als `ScriptEvent` (Name, Wert, Text,
  sender = target = Animator) in `m_pendingEvents`; `OnEvent` der Scripts am Animator-Objekt bekommt sie beim naechsten
  Script-Update nach OnUpdate (ein Frame spaeter, wie Kollisions-Events). Script-API unveraendert.
  Abweichung vom Entwurf („selber Frame“): Script-Dienste entstehen nur in UpdateScripts; dem Nutzer gemeldet.
- Editor (`EditorAnimationWindow.cpp`): Event-Band oben im Zeitlineal (Marker als Faehnchen mit Name, Klick waehlt,
  Ziehen verschiebt mit Raster, Rechtsklick „Event hinzufuegen“/„Loeschen“, Entf). Inspector-Karte „Event“ (Name, Zeit bzw.
  Frame bei Modell-Clips, Wert, Text, Loeschen). Undo ueber die vorhandenen Gesten (Clip bzw. Clip-Einstellungen).
  Modell-Clips: Events in den Einstellungen, Anzeige gespiegelt; Sortierung nach Ende der Bearbeitung (`SortClipEvents`).
- Controller-Vorschau: zuletzt gefeuertes Event steht 1,2 s ueber dem aktiven State im Graph (verblassend).
- Texte de/en.

Pruefung: Build x64-Debug. `krom_tests` gruen, neu „Clip event files“, „Clip event firing“, „Game frame: animation
events“. UI: Standard 18, `--animation-ui` 95 (neu: Event anlegen/auswaehlen, Speichern in .anim, Marker im Band,
Event am Modell-Clip in Quell-Frames, Ausschnitt-Clip behaelt Events relativ zum Start, Vorschau feuert Events, Anzeige im
Graph), `--play-ui` 33, `--settings-ui` 15 PASS.
Nicht geprueft: Maus-Bedienung des Event-Bands (Ziehen, Rechtsklickmenue) und der Karte; Events im laufenden Spiel im
Editor per Script (nur `krom_tests` ueber RunFrame); Export-Runtime.

Pruefauftraege Codex: Zeitpunkt der Zustellung (ein Frame spaeter) gegen Erwartung; Events bei Exit-Time-Transition im
selben Schritt; Event genau am Clip-Ende bei Schleifen (0 und Ende doppelt?); Undo/Redo von Events in Modell-Clips.

### Neu (Claude, 2026-11-11): Gemeinsame Graph-Arbeitsflaeche, Materialgraph mit Raster und Zoom

Nutzerwunsch: Materialeditor mit demselben Raster und Zoom wie der Controller-Graph.

- Neu `addons/editor/EditorGraphCanvas.hpp` (`graph_canvas::Update`, `View`, `Snap`, `kGrid`):
  Hintergrund, Raster (jede 5. Linie kraeftiger, Theme-Linearfarben), Pan mit mittlerer Maustaste,
  Zoom mit Mausrad um die Mausposition, Einrasten (Alt = frei). Pan/Zoom gehoeren dem Aufrufer.
- Controller-Graph nutzt den Baustein (eigener Raster-/Pan-/Zoom-Code entfernt).
- Materialgraph (`EditorMaterialGraph.cpp`): Knoten in Weltkoordinaten (`View::ToScreen/ToWorld`),
  Ansicht im State-Storage des Canvas-Fensters (`##canvas_pan_x/y`, `##canvas_zoom`, 0.5–2.0).
  Zoom skaliert Schrift (`PushFont(nullptr, FontSizeBase*zoom)`), FramePadding/ItemSpacing/
  ItemInnerSpacing/FrameRounding und alle Pixelmasse (`kNodeWidth`/`kInset` pro Frame, `Z()`).
  Ziehen ab Startposition mit Einrasten; neue Knoten (Rechtsklick, Textur-Drop) auf dem Raster.
  Material-Karte bleibt am rechten Rand der Ansicht (keine Position in der .mat; Format unveraendert).
  Kein Scrollen mehr (NoScrollbar/NoScrollWithMouse): Mausrad = Zoom.
- Tests angepasst: „Material graph sockets ...“ erwartet die eingerastete Position (240 statt 230);
  „Material preview zoom ...“ prueft jetzt, dass das Mausrad ueber dem Graph zoomt und nicht scrollt.

Pruefung: Build x64-Debug (krom_tests, Editor DX11/Vulkan, Loading-Tests). `krom_tests` gruen; UI Standard 18,
`--animation-ui` 95, `--play-ui` 33, `--settings-ui` 15, `--detached-window` 13 PASS.
Nicht geprueft: Aussehen/Bedienung per Maus im Materialeditor (Lesbarkeit bei 0.5, Combos/Popups gezoomt),
Ziehen mit Einrasten (kein eigener Test).

Pruefauftraege Codex: Multi-Viewport-Materialfenster bei Zoom; Texturbilder und Farbwaehler bei kleinem Zoom.

### Neu (Claude, 2026-11-11, Nachtrag 4): Skala/Versatz/Lauf wirken an Normal- und ORM-Texturknoten

Nutzerbefund: Skala am Normal-Texturknoten ohne Wirkung, erst ein angedockter UV-Knoten aenderte etwas.
Ursache: Normal-/ORM-Map haben eigene Textur-Slots und wurden mit der Mesh-UV abgetastet; die Felder
waren dort absichtlich ausgegraut.

- `MaterialGraph.hpp`: Slot-Vergabe als `slotFor`. Normal/ORM ohne UV-Link mit Nicht-Identitaets-
  Transformation: Slot ohne Textur traegt scale/offset/scroll, neuer Befehl Op 15 (Mesh-UV mal Slot-
  Transformation), Index in `outputs.w` Bits 10–14 / 15–19 wie beim UV-Link. Identitaet: nichts
  (Graph bleibt inaktiv, wenn sonst nichts pro Pixel laeuft). Kein freier Slot/Befehl -> Graph ungueltig.
- Shader (`pbr_material_graph.hlsl` / `.opengl.glsl`): Op 15.
- `EditorMaterialGraph.cpp`: UV-Felder nur noch ausgegraut, solange eine UV-Quelle angedockt ist.
- Test im „Material graph UV, Position and Vector2 nodes“: Op 15, Slot ohne Textur, Bits, Identitaet.

Pruefung: `krom_tests` gruen; UI (DX11) Standard 18, `--animation-ui` 96, `--play-ui` 33,
`--material-ui` 18, `--detached-window` 13 PASS; 6 HLSL-PS x 2 Varianten mit fxc/dxc -spirv, 5 GLSL-FS
x 2 Varianten mit glslang fehlerfrei. Nicht geprueft: Bild im Editor/Vulkan-Laufzeit.

Nachtrag „eine Quelle der Wahrheit“ (Nutzerbefund: Wert-Knoten an Normal Strength/Roughness/Metallic
konnte groesser sein als die Karte zeigt; UV-Knoten an der Textur aenderte deren angezeigte Werte nicht):
- `EditorMaterialGraph.cpp`: `CardValueRange` – ein Wert-Knoten an einem Kartenfeld bekommt dessen Bereich
  (Slider, harte Grenze; bei mehreren Feldern die engste), Werte ausserhalb werden beim Zeichnen
  begrenzt. Kartenfeld bei angedocktem Wert-Knoten ausgegraut, zeigt nur den Knotenwert.
- Texturknoten mit UV-Link: Felder ausgegraut und zeigen die Kachelung des UV-Knotens (Versatz/Lauf 0);
  andere Quellen -> Text „vom Graphen gesteuert“ statt veralteter Werte.
- Tests: Wert-Knoten 3 an Roughness -> 1, 7 an Normal Strength -> 4; „Material property fields keep
  their width“: Kartenklick aendert den verlinkten Knoten nicht mehr.

Nachtrag UV-Koordinaten (vom Nutzer entschieden: ausgrauen): Jede Textur hat genau eine UV-Quelle.
- Graph arbeitet auf der unveraenderten Mesh-UV: PBR-PS (`pbr_lit` HLSL/GLSL, Forward+, `shadow_pbr`
  HLSL/GLSL) rechnen uvScale/uvOffset per `KromGraphMeshUV` heraus (nicht bei Box-Projektion). Die
  Sektion „UV-Koordinaten“ gilt nur noch fuer direkt auf der Karte gesetzte Texturen.
- Compiler: Normal/ORM-Texturknoten ohne UV-Link bekommen immer Op 15 (auch ohne eigene Transformation),
  damit sie nicht die material-transformierte Fallback-UV nutzen.
- Format: `graphOutputs` hat hinten ein Flag `rawMeshUV` (1). Aeltere Dateien (ohne Flag, pbr-lit, ohne
  Box-Projektion): `MaterialGraph::AdoptMaterialUV` uebernimmt die Material-Transformation einmalig in die
  Knoten (Textur ohne Link: scale = S*s, offset = O*s + o; UV-Knoten: Kachelung *= S, Versatz entfaellt).
  Beide .mat-Leser (AssetPipeline, EditorMaterialLibrary) rufen das auf.
- Editor: Sektion ausgegraut mit Hinweis, wenn Texturknoten da sind und keine direkte Textur; bei
  Mischung aktiv mit Hinweis „gilt nur fuer direkt zugewiesene Texturen“. Neue Texte de/en.
- Tests: Op 15 auch bei Identitaet, Uebernahme alter Werte (einmalig, Flag in der Rundreise),
  „Material graph compiles“: 7 statt 5 Befehle.
Pruefung: `krom_tests` gruen; UI DX11 Standard 18, Material 18, Animation 96, Play 33, Detached 13 PASS;
6 HLSL-PS x 2 Varianten fxc/dxc, 5 GLSL-FS x 2 Varianten glslang fehlerfrei. Nicht geprueft: Bild im
Editor/Spiel, Vulkan-Laufzeit, Anzeige alter DemoProject-Materialien nach der Uebernahme.
Pruefauftrag Codex: Mischfall direkte Textur + Texturknoten; Normalmap-Tangentenbasis mit Op-15-UV.

### Neu (Claude, 2026-11-11, Nachtrag 3): Gizmo auch in der Controller-Vorschau, Stopp neben Play

Nutzerbefund: Gizmo verschwand weiter mit der Controller-Vorschau; der Nutzer hatte „Preview“ (= Pause)
statt des Quadrats ganz rechts gedrueckt – Stopp war nicht erkennbar. Vorgabe: Gizmo immer zeigen, wenn
nichts dagegen spricht; Stopp direkt neben Play.

- `EditorAnimationWindow.cpp`: `AnimationPreviewOwnsGizmos` ohne `!controllerMode`; die Wurzel einer
  Nicht-Skelett-Kopie wird auch im Controller-Modus vorausgewaehlt. Ziehen bewegt nur die Vorschau-Kopie
  (animierte Kanaele setzt der Controller im naechsten Frame neu).
- `EditorControllerGraph.cpp`: Reihenfolge Play/Pause, Stopp, Neustart.
- Test: „the controller preview shows the gizmo like the clip preview“ ersetzt die alte Gegenpruefung.

Pruefung: `krom_tests` gruen; UI (DX11) Standard 18, `--animation-ui` 96, `--play-ui` PASS. Nicht von
Hand geprueft: Gizmo-Optik waehrend der laufenden Vorschau.

### Neu (Claude, 2026-11-11, Nachtrag 2): Controller-Vorschau mit Stopp, Material-Karte im Graph

Nutzerbefunde: (1) Nach Start der Controller-Vorschau verschwand das Transform-Gizmo im Animationsfenster
und kam nicht zurueck. (2) Materialgraph: Knoten nur in einem festen Bereich verschiebbar; beim Zoomen
schoben sich Knoten unter die Material-Karte.

- (1) `EditorAnimationWindow.cpp`: Vorschau laeuft nur noch nach „Vorschau“ (`controllerRequested`), nicht
  mehr automatisch bei Auswahl. Neu `StopControllerPreview` (Knopf mit Quadrat im Graph-Fenster): beendet
  die Vorschau, das Fenster zeigt wieder die Clips des Objekts samt Gizmos. Graph oder Animationsfenster
  schliessen setzt die Anforderung zurueck. UI-Test angepasst (Start per Knopf) + neu „Stop ends the preview ...“.
- (2) `EditorMaterialGraph.cpp`: Material-Karte liegt in Weltkoordinaten (zoomt/verschiebt sich mit), neue
  Titelzeile „Material“ zum Ziehen (rastet ein); Position pro Sitzung im State-Storage (keine Position in
  der .mat, Format unveraendert). Startposition wie bisher am rechten Rand.
  Grenze 0..3000 aufgehoben: `kMaterialGraphCoordinateLimit` = ±100000 im Editor und im .mat-Parser
  (`MaterialGraph.hpp`); alte Dateien laden unveraendert.
  Test „Material graph sockets ...“: Eingaenge der Karte eine Zeile tiefer (Titelzeile).

Pruefung: `krom_tests` gruen; UI Standard 18, `--animation-ui` 96, `--play-ui` 33, `--settings-ui` 15,
`--detached-window` 13 PASS; Editor DX11/Vulkan gebaut. Nicht von Hand geprueft: Ziehen der Karte, Zoom-Optik.

### Neu (Claude, 2026-11-11): Materialgraph – Knoten UV, Position, Vector2 und UV-Eingang der Textur

Vom Nutzer freigegeben (Formaterweiterung .mat). Nicht committet.

- `include/assets/MaterialGraph.hpp`: neue Typen `UV`, `Position`, `Vector2` (hinten angehaengt,
  `kLastMaterialNodeType`; unbekannte neuere Typen werden beim Lesen uebersprungen). Texture nutzt `a` als
  UV-Eingang (`MaterialNodeInputCount(Texture)` = 1). Compiler: Texture-Befehl `y` = UV-Quelle + 1 (verlinkt:
  Slot-Transform Identitaet, kein Scroll); UV = Op 13 (Kachelung value.xy), Position = Op 14 (Skalierung
  value.x, Ebene value.y 0 XZ/1 XY/2 YZ), Vector2 = Konstante. UV-Links an Normal- und ORM-Texturknoten
  werden mitkompiliert und in `outputs.w` Bits 10–14 / 15–19 uebergeben.
- Shader (`pbr_material_graph.hlsl` / `.opengl.glsl`): `EvaluateMaterialGraph(uv, positionWS, ...)`,
  alte Signatur leitet mit Position 0 weiter (Schatten-/Quad-Paesse ohne Weltposition: Position-Knoten = 0).
  `KromGraphNormalUV`/`KromGraphOrmUV`; `KromGraphRoughness` maskiert jetzt auf 5 Bit. PBR (HLSL/GLSL),
  Forward+, Lit (HLSL/GLSL) tasten Normal-/ORM-Map an der verlinkten UV ab.
- Editor (`EditorMaterialGraph.cpp`): Knoten im Menue, Felder (Kachelung, Ebene+Skalierung, Wertepaar),
  UV-Steckplatz links am Bild des Texturknotens; eigene UV-Felder ausgegraut, solange verlinkt.

Pruefung: `krom_tests` gruen (neu „Material graph UV, Position and Vector2 nodes“: Kodierung, Slots,
Normal-UV-Bits, Datei-Rundreise, unbekannter Typ, Zyklus, Zeichnen inkl. Zoom). Shader uebersetzt ohne
Fehler: 6 HLSL-PS x 4 Varianten mit fxc (ps_5_0) und dxc -spirv (ps_6_0), 5 GLSL-FS x 4 Varianten mit
glslang. UI Standard 18, `--animation-ui` 96, `--play-ui` 33, `--settings-ui` 15, `--detached-window` 13 PASS.
Nicht geprueft: Bild im Editor/Spiel (Kachelung sichtbar, Normalmap mit Position-UV), Maus-Bedienung.

Pruefauftraege Codex: Normalmap-Tangentenraum bei Position-UV (abgeleitete Basis nutzt UV-Ableitungen);
Vulkan-Laufzeit mit Graph-Material.

### [P1] Punktlicht hellt Geometrie hinter einer geschlossenen Wand auf (2026-11-07)

Der Nutzer beobachtet eine aufgehellte Wand hinter einer anderen Wand und hat den
Lichttyp Punkt bestätigt. Der Punktlichtpfad mischt bis zu drei Würfelseiten nach
den absoluten Richtungskomponenten (`ChoosePointShadowFaces`). Die Projektionen
in `ShadowViewBuilder::BuildPoint` haben jeweils 90 Grad Öffnungswinkel. Außerhalb
einer Seite liefert `SampleShadowAtlas` den Wert `1.0` für volle Sichtbarkeit.
Dieser Wert geht mit positivem Gewicht in den Mittelwert ein.

Rechenprobe mit Licht-zu-Empfänger-Vektor `(4, 1, 0)` und Schattenstärke `1`.
Die +X-Seite enthält den Empfänger und liefert bei voller Abschattung `0.0`.
Auf +Y liegt seine projizierte X-Koordinate bei `4` und damit außerhalb des
Intervalls `[-1, 1]`. Der Mittelwert ergibt `0.8 * 0 + 0.2 * 1 = 0.2`.
Das Verhalten steckt in `pbr_lit.ps.hlsl`, `pbr_forward_plus.ps.hlsl`,
`lit.ps.hlsl`, `pbr_lit.opengl.fs.glsl` und `lit.opengl.fs.glsl`.

Korrektur in allen fünf Varianten. Die Würfelseite über die betragsmäßig größte
Komponente des Vektors vom Licht zum Empfänger wählen. Für die Auswahl dieselbe
um den Normalen-Bias versetzte Position wie bei der anschließenden Projektion
verwenden. Vier Varianten enthalten schon `ChoosePointShadowFace` für die
Achsenwahl. Den Tiefenfilter innerhalb der gewählten Atlas-Kachel beibehalten.

Regression im Hauptviewport mit einem Punktlicht, einem deckenden Wandquader
und einem PBR-Empfänger hinter der Wand prüfen. Den Empfänger seitlich versetzen
und die +X/+Y-Grenze auf beiden Seiten prüfen. Bei Schattenstärke `1` soll er im
Inneren des Schattens in F9 dunkel bleiben. Bei freiem Lichtweg muss er wieder
hell sein. Der Rechenpfad ist nachvollzogen, die Nutzerszene wurde visuell
noch nicht geprüft.

Nachtrag (Codex, 2026-11-07). Die Korrektur ist in allen fünf Shader-Varianten
umgesetzt. Vulkan-SPIR-V validiert, DX11 und OpenGL kompiliert, `krom_tests`
bestanden. Die Rechenprobe ergibt jetzt `0.0` bei verdecktem und `1.0` bei
freiem Lichtweg. Die visuelle Prüfung in der Nutzerszene steht aus.

### Neu (Codex, 2026-11-11) – Zahlenfelder und UV-Links im Materialeditor

- Materialwerte verwenden Zahlenfelder mit Ziehen, Doppelklick und Ctrl+Klick.
  Enter übernimmt, Escape verwirft. Die Grenzen sind 0–1 für Metallic, Roughness,
  Alpha und Alpha Cutoff, 0–4 für Normal Strength und Occlusion sowie 0–1000
  für Emissive Strength. Ein Value an mehreren Kartenfeldern übernimmt die
  engste Grenze. Die Karte zeigt den wirksamen Wert schreibgeschützt.
- Texturknoten haben getrennte Eingänge für UV, Scale, Offset und Scroll.
  Die Textur verwendet `UV * Scale + Offset + Scroll * Zeit`. Die drei
  Transformationseingänge funktionieren auch für Normal- und ORM-Knoten.
  Karten-UV-Links gelten für direkt zugewiesene Texturen und sind bei
  Box-Projektion ausgeblendet. Sind nur Graph-Texturen vorhanden, sind die
  Kartenfelder und Steckplätze gesperrt.
- Typregel für Scale, Offset und Scroll sowie die beiden Karten-UV-Eingänge.
  Value, Time und Separate liefern Skalare, die beide Achsen belegen.
  Vector2, UV, Position und Parameter liefern ein Paar für diese Eingänge
  (Parameter verwendet X/Y). Komponentenweise Mathematik übernimmt die
  größte Dimension ihrer Eingänge, Mix die von A/B. Color, Texture und
  Farbergebnisse aus Mathematik sind an diesen Steckplätzen ungültig.
  Konstante Paare und Parameter-Standardwerte stehen schreibgeschützt am
  Ziel. Bei UV ist dort die Kachelung sichtbar. Sonstige dynamische Quellen
  werden als „vom Graphen gesteuert“ angezeigt.
- Die Link-IDs stehen am Ende der bestehenden `graphNode`- und
  `graphOutputs`-Zeilen. Alte Dateien mit belegtem UV-Eingang erhalten eine
  Identitätstransformation, damit die zuvor übergangenen lokalen Werte
  beim Laden das Bild nicht ändern. `AdoptMaterialUV` übernimmt weiterhin
  die UV-Transformation älterer Materialien. Der Compiler hält die Grenzen
  von 16 Anweisungen und vier Slots ein und meldet Überschreitungen. Die
  Shader verwenden freie Komponenten von `graphUV` für die Karten-UV-Links.
  PBR gibt die Mesh-UV zusätzlich an den Pixelshader weiter, damit auch
  eine Materialskala von null die Graph-UV nicht verfälscht.
- `SetMaterialParameter(name, math::Vec2, entity)` setzt X/Y und Z=0.
  `SetMaterialUV(entity, scale, offset)` schreibt die vorhandenen UV-Felder
  einer Materialinstanz, auch bei PBR-Materialien ohne Graph. Graph-Links
  haben auf den verknüpften Achsenpaaren Vorrang. Die Renderbeschreibung
  wird bei verschachtelten Materialinstanzen bis zur Shader-Variante
  aufgelöst, damit die PBR-Script-Instanz gerendert wird.

Geprüft mit `krom_tests` und den DX11-Modi Standard, Material, Animation,
Play und Detached Window. 153 Shader-Prüfungen mit fxc (ps_5_0), dxc
(SPIR-V, ps_6_0) und glslang bestanden, einschließlich aller Kombinationen
von Normal-, ORM- und Alpha-Test-Defines und der geänderten Vertex-Stufen.
Der Vulkan-Play-Test hat alle fünf UV-Parameter per Script an einem
PBR-Material gesetzt. Er hat den GPU-Upload der Instanz, Draws beider
Objekte und die unveränderten Werte des zweiten Objekts bestätigt.
Die Vulkan-Validierung meldete trotzdem Bildlayout- und Semaphore-Fehler
sowie Attachment-Formatfehler. Die Attachment-Fehler traten auch im
regulären Vulkan-Play-Test auf.

Offen für die Sichtprüfung sind die Texturkachelung im Bild, der
Normalmap-Tangentenraum bei projizierten UVs, der Alpha-Schatten und das
Layout der Steckplätze im losgelösten Fenster. Maus- und Tastatureingaben
wurden automatisiert geprüft.
