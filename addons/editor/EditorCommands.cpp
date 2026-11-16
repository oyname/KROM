#include "addons/editor/EditorCommands.hpp"
#include "addons/editor/EditorLocalization.hpp"

#include "addons/editor/EditorSerialization.hpp"
#include "addons/camera/CameraComponents.hpp"
#include "addons/camera/CameraSerialization.hpp"
#include "addons/animation/AnimationComponents.hpp"
#include "addons/mesh_renderer/MeshRendererComponents.hpp"
#include "addons/mesh_renderer/MeshBounds.hpp"
#include "addons/lighting/LightingSerialization.hpp"
#include "addons/mesh_renderer/MeshRendererSerialization.hpp"
#include "addons/script/ScriptSerialization.hpp"
#include "addons/physics/PhysicsComponents.hpp"
#include "addons/physics/PhysicsSerialization.hpp"
#include "addons/audio/AudioSerialization.hpp"
#include "addons/particles/ParticleSerialization.hpp"
#include "addons/probes/ProbeSerialization.hpp"
#include "addons/particles/ParticleComponents.hpp"
#include "addons/audio/AudioComponents.hpp"
#include "addons/script/ScriptList.hpp"
#include "addons/script/ScriptRegistry.hpp"
#include "addons/prefab/Prefab.hpp"
#include "addons/prefab/PrefabSerialization.hpp"
#include "addons/editor/EditorMaterialLibrary.hpp"
#include "addons/editor/EditorPrefabWindow.hpp"
#include "assets/AssetPipeline.hpp"
#include "core/Debug.hpp"
#include "core/PathUtf8.hpp"
#include "addons/probes/ProbeComponents.hpp"
#include "ecs/Components.hpp"
#include "serialization/SceneSerializer.hpp"

#include <algorithm>
#include <type_traits>
#include <cmath>
#include <cstdlib>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

namespace engine::renderer::addons::editor {
namespace {

struct EditorSelection
{
    EditorEntityId active = 0;
    std::vector<EditorEntityId> multiple;
    math::Vec3 pivot{};
};

struct SceneSnapshot
{
    std::string sceneJson;
    std::unordered_map<uint32_t, std::string> sceneTags;
    std::unordered_map<uint32_t, EditorEntityId> stableEntityIds;
    EditorSelection selection;
};

struct SnapshotOperation
{
    SceneSnapshot before;
    SceneSnapshot after;
};

struct ComponentOperation
{
    EditorEntityId entity;
    uint32_t typeId;
    std::string before;
    std::string after;
    MeshHandle meshBefore;
    MeshHandle meshAfter;
};

using HistoryOperation = std::variant<EditorHistoryOperation, ComponentOperation, SnapshotOperation>;

struct SelectionChange
{
    EditorSelection before;
    EditorSelection after;
};

struct EditorHistoryEntry
{
    std::string label;
    std::vector<HistoryOperation> operations;
    std::optional<SelectionChange> selection;
};

struct PendingSnapshotEdit
{
    std::string   label;
    SceneSnapshot before;
};

struct PendingComponentEdit
{
    std::string label;
    std::vector<ComponentOperation> components;
    EditorSelection selection;
};

struct EditorHistoryState
{
    std::vector<EditorHistoryEntry> undoStack;
    std::vector<EditorHistoryEntry> redoStack;
    std::unordered_map<std::string, PendingSnapshotEdit> pendingEdits;
    std::vector<std::pair<std::string, PendingComponentEdit>> pendingComponentEdits;
    std::optional<EditorHistoryEntry> openTransaction;
    bool applyingHistory = false;
};

static std::unordered_map<EditorState*, EditorHistoryState> g_editorHistory;
constexpr size_t kMaxHistoryEntries = 64u;

static EditorHistoryState& GetEditorHistory(EditorState& state)
{
    return g_editorHistory[&state];
}

static void BindEditorEntity(EditorState& state, EntityID entity, EditorEntityId id)
{
    state.stableEntityIds.emplace(entity.value, id);
    state.entitiesByStableId.emplace(id, entity);
}

static EditorSelection CaptureEditorSelection(EditorFrameContext& ctx)
{
    EditorSelection selection;
    selection.active = GetStableEditorEntityId(ctx, ctx.state.selectedEntity);
    for (const EntityID entity : ctx.state.multiSelection)
    {
        if (const auto id = GetStableEditorEntityId(ctx, entity))
            selection.multiple.push_back(id);
    }
    selection.pivot = ctx.state.multiSelectionPivot;
    return selection;
}

static void RestoreEditorSelection(EditorFrameContext& ctx, const EditorSelection& selection)
{
    ClearMultiSelection(ctx);
    ctx.state.selectedEntity = ResolveEditorEntity(ctx, selection.active);
    for (const EditorEntityId id : selection.multiple)
    {
        if (const EntityID entity = ResolveEditorEntity(ctx, id))
            ctx.state.multiSelection.push_back(entity);
    }
    if (!ctx.state.multiSelection.empty())
        ctx.state.multiSelectionPivot = selection.pivot;
    ctx.state.rotationEditActive = false;
    ctx.state.rotationEditInitialized = false;
    ctx.state.deleteConfirmOpen = false;
}

static bool IsEditorRuntimeEntity(EditorFrameContext& ctx, EntityID id)
{
    if (ctx.world.Has<EditorRuntimeGizmoTag>(id))
        return true;
    if (ctx.world.Has<EditorMaterialPreviewComponent>(id))
        return true;
    if (ctx.world.Has<EditorAssetThumbnailComponent>(id))
        return true;
    if (ctx.world.Has<EditorPrefabPreviewComponent>(id))
        return true;
    if (ctx.world.Has<EditorPreviewLightComponent>(id))
        return true;
    if (ctx.world.Has<CameraComponent>(id) &&
        !ctx.world.Has<EditorHierarchyComponent>(id) && !ctx.world.Has<PrefabSourceComponent>(id))
        return true;
    return false;
}

static void DestroyEditorRuntimeEntities(EditorFrameContext& ctx)
{
    std::vector<EntityID> runtimeEntities;
    ctx.world.ForEachAlive([&](EntityID id) {
        if (IsEditorRuntimeEntity(ctx, id))
            runtimeEntities.push_back(id);
    });
    for (EntityID id : runtimeEntities)
        ctx.world.DestroyEntity(id);
}

static serialization::SceneSerializer MakeEditorSceneSerializer(EditorFrameContext& ctx)
{
    serialization::SceneSerializer serializer(ctx.world);
    serializer.RegisterDefaultHandlers();
    engine::addons::camera::RegisterCameraSerializationHandlers(serializer);
    engine::addons::lighting::RegisterLightingSerializationHandlers(serializer);
    engine::addons::mesh_renderer::RegisterMeshRendererSerializationHandlers(serializer);
    engine::script::RegisterScriptSerializationHandlers(serializer);
    engine::physics::RegisterPhysicsSerializationHandlers(serializer);
    engine::audio::RegisterAudioSerializationHandlers(serializer);
    engine::particles::RegisterParticleSerializationHandlers(serializer);
    engine::addons::probes::RegisterProbeSerializationHandlers(serializer);
    engine::addons::prefab::RegisterPrefabSerializationHandlers(serializer);
    RegisterEditorSerializationHandlers(serializer);
    return serializer;
}

static serialization::SceneDeserializer MakeEditorSceneDeserializer(EditorFrameContext& ctx)
{
    serialization::SceneDeserializer deserializer(ctx.world);
    deserializer.RegisterDefaultHandlers();
    engine::addons::camera::RegisterCameraDeserializationHandlers(deserializer);
    engine::addons::lighting::RegisterLightingDeserializationHandlers(deserializer);
    engine::addons::mesh_renderer::RegisterMeshRendererDeserializationHandlers(deserializer);
    engine::script::RegisterScriptDeserializationHandlers(deserializer, ctx.scriptRegistry);
    engine::physics::RegisterPhysicsDeserializationHandlers(deserializer);
    engine::audio::RegisterAudioDeserializationHandlers(deserializer);
    engine::particles::RegisterParticleDeserializationHandlers(deserializer);
    engine::addons::probes::RegisterProbeDeserializationHandlers(deserializer);
    engine::addons::prefab::RegisterPrefabDeserializationHandlers(deserializer);
    RegisterEditorDeserializationHandlers(deserializer);
    return deserializer;
}

static SceneSnapshot CaptureSceneSnapshot(EditorFrameContext& ctx)
{
    auto serializer = MakeEditorSceneSerializer(ctx);
    serializer.SetEntityFilter([&ctx](EntityID id) -> bool {
        return !IsEditorRuntimeEntity(ctx, id);
    });

    SceneSnapshot snapshot;
    snapshot.sceneJson = serializer.SerializeToJson("EditorScene");
    ctx.world.ForEachAlive([&](EntityID entity) {
        if (IsEditorRuntimeEntity(ctx, entity))
            return;
        if (const auto* tag = ctx.world.Get<EditorSceneTagComponent>(entity))
            snapshot.sceneTags.emplace(entity.value, tag->sceneName);
        snapshot.stableEntityIds.emplace(entity.value, GetStableEditorEntityId(ctx, entity));
    });
    snapshot.selection = CaptureEditorSelection(ctx);
    return snapshot;
}

static bool RestoreSceneSnapshot(EditorFrameContext& ctx, const SceneSnapshot& snapshot)
{
    std::string error;
    const auto root = serialization::JsonParser::Parse(snapshot.sceneJson, error);
    const auto* serializedEntities = root.Get("entities");
    if (!error.empty() || !serializedEntities || !serializedEntities->IsArray())
    {
        ctx.lastFileMessage = Translate("history.failed_prefix") + error;
        return false;
    }

    std::vector<EntityID> entities;
    ctx.world.ForEachAlive([&](EntityID id) {
        entities.push_back(id);
    });
    for (EntityID id : entities)
        ctx.world.DestroyEntity(id);

    auto deserializer = MakeEditorSceneDeserializer(ctx);

    const serialization::DeserializeResult result = deserializer.DeserializeFromValue(root);
    if (!result.success)
    {
        ctx.lastFileMessage = Translate("history.failed_prefix") + result.error;
        return false;
    }
    if (ctx.scriptRegistry)
        engine::script::ResolveScriptEntityReferences(ctx.world, *ctx.scriptRegistry);
    for (const auto& [oldId, sceneName] : snapshot.sceneTags)
        if (const auto it = result.entityRemap.find(oldId); it != result.entityRemap.end())
            ctx.world.Add<EditorSceneTagComponent>(it->second, EditorSceneTagComponent{sceneName});

    DestroyEditorRuntimeEntities(ctx);

    for (const auto& [oldId, entity] : result.entityRemap)
    {
        const auto it = snapshot.stableEntityIds.find(oldId);
        if (it != snapshot.stableEntityIds.end())
            BindEditorEntity(ctx.state, entity, it->second);
    }

    std::unordered_map<uint32_t, std::vector<EntityID>> siblingsByParent;
    ctx.world.ForEachAlive([&](EntityID id) {
        const auto* parentComponent = ctx.world.Get<ParentComponent>(id);
        const EntityID parent = parentComponent ? parentComponent->parent : NULL_ENTITY;
        siblingsByParent[parent.IsValid() ? parent.value : NULL_ENTITY.value].push_back(id);
    });
    for (auto& [parentValue, siblings] : siblingsByParent)
    {
        (void)parentValue;
        std::sort(siblings.begin(), siblings.end(), [&](EntityID a, EntityID b) {
            const auto* hierarchyA = ctx.world.Get<EditorHierarchyComponent>(a);
            const auto* hierarchyB = ctx.world.Get<EditorHierarchyComponent>(b);
            const uint32_t orderA = hierarchyA ? hierarchyA->order : a.value;
            const uint32_t orderB = hierarchyB ? hierarchyB->order : b.value;
            if (orderA != orderB)
                return orderA < orderB;
            const auto* nameA = ctx.world.Get<NameComponent>(a);
            const auto* nameB = ctx.world.Get<NameComponent>(b);
            const std::string textA = nameA ? nameA->name : std::string{};
            const std::string textB = nameB ? nameB->name : std::string{};
            if (textA != textB)
                return textA < textB;
            return a.value < b.value;
        });
        for (uint32_t i = 0; i < siblings.size(); ++i)
        {
            if (auto* hierarchy = ctx.world.Get<EditorHierarchyComponent>(siblings[i]))
                hierarchy->order = i;
            else
                ctx.world.Add<EditorHierarchyComponent>(siblings[i], EditorHierarchyComponent{i});
        }
    }

    RestoreEditorSelection(ctx, snapshot.selection);
    if (ctx.syncSceneState)
        ctx.syncSceneState();
    return true;
}

static void PushHistoryEntry(EditorHistoryState& history, EditorHistoryEntry entry)
{
    history.redoStack.clear();
    history.undoStack.push_back(std::move(entry));
    if (history.undoStack.size() > kMaxHistoryEntries)
        history.undoStack.erase(history.undoStack.begin());
}

static bool SameSnapshotData(const SceneSnapshot& a, const SceneSnapshot& b)
{
    return a.sceneJson == b.sceneJson && a.sceneTags == b.sceneTags;
}

static void PushSnapshotOperation(EditorHistoryState& history, std::string label,
                                  SceneSnapshot before, SceneSnapshot after)
{
    // Field pastes can also pass through a pending component edit.
    if (!history.undoStack.empty() && history.undoStack.back().operations.size() == 1)
    {
        const auto* previous = std::get_if<SnapshotOperation>(&history.undoStack.back().operations.front());
        if (previous && SameSnapshotData(previous->before, before) && SameSnapshotData(previous->after, after))
            return;
    }
    EditorHistoryEntry entry;
    entry.label = std::move(label);
    entry.operations.emplace_back(SnapshotOperation{std::move(before), std::move(after)});
    PushHistoryEntry(history, std::move(entry));
}

static PendingComponentEdit CaptureComponentEdit(EditorFrameContext& ctx, const char* label,
    std::span<const EditorComponentTarget> targets, const void* previous = nullptr)
{
    PendingComponentEdit edit;
    edit.label = label ? label : "Editor action";
    edit.selection = CaptureEditorSelection(ctx);
    const auto serializer = MakeEditorSceneSerializer(ctx);
    for (const auto& [entity, typeId] : targets)
    {
        if (!ctx.world.IsAlive(entity) || !serializer.HasComponentHandler(typeId))
            continue;
        auto before = previous ? serializer.SerializeComponentValue(typeId, previous)
                               : serializer.SerializeComponent(entity, typeId);
        ComponentOperation operation{GetStableEditorEntityId(ctx, entity), typeId, std::move(before), {}, {}, {}};
        if (typeId == ComponentTypeID<MeshComponent>::Get())
        {
            const auto* mesh = previous ? static_cast<const MeshComponent*>(previous)
                                        : ctx.world.Get<MeshComponent>(entity);
            if (mesh && mesh->meshAssetPath.empty())
                operation.meshBefore = mesh->mesh;
        }
        edit.components.push_back(std::move(operation));
    }
    return edit;
}

static bool FinishComponentEdit(EditorFrameContext& ctx, PendingComponentEdit edit)
{
    if (edit.components.empty())
        return false;
    EditorHistoryEntry entry;
    const auto serializer = MakeEditorSceneSerializer(ctx);
    for (auto& component : edit.components)
    {
        const EntityID entity = ResolveEditorEntity(ctx, component.entity);
        if (!entity.IsValid())
            continue;
        component.after = serializer.SerializeComponent(entity, component.typeId);
        if (component.typeId == ComponentTypeID<MeshComponent>::Get())
        {
            if (const auto* mesh = ctx.world.Get<MeshComponent>(entity); mesh && mesh->meshAssetPath.empty())
                component.meshAfter = mesh->mesh;
        }
        if (component.after != component.before || component.meshAfter != component.meshBefore)
            entry.operations.emplace_back(std::move(component));
    }
    if (entry.operations.empty())
        return false;
    auto& history = GetEditorHistory(ctx.state);
    if (history.openTransaction)
    {
        auto& operations = history.openTransaction->operations;
        for (auto& operation : entry.operations)
            operations.push_back(std::move(operation));
    }
    else
    {
        entry.label = std::move(edit.label);
        entry.selection = SelectionChange{std::move(edit.selection), CaptureEditorSelection(ctx)};
        PushHistoryEntry(history, std::move(entry));
    }
    return true;
}

static auto FindPendingComponentEdit(EditorHistoryState& history, const std::string& key)
{
    return std::find_if(history.pendingComponentEdits.begin(), history.pendingComponentEdits.end(),
        [&](const auto& edit) { return edit.first == key; });
}

static bool ApplyComponentOperation(EditorFrameContext& ctx, const ComponentOperation& operation, bool redo,
                                    serialization::SceneDeserializer& deserializer)
{
    const EntityID entity = ResolveEditorEntity(ctx, operation.entity);
    if (!entity.IsValid() || !ctx.world.GetComponentMetaRegistry().Get(operation.typeId))
        return false;
    const auto& value = redo ? operation.after : operation.before;
    const bool hadComponent = ctx.world.Get(entity, operation.typeId) != nullptr;
    const auto* oldMesh = operation.typeId == ComponentTypeID<MeshComponent>::Get()
        ? ctx.world.Get<MeshComponent>(entity) : nullptr;
    const MeshHandle oldMeshHandle = oldMesh ? oldMesh->mesh : MeshHandle{};
    if (value.empty())
    {
        if (hadComponent)
            ctx.world.Remove(entity, operation.typeId);
    }
    else
    {
        std::string error;
        const auto component = serialization::JsonParser::Parse(value, error);
        if (!error.empty() || !deserializer.DeserializeComponent(entity, component) ||
            !ctx.world.Get(entity, operation.typeId))
            return false;
    }

    const std::span<const EntityID> target{&entity, 1};
    if (operation.typeId == ComponentTypeID<TransformComponent>::Get())
    {
        if (!hadComponent && !value.empty() && !ctx.world.Has<WorldTransformComponent>(entity))
            ctx.world.Add<WorldTransformComponent>(entity);
        if (hadComponent && value.empty() && ctx.world.Has<WorldTransformComponent>(entity))
            ctx.world.Remove<WorldTransformComponent>(entity);
    }
    if (operation.typeId == ComponentTypeID<MeshComponent>::Get() && !value.empty())
    {
        auto* mesh = ctx.world.Get<MeshComponent>(entity);
        if (mesh->meshAssetPath.empty())
            mesh->mesh = redo ? operation.meshAfter : operation.meshBefore;
        else if (ctx.assetPipeline && !ctx.registry.meshes.Get(mesh->mesh))
            engine::addons::mesh_renderer::ResolveMeshAssetBindings(*ctx.assetPipeline, ctx.registry, ctx.world, target);
        if (ctx.world.Get<MeshComponent>(entity)->mesh != oldMeshHandle)
            engine::mesh_renderer::UpdateLocalBoundsForEntity(ctx.world, entity, ctx.registry);
    }
    if (operation.typeId == ComponentTypeID<MaterialComponent>::Get() && !value.empty())
        ResolveMaterialAssetBindings(ctx, target);
    if (operation.typeId == ComponentTypeID<AnimationPlayerComponent>::Get() && ctx.assetPipeline && !value.empty())
        engine::addons::mesh_renderer::ResolveAnimationAssetBindings(*ctx.assetPipeline, ctx.registry, ctx.world, target);
    return true;
}

static void SetExclusiveMainCameraInternal(EditorFrameContext& ctx, EntityID entity)
{
    ctx.world.View<CameraComponent>([&](EntityID otherId, CameraComponent& camera)
    {
        camera.isMainCamera = (otherId == entity);
    });
}

static bool HasActiveSceneCamera(EditorFrameContext& ctx)
{
    bool found = false;
    ctx.world.View<CameraComponent>([&](EntityID, const CameraComponent& camera)
    {
        found = found || camera.isMainCamera;
    });
    return found;
}

static std::string MakeUniqueEntityName(EditorFrameContext& ctx, const char* baseName)
{
    uint32_t maxSuffix = 0u;
    bool baseExists = false;
    const std::string base = baseName ? baseName : "Entity";

    ctx.world.ForEachAlive([&](EntityID id)
    {
        const auto* name = ctx.world.Get<NameComponent>(id);
        if (!name)
            return;

        if (name->name == base)
        {
            baseExists = true;
            maxSuffix = std::max(maxSuffix, 1u);
            return;
        }

        const std::string prefix = base + " ";
        if (name->name.rfind(prefix, 0u) != 0u)
            return;

        const char* suffixText = name->name.c_str() + prefix.size();
        char* suffixEnd = nullptr;
        const unsigned long suffix = std::strtoul(suffixText, &suffixEnd, 10);
        if (suffixEnd && *suffixEnd == '\0' && suffix > 0ul)
            maxSuffix = std::max(maxSuffix, static_cast<uint32_t>(suffix));
    });

    if (!baseExists && maxSuffix == 0u)
        return base;
    return base + " " + std::to_string(maxSuffix + 1u);
}

static TransformComponent MakeEditorPlacedTransform(const EditorFrameContext& ctx)
{
    TransformComponent transform{};
    transform.localPosition = ctx.state.editorCamera.position;
    transform.localRotation = math::Quat::FromEulerDeg(
        ctx.state.editorCamera.pitchDeg,
        ctx.state.editorCamera.yawDeg,
        0.f);
    transform.localScale = {1.f, 1.f, 1.f};
    transform.dirty = true;
    return transform;
}

// Empty entities: no rotation, a few units in front of the editor camera
// (not inside it), so a mesh added later is upright and in view.
static TransformComponent MakeEditorPlacedEmptyTransform(const EditorFrameContext& ctx)
{
    constexpr float kPlaceDistance = 5.f;
    const EditorCameraState& cam = ctx.state.editorCamera;
    const math::Vec3 forward =
        math::Quat::FromEulerDeg(cam.pitchDeg, cam.yawDeg, 0.f).Rotate({0.f, 0.f, -1.f});
    TransformComponent transform{};
    transform.localPosition = cam.position + forward * kPlaceDistance;
    transform.localRotation = math::Quat::Identity();
    transform.localScale = {1.f, 1.f, 1.f};
    transform.dirty = true;
    return transform;
}

static uint32_t ComputeNextSiblingOrder(EditorFrameContext& ctx, EntityID parent)
{
    uint32_t maxOrder = 0u;
    bool found = false;
    ctx.world.ForEachAlive([&](EntityID id) {
        if (GetEntityParent(ctx, id) != parent)
            return;
        maxOrder = std::max(maxOrder, GetEntityHierarchyOrder(ctx, id));
        found = true;
    });
    return found ? (maxOrder + 1u) : 0u;
}

static void EnsureEntityHierarchyOrder(EditorFrameContext& ctx,
                                       EntityID            entity,
                                       uint32_t            preferredOrder);

// Places a freshly created entity in the hierarchy: as last child of `parent`
// (at the parent origin) when that parent is usable, otherwise at the end of
// the root level. Folders only nest under folders.
static void PlaceNewEntityInHierarchy(EditorFrameContext& ctx, EntityID entity, EntityID parent)
{
    const bool usableParent =
        parent.IsValid() && parent != entity && ctx.world.IsAlive(parent) &&
        (!ctx.world.Has<FolderComponent>(entity) || ctx.world.Has<FolderComponent>(parent));
    if (!usableParent)
    {
        EnsureEntityHierarchyOrder(ctx, entity, ComputeNextSiblingOrder(ctx, NULL_ENTITY));
        return;
    }

    EnsureEntityHierarchyOrder(ctx, entity, ComputeNextSiblingOrder(ctx, parent));
    ctx.world.Add<ParentComponent>(entity, ParentComponent{parent});
    if (auto* children = ctx.world.Get<ChildrenComponent>(parent))
        children->Add(entity);
    else
        ctx.world.Add<ChildrenComponent>(parent).Add(entity);

    if (auto* transform = ctx.world.Get<TransformComponent>(entity))
    {
        transform->localPosition = {0.f, 0.f, 0.f};
        transform->localRotation = math::Quat::Identity();
        transform->localScale    = {1.f, 1.f, 1.f};
        transform->dirty = true;
        ++transform->localVersion;
    }
}

static void EnsureEntityHierarchyOrder(EditorFrameContext& ctx,
                                       EntityID            entity,
                                       uint32_t            preferredOrder)
{
    if (auto* hierarchy = ctx.world.Get<EditorHierarchyComponent>(entity))
    {
        hierarchy->order = preferredOrder;
        return;
    }
    ctx.world.Add<EditorHierarchyComponent>(entity, EditorHierarchyComponent{preferredOrder});
}

static std::vector<EntityID> CollectOrderedSiblings(EditorFrameContext& ctx, EntityID entity)
{
    std::vector<EntityID> siblings;
    const EntityID parent = GetEntityParent(ctx, entity);
    ctx.world.ForEachAlive([&](EntityID id) {
        if (GetEntityParent(ctx, id) == parent)
            siblings.push_back(id);
    });
    std::sort(siblings.begin(), siblings.end(), [&](EntityID a, EntityID b) {
        const uint32_t orderA = GetEntityHierarchyOrder(ctx, a);
        const uint32_t orderB = GetEntityHierarchyOrder(ctx, b);
        if (orderA != orderB)
            return orderA < orderB;
        return a.value < b.value;
    });
    return siblings;
}

static bool IsDescendantOf(EditorFrameContext& ctx, EntityID candidate, EntityID ancestor)
{
    EntityID cursor = candidate;
    uint32_t depth = 0u;
    while (cursor.IsValid() && ctx.world.IsAlive(cursor) && depth++ < 1024u)
    {
        if (cursor == ancestor)
            return true;

        const auto* parent = ctx.world.Get<ParentComponent>(cursor);
        cursor = parent ? parent->parent : NULL_ENTITY;
    }
    return false;
}

static void RemoveFromParentChildren(EditorFrameContext& ctx, EntityID child)
{
    const auto* parent = ctx.world.Get<ParentComponent>(child);
    if (!parent || !parent->parent.IsValid() || !ctx.world.IsAlive(parent->parent))
        return;

    if (auto* children = ctx.world.Get<ChildrenComponent>(parent->parent))
        children->Remove(child);
}

static math::Mat4 CurrentWorldMatrix(EditorFrameContext& ctx, EntityID entity) noexcept
{
    if (const auto* wt = ctx.world.Get<WorldTransformComponent>(entity))
        return wt->matrix;
    return math::Mat4::Identity();
}

static math::Vec3 MatrixTranslation(const math::Mat4& m) noexcept
{
    return {m.m[3][0], m.m[3][1], m.m[3][2]};
}

static math::Quat QuatFromRotationMatrix(const math::Mat4& m) noexcept
{
    const float trace = m.m[0][0] + m.m[1][1] + m.m[2][2];
    if (trace > 0.f)
    {
        const float s = std::sqrt(trace + 1.f) * 2.f;
        return { (m.m[1][2] - m.m[2][1]) / s,
                 (m.m[2][0] - m.m[0][2]) / s,
                 (m.m[0][1] - m.m[1][0]) / s,
                 0.25f * s };
    }
    if (m.m[0][0] > m.m[1][1] && m.m[0][0] > m.m[2][2])
    {
        const float s = std::sqrt(1.f + m.m[0][0] - m.m[1][1] - m.m[2][2]) * 2.f;
        return { 0.25f * s,
                 (m.m[1][0] + m.m[0][1]) / s,
                 (m.m[2][0] + m.m[0][2]) / s,
                 (m.m[1][2] - m.m[2][1]) / s };
    }
    if (m.m[1][1] > m.m[2][2])
    {
        const float s = std::sqrt(1.f + m.m[1][1] - m.m[0][0] - m.m[2][2]) * 2.f;
        return { (m.m[1][0] + m.m[0][1]) / s,
                 0.25f * s,
                 (m.m[2][1] + m.m[1][2]) / s,
                 (m.m[2][0] - m.m[0][2]) / s };
    }

    const float s = std::sqrt(1.f + m.m[2][2] - m.m[0][0] - m.m[1][1]) * 2.f;
    return { (m.m[2][0] + m.m[0][2]) / s,
             (m.m[2][1] + m.m[1][2]) / s,
             0.25f * s,
             (m.m[0][1] - m.m[1][0]) / s };
}

static math::Vec3 MatrixScale(const math::Mat4& m) noexcept
{
    const math::Vec3 x{m.m[0][0], m.m[0][1], m.m[0][2]};
    const math::Vec3 y{m.m[1][0], m.m[1][1], m.m[1][2]};
    const math::Vec3 z{m.m[2][0], m.m[2][1], m.m[2][2]};
    return {x.Length(), y.Length(), z.Length()};
}

static void DecomposeTRS(const math::Mat4& m,
                         math::Vec3& outTranslation,
                         math::Quat& outRotation,
                         math::Vec3& outScale) noexcept
{
    outTranslation = MatrixTranslation(m);
    outScale = MatrixScale(m);

    math::Mat4 rotationMatrix = math::Mat4::Identity();
    if (outScale.x > math::EPSILON)
    {
        rotationMatrix.m[0][0] = m.m[0][0] / outScale.x;
        rotationMatrix.m[0][1] = m.m[0][1] / outScale.x;
        rotationMatrix.m[0][2] = m.m[0][2] / outScale.x;
    }
    if (outScale.y > math::EPSILON)
    {
        rotationMatrix.m[1][0] = m.m[1][0] / outScale.y;
        rotationMatrix.m[1][1] = m.m[1][1] / outScale.y;
        rotationMatrix.m[1][2] = m.m[1][2] / outScale.y;
    }
    if (outScale.z > math::EPSILON)
    {
        rotationMatrix.m[2][0] = m.m[2][0] / outScale.z;
        rotationMatrix.m[2][1] = m.m[2][1] / outScale.z;
        rotationMatrix.m[2][2] = m.m[2][2] / outScale.z;
    }

    outRotation = QuatFromRotationMatrix(rotationMatrix).Normalized();
}

static void SetWorldTransform(EditorFrameContext& ctx,
                              EntityID entity,
                              TransformComponent& transform,
                              const math::Mat4& desiredWorldMatrix)
{
    math::Vec3 worldPosition{};
    math::Quat worldRotation = math::Quat::Identity();
    math::Vec3 worldScale{1.f, 1.f, 1.f};
    DecomposeTRS(desiredWorldMatrix, worldPosition, worldRotation, worldScale);

    if (const auto* parent = ctx.world.Get<ParentComponent>(entity))
    {
        if (parent->parent.IsValid())
        {
            if (const auto* parentWorld = ctx.world.Get<WorldTransformComponent>(parent->parent))
            {
                const math::Quat invParentRotation = parentWorld->rotation.Conjugate().Normalized();
                transform.localPosition = invParentRotation.Rotate(worldPosition - parentWorld->position);
                if (transform.inheritParentScale)
                {
                    transform.localPosition = {
                        std::abs(parentWorld->scale.x) > math::EPSILON ? transform.localPosition.x / parentWorld->scale.x : transform.localPosition.x,
                        std::abs(parentWorld->scale.y) > math::EPSILON ? transform.localPosition.y / parentWorld->scale.y : transform.localPosition.y,
                        std::abs(parentWorld->scale.z) > math::EPSILON ? transform.localPosition.z / parentWorld->scale.z : transform.localPosition.z
                    };
                }
                transform.localRotation = (invParentRotation * worldRotation).Normalized();
                if (transform.inheritParentScale)
                {
                    transform.localScale = {
                        std::abs(parentWorld->scale.x) > math::EPSILON ? worldScale.x / parentWorld->scale.x : worldScale.x,
                        std::abs(parentWorld->scale.y) > math::EPSILON ? worldScale.y / parentWorld->scale.y : worldScale.y,
                        std::abs(parentWorld->scale.z) > math::EPSILON ? worldScale.z / parentWorld->scale.z : worldScale.z
                    };
                }
                else
                {
                    transform.localScale = worldScale;
                }
                transform.dirty = true;
                return;
            }
        }
    }

    transform.localPosition = worldPosition;
    transform.localRotation = worldRotation;
    transform.localScale    = worldScale;
    transform.dirty = true;
}

static EntityID CreateCameraEntityRaw(EditorFrameContext& ctx, EntityID parent)
{
    const EntityID entity = ctx.world.CreateEntity();
    ctx.world.Add<NameComponent>(entity, NameComponent{MakeUniqueEntityName(ctx, "Camera")});
    ctx.world.Add<TransformComponent>(entity, MakeEditorPlacedTransform(ctx));
    ctx.world.Add<WorldTransformComponent>(entity);
    ctx.world.Add<ActiveComponent>(entity, ActiveComponent{true});
    PlaceNewEntityInHierarchy(ctx, entity, parent);

    CameraComponent camera{};
    camera.projection = ProjectionType::Perspective;
    camera.fovYDeg = 60.f;
    camera.nearPlane = 0.05f;
    camera.farPlane = 1000.f;
    camera.isMainCamera = !HasActiveSceneCamera(ctx);
    ctx.world.Add<CameraComponent>(entity, camera);

    if (camera.isMainCamera)
        SetExclusiveMainCameraInternal(ctx, entity);

    ClearMultiSelection(ctx);
    ctx.state.selectedEntity = entity;
    return entity;
}

static const char* LightTypeName(LightType type) noexcept
{
    switch (type)
    {
    case LightType::Directional: return "Directional Light";
    case LightType::Point:       return "Point Light";
    case LightType::Spot:        return "Spot Light";
    default:                     return "Light";
    }
}

static EntityID CreateLightEntityRaw(EditorFrameContext& ctx, LightType type, EntityID parent)
{
    const EntityID entity = ctx.world.CreateEntity();
    ctx.world.Add<NameComponent>(entity, NameComponent{MakeUniqueEntityName(ctx, LightTypeName(type))});
    ctx.world.Add<TransformComponent>(entity, MakeEditorPlacedTransform(ctx));
    ctx.world.Add<WorldTransformComponent>(entity);
    ctx.world.Add<ActiveComponent>(entity, ActiveComponent{true});
    PlaceNewEntityInHierarchy(ctx, entity, parent);

    LightComponent light{};
    light.type = type;
    light.color = {1.f, 0.98f, 0.92f};
    light.intensity = type == LightType::Directional ? 3.f : 10.f;
    light.range = type == LightType::Directional ? 100.f : 12.f;
    light.spotInnerDeg = 18.f;
    light.spotOuterDeg = 35.f;
    light.castShadows = true;
    light.shadowSettings.enabled = light.castShadows;
    light.shadowSettings.resolution = type == LightType::Directional ? 2048u : 1024u;
    light.shadowSettings.bias = 0.0015f;
    light.shadowSettings.normalBias = 0.001f;
    light.shadowSettings.maxDistance = type == LightType::Directional ? 100.f : light.range;
    light.shadowSettings.strength = 1.f;
    ctx.world.Add<LightComponent>(entity, light);

    ClearMultiSelection(ctx);
    ctx.state.selectedEntity = entity;
    return entity;
}

static bool ReparentEntityRaw(EditorFrameContext& ctx, EntityID child, EntityID newParent)
{
    if (!child.IsValid() || !ctx.world.IsAlive(child))
        return false;
    if (newParent.IsValid() && !ctx.world.IsAlive(newParent))
        return false;
    if (newParent == child || (newParent.IsValid() && IsDescendantOf(ctx, newParent, child)))
    {
        Debug::Log("[Reparent] SKIP child=%u -> newParent=%u (Zyklus oder self)", child.value, newParent.value);
        return false;
    }

    if (ctx.world.Has<FolderComponent>(child) && newParent.IsValid() && !ctx.world.Has<FolderComponent>(newParent))
        return false;

    const auto* currentParent = ctx.world.Get<ParentComponent>(child);
    const EntityID oldParent = currentParent ? currentParent->parent : NULL_ENTITY;
    if (oldParent == newParent)
    {
        Debug::Log("[Reparent] SKIP child=%u -> newParent=%u (gleicher Parent %u)", child.value, newParent.value, oldParent.value);
        return false;
    }

    Debug::Log("[Reparent] child=%u oldParent=%u -> newParent=%u", child.value, oldParent.value, newParent.value);

    const math::Mat4 worldMatrix = CurrentWorldMatrix(ctx, child);

    RemoveFromParentChildren(ctx, child);

    if (ctx.world.Has<ParentComponent>(child))
        ctx.world.Remove<ParentComponent>(child);

    if (newParent.IsValid())
    {
        ctx.world.Add<ParentComponent>(child, ParentComponent{newParent});

        if (auto* children = ctx.world.Get<ChildrenComponent>(newParent))
            children->Add(child);
        else
            ctx.world.Add<ChildrenComponent>(newParent).Add(child);
    }

    if (auto* transform = ctx.world.Get<TransformComponent>(child))
        SetWorldTransform(ctx, child, *transform, worldMatrix);

    EnsureEntityHierarchyOrder(ctx, child, ComputeNextSiblingOrder(ctx, newParent));
    ClearMultiSelection(ctx);
    ctx.state.selectedEntity = child;
    Debug::Log("[Reparent] DONE child=%u ist jetzt unter %u", child.value, newParent.value);
    return true;
}

static EntityID DuplicateEntitySelfRaw(EditorFrameContext& ctx, EntityID source)
{
    if (!source.IsValid() || !ctx.world.IsAlive(source))
        return NULL_ENTITY;

    const EntityID copy = ctx.world.CreateEntity();

    if (const auto* t = ctx.world.Get<TransformComponent>(source))
    {
        TransformComponent tc = *t;
        tc.dirty = true;
        ctx.world.Add<TransformComponent>(copy, tc);
    }

    if (const auto* wt = ctx.world.Get<WorldTransformComponent>(source))
        ctx.world.Add<WorldTransformComponent>(copy, *wt);
    if (const auto* a = ctx.world.Get<ActiveComponent>(source))
        ctx.world.Add<ActiveComponent>(copy, *a);
    if (const auto* b = ctx.world.Get<BoundsComponent>(source))
        ctx.world.Add<BoundsComponent>(copy, *b);

    {
        std::string baseName;
        if (const auto* n = ctx.world.Get<NameComponent>(source))
            baseName = n->name.empty() ? "Entity" : (n->name + " Kopie");
        else
            baseName = "Entity Kopie";
        ctx.world.Add<NameComponent>(copy, NameComponent{MakeUniqueEntityName(ctx, baseName.c_str())});
    }

    if (const auto* l = ctx.world.Get<LightComponent>(source))
        ctx.world.Add<LightComponent>(copy, *l);
    if (const auto* m = ctx.world.Get<MeshComponent>(source))
        ctx.world.Add<MeshComponent>(copy, *m);
    if (const auto* mat = ctx.world.Get<MaterialComponent>(source))
        ctx.world.Add<MaterialComponent>(copy, *mat);

    if (const auto* cam = ctx.world.Get<CameraComponent>(source))
    {
        CameraComponent camCopy = *cam;
        camCopy.isMainCamera = false;
        ctx.world.Add<CameraComponent>(copy, camCopy);
    }

    // Uebrige Komponenten: Kollision, Physik, Audio, Partikel, Scripts.
    auto copyIf = [&](auto tag) {
        using T = typename decltype(tag)::type;
        if (const T* c = ctx.world.Get<T>(source))
            ctx.world.Add<T>(copy, *c);
    };
    copyIf(std::type_identity<OBBComponent>{});
    copyIf(std::type_identity<FolderComponent>{});
    if (const auto* probe = ctx.world.Get<ReflectionProbeComponent>(source))
    {
        // The copy bakes to its own file.
        ReflectionProbeComponent probeCopy = *probe;
        probeCopy.probeId = engine::addons::probes::GenerateProbeId();
        probeCopy.bakedDataPath.clear();
        probeCopy.bakeLocked = false;
        probeCopy.autoSizeLocked = false;
        ctx.world.Add<ReflectionProbeComponent>(copy, probeCopy);
    }
    copyIf(std::type_identity<RigidBodyComponent>{});
    copyIf(std::type_identity<SphereColliderComponent>{});
    copyIf(std::type_identity<CapsuleColliderComponent>{});
    copyIf(std::type_identity<CharacterControllerComponent>{});
    copyIf(std::type_identity<AudioSourceComponent>{});
    copyIf(std::type_identity<AudioListenerComponent>{});
    copyIf(std::type_identity<ParticleEmitterComponent>{});
    if (ctx.world.Has<engine::script::ScriptList>(source))
    {
        engine::script::ScriptList list;
        list.SetOwnerEntity(copy);
        ctx.world.Add<engine::script::ScriptList>(copy, std::move(list));
        // Erst danach holen: Add kann Komponenten-Speicher verschieben.
        auto* target = ctx.world.Get<engine::script::ScriptList>(copy);
        auto* scripts = ctx.world.Get<engine::script::ScriptList>(source);
        for (const engine::script::ScriptInstance& instance : scripts->Instances_Mutable())
        {
            const bool live = ctx.scriptRegistry && ctx.scriptRegistry->Has(instance.className);
            if (live)
                target->Add(instance.className, copy, *ctx.scriptRegistry);
            else
                target->AddNameOnly(instance.className, instance.instanceName);
            const size_t index = target->Instances_Mutable().size() - 1u;
            target->Instances_Mutable()[index].instanceName = instance.instanceName;
            target->Instances_Mutable()[index].fieldValues = instance.fieldValues;
            if (live)
                target->ApplyStoredFieldValues(index, *ctx.scriptRegistry);
        }
    }

    EnsureEntityHierarchyOrder(ctx, copy, ComputeNextSiblingOrder(ctx, GetEntityParent(ctx, copy)));
    return copy;
}

static void AttachCopiedChild(EditorFrameContext& ctx, EntityID child, EntityID parent)
{
    if (!child.IsValid() || !parent.IsValid() ||
        !ctx.world.IsAlive(child) || !ctx.world.IsAlive(parent))
        return;

    if (ctx.world.Has<ParentComponent>(child))
        ctx.world.Remove<ParentComponent>(child);
    ctx.world.Add<ParentComponent>(child, ParentComponent{parent});

    if (auto* children = ctx.world.Get<ChildrenComponent>(parent))
        children->Add(child);
    else
        ctx.world.Add<ChildrenComponent>(parent).Add(child);

    EnsureEntityHierarchyOrder(ctx, child, ComputeNextSiblingOrder(ctx, parent));
}

static EntityID DuplicateEntityHierarchyRaw(EditorFrameContext& ctx,
                                            EntityID            source,
                                            EntityID            copyParent,
                                            std::vector<std::pair<EntityID, EntityID>>& copies,
                                            bool preservePrefabNames = false)
{
    const EntityID copy = DuplicateEntitySelfRaw(ctx, source);
    if (!copy.IsValid())
        return NULL_ENTITY;
    copies.emplace_back(source, copy);
    preservePrefabNames |= ctx.world.Has<PrefabInstanceComponent>(source);
    if (copyParent.IsValid() && preservePrefabNames)
        if (const auto* name = ctx.world.Get<NameComponent>(source))
            ctx.world.Get<NameComponent>(copy)->name = name->name;

    if (copyParent.IsValid())
    {
        AttachCopiedChild(ctx, copy, copyParent);
    }
    else if (const auto* sourceParent = ctx.world.Get<ParentComponent>(source))
    {
        const EntityID originalParent = sourceParent->parent;
        if (originalParent.IsValid() && ctx.world.IsAlive(originalParent))
            AttachCopiedChild(ctx, copy, originalParent);
    }

    std::vector<EntityID> children;
    if (const auto* cc = ctx.world.Get<ChildrenComponent>(source))
        children = cc->children;

    ctx.world.ForEachAlive([&](EntityID candidate)
    {
        if (candidate == source)
            return;

        const auto* pc = ctx.world.Get<ParentComponent>(candidate);
        if (!pc || pc->parent != source)
            return;

        if (std::find(children.begin(), children.end(), candidate) == children.end())
            children.push_back(candidate);
    });

    std::sort(children.begin(), children.end(), [&](EntityID a, EntityID b) {
        const uint32_t orderA = GetEntityHierarchyOrder(ctx, a);
        const uint32_t orderB = GetEntityHierarchyOrder(ctx, b);
        if (orderA != orderB)
            return orderA < orderB;
        return a.value < b.value;
    });

    for (EntityID child : children)
        DuplicateEntityHierarchyRaw(ctx, child, copy, copies, preservePrefabNames);

    return copy;
}

static void DestroyEntityHierarchyRaw(EditorFrameContext& ctx, EntityID entity)
{
    if (!entity.IsValid() || !ctx.world.IsAlive(entity))
        return;

    std::vector<EntityID> children;
    if (const auto* cc = ctx.world.Get<ChildrenComponent>(entity))
        children = cc->children;

    ctx.world.ForEachAlive([&](EntityID candidate)
    {
        if (candidate == entity)
            return;

        const auto* pc = ctx.world.Get<ParentComponent>(candidate);
        if (!pc || pc->parent != entity)
            return;

        if (std::find(children.begin(), children.end(), candidate) == children.end())
            children.push_back(candidate);
    });

    for (EntityID child : children)
        DestroyEntityHierarchyRaw(ctx, child);

    if (const auto* pc = ctx.world.Get<ParentComponent>(entity))
    {
        const EntityID parent = pc->parent;
        if (parent.IsValid() && ctx.world.IsAlive(parent))
        {
            if (auto* parentChildren = ctx.world.Get<ChildrenComponent>(parent))
                parentChildren->Remove(entity);
        }
    }

    ctx.world.DestroyEntity(entity);
}

} // namespace

EditorEntityId GetStableEditorEntityId(EditorFrameContext& ctx, EntityID entity)
{
    if (!ctx.world.IsAlive(entity))
        return 0;
    if (const auto it = ctx.state.stableEntityIds.find(entity.value); it != ctx.state.stableEntityIds.end())
        return it->second;
    const EditorEntityId id = ctx.state.nextStableEntityId++;
    BindEditorEntity(ctx.state, entity, id);
    return id;
}

EntityID ResolveEditorEntity(const EditorFrameContext& ctx, EditorEntityId id)
{
    const auto it = ctx.state.entitiesByStableId.find(id);
    return it != ctx.state.entitiesByStableId.end() && ctx.world.IsAlive(it->second)
        ? it->second : NULL_ENTITY;
}

void ClearEditorHistory(EditorState& state)
{
    EditorHistoryState& history = GetEditorHistory(state);
    if (!history.applyingHistory)
        history = EditorHistoryState{};
}

bool CanUndoEditorHistory(EditorState& state)
{
    const auto& history = GetEditorHistory(state);
    return !history.applyingHistory && !history.openTransaction && !history.undoStack.empty();
}

bool CanRedoEditorHistory(EditorState& state)
{
    const auto& history = GetEditorHistory(state);
    return !history.applyingHistory && !history.openTransaction && !history.redoStack.empty();
}

std::string GetUndoEditorHistoryLabel(EditorState& state)
{
    EditorHistoryState& history = GetEditorHistory(state);
    return history.undoStack.empty() ? "" : Translate(history.undoStack.back().label.c_str());
}

std::string GetRedoEditorHistoryLabel(EditorState& state)
{
    EditorHistoryState& history = GetEditorHistory(state);
    return history.redoStack.empty() ? "" : Translate(history.redoStack.back().label.c_str());
}

bool BeginEditorTransaction(EditorFrameContext& ctx, const char* label)
{
    EditorHistoryState& history = GetEditorHistory(ctx.state);
    if (history.applyingHistory || history.openTransaction)
        return false;
    EditorHistoryEntry& entry = history.openTransaction.emplace();
    entry.label = label ? label : "Editor action";
    entry.selection = SelectionChange{CaptureEditorSelection(ctx), {}};
    return true;
}

bool AddEditorTransactionOperation(EditorFrameContext& ctx, EditorHistoryOperation operation)
{
    EditorHistoryState& history = GetEditorHistory(ctx.state);
    if (!history.openTransaction || !operation.undo || !operation.redo)
        return false;
    history.openTransaction->operations.emplace_back(std::move(operation));
    return true;
}

bool CommitEditorTransaction(EditorFrameContext& ctx)
{
    EditorHistoryState& history = GetEditorHistory(ctx.state);
    if (!history.openTransaction)
        return false;
    CommitPendingComponentEdits(ctx);
    EditorHistoryEntry entry = std::move(*history.openTransaction);
    history.openTransaction.reset();
    if (entry.operations.empty())
        return false;
    entry.selection->after = CaptureEditorSelection(ctx);
    PushHistoryEntry(history, std::move(entry));
    return true;
}

bool EditComponents(EditorFrameContext& ctx, const char* label,
                    std::span<const EditorComponentTarget> targets,
                    const std::function<void()>& mutation)
{
    if (GetEditorHistory(ctx.state).applyingHistory)
    {
        mutation();
        return true;
    }
    CommitPendingComponentEdits(ctx);
    auto edit = CaptureComponentEdit(ctx, label, targets);
    mutation();
    return FinishComponentEdit(ctx, std::move(edit));
}

void BeginPendingComponentEdit(EditorFrameContext& ctx, const std::string& key, const char* label,
                               EntityID entity, uint32_t typeId, const void* before)
{
    auto& history = GetEditorHistory(ctx.state);
    if (history.applyingHistory || FindPendingComponentEdit(history, key) != history.pendingComponentEdits.end())
        return;
    const EditorComponentTarget target{entity, typeId};
    history.pendingComponentEdits.emplace_back(key, CaptureComponentEdit(ctx, label, {&target, 1}, before));
}

void BeginPendingSelectionComponentEdit(EditorFrameContext& ctx, const std::string& key,
                                        const char* label, uint32_t typeId)
{
    auto& history = GetEditorHistory(ctx.state);
    if (history.applyingHistory || FindPendingComponentEdit(history, key) != history.pendingComponentEdits.end())
        return;
    std::vector<EditorComponentTarget> targets;
    for (const EntityID entity : GetSelectedEntities(ctx))
        targets.push_back({entity, typeId});
    history.pendingComponentEdits.emplace_back(key, CaptureComponentEdit(ctx, label, targets));
}

bool CommitPendingComponentEdit(EditorFrameContext& ctx, const std::string& key)
{
    auto& history = GetEditorHistory(ctx.state);
    const auto it = FindPendingComponentEdit(history, key);
    if (history.applyingHistory || it == history.pendingComponentEdits.end())
        return false;
    auto edit = std::move(it->second);
    history.pendingComponentEdits.erase(it);
    return FinishComponentEdit(ctx, std::move(edit));
}

void CommitPendingComponentEdits(EditorFrameContext& ctx)
{
    auto& history = GetEditorHistory(ctx.state);
    if (history.applyingHistory)
        return;
    auto edits = std::move(history.pendingComponentEdits);
    history.pendingComponentEdits.clear();
    for (auto& [key, edit] : edits)
        FinishComponentEdit(ctx, std::move(edit));
}

bool ExecuteSceneMutation(EditorFrameContext& ctx,
                          const char*         label,
                          const std::function<void()>& mutation)
{
    EditorHistoryState& history = GetEditorHistory(ctx.state);
    if (history.applyingHistory)
    {
        mutation();
        return true;
    }

    CommitPendingComponentEdits(ctx);
    SceneSnapshot before = CaptureSceneSnapshot(ctx);
    mutation();
    // Im selben Undo-Schritt unter die Prefab-Wurzel haengen.
    EnforcePrefabModeSingleRoot(ctx);
    SceneSnapshot after = CaptureSceneSnapshot(ctx);
    if (SameSnapshotData(before, after))
        return false;

    PushSnapshotOperation(history, label ? label : "Editor-Aktion", std::move(before), std::move(after));
    return true;
}

bool AssignModelMesh(EditorFrameContext& ctx, EntityID entity, const std::filesystem::path& model)
{
    if (!ctx.world.IsAlive(entity) || model.empty() || !ctx.assetPipeline || ctx.state.prefabModeReadOnly)
        return false;

    const std::string modelPath = ctx.assetPipeline->ToAssetRelative(model);
    if (!IsPortableAssetPath(modelPath))
        return false;

    // Without a mesh component yet (fresh entity), the drop adds one in the
    // same undo step.
    const auto* existing = ctx.world.Get<MeshComponent>(entity);
    const std::string currentPath = existing ? existing->meshAssetPath : std::string{};
    const size_t fragmentStart = currentPath.rfind('#');
    const std::string fragment = fragmentStart == std::string::npos
        ? std::string{} : currentPath.substr(fragmentStart);
    const std::string path = modelPath + (fragment.starts_with("#mesh/") ? fragment : "#mesh/0");
    if (existing && existing->meshAssetPath == path && ctx.registry.meshes.Get(existing->mesh))
        return true;

    const std::span<const EntityID> target{&entity, 1u};
    bool bound = false;
    ExecuteSceneMutation(ctx, "history.assign_mesh", [&] {
        const bool addedMesh = !ctx.world.Has<MeshComponent>(entity);
        const bool addedBounds = !ctx.world.Has<BoundsComponent>(entity);
        if (addedMesh)
            ctx.world.Add<MeshComponent>(entity, MeshComponent{});
        if (addedBounds)
            ctx.world.Add<BoundsComponent>(entity);
        const MeshComponent previous = *ctx.world.Get<MeshComponent>(entity);
        {
            auto* mesh = ctx.world.Get<MeshComponent>(entity);
            mesh->meshAssetPath = path;
            mesh->mesh = MeshHandle::Invalid();
        }
        engine::addons::mesh_renderer::ResolveMeshAssetBindings(
            *ctx.assetPipeline, ctx.registry, ctx.world, target);
        auto* mesh = ctx.world.Get<MeshComponent>(entity);
        bound = mesh && ctx.registry.meshes.Get(mesh->mesh) != nullptr;
        if (!bound)
        {
            if (addedMesh)
                ctx.world.Remove<MeshComponent>(entity);
            else if (mesh)
                *mesh = previous;
            if (addedBounds)
                ctx.world.Remove<BoundsComponent>(entity);
            return;
        }

        ResolveMaterialAssetBindings(ctx, target);
        engine::mesh_renderer::UpdateLocalBoundsForEntity(ctx.world, entity, ctx.registry);
    });
    return bound;
}

std::string MakeEditHistoryKey(EntityID entity, std::string_view field)
{
    return std::to_string(entity.value) + ":" + std::string(field);
}

void BeginPendingSceneEdit(EditorFrameContext& ctx,
                           const std::string&  key,
                           const char*         label)
{
    EditorHistoryState& history = GetEditorHistory(ctx.state);
    if (history.applyingHistory || history.pendingEdits.find(key) != history.pendingEdits.end())
        return;

    history.pendingEdits.emplace(key, PendingSnapshotEdit{
        label ? label : "Editor-Aktion",
        CaptureSceneSnapshot(ctx)
    });
}

void CommitPendingSceneEdit(EditorFrameContext& ctx, const std::string& key)
{
    EditorHistoryState& history = GetEditorHistory(ctx.state);
    if (history.applyingHistory)
        return;
    const auto it = history.pendingEdits.find(key);
    if (it == history.pendingEdits.end())
        return;

    SceneSnapshot after = CaptureSceneSnapshot(ctx);
    if (!SameSnapshotData(it->second.before, after))
        PushSnapshotOperation(history, std::move(it->second.label), std::move(it->second.before), std::move(after));
    history.pendingEdits.erase(it);
}

void RecordEditorValueChange(EditorFrameContext& ctx, const char* label,
                            std::function<void()> undo, std::function<void()> redo)
{
    EditorHistoryState& history = GetEditorHistory(ctx.state);
    if (history.applyingHistory || !undo || !redo)
        return;
    EditorHistoryEntry entry;
    entry.label = label ? label : "Editor action";
    entry.operations.emplace_back(EditorHistoryOperation{
        [undo = std::move(undo)](EditorFrameContext&) { undo(); return true; },
        [redo = std::move(redo)](EditorFrameContext&) { redo(); return true; }
    });
    PushHistoryEntry(history, std::move(entry));
}

namespace {

bool ApplyHistoryOperation(EditorFrameContext& ctx, const HistoryOperation& operation, bool redo,
                           std::optional<serialization::SceneDeserializer>& deserializer)
{
    if (const auto* component = std::get_if<ComponentOperation>(&operation))
    {
        if (!deserializer)
            deserializer.emplace(MakeEditorSceneDeserializer(ctx));
        return ApplyComponentOperation(ctx, *component, redo, *deserializer);
    }
    if (const auto* snapshot = std::get_if<SnapshotOperation>(&operation))
        return RestoreSceneSnapshot(ctx, redo ? snapshot->after : snapshot->before);
    const auto& callbacks = std::get<EditorHistoryOperation>(operation);
    return (redo ? callbacks.redo : callbacks.undo)(ctx);
}

bool ApplyEditorHistory(EditorFrameContext& ctx, bool redo)
{
    EditorHistoryState& history = GetEditorHistory(ctx.state);
    auto& source = redo ? history.redoStack : history.undoStack;
    auto& destination = redo ? history.undoStack : history.redoStack;
    if (history.applyingHistory || history.openTransaction)
        return false;
    CommitPendingComponentEdits(ctx);
    if (source.empty())
        return false;

    struct ResetApplyingHistory
    {
        bool& flag;
        ~ResetApplyingHistory() { flag = false; }
    } reset{history.applyingHistory};
    history.applyingHistory = true;

    EditorHistoryEntry& entry = source.back();
    const auto originalSelection = entry.selection ? CaptureEditorSelection(ctx) : EditorSelection{};
    const std::string originalMessage = ctx.lastFileMessage;
    const size_t count = entry.operations.size();
    std::optional<serialization::SceneDeserializer> deserializer;
    const auto operationAt = [&](size_t offset) -> const HistoryOperation& {
        return entry.operations[redo ? offset : count - offset - 1];
    };
    for (size_t applied = 0; applied < count; ++applied)
    {
        if (ApplyHistoryOperation(ctx, operationAt(applied), redo, deserializer))
            continue;
        while (applied > 0)
        {
            if (!ApplyHistoryOperation(ctx, operationAt(--applied), !redo, deserializer))
                Debug::LogError("Editor history rollback failed for '%s'", entry.label.c_str());
        }
        if (entry.selection)
            RestoreEditorSelection(ctx, originalSelection);
        if (ctx.lastFileMessage == originalMessage)
            ctx.lastFileMessage = std::string(Translate("history.failed_prefix")) + Translate(entry.label.c_str());
        return false;
    }
    if (entry.selection)
        RestoreEditorSelection(ctx, redo ? entry.selection->after : entry.selection->before);
    history.pendingEdits.clear();
    destination.push_back(std::move(entry));
    source.pop_back();
    return true;
}

} // namespace

bool UndoEditorHistory(EditorFrameContext& ctx)
{
    return ApplyEditorHistory(ctx, false);
}

bool RedoEditorHistory(EditorFrameContext& ctx)
{
    return ApplyEditorHistory(ctx, true);
}

uint32_t GetEntityHierarchyOrder(EditorFrameContext& ctx, EntityID entity)
{
    if (const auto* hierarchy = ctx.world.Get<EditorHierarchyComponent>(entity))
        return hierarchy->order;
    return entity.value;
}

EntityID GetEntityParent(EditorFrameContext& ctx, EntityID entity)
{
    if (const auto* parent = ctx.world.Get<ParentComponent>(entity))
        return parent->parent;
    return NULL_ENTITY;
}

bool MoveEntityInSiblingOrder(EditorFrameContext& ctx, EntityID entity, int direction)
{
    if (!entity.IsValid() || !ctx.world.IsAlive(entity))
        return false;

    std::vector<EntityID> siblings = CollectOrderedSiblings(ctx, entity);
    const auto it = std::find(siblings.begin(), siblings.end(), entity);
    if (it == siblings.end())
        return false;

    const ptrdiff_t index = std::distance(siblings.begin(), it);
    const ptrdiff_t otherIndex = index + static_cast<ptrdiff_t>(direction);
    if (otherIndex < 0 || otherIndex >= static_cast<ptrdiff_t>(siblings.size()))
        return false;

    auto* selfHierarchy = ctx.world.Get<EditorHierarchyComponent>(entity);
    auto* otherHierarchy = ctx.world.Get<EditorHierarchyComponent>(siblings[otherIndex]);
    if (!selfHierarchy || !otherHierarchy)
        return false;

    std::swap(selfHierarchy->order, otherHierarchy->order);
    return true;
}

void SetExclusiveMainCamera(EditorFrameContext& ctx, EntityID entity)
{
    SetExclusiveMainCameraInternal(ctx, entity);
}

EntityID CreateEmptyEntity(EditorFrameContext& ctx, const char* name, EntityID parent)
{
    EntityID entity = NULL_ENTITY;
    ExecuteSceneMutation(ctx, "history.create_entity", [&]() {
        entity = ctx.world.CreateEntity();
        ctx.world.Add<NameComponent>(entity, NameComponent{MakeUniqueEntityName(ctx, name ? name : "Entity")});
        ctx.world.Add<TransformComponent>(entity, MakeEditorPlacedEmptyTransform(ctx));
        ctx.world.Add<WorldTransformComponent>(entity);
        ctx.world.Add<ActiveComponent>(entity, ActiveComponent{true});
        PlaceNewEntityInHierarchy(ctx, entity, parent);
    });
    return entity;
}

EntityID CreateFolderEntity(EditorFrameContext& ctx, const char* name, EntityID parent)
{
    EntityID entity = NULL_ENTITY;
    ExecuteSceneMutation(ctx, "history.create_folder", [&]() {
        entity = ctx.world.CreateEntity();
        ctx.world.Add<NameComponent>(entity, NameComponent{MakeUniqueEntityName(ctx, name ? name : "Folder")});
        ctx.world.Add<ActiveComponent>(entity, ActiveComponent{true});
        ctx.world.Add<FolderComponent>(entity);
        PlaceNewEntityInHierarchy(ctx, entity, parent);
        ClearMultiSelection(ctx);
        ctx.state.selectedEntity = entity;
        ctx.state.selectionSource = SelectionSource::Outliner;
    });
    return entity;
}

EntityID CreateCameraEntity(EditorFrameContext& ctx, EntityID parent)
{
    EntityID entity = NULL_ENTITY;
    ExecuteSceneMutation(ctx, "history.create_camera", [&]() {
        entity = CreateCameraEntityRaw(ctx, parent);
    });
    return entity;
}

EntityID CreateLightEntity(EditorFrameContext& ctx, LightType type, EntityID parent)
{
    EntityID entity = NULL_ENTITY;
    ExecuteSceneMutation(ctx, "history.create_light", [&]() {
        entity = CreateLightEntityRaw(ctx, type, parent);
    });
    return entity;
}

EntityID DuplicateEntity(EditorFrameContext& ctx, EntityID source)
{
    EntityID entity = NULL_ENTITY;
    ExecuteSceneMutation(ctx, "history.duplicate_entity", [&]() {
        std::vector<std::pair<EntityID, EntityID>> copies;
        entity = DuplicateEntityHierarchyRaw(ctx, source, NULL_ENTITY, copies);
        engine::addons::prefab::RebindDuplicatedPrefabEntities(ctx.world, copies);
        if (ctx.scriptRegistry) engine::script::ResolveScriptEntityReferences(ctx.world, *ctx.scriptRegistry);
        if (entity.IsValid())
        {
            ClearMultiSelection(ctx);
            ctx.state.selectedEntity = entity;
        }
    });
    return entity;
}

bool ReparentEntity(EditorFrameContext& ctx, EntityID child, EntityID newParent)
{
    bool changed = false;
    ExecuteSceneMutation(ctx, "history.hierarchy", [&]() {
        changed = ReparentEntityRaw(ctx, child, newParent);
    });
    return changed;
}

bool MoveEntityInHierarchy(EditorFrameContext& ctx, EntityID entity, EntityID newParent,
                           EntityID anchor, bool insertAfter)
{
    if (!entity.IsValid() || !ctx.world.IsAlive(entity) || anchor == entity)
        return false;
    if (newParent.IsValid() &&
        (!ctx.world.IsAlive(newParent) || newParent == entity || IsDescendantOf(ctx, newParent, entity)))
        return false;
    // A folder has no transform; under a moving object it would not follow.
    if (ctx.world.Has<FolderComponent>(entity) && newParent.IsValid() && !ctx.world.Has<FolderComponent>(newParent))
        return false;
    if (anchor.IsValid() && (!ctx.world.IsAlive(anchor) || GetEntityParent(ctx, anchor) != newParent))
        anchor = NULL_ENTITY;

    // Skip no-op drops so they do not create an undo entry.
    if (GetEntityParent(ctx, entity) == newParent)
    {
        std::vector<EntityID> siblings = CollectOrderedSiblings(ctx, entity);
        siblings.erase(std::remove(siblings.begin(), siblings.end(), entity), siblings.end());
        auto at = siblings.end();
        if (anchor.IsValid())
        {
            at = std::find(siblings.begin(), siblings.end(), anchor);
            if (at != siblings.end() && insertAfter)
                ++at;
        }
        std::vector<EntityID> current = CollectOrderedSiblings(ctx, entity);
        siblings.insert(at, entity);
        if (siblings == current)
            return false;
    }

    bool changed = false;
    ExecuteSceneMutation(ctx, "history.reorder_hierarchy", [&]() {
        if (GetEntityParent(ctx, entity) != newParent && !ReparentEntityRaw(ctx, entity, newParent))
            return;

        std::vector<EntityID> siblings = CollectOrderedSiblings(ctx, entity);
        siblings.erase(std::remove(siblings.begin(), siblings.end(), entity), siblings.end());
        auto at = siblings.end();
        if (anchor.IsValid())
        {
            at = std::find(siblings.begin(), siblings.end(), anchor);
            if (at != siblings.end() && insertAfter)
                ++at;
        }
        siblings.insert(at, entity);
        for (size_t i = 0; i < siblings.size(); ++i)
            EnsureEntityHierarchyOrder(ctx, siblings[i], static_cast<uint32_t>(i));

        ClearMultiSelection(ctx);
        ctx.state.selectedEntity = entity;
        changed = true;
    });
    return changed;
}

EntityID FindOutermostPrefabInstanceRoot(EditorFrameContext& ctx, EntityID entity)
{
    EntityID root = NULL_ENTITY;
    EntityID cursor = entity;
    for (uint32_t depth = 0u; cursor.IsValid() && ctx.world.IsAlive(cursor) && depth < 1024u; ++depth)
    {
        if (ctx.world.Get<PrefabInstanceComponent>(cursor) != nullptr)
            root = cursor;
        cursor = GetEntityParent(ctx, cursor);
    }
    return root;
}

bool UnpackPrefabInstance(EditorFrameContext& ctx, EntityID entity)
{
    const EntityID root = FindOutermostPrefabInstanceRoot(ctx, entity);
    if (!root.IsValid())
        return false;

    bool changed = false;
    ExecuteSceneMutation(ctx, "history.unpack_prefab", [&]() {
        // Strip the record identities of this instance, but not inside nested
        // prefab instances: those keep their own link.
        std::vector<EntityID> pending{root};
        while (!pending.empty())
        {
            const EntityID current = pending.back();
            pending.pop_back();
            if (current != root && ctx.world.Get<PrefabInstanceComponent>(current) != nullptr)
                continue;
            if (ctx.world.Has<PrefabSourceComponent>(current))
                ctx.world.Remove<PrefabSourceComponent>(current);
            if (const auto* children = ctx.world.Get<ChildrenComponent>(current))
                pending.insert(pending.end(), children->children.begin(), children->children.end());
        }
        ctx.world.Remove<PrefabInstanceComponent>(root);
        changed = true;
    });
    return changed;
}

EntityID CreateColliderChildEntity(EditorFrameContext& ctx, EntityID parent, ColliderChildShape shape)
{
    if (!parent.IsValid() || !ctx.world.IsAlive(parent))
        return NULL_ENTITY;

    EntityID entity = NULL_ENTITY;
    ExecuteSceneMutation(ctx, "history.create_collider_child", [&]() {
        entity = ctx.world.CreateEntity();
        ctx.world.Add<NameComponent>(entity, NameComponent{MakeUniqueEntityName(ctx, "Collider")});
        ctx.world.Add<TransformComponent>(entity);
        ctx.world.Add<WorldTransformComponent>(entity);
        ctx.world.Add<ActiveComponent>(entity, ActiveComponent{true});
        ReparentEntityRaw(ctx, entity, parent);

        // Lokal am Parent-Ursprung — ReparentEntityRaw erhaelt sonst die Weltpose.
        if (auto* transform = ctx.world.Get<TransformComponent>(entity))
        {
            transform->localPosition = {0.f, 0.f, 0.f};
            transform->localRotation = math::Quat::Identity();
            transform->localScale    = {1.f, 1.f, 1.f};
            transform->dirty = true;
            ++transform->localVersion;
        }

        math::Vec3 center{0.f, 0.f, 0.f};
        math::Vec3 extents{0.5f, 0.5f, 0.5f};
        if (const auto* bounds = ctx.world.Get<BoundsComponent>(parent))
        {
            center  = bounds->centerLocal;
            extents = bounds->extentsLocal;
        }
        if (shape == ColliderChildShape::Box)
        {
            OBBComponent obb{};
            obb.centerOffset = center;
            obb.halfExtents  = {std::max(0.001f, extents.x * 0.5f),
                                std::max(0.001f, extents.y * 0.5f),
                                std::max(0.001f, extents.z * 0.5f)};
            obb.showInEditor = true;
            ctx.world.Add<OBBComponent>(entity, obb);
        }
        else
        {
            SphereColliderComponent sphere{};
            sphere.centerOffset = center;
            sphere.radius       = std::max(0.001f, std::max({extents.x, extents.y, extents.z}) * 0.5f);
            sphere.showInEditor = true;
            ctx.world.Add<SphereColliderComponent>(entity, sphere);
        }
        ctx.state.selectedEntity = entity;
    });
    return entity;
}

bool DestroySelectedEntityHierarchies(EditorFrameContext& ctx)
{
    const auto selection = GetSelectedEntities(ctx);
    if (selection.empty())
        return false;

    return ExecuteSceneMutation(ctx, selection.size() == 1 ? "history.delete_entity" : "history.delete_entities", [&]() {
        for (EntityID entity : selection)
            DestroyEntityHierarchyRaw(ctx, entity);
        ClearMultiSelection(ctx);
    });
}

// =============================================================================
// Multi-Selektion
// =============================================================================

namespace {

static void UpdateMultiSelectionPivot(EditorFrameContext& ctx)
{
    EditorState& st = ctx.state;
    if (st.multiSelection.empty())
    {
        st.multiSelectionPivot = {0.f, 0.f, 0.f};
        return;
    }

    math::Vec3 center{0.f, 0.f, 0.f};
    uint32_t   cnt = 0;
    for (EntityID e : st.multiSelection)
    {
        if (!e.IsValid() || !ctx.world.IsAlive(e)) continue;
        if (const auto* wtc = ctx.world.Get<WorldTransformComponent>(e))
            center = center + wtc->position;
        ++cnt;
    }
    if (cnt > 0)
        center = center * (1.f / static_cast<float>(cnt));

    st.multiSelectionPivot = center;
}

} // anonymous namespace

std::vector<EntityID> GetSelectedEntities(const EditorFrameContext& ctx)
{
    const EntityID active = ctx.state.selectedEntity;
    std::vector<EntityID> selection;
    selection.reserve(ctx.state.multiSelection.size() + 1);
    if (active.IsValid() && ctx.world.IsAlive(active))
        selection.push_back(active);
    for (EntityID entity : ctx.state.multiSelection)
        if (entity != active && entity.IsValid() && ctx.world.IsAlive(entity))
            selection.push_back(entity);
    return selection;
}

bool IsEntityInMultiSelection(const EditorState& state, EntityID id)
{
    for (EntityID e : state.multiSelection)
        if (e == id) return true;
    return false;
}

void AddToMultiSelection(EditorFrameContext& ctx, EntityID id)
{
    if (!id.IsValid() || !ctx.world.IsAlive(id)) return;

    EditorState& st = ctx.state;

    // Bestehende Einzel-Selektion in die Multi-Selektion übernehmen
    if (st.multiSelection.empty()
        && st.selectedEntity.IsValid()
        && ctx.world.IsAlive(st.selectedEntity)
        && st.selectedEntity != id)
    {
        st.multiSelection.push_back(st.selectedEntity);
    }

    // Entity togglen (hinzufügen oder entfernen)
    for (size_t i = 0; i < st.multiSelection.size(); ++i)
    {
        if (st.multiSelection[i] == id)
        {
            st.multiSelection.erase(st.multiSelection.begin() + i);
            UpdateMultiSelectionPivot(ctx);
            return;
        }
    }
    st.multiSelection.push_back(id);
    UpdateMultiSelectionPivot(ctx);
}

void ClearMultiSelection(EditorFrameContext& ctx)
{
    ctx.state.multiSelection.clear();
    ctx.state.multiSelectionDragStartPositions.clear();
    ctx.state.multiSelectionDragStartRotations.clear();
    ctx.state.multiSelectionDragStartScales.clear();
    ctx.state.multiSelectionPivot = {0.f, 0.f, 0.f};
    ctx.state.selectedEntity      = NULL_ENTITY;
}

// ── Prefab-Modus (wie Unity) ─────────────────────────────────────────────────
// Die Szene wird als Schnappschuss beiseitegelegt, die Welt zeigt nur das
// Prefab. Hierarchie, Inspector, Gizmos und Viewport arbeiten wie gewohnt.
// Verlassen speichert automatisch und stellt die Szene wieder her.

namespace {

struct PrefabModeStash
{
    SceneSnapshot       scene;
    EditorCameraState   camera;
    EditorHistoryState  history;
    EditorEntityId      root = 0;
};

std::unordered_map<EditorState*, PrefabModeStash> g_prefabModeStash;

void ClearSceneEntities(EditorFrameContext& ctx)
{
    std::vector<EntityID> entities;
    ctx.world.ForEachAlive([&](EntityID id) { entities.push_back(id); });
    for (EntityID id : entities)
        ctx.world.DestroyEntity(id);
}

// Oberste Objekte des Prefabs (ohne Editor-Hilfsobjekte).
std::vector<EntityID> PrefabModeRoots(EditorFrameContext& ctx)
{
    std::vector<EntityID> roots;
    ctx.world.ForEachAlive([&](EntityID id) {
        if (IsEditorRuntimeEntity(ctx, id) || GetEntityParent(ctx, id).IsValid())
            return;
        roots.push_back(id);
    });
    return roots;
}

void EnsureHierarchyOrderRecursive(EditorFrameContext& ctx, EntityID entity, uint32_t order)
{
    EnsureEntityHierarchyOrder(ctx, entity, order);
    if (const auto* children = ctx.world.Get<ChildrenComponent>(entity))
    {
        uint32_t childOrder = 0u;
        for (const EntityID child : children->children)
        {
            if (ctx.world.IsAlive(child))
                EnsureHierarchyOrderRecursive(ctx, child, childOrder++);
        }
    }
}

} // namespace

bool IsInPrefabMode(const EditorState& state)
{
    return !state.prefabModePath.empty();
}

namespace {

// Kamera so setzen, dass alle Meshes ins Bild passen (ohne Meshes: kleiner Abstand).
void FrameIsolatedCamera(EditorFrameContext& ctx)
{
    math::Vec3 minP{1e30f, 1e30f, 1e30f};
    math::Vec3 maxP{-1e30f, -1e30f, -1e30f};
    bool any = false;
    ctx.world.View<MeshComponent>([&](EntityID, MeshComponent& mesh) {
        const assets::MeshAsset* asset = ctx.registry.meshes.Get(mesh.mesh);
        if (!asset)
            return;
        math::Vec3 a, b;
        asset->ComputeBounds(a, b);
        minP = math::Vec3{std::min(minP.x, a.x), std::min(minP.y, a.y), std::min(minP.z, a.z)};
        maxP = math::Vec3{std::max(maxP.x, b.x), std::max(maxP.y, b.y), std::max(maxP.z, b.z)};
        any = true;
    });
    math::Vec3 center{0.f, 0.75f, 0.f};
    float radius = 2.f;
    if (any)
    {
        center = (minP + maxP) * 0.5f;
        const math::Vec3 extent = (maxP - minP) * 0.5f;
        radius = std::max(0.25f, std::sqrt(extent.x * extent.x + extent.y * extent.y + extent.z * extent.z));
    }
    // Leicht von oben, Abstand passend zum 60-Grad-Bildwinkel.
    const float distance = radius * 2.2f;
    ctx.state.editorCamera.position = center + math::Vec3{0.f, distance * 0.25f, distance};
    ctx.state.editorCamera.pitchDeg = -14.f;
    ctx.state.editorCamera.yawDeg = 0.f;
}

// Gemeinsamer Einstieg fuer Prefab-Modus und Modell-Ansicht.
bool EnterIsolatedMode(EditorFrameContext& ctx, const std::filesystem::path& path,
                       const engine::addons::prefab::PrefabAsset& prefab, bool readOnly);

} // namespace

bool EnterPrefabMode(EditorFrameContext& ctx, const std::filesystem::path& prefabPath)
{
    // Play keeps the edit scene stashed and runs the game in ctx.world. Prefab mode
    // would stash the game world on top; Stop then restores the edit scene into the
    // prefab mode, which merges the whole scene into the prefab root.
    if (IsPlaySessionActive(ctx.state))
    {
        ctx.lastFileMessage = Translate("prefab.blocked_during_play");
        return false;
    }
    if (IsInPrefabMode(ctx.state))
    {
        if (PathFromUtf8(ctx.state.prefabModePath) == prefabPath)
            return true;
        if (!ExitPrefabMode(ctx))
            return false;
    }

    engine::addons::prefab::PrefabAsset prefab;
    std::string error;
    if (!engine::addons::prefab::LoadPrefabFromFile(prefabPath, ctx.registry, prefab, &error))
    {
        ctx.lastFileMessage = Translate("prefab.load_failed_prefix") + error;
        return false;
    }
    return EnterIsolatedMode(ctx, prefabPath, prefab, false);
}

bool EnterModelView(EditorFrameContext& ctx, const std::filesystem::path& modelPath,
                    const engine::addons::prefab::PrefabAsset& modelPrefab)
{
    // Play keeps the edit scene stashed and runs the game in ctx.world. Prefab mode
    // would stash the game world on top; Stop then restores the edit scene into the
    // prefab mode, which merges the whole scene into the prefab root.
    if (IsPlaySessionActive(ctx.state))
    {
        ctx.lastFileMessage = Translate("prefab.blocked_during_play");
        return false;
    }
    if (IsInPrefabMode(ctx.state))
    {
        if (PathFromUtf8(ctx.state.prefabModePath) == modelPath)
            return true;
        if (!ExitPrefabMode(ctx))
            return false;
    }
    return EnterIsolatedMode(ctx, modelPath, modelPrefab, true);
}

EntityID PrefabModeRootEntity(EditorFrameContext& ctx)
{
    const std::vector<EntityID> roots = PrefabModeRoots(ctx);
    return roots.size() == 1u ? roots.front() : NULL_ENTITY;
}

bool EnforcePrefabModeSingleRoot(EditorFrameContext& ctx)
{
    if (!IsInPrefabMode(ctx.state) || ctx.state.prefabModeReadOnly)
        return false;
    const auto stashIt = g_prefabModeStash.find(&ctx.state);
    if (stashIt == g_prefabModeStash.end())
        return false;
    const EntityID knownRoot = ResolveEditorEntity(ctx, stashIt->second.root);

    const std::vector<EntityID> roots = PrefabModeRoots(ctx);
    if (roots.size() <= 1u)
    {
        if (roots.size() == 1u)
            stashIt->second.root = GetStableEditorEntityId(ctx, roots.front());
        return false;
    }

    // Undo kann Entities neu anlegen; dann gilt das oberste in der Hierarchie.
    EntityID root = NULL_ENTITY;
    if (std::find(roots.begin(), roots.end(), knownRoot) != roots.end())
        root = knownRoot;
    else
    {
        uint32_t bestOrder = std::numeric_limits<uint32_t>::max();
        for (const EntityID candidate : roots)
        {
            const auto* order = ctx.world.Get<EditorHierarchyComponent>(candidate);
            const uint32_t value = order ? order->order : std::numeric_limits<uint32_t>::max() - 1u;
            if (!root.IsValid() || value < bestOrder)
            {
                root = candidate;
                bestOrder = value;
            }
        }
    }

    bool changed = false;
    for (const EntityID other : roots)
        if (other != root)
            changed |= ReparentEntityRaw(ctx, other, root);
    stashIt->second.root = GetStableEditorEntityId(ctx, root);
    return changed;
}

namespace {

bool EnterIsolatedMode(EditorFrameContext& ctx, const std::filesystem::path& prefabPath,
                       const engine::addons::prefab::PrefabAsset& prefab, bool readOnly)
{
    CommitPendingComponentEdits(ctx);
    CommitEditorTransaction(ctx);
    // Szene, Kamera und Rueckgaengig-Liste beiseitelegen.
    EditorHistoryState& history = GetEditorHistory(ctx.state);
    PrefabModeStash& stash = g_prefabModeStash[&ctx.state];
    stash.scene = CaptureSceneSnapshot(ctx);
    stash.camera = ctx.state.editorCamera;
    stash.history = std::move(history);
    history = EditorHistoryState{};

    ClearSceneEntities(ctx);
    engine::addons::prefab::PrefabInstantiateOptions options{};
    options.scriptRegistry = ctx.scriptRegistry;
    const engine::addons::prefab::PrefabInstance instance =
        engine::addons::prefab::InstantiatePrefab(ctx.world, prefab, options);
    if (instance.IsValid())
    {
        EnsureHierarchyOrderRecursive(ctx, instance.root, 0u);
        if (ctx.assetPipeline)
        {
            engine::addons::mesh_renderer::ResolveMeshAssetBindings(*ctx.assetPipeline, ctx.registry, ctx.world);
            ResolveMaterialAssetBindings(ctx);
        }
    }

    ctx.state.prefabModePath = PathToUtf8(prefabPath);
    ctx.state.prefabModeReadOnly = readOnly;
    stash.root = GetStableEditorEntityId(ctx, instance.root);
    ClearMultiSelection(ctx);
    ctx.state.selectedEntity = instance.root;
    FrameIsolatedCamera(ctx);
    ctx.lastFileMessage = (readOnly ? Translate("prefab.model_view_prefix") : Translate("prefab.mode_prefix")) + PathToUtf8(prefabPath.filename());
    if (ctx.syncSceneState)
        ctx.syncSceneState();
    return true;
}

} // namespace

bool SavePrefabMode(EditorFrameContext& ctx)
{
    if (!IsInPrefabMode(ctx.state))
        return false;
    if (ctx.state.prefabModeReadOnly)
    {
        ctx.lastFileMessage = Translate("prefab.model_read_only");
        return false;
    }
    const std::vector<EntityID> roots = PrefabModeRoots(ctx);
    if (roots.size() != 1u)
    {
        ctx.lastFileMessage = roots.empty()
            ? Translate("prefab.no_root")
            : Translate("prefab.multiple_roots");
        return false;
    }
    std::string name = "Prefab";
    if (const auto* nc = ctx.world.Get<NameComponent>(roots.front()); nc && !nc->name.empty())
        name = nc->name;
    const engine::addons::prefab::PrefabAsset prefab = engine::addons::prefab::BuildPrefabFromWorldEntity(
        ctx.world, roots.front(), name, &ctx.registry, ctx.scriptRegistry);
    std::string error;
    if (prefab.Empty() ||
        !engine::addons::prefab::SavePrefabToFile(prefab, PathFromUtf8(ctx.state.prefabModePath), &error))
    {
        ctx.lastFileMessage = Translate("prefab.save_failed_prefix") + (error.empty() ? std::string("leer") : error);
        return false;
    }
    // Newly added nodes need their source IDs stamped immediately, including
    // repeated saves during the same isolated editing session.
    std::vector<EntityID> stack{roots.front()};
    size_t recordIndex = 0u;
    while (!stack.empty() && recordIndex < prefab.records.size())
    {
        const auto entity = stack.back(); stack.pop_back();
        PrefabSourceComponent identity{prefab.records[recordIndex++].id};
        if (auto* source = ctx.world.Get<PrefabSourceComponent>(entity)) *source = std::move(identity);
        else ctx.world.Add<PrefabSourceComponent>(entity, std::move(identity));
        if (const auto* children = ctx.world.Get<ChildrenComponent>(entity))
            for (auto it = children->children.rbegin(); it != children->children.rend(); ++it) stack.push_back(*it);
    }
    ctx.lastFileMessage = Translate("prefab.saved_prefix") + PathToUtf8(PathFromUtf8(ctx.state.prefabModePath).filename());
    return true;
}

namespace {

struct PlaySessionStash
{
    SceneSnapshot      scene;
    EditorHistoryState history;
};

std::unordered_map<EditorState*, PlaySessionStash> g_playSessionStash;

} // namespace

EditorFrameContext::EditorFrameContext(ecs::World& w, MaterialSystem& m,
                                       assets::AssetRegistry& r, EditorState& s)
    : world(w), materials(m), registry(r), state(s)
{
    world.SetEntityDestroyedCallback([&s](EntityID entity) {
        const auto it = s.stableEntityIds.find(entity.value);
        if (it == s.stableEntityIds.end())
            return;
        s.entitiesByStableId.erase(it->second);
        s.stableEntityIds.erase(it);
    });
}

EditorFrameContext::~EditorFrameContext()
{
    world.SetEntityDestroyedCallback({});
    state.stableEntityIds.clear();
    state.entitiesByStableId.clear();
    g_editorHistory.erase(&state);
    g_playSessionStash.erase(&state);
    g_prefabModeStash.erase(&state);
}

bool BeginPlaySession(EditorFrameContext& ctx)
{
    if (IsPlaySessionActive(ctx.state))
        return true;
    CommitPendingComponentEdits(ctx);
    CommitEditorTransaction(ctx);
    PlaySessionStash stash;
    stash.scene = CaptureSceneSnapshot(ctx);
    stash.history = std::move(GetEditorHistory(ctx.state));
    GetEditorHistory(ctx.state) = EditorHistoryState{};
    g_playSessionStash[&ctx.state] = std::move(stash);
    return true;
}

bool EndPlaySession(EditorFrameContext& ctx)
{
    const auto it = g_playSessionStash.find(&ctx.state);
    if (it == g_playSessionStash.end())
        return true;
    PlaySessionStash stash = std::move(it->second);
    g_playSessionStash.erase(it);
    // Safety net: a prefab mode opened during play holds the game world, not the
    // edit scene. Drop it without saving so the edit scene is restored as a scene.
    if (IsInPrefabMode(ctx.state))
    {
        if (const auto prefabIt = g_prefabModeStash.find(&ctx.state); prefabIt != g_prefabModeStash.end())
        {
            ctx.state.editorCamera = prefabIt->second.camera;
            g_prefabModeStash.erase(prefabIt);
        }
        ctx.state.prefabModePath.clear();
        ctx.state.prefabModeReadOnly = false;
    }
    const bool restored = RestoreSceneSnapshot(ctx, stash.scene);
    GetEditorHistory(ctx.state) = std::move(stash.history);
    return restored;
}

bool IsPlaySessionActive(const EditorState& state)
{
    return g_playSessionStash.count(const_cast<EditorState*>(&state)) != 0u;
}

bool ExitPrefabMode(EditorFrameContext& ctx)
{
    if (!IsInPrefabMode(ctx.state))
        return true;
    // Modell-Ansicht: nur angesehen, nichts zu speichern.
    if (!ctx.state.prefabModeReadOnly && !SavePrefabMode(ctx))
        return false;

    const auto changedPath = ctx.state.prefabModeReadOnly ? std::filesystem::path{} : PathFromUtf8(ctx.state.prefabModePath);
    PrefabModeStash stash = std::move(g_prefabModeStash[&ctx.state]);
    g_prefabModeStash.erase(&ctx.state);
    ctx.state.prefabModePath.clear();
    ctx.state.prefabModeReadOnly = false;

    RestoreSceneSnapshot(ctx, stash.scene);
    ctx.state.editorCamera = stash.camera;
    GetEditorHistory(ctx.state) = std::move(stash.history);
    // Instanzen dieses Prefabs in der Szene aktualisieren.
    engine::addons::editor::ResolvePrefabBindings(ctx, changedPath);
    return true;
}

} // namespace engine::renderer::addons::editor
