#include "addons/editor/EditorCommands.hpp"

#include "addons/editor/EditorSerialization.hpp"
#include "addons/camera/CameraComponents.hpp"
#include "addons/camera/CameraSerialization.hpp"
#include "addons/mesh_renderer/MeshRendererComponents.hpp"
#include "addons/lighting/LightingSerialization.hpp"
#include "addons/mesh_renderer/MeshRendererSerialization.hpp"
#include "addons/script/ScriptSerialization.hpp"
#include "ecs/Components.hpp"
#include "serialization/SceneSerializer.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace engine::renderer::addons::editor {
namespace {

struct SceneSnapshot
{
    std::string sceneJson;
    uint32_t    selectedEntityValue = NULL_ENTITY.value;
};

struct SnapshotCommand
{
    std::string   label;
    SceneSnapshot before;
    SceneSnapshot after;
};

struct PendingSnapshotEdit
{
    std::string   label;
    SceneSnapshot before;
};

struct EditorHistoryState
{
    std::vector<SnapshotCommand>                  undoStack;
    std::vector<SnapshotCommand>                  redoStack;
    std::unordered_map<std::string, PendingSnapshotEdit> pendingEdits;
    bool                                          applyingHistory = false;
};

static std::unordered_map<EditorState*, EditorHistoryState> g_editorHistory;
constexpr size_t kMaxHistoryEntries = 64u;

static EditorHistoryState& GetEditorHistory(EditorState& state)
{
    return g_editorHistory[&state];
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
    if (ctx.world.Has<CameraComponent>(id) &&
        !ctx.world.Has<EditorHierarchyComponent>(id))
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

static SceneSnapshot CaptureSceneSnapshot(EditorFrameContext& ctx)
{
    serialization::SceneSerializer serializer(ctx.world);
    serializer.RegisterDefaultHandlers();
    engine::addons::camera::RegisterCameraSerializationHandlers(serializer);
    engine::addons::lighting::RegisterLightingSerializationHandlers(serializer);
    engine::addons::mesh_renderer::RegisterMeshRendererSerializationHandlers(serializer);
    engine::script::RegisterScriptSerializationHandlers(serializer);
    RegisterEditorSerializationHandlers(serializer);
    serializer.SetEntityFilter([&ctx](EntityID id) -> bool {
        return !IsEditorRuntimeEntity(ctx, id);
    });

    SceneSnapshot snapshot;
    snapshot.sceneJson = serializer.SerializeToJson("EditorScene");
    snapshot.selectedEntityValue = ctx.state.selectedEntity.value;
    return snapshot;
}

static bool RestoreSceneSnapshot(EditorFrameContext& ctx, const SceneSnapshot& snapshot)
{
    std::vector<EntityID> entities;
    ctx.world.ForEachAlive([&](EntityID id) {
        entities.push_back(id);
    });
    for (EntityID id : entities)
        ctx.world.DestroyEntity(id);

    serialization::SceneDeserializer deserializer(ctx.world);
    deserializer.RegisterDefaultHandlers();
    engine::addons::camera::RegisterCameraDeserializationHandlers(deserializer);
    engine::addons::lighting::RegisterLightingDeserializationHandlers(deserializer);
    engine::addons::mesh_renderer::RegisterMeshRendererDeserializationHandlers(deserializer);
    engine::script::RegisterScriptDeserializationHandlers(deserializer, ctx.scriptRegistry);
    RegisterEditorDeserializationHandlers(deserializer);

    const serialization::DeserializeResult result = deserializer.DeserializeFromJson(snapshot.sceneJson);
    if (!result.success)
    {
        ctx.lastFileMessage = "Undo/Redo fehlgeschlagen: " + result.error;
        return false;
    }
    if (ctx.scriptRegistry)
        engine::script::ResolveScriptEntityReferences(ctx.world, *ctx.scriptRegistry);

    DestroyEditorRuntimeEntities(ctx);

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

    ctx.state.multiSelection.clear();
    ctx.state.multiSelectionPivot = {0.f, 0.f, 0.f};
    ctx.state.multiSelectionDragStartPositions.clear();
    ctx.state.multiSelectionDragStartRotations.clear();
    ctx.state.multiSelectionDragStartScales.clear();
    ctx.state.selectedEntity = NULL_ENTITY;
    if (snapshot.selectedEntityValue != NULL_ENTITY.value)
    {
        if (const auto it = result.entityRemap.find(snapshot.selectedEntityValue);
            it != result.entityRemap.end())
        {
            ctx.state.selectedEntity = it->second;
        }
    }

    ctx.state.rotationEditActive = false;
    ctx.state.rotationEditInitialized = false;
    ctx.state.deleteConfirmOpen = false;
    if (ctx.syncSceneState)
        ctx.syncSceneState();
    return true;
}

static void PushHistoryCommand(EditorFrameContext& ctx,
                               std::string        label,
                               SceneSnapshot      before,
                               SceneSnapshot      after)
{
    EditorHistoryState& history = GetEditorHistory(ctx.state);
    history.redoStack.clear();
    history.undoStack.push_back(SnapshotCommand{
        std::move(label),
        std::move(before),
        std::move(after)
    });
    if (history.undoStack.size() > kMaxHistoryEntries)
        history.undoStack.erase(history.undoStack.begin());
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

static EntityID CreateCameraEntityRaw(EditorFrameContext& ctx)
{
    const EntityID entity = ctx.world.CreateEntity();
    ctx.world.Add<NameComponent>(entity, NameComponent{MakeUniqueEntityName(ctx, "Camera")});
    EnsureEntityHierarchyOrder(ctx, entity, ComputeNextSiblingOrder(ctx, NULL_ENTITY));
    ctx.world.Add<TransformComponent>(entity, MakeEditorPlacedTransform(ctx));
    ctx.world.Add<WorldTransformComponent>(entity);
    ctx.world.Add<ActiveComponent>(entity, ActiveComponent{true});

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

static EntityID CreateLightEntityRaw(EditorFrameContext& ctx, LightType type)
{
    const EntityID entity = ctx.world.CreateEntity();
    ctx.world.Add<NameComponent>(entity, NameComponent{MakeUniqueEntityName(ctx, LightTypeName(type))});
    EnsureEntityHierarchyOrder(ctx, entity, ComputeNextSiblingOrder(ctx, NULL_ENTITY));
    ctx.world.Add<TransformComponent>(entity, MakeEditorPlacedTransform(ctx));
    ctx.world.Add<WorldTransformComponent>(entity);
    ctx.world.Add<ActiveComponent>(entity, ActiveComponent{true});

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

static EntityID DuplicateEntityRaw(EditorFrameContext& ctx, EntityID source)
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

    if (const auto* p = ctx.world.Get<ParentComponent>(source))
    {
        ctx.world.Add<ParentComponent>(copy, *p);
        if (p->parent.IsValid() && ctx.world.IsAlive(p->parent))
        {
            if (auto* siblings = ctx.world.Get<ChildrenComponent>(p->parent))
                siblings->Add(copy);
            else
                ctx.world.Add<ChildrenComponent>(p->parent).Add(copy);
        }
    }

    EnsureEntityHierarchyOrder(ctx, copy, ComputeNextSiblingOrder(ctx, GetEntityParent(ctx, copy)));
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

void ClearEditorHistory(EditorState& state)
{
    EditorHistoryState& history = GetEditorHistory(state);
    history.undoStack.clear();
    history.redoStack.clear();
    history.pendingEdits.clear();
}

bool CanUndoEditorHistory(EditorState& state)
{
    return !GetEditorHistory(state).undoStack.empty();
}

bool CanRedoEditorHistory(EditorState& state)
{
    return !GetEditorHistory(state).redoStack.empty();
}

std::string GetUndoEditorHistoryLabel(EditorState& state)
{
    EditorHistoryState& history = GetEditorHistory(state);
    return history.undoStack.empty() ? std::string{} : history.undoStack.back().label;
}

std::string GetRedoEditorHistoryLabel(EditorState& state)
{
    EditorHistoryState& history = GetEditorHistory(state);
    return history.redoStack.empty() ? std::string{} : history.redoStack.back().label;
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

    SceneSnapshot before = CaptureSceneSnapshot(ctx);
    mutation();
    SceneSnapshot after = CaptureSceneSnapshot(ctx);
    if (before.sceneJson == after.sceneJson)
        return false;

    PushHistoryCommand(ctx, label ? label : "Editor-Aktion", std::move(before), std::move(after));
    return true;
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
    const auto it = history.pendingEdits.find(key);
    if (it == history.pendingEdits.end())
        return;

    const SceneSnapshot after = CaptureSceneSnapshot(ctx);
    if (it->second.before.sceneJson != after.sceneJson)
        PushHistoryCommand(ctx, it->second.label, std::move(it->second.before), after);
    history.pendingEdits.erase(it);
}

bool UndoEditorHistory(EditorFrameContext& ctx)
{
    EditorHistoryState& history = GetEditorHistory(ctx.state);
    if (history.undoStack.empty())
        return false;

    SnapshotCommand command = std::move(history.undoStack.back());
    history.undoStack.pop_back();
    history.pendingEdits.clear();

    history.applyingHistory = true;
    const bool restored = RestoreSceneSnapshot(ctx, command.before);
    history.applyingHistory = false;
    if (!restored)
        return false;

    history.redoStack.push_back(std::move(command));
    if (history.redoStack.size() > kMaxHistoryEntries)
        history.redoStack.erase(history.redoStack.begin());
    return true;
}

bool RedoEditorHistory(EditorFrameContext& ctx)
{
    EditorHistoryState& history = GetEditorHistory(ctx.state);
    if (history.redoStack.empty())
        return false;

    SnapshotCommand command = std::move(history.redoStack.back());
    history.redoStack.pop_back();
    history.pendingEdits.clear();

    history.applyingHistory = true;
    const bool restored = RestoreSceneSnapshot(ctx, command.after);
    history.applyingHistory = false;
    if (!restored)
        return false;

    history.undoStack.push_back(std::move(command));
    if (history.undoStack.size() > kMaxHistoryEntries)
        history.undoStack.erase(history.undoStack.begin());
    return true;
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

EntityID CreateEmptyEntity(EditorFrameContext& ctx, const char* name)
{
    EntityID entity = NULL_ENTITY;
    ExecuteSceneMutation(ctx, "Entity erstellt", [&]() {
        entity = ctx.world.CreateEntity();
        ctx.world.Add<NameComponent>(entity, NameComponent{MakeUniqueEntityName(ctx, name ? name : "Entity")});
        EnsureEntityHierarchyOrder(ctx, entity, ComputeNextSiblingOrder(ctx, NULL_ENTITY));
        ctx.world.Add<TransformComponent>(entity, MakeEditorPlacedTransform(ctx));
        ctx.world.Add<WorldTransformComponent>(entity);
        ctx.world.Add<ActiveComponent>(entity, ActiveComponent{true});
    });
    return entity;
}

EntityID CreateCameraEntity(EditorFrameContext& ctx)
{
    EntityID entity = NULL_ENTITY;
    ExecuteSceneMutation(ctx, "Camera erstellt", [&]() {
        entity = CreateCameraEntityRaw(ctx);
    });
    return entity;
}

EntityID CreateLightEntity(EditorFrameContext& ctx, LightType type)
{
    EntityID entity = NULL_ENTITY;
    ExecuteSceneMutation(ctx, "Licht erstellt", [&]() {
        entity = CreateLightEntityRaw(ctx, type);
    });
    return entity;
}

EntityID DuplicateEntity(EditorFrameContext& ctx, EntityID source)
{
    EntityID entity = NULL_ENTITY;
    ExecuteSceneMutation(ctx, "Entity dupliziert", [&]() {
        entity = DuplicateEntityRaw(ctx, source);
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
    ExecuteSceneMutation(ctx, "Hierarchie geaendert", [&]() {
        changed = ReparentEntityRaw(ctx, child, newParent);
    });
    return changed;
}

bool DestroyEntityHierarchy(EditorFrameContext& ctx, EntityID entity)
{
    bool changed = false;
    ExecuteSceneMutation(ctx, "Entity geloescht", [&]() {
        if (!entity.IsValid() || !ctx.world.IsAlive(entity))
            return;

        DestroyEntityHierarchyRaw(ctx, entity);
        if (ctx.state.selectedEntity == entity)
            ctx.state.selectedEntity = NULL_ENTITY;
        changed = true;
    });
    return changed;
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

} // namespace engine::renderer::addons::editor
