# Editor Gizmo Material Leak Post Mortem

## Summary

`projects/DemoProject/Assets/Prefabs/mask2.prefab` contained mesh records with

```json
"materialAssetPath": "F:/working/krom codex/editor/dpad_gelb.mat"
```

This is the yellow highlight material of the editor's transform gizmo. It is an
absolute path into the engine's own `editor/` folder, outside the project. The
same path was also copied into the prefab baseline (`baselinePrefabJson`) in
`Assets/Scenes/Testraum.json`. A fresh clone on another machine could not resolve
it.

The data was fixed by hand in `50c0e05` (pointing at the identical project copy
`Materials/dpad_gelb.mat`). This document covers the editor bug that wrote the path
and the code fix in `d637d95`.

## Impact

- Scenes and prefabs saved by the editor could store a machine-specific, absolute
  material path. Projects were then not portable.
- The affected scene meshes also had their material slot overrides cleared, their
  transform overwritten and their active flag toggled by the gizmo for as long as
  the collision lasted. In `mask2.prefab` the user had reassigned the slot
  materials afterwards. The leaked entity-wide path stayed behind unnoticed,
  because slot overrides take priority over it.
- No data loss beyond that. The hand-fixed files load correctly.

## Root Cause

The translate, rotate and scale gizmos (`addons/editor/EditorUIViewport.cpp`) are
made of ordinary ECS entities: three arrows or rings plus a center sphere. Their
IDs are cached in function-local statics (`GetDpadGizmoRuntime()`,
`GetRotateGizmoRuntime()`, `GetScaleGizmoRuntime()`). Every frame,
`Update*Gizmo` writes material path, transform and active state through these
cached IDs. The only safety check was `ctx.world.IsAlive(id)`.

That check is not enough in the editor:

1. Undo/redo (`RestoreSceneSnapshot`), scene loads and entering or leaving prefab
   mode (`ClearSceneEntities`) destroy **all** entities and recreate them. The
   gizmo entities are destroyed too, and their indices go back to the free list.
2. `World::CreateEntity` reuses freed indices and bumps their generation.
   `EntityID` generations are only 8 bits wide (`GEN_BITS = 8`), so they wrap
   after 255 reuses.
3. Assume a gizmo was not shown for a while (no selection, or another tool
   active). Its cached IDs then go stale. After enough full recreate cycles, the
   same index carries the same generation again, now belonging to a scene mesh.
   Because every cycle reuses the indices in the same pattern, all four gizmo IDs
   tend to line up at the same time.
4. `IsAlive` now returns true, and `Update*Gizmo` treats the scene meshes as
   gizmo parts. It clears their `slotOverrides` and writes
   `editor/dpad_*.mat` (`dpad_gelb.mat` while an axis is hovered) as an absolute
   path into `MaterialComponent::materialAssetPath`.
5. `NormalizeMaterialAssetPath` deliberately keeps absolute paths into the engine
   editor folder unchanged, so that gizmos never get rebound to a same-named
   project material. Saving the scene or the prefab therefore wrote the path
   verbatim. `LinkPrefabInstance` / `SynchronizePrefabInstance` then copied it into the instance baseline.

Nothing on the save path checked that a stored material path was project-relative.

### Evidence

- History of `mask2.prefab`: in `da5debe` the record pointed at the project copy
  `Materials/dpad_gelb.mat`. After `cd2a84c` (prefab mode work, many full
  recreate cycles) a record held the absolute editor path.
- The only code in the editor that produces absolute `editor/dpad_*.mat` paths is
  the gizmo update.
- The leaked path sat on the entity-wide field, next to intact slot overrides that
  the user had set again later. That matches a gizmo write (which clears slots)
  followed by manual repair.

The collision was not reproduced in a live editor session. The chain above follows
from the code and the file history.

## Fix (`d637d95`)

### 1. Gizmos only touch their own entities

`IsLiveGizmoEntity<TGizmoComponent>(ctx, id)` requires that the entity is alive
**and** carries both `EditorRuntimeGizmoTag` and the gizmo's own component
(`EditorDpadGizmoComponent`, `EditorRotateGizmoComponent`,
`EditorScaleGizmoComponent`). All `Ensure*`, `Update*` and `Set*Active` paths use
it. A stale ID that now points at a scene entity counts as "not a gizmo", so the
gizmo is recreated instead of writing into the scene.

Before recreating, `DestroyGizmoEntities` removes any part of the old set that is
still a real gizmo entity. A partly destroyed gizmo therefore no longer leaks
entities.

### 2. Saved material paths must be portable

`IsPortableAssetPath(utf8)` in `core/PathUtf8.hpp` accepts only paths relative to
the project's Assets folder. It rejects:

- drive letters (`F:/...`, `C:relative`)
- root directories (`/...`) and UNC paths (`\\server\...`)
- anything that escapes the root via `..`

Virtual sub-asset keys such as `Models/a.glb#material/0` stay valid.

The scene serializer (`MeshRendererSerialization.cpp`) and the prefab writer
(`WriteRecord` in `Prefab.cpp`, which also produces instance baselines) skip
non-portable material paths, on the entity and on every slot. The matching
`materialOverrideSource` is dropped as well, and a warning is logged. Loading is
unchanged, so older files still open.

This is defense in depth: even if a future bug puts an engine-internal or absolute
path on a scene mesh, it can no longer end up in a project file.

### Tests

- `Serialization / Portable asset path rule`
- `Serialization / Scene drops non-portable material paths`
- `Prefab / JSON drops non-portable material paths` (mirrors the `mask2.prefab` records)

`krom_tests` passed in full (Debug x64). The gizmo guard has no unit test of its
own, because the gizmo helpers are file-local in `EditorUIViewport.cpp`.

## Lessons

- **Cached `EntityID`s need an ownership check, not just `IsAlive`.** With 8-bit
  generations and editor workflows that recreate the whole world, generation
  wrap is a realistic event, not a theoretical one. This is the second
  generation-wrap bug after the backend handle-store issue
  (`POSTMORTEM_RENDERING_HANDLE_STORE_BUG.md`). Any long-lived cache of IDs
  should verify that the entity still carries a marker component it owns.
- **Editor-internal assets must stay out of project data.** Keeping engine
  editor paths absolute was the right call for the gizmo. But the save path had
  no rule to stop such a path from leaking. Project files now enforce
  "relative to Assets" at write time.
- **A wrong value hidden behind a higher-priority field goes unnoticed.** The slot
  overrides masked the bad entity-wide path. Validation at the save boundary
  catches these cases regardless of what the renderer shows.

## Follow-ups

- Other editor caches that hold `EntityID`s across full world recreation should
  get the same ownership check. No other static gizmo-style cache was found in
  `addons/editor` or `app`.
- The same portability rule could cover `meshAssetPath` and texture paths. Only
  material paths are enforced today.
