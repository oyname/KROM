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

## Befunde (Codex)

(noch keine)
