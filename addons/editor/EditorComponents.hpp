#pragma once

#include "ecs/ComponentMeta.hpp"
#include "core/Types.hpp"
#include <cstdint>
#include <string>

namespace engine::renderer::addons::editor {

struct EditorHierarchyComponent
{
    uint32_t order = 0u;
};

// Tag-Komponente: Anwesenheit bedeutet "persistent".
// Entity überlebt LoadScene() / UnloadScene() im Runtime-Wrapper.
struct EditorPersistentComponent {};

// Laufzeit-Tag: welche Editor-Szene hat diese Entity erzeugt?
// Wird beim Laden gesetzt und NICHT serialisiert — rein zur Laufzeit.
// Ermöglicht UnloadEditorScene() gezielt alle Entities einer Szene zu entfernen.
struct EditorSceneTagComponent
{
    std::string sceneName; // z.B. "Level1"
};

// Laufzeit-Tag: Entity ist ein reines Editor-Kamera-Gizmo-Mesh.
// - Wird NICHT serialisiert / gespeichert.
// - Wird NICHT in der Entity-Liste angezeigt.
// - Wird NICHT im Inspector als Mesh/Material dargestellt.
// - Transform folgt jedes Frame der verknüpften cameraEntity.
struct EditorCameraGizmoComponent
{
    EntityID cameraEntity = NULL_ENTITY;
};

// Laufzeit-Tag: Entity ist Teil des Transform-DPad-Gizmos.
// Gemeinsamer Marker für alle Laufzeit-Gizmo-Entities.
// Wird bei jeder Gizmo-Entity-Erstellung gesetzt — unabhängig vom Gizmo-Typ.
// Ermöglicht einheitliche Filter (Outliner, Serializer, Commands) ohne
// Kenntnis des konkreten Gizmo-Typs. Für neues Gizmo: nur dieses Tag setzen,
// kein Filter-Code anpassen.
struct EditorRuntimeGizmoTag {};

// axis: -1 = Zentrum, 0 = X, 1 = Y, 2 = Z.
struct EditorDpadGizmoComponent
{
    int axis = -1;
};

// Laufzeit-Tag: Entity ist Teil des Rotation-Gizmos.
// axis: 0 = X, 1 = Y, 2 = Z.
struct EditorRotateGizmoComponent
{
    int axis = -1;
};

// Laufzeit-Tag: Entity ist Teil des Scale-Gizmos.
// axis: 0 = X, 1 = Y, 2 = Z.
struct EditorScaleGizmoComponent
{
    int axis = -1;
};

// Laufzeit-Tag: Entity ist die interne Kugel fuer den Materialeditor.
// Sie wird weder gespeichert noch in Outliner/Inspector angezeigt.
struct EditorMaterialPreviewComponent {};

// Laufzeit-Tag: Entity gehoert zu einem Asset-Browser-Thumbnail.
struct EditorAssetThumbnailComponent {};

// Laufzeit-Tag: Entity gehoert zur Prefab-Editor-Vorschau.
// Wird weder serialisiert noch im Outliner/Inspector angezeigt.
// Wird im Prefab-Preview-RT gerendert, nicht im Haupt-Viewport.
struct EditorPrefabPreviewComponent {};

inline void RegisterEditorComponents(ecs::ComponentMetaRegistry& registry)
{
    using namespace ecs;
    RegisterComponent<EditorRuntimeGizmoTag>(registry,        "EditorRuntimeGizmoTag");
    RegisterComponent<EditorHierarchyComponent>(registry,    "EditorHierarchyComponent");
    RegisterComponent<EditorPersistentComponent>(registry,   "EditorPersistentComponent");
    RegisterComponent<EditorSceneTagComponent>(registry,     "EditorSceneTagComponent");
    RegisterComponent<EditorCameraGizmoComponent>(registry,  "EditorCameraGizmoComponent");
    RegisterComponent<EditorDpadGizmoComponent>(registry,    "EditorDpadGizmoComponent");
    RegisterComponent<EditorRotateGizmoComponent>(registry,  "EditorRotateGizmoComponent");
    RegisterComponent<EditorScaleGizmoComponent>(registry,   "EditorScaleGizmoComponent");
    RegisterComponent<EditorMaterialPreviewComponent>(registry, "EditorMaterialPreviewComponent");
    RegisterComponent<EditorAssetThumbnailComponent>(registry, "EditorAssetThumbnailComponent");
    RegisterComponent<EditorPrefabPreviewComponent>(registry, "EditorPrefabPreviewComponent");
}

} // namespace engine::renderer::addons::editor
