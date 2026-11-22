#include "KromEntityApi.hpp"

#include "addons/camera/CameraComponents.hpp"
#include "addons/lighting/LightingComponents.hpp"
#include "addons/mesh_renderer/MeshRendererComponents.hpp"
#include "ecs/Components.hpp"
#include "renderer/RenderLayers.hpp"

#include <algorithm>
#include <cmath>
#include <functional>
#include <string>

namespace Krom::internal {

// =============================================================================
// Hierarchy helpers
// =============================================================================

void DestroyEntityRecursive(engine::ecs::World& world, LPENTITY entity)
{
    if (!world.IsAlive(entity))
        return;
    if (const auto* children = world.Get<engine::ChildrenComponent>(entity))
    {
        const auto copy = children->children;
        for (const LPENTITY child : copy)
            DestroyEntityRecursive(world, child);
    }
    DetachEntityFromParent(world, entity);
    world.DestroyEntity(entity);
}

void DetachEntityFromParent(engine::ecs::World& world, LPENTITY entity)
{
    if (!world.IsAlive(entity) || !world.Has<engine::ParentComponent>(entity))
        return;

    const LPENTITY parent = world.Get<engine::ParentComponent>(entity)->parent;
    if (world.IsAlive(parent) && world.Has<engine::ChildrenComponent>(parent))
        world.Get<engine::ChildrenComponent>(parent)->Remove(entity);
    world.Remove<engine::ParentComponent>(entity);
}

// =============================================================================
// Transform helpers (only used by the functions below)
// =============================================================================

static engine::math::Quat QuatFromRotationMatrix(const engine::math::Mat4& m) noexcept
{
    const float trace = m.m[0][0] + m.m[1][1] + m.m[2][2];
    if (trace > 0.f)
    {
        const float s = std::sqrt(trace + 1.f) * 2.f;
        return {(m.m[1][2] - m.m[2][1]) / s,
                (m.m[2][0] - m.m[0][2]) / s,
                (m.m[0][1] - m.m[1][0]) / s,
                0.25f * s};
    }
    if (m.m[0][0] > m.m[1][1] && m.m[0][0] > m.m[2][2])
    {
        const float s = std::sqrt(1.f + m.m[0][0] - m.m[1][1] - m.m[2][2]) * 2.f;
        return {0.25f * s,
                (m.m[1][0] + m.m[0][1]) / s,
                (m.m[2][0] + m.m[0][2]) / s,
                (m.m[1][2] - m.m[2][1]) / s};
    }
    if (m.m[1][1] > m.m[2][2])
    {
        const float s = std::sqrt(1.f + m.m[1][1] - m.m[0][0] - m.m[2][2]) * 2.f;
        return {(m.m[1][0] + m.m[0][1]) / s,
                0.25f * s,
                (m.m[2][1] + m.m[1][2]) / s,
                (m.m[2][0] - m.m[0][2]) / s};
    }

    const float s = std::sqrt(1.f + m.m[2][2] - m.m[0][0] - m.m[1][1]) * 2.f;
    return {(m.m[2][0] + m.m[0][2]) / s,
            (m.m[2][1] + m.m[1][2]) / s,
            0.25f * s,
            (m.m[0][1] - m.m[1][0]) / s};
}

// Current world rotation and position of an entity, also right after a setter
// (before the next TransformSystem::Update()). Fallbacks as before: world
// transform only -> stored value; local transform only -> local value.
static bool ResolveCurrentWorld(const engine::ecs::World* world, LPENTITY entity,
                                engine::math::Vec3& outPosition, engine::math::Quat& outRotation)
{
    if (!world || !world->IsAlive(entity))
        return false;
    const auto* wt = world->Get<engine::WorldTransformComponent>(entity);
    const auto* tr = world->Get<engine::TransformComponent>(entity);
    if (wt && tr)
    {
        const engine::WorldTransformComponent current =
            engine::TransformSystem::ComputeCurrentWorldTransform(*world, entity);
        outPosition = current.position;
        outRotation = current.rotation;
        return true;
    }
    if (wt)
    {
        outPosition = wt->position;
        outRotation = wt->rotation;
        return true;
    }
    if (tr)
    {
        outPosition = tr->localPosition;
        outRotation = tr->localRotation;
        return true;
    }
    return false;
}

static engine::math::Vec3 ResolveEntityPosition(engine::ecs::World* world, LPENTITY entity)
{
    engine::math::Vec3 position{};
    engine::math::Quat rotation = engine::math::Quat::Identity();
    return ResolveCurrentWorld(world, entity, position, rotation) ? position : engine::math::Vec3{};
}

static bool CollectWorldBoundsRecursive(const engine::ecs::World& world,
                                        LPENTITY entity,
                                        engine::math::Vec3& ioMin,
                                        engine::math::Vec3& ioMax,
                                        bool& ioHasBounds)
{
    if (!world.IsAlive(entity))
        return false;

    // Current box, also right after a transform setter (before the frame's
    // transform/bounds update). Reads only.
    engine::math::Vec3 center{};
    engine::math::Vec3 extents{};
    if (engine::BoundsSystem::ComputeCurrentWorldBounds(world, entity, center, extents))
    {
        const engine::math::Vec3 localMin = center - extents;
        const engine::math::Vec3 localMax = center + extents;
        if (!ioHasBounds)
        {
            ioMin = localMin;
            ioMax = localMax;
            ioHasBounds = true;
        }
        else
        {
            ioMin.x = std::min(ioMin.x, localMin.x);
            ioMin.y = std::min(ioMin.y, localMin.y);
            ioMin.z = std::min(ioMin.z, localMin.z);
            ioMax.x = std::max(ioMax.x, localMax.x);
            ioMax.y = std::max(ioMax.y, localMax.y);
            ioMax.z = std::max(ioMax.z, localMax.z);
        }
    }

    if (const auto* children = world.Get<engine::ChildrenComponent>(entity))
    {
        for (const LPENTITY child : children->children)
            CollectWorldBoundsRecursive(world, child, ioMin, ioMax, ioHasBounds);
    }

    return ioHasBounds;
}

struct CapturedTransform
{
    engine::math::Vec3 position{0.0f, 0.0f, 0.0f};
    engine::math::Quat rotation = engine::math::Quat::Identity();
    engine::math::Vec3 scale{1.0f, 1.0f, 1.0f};
};

static CapturedTransform CaptureWorldTransform(const EntityContext& ctx, LPENTITY entity)
{
    CapturedTransform result{};
    if (!ctx.world || !ctx.world->IsAlive(entity))
        return result;

    ctx.transformSystem.Update(*ctx.world);
    if (const auto* worldTransform = ctx.world->Get<engine::WorldTransformComponent>(entity))
    {
        result.position = worldTransform->position;
        result.rotation = worldTransform->rotation;
        result.scale = worldTransform->scale;
        return result;
    }
    if (const auto* transform = ctx.world->Get<engine::TransformComponent>(entity))
    {
        result.position = transform->localPosition;
        result.rotation = transform->localRotation;
        result.scale = transform->localScale;
    }
    return result;
}

static bool IsDescendantOf(const engine::ecs::World& world, LPENTITY entity, LPENTITY possibleAncestor)
{
    LPENTITY cursor = entity;
    uint32_t depth = 0u;
    while (world.IsAlive(cursor) && depth < 1024u)
    {
        if (cursor == possibleAncestor)
            return true;
        const auto* parent = world.Get<engine::ParentComponent>(cursor);
        if (!parent || !parent->parent.IsValid())
            return false;
        cursor = parent->parent;
        ++depth;
    }
    return false;
}

static void ApplyLocalFromWorld(const EntityContext& ctx, LPENTITY entity, const CapturedTransform& worldTransform)
{
    if (!ctx.world || !ctx.world->IsAlive(entity))
        return;

    auto* transform = ctx.world->Get<engine::TransformComponent>(entity);
    if (!transform)
        return;

    const auto* parent = ctx.world->Get<engine::ParentComponent>(entity);
    if (parent && parent->parent.IsValid() && ctx.world->IsAlive(parent->parent))
    {
        ctx.transformSystem.Update(*ctx.world);
        if (const auto* parentWorld = ctx.world->Get<engine::WorldTransformComponent>(parent->parent))
        {
            const engine::math::Quat inverseParentRotation = parentWorld->rotation.Conjugate().Normalized();
            engine::math::Vec3 localPosition = inverseParentRotation.Rotate(worldTransform.position - parentWorld->position);
            if (transform->inheritParentScale)
            {
                localPosition.x = std::abs(parentWorld->scale.x) > engine::math::EPSILON
                    ? localPosition.x / parentWorld->scale.x : localPosition.x;
                localPosition.y = std::abs(parentWorld->scale.y) > engine::math::EPSILON
                    ? localPosition.y / parentWorld->scale.y : localPosition.y;
                localPosition.z = std::abs(parentWorld->scale.z) > engine::math::EPSILON
                    ? localPosition.z / parentWorld->scale.z : localPosition.z;
            }

            transform->localPosition = localPosition;
            transform->localRotation = (inverseParentRotation * worldTransform.rotation).Normalized();
            transform->localScale = transform->inheritParentScale
                ? engine::math::Vec3{
                    std::abs(parentWorld->scale.x) > engine::math::EPSILON ? worldTransform.scale.x / parentWorld->scale.x : worldTransform.scale.x,
                    std::abs(parentWorld->scale.y) > engine::math::EPSILON ? worldTransform.scale.y / parentWorld->scale.y : worldTransform.scale.y,
                    std::abs(parentWorld->scale.z) > engine::math::EPSILON ? worldTransform.scale.z / parentWorld->scale.z : worldTransform.scale.z}
                : worldTransform.scale;
            transform->dirty = true;
            return;
        }
    }

    transform->localPosition = worldTransform.position;
    transform->localRotation = worldTransform.rotation;
    transform->localScale = worldTransform.scale;
    transform->dirty = true;
}

// =============================================================================
// krom.h functions
// =============================================================================

LPENTITY CreateEntity(engine::ecs::World* world, const char* name)
{
    if (!world)
        return NULL_LPENTITY;

    const LPENTITY entity = world->CreateEntity();
    world->Add<engine::TransformComponent>(entity);
    world->Add<engine::WorldTransformComponent>(entity);
    world->Add<engine::ActiveComponent>(entity);
    if (name && name[0] != '\0')
        world->Add<engine::NameComponent>(entity, engine::NameComponent{name});
    return entity;
}

bool IsAlive(engine::ecs::World* world, LPENTITY entity)
{
    return world && world->IsAlive(entity);
}

bool SetName(engine::ecs::World* world, LPENTITY entity, const char* name)
{
    if (!world || !world->IsAlive(entity))
        return false;

    const std::string value = name ? name : "";
    if (auto* nameComponent = world->Get<engine::NameComponent>(entity))
        nameComponent->name = value;
    else
        world->Add<engine::NameComponent>(entity, engine::NameComponent{value});
    return true;
}

const char* GetName(engine::ecs::World* world, LPENTITY entity)
{
    if (!world || !world->IsAlive(entity))
        return "";
    if (const auto* nameComponent = world->Get<engine::NameComponent>(entity))
        return nameComponent->name.c_str();
    return "";
}

bool SetLayer(engine::ecs::World* world, LPENTITY entity, uint32_t layerMask)
{
    if (!world || !world->IsAlive(entity)) return false;
    if (auto* mesh = world->Get<engine::MeshComponent>(entity))
    { mesh->layerMask = layerMask; return true; }
    return false;
}

uint32_t GetLayer(engine::ecs::World* world, LPENTITY entity)
{
    if (!world || !world->IsAlive(entity)) return engine::renderer::LAYER_DEFAULT;
    if (const auto* mesh = world->Get<engine::MeshComponent>(entity))
        return mesh->layerMask;
    return engine::renderer::LAYER_DEFAULT;
}

bool SetCameraLayers(engine::ecs::World* world, LPENTITY camera, uint32_t cullingMask)
{
    if (!world || !world->IsAlive(camera)) return false;
    if (auto* cam = world->Get<engine::CameraComponent>(camera))
    { cam->cullingMask = cullingMask; return true; }
    return false;
}

uint32_t GetCameraLayers(engine::ecs::World* world, LPENTITY camera)
{
    if (!world || !world->IsAlive(camera)) return engine::renderer::LAYER_ALL;
    if (const auto* cam = world->Get<engine::CameraComponent>(camera))
        return cam->cullingMask;
    return engine::renderer::LAYER_ALL;
}

bool SetTag(engine::ecs::World* world, LPENTITY entity, const char* tag)
{
    if (!world || !world->IsAlive(entity)) return false;
    const std::string value = tag ? tag : "";
    if (!world->Has<engine::TagComponent>(entity))
        world->Add<engine::TagComponent>(entity, engine::TagComponent{value});
    else if (auto* tc = world->Get<engine::TagComponent>(entity))
        tc->tag = value;
    return true;
}

const char* GetTag(engine::ecs::World* world, LPENTITY entity)
{
    if (!world || !world->IsAlive(entity)) return "";
    if (const auto* tc = world->Get<engine::TagComponent>(entity))
        return tc->tag.c_str();
    return "";
}

LPENTITY FindEntityByTag(engine::ecs::World* world, const char* tag)
{
    if (!world || !tag) return NULL_LPENTITY;
    LPENTITY found = NULL_LPENTITY;
    world->View<engine::TagComponent>([&](LPENTITY id, const engine::TagComponent& tc)
    {
        if (!found.IsValid() && tc.tag == tag)
            found = id;
    });
    return found;
}

LPENTITY FindChild(engine::ecs::World* world, LPENTITY rootEntity, const char* name)
{
    if (!world || !world->IsAlive(rootEntity) || !name)
        return NULL_LPENTITY;

    LPENTITY found = NULL_LPENTITY;
    std::function<void(LPENTITY)> search = [&](LPENTITY entity)
    {
        if (found.IsValid() || !world->IsAlive(entity))
            return;

        if (const auto* nameComponent = world->Get<engine::NameComponent>(entity))
        {
            if (nameComponent->name == name)
            {
                found = entity;
                return;
            }
        }

        if (const auto* children = world->Get<engine::ChildrenComponent>(entity))
        {
            for (const LPENTITY child : children->children)
                search(child);
        }
    };
    search(rootEntity);
    return found;
}

int CountChildren(engine::ecs::World* world, LPENTITY entity)
{
    if (!world || !world->IsAlive(entity))
        return 0;
    if (const auto* children = world->Get<engine::ChildrenComponent>(entity))
        return static_cast<int>(children->children.size());
    return 0;
}

LPENTITY GetChild(engine::ecs::World* world, LPENTITY entity, int index)
{
    if (!world || !world->IsAlive(entity) || index < 0)
        return NULL_LPENTITY;
    const auto* children = world->Get<engine::ChildrenComponent>(entity);
    if (!children)
        return NULL_LPENTITY;
    const size_t childIndex = static_cast<size_t>(index);
    if (childIndex >= children->children.size())
        return NULL_LPENTITY;
    return children->children[childIndex];
}

bool SetVisible(engine::ecs::World* world, LPENTITY entity, bool visible, bool recursive)
{
    // Nur Rendering an/aus — Entity laeuft weiter durch Systeme und Transform.
    // Fuer vollstaendige Deaktivierung: SetActive() verwenden.
    if (!world || !world->IsAlive(entity))
        return false;

    std::function<void(LPENTITY)> apply = [&](LPENTITY current)
    {
        if (!world->IsAlive(current))
            return;

        world->SetComponent(current, engine::VisibilityComponent{visible});
        if (auto* mesh = world->Get<engine::MeshComponent>(current))
            mesh->visible = visible;

        if (!recursive)
            return;
        if (const auto* children = world->Get<engine::ChildrenComponent>(current))
        {
            // Adding state to a descendant can migrate an ancestor's archetype slot.
            const auto descendants = children->children;
            for (const LPENTITY child : descendants)
                apply(child);
        }
    };
    apply(entity);
    return true;
}

bool SetActive(engine::ecs::World* world, LPENTITY entity, bool active, bool recursive)
{
    // Vollstaendige Deaktivierung — Entity wird aus Rendering UND Systemen entfernt.
    // Fuer reines Verstecken: SetVisible() verwenden.
    if (!world || !world->IsAlive(entity))
        return false;

    std::function<void(LPENTITY)> apply = [&](LPENTITY current)
    {
        if (!world->IsAlive(current))
            return;

        if (auto* a = world->Get<engine::ActiveComponent>(current))
            a->active = active;
        else
            world->Add<engine::ActiveComponent>(current, engine::ActiveComponent{active});

        if (!recursive)
            return;
        if (const auto* children = world->Get<engine::ChildrenComponent>(current))
        {
            // Adding state to a descendant can migrate an ancestor's archetype slot.
            const auto descendants = children->children;
            for (const LPENTITY child : descendants)
                apply(child);
        }
    };
    apply(entity);
    return true;
}

bool SetParent(const EntityContext& ctx, LPENTITY child, LPENTITY parent, bool keepWorldTransform)
{
    if (!ctx.world ||
        !ctx.world->IsAlive(child) ||
        !ctx.world->IsAlive(parent) ||
        child == parent ||
        IsDescendantOf(*ctx.world, parent, child))
    {
        return false;
    }

    const CapturedTransform worldTransform = keepWorldTransform
        ? CaptureWorldTransform(ctx, child)
        : CapturedTransform{};

    DetachEntityFromParent(*ctx.world, child);
    ctx.world->Add<engine::ParentComponent>(child, parent);
    if (!ctx.world->Has<engine::ChildrenComponent>(parent))
        ctx.world->Add<engine::ChildrenComponent>(parent);
    ctx.world->Get<engine::ChildrenComponent>(parent)->Add(child);

    if (keepWorldTransform)
        ApplyLocalFromWorld(ctx, child, worldTransform);
    else if (auto* transform = ctx.world->Get<engine::TransformComponent>(child))
        transform->dirty = true;

    ctx.transformSystem.Update(*ctx.world);
    ctx.boundsSystem.Update(*ctx.world);
    return true;
}

bool Detach(const EntityContext& ctx, LPENTITY child, bool keepWorldTransform)
{
    if (!ctx.world || !ctx.world->IsAlive(child))
        return false;

    const CapturedTransform worldTransform = keepWorldTransform
        ? CaptureWorldTransform(ctx, child)
        : CapturedTransform{};

    DetachEntityFromParent(*ctx.world, child);
    if (keepWorldTransform)
        ApplyLocalFromWorld(ctx, child, worldTransform);
    else if (auto* transform = ctx.world->Get<engine::TransformComponent>(child))
        transform->dirty = true;

    ctx.transformSystem.Update(*ctx.world);
    ctx.boundsSystem.Update(*ctx.world);
    return true;
}

bool SetPosition(engine::ecs::World* world, LPENTITY entity, const engine::math::Vec3& position)
{
    if (!world || !world->IsAlive(entity))
        return false;

    if (auto* transform = world->Get<engine::TransformComponent>(entity))
    {
        transform->localPosition = position;
        transform->dirty = true;
        return true;
    }
    return false;
}

bool SetRotationEulerDeg(engine::ecs::World* world, LPENTITY entity, float pitch, float yaw, float roll)
{
    if (!world || !world->IsAlive(entity))
        return false;

    if (auto* transform = world->Get<engine::TransformComponent>(entity))
    {
        transform->SetEulerDeg(pitch, yaw, roll);
        return true;
    }
    return false;
}

engine::math::Vec3 GetRotationEulerDeg(engine::ecs::World* world, LPENTITY entity)
{
    if (!world || !world->IsAlive(entity))
        return {};

    // Current world rotation (also right after a setter, before the frame's
    // transform update). Quaternion -> Euler (order YXZ = yaw -> pitch -> roll).
    engine::math::Vec3 position{};
    engine::math::Quat rotation = engine::math::Quat::Identity();
    if (!ResolveCurrentWorld(world, entity, position, rotation))
        return {};
    const engine::math::Quat* q = &rotation;

    // Same formulas as the editor inspector (QuatToEulerDeg in EditorUI.cpp).
    // Both atan2 argument pairs have the length cos(pitch). At gimbal lock
    // (|pitch| = 90) they go to zero; there only yaw - roll (pitch +90) or
    // yaw + roll (pitch -90) is defined, so roll is set to 0 and yaw taken from
    // other matrix elements (the editor's gimbal branch). The switch happens only
    // when cos(pitch) is really tiny: ignoring roll costs about cos(pitch) * |roll|
    // of rotation, so the editor's |sin(pitch)| >= 0.9999 (|pitch| > ~89.2 deg)
    // would lose up to ~2.5 deg. Pitch from atan2(sin, cos) stays accurate near 90.
    const float sinP = std::clamp(2.0f * (q->w * q->x - q->z * q->y), -1.0f, 1.0f);
    const float yawSin = 2.0f * (q->w * q->y + q->x * q->z);
    const float yawCos = 1.0f - 2.0f * (q->y * q->y + q->x * q->x);
    const float cosP = std::sqrt(yawSin * yawSin + yawCos * yawCos);
    const float pitch = engine::math::RAD_TO_DEG * std::atan2(sinP, cosP);
    float yaw = 0.0f;
    float roll = 0.0f;
    if (cosP > 3.0e-4f)
    {
        yaw = engine::math::RAD_TO_DEG * std::atan2(yawSin, yawCos);
        roll = engine::math::RAD_TO_DEG *
            std::atan2(2.0f * (q->w * q->z + q->x * q->y),
                       1.0f - 2.0f * (q->x * q->x + q->z * q->z));
    }
    else
    {
        yaw = engine::math::RAD_TO_DEG *
            std::atan2(-2.0f * (q->x * q->z - q->w * q->y),
                       1.0f - 2.0f * (q->y * q->y + q->z * q->z));
    }
    return {pitch, yaw, roll};
}

bool SetScale(engine::ecs::World* world, LPENTITY entity, const engine::math::Vec3& scale)
{
    if (!world || !world->IsAlive(entity))
        return false;

    if (auto* transform = world->Get<engine::TransformComponent>(entity))
    {
        transform->localScale = scale;
        transform->dirty = true;
        return true;
    }
    return false;
}

bool MoveEntity(engine::ecs::World* world, LPENTITY entity, float x, float y, float z)
{
    if (!world || !world->IsAlive(entity))
        return false;

    auto* transform = world->Get<engine::TransformComponent>(entity);
    if (!transform)
        return false;

    const engine::math::Vec3 localOffset = transform->localRotation.Rotate({x, y, z});
    transform->localPosition += localOffset;
    transform->dirty = true;
    return true;
}

bool TurnEntity(engine::ecs::World* world, LPENTITY entity, float pitch, float yaw, float roll)
{
    if (!world || !world->IsAlive(entity))
        return false;

    auto* transform = world->Get<engine::TransformComponent>(entity);
    if (!transform)
        return false;

    transform->RotateLocalEulerDeg(pitch, yaw, roll);
    return true;
}

bool GetWorldBounds(engine::ecs::World* world,
                    LPENTITY entity,
                    engine::math::Vec3& outCenter,
                    engine::math::Vec3& outExtents,
                    float& outBoundingSphere)
{
    if (!world || !world->IsAlive(entity))
        return false;

    engine::math::Vec3 boundsMin{};
    engine::math::Vec3 boundsMax{};
    bool hasBounds = false;
    if (!CollectWorldBoundsRecursive(*world, entity, boundsMin, boundsMax, hasBounds))
        return false;

    outCenter = (boundsMin + boundsMax) * 0.5f;
    outExtents = (boundsMax - boundsMin) * 0.5f;
    outBoundingSphere = outExtents.Length();
    return true;
}

bool LookAt(engine::ecs::World* world, LPENTITY entity, const engine::math::Vec3& target, const engine::math::Vec3& up)
{
    if (!world || !world->IsAlive(entity))
        return false;

    auto* transform = world->Get<engine::TransformComponent>(entity);
    if (!transform)
        return false;

    const engine::math::Vec3 position = ResolveEntityPosition(world, entity);
    const engine::math::Vec3 forward = (target - position).Normalized();
    if (forward.LengthSq() <= engine::math::EPSILON)
        return false;

    engine::math::Vec3 right = engine::math::Vec3::Cross(forward, up).Normalized();
    if (right.LengthSq() <= engine::math::EPSILON)
        right = engine::math::Vec3::Cross(forward, engine::math::Vec3::Right()).Normalized();
    if (right.LengthSq() <= engine::math::EPSILON)
        return false;

    const engine::math::Vec3 correctedUp = engine::math::Vec3::Cross(right, forward).Normalized();
    const engine::math::Vec3 back = -forward;

    engine::math::Mat4 rotation = engine::math::Mat4::Identity();
    rotation.m[0][0] = right.x;
    rotation.m[0][1] = right.y;
    rotation.m[0][2] = right.z;
    rotation.m[1][0] = correctedUp.x;
    rotation.m[1][1] = correctedUp.y;
    rotation.m[1][2] = correctedUp.z;
    rotation.m[2][0] = back.x;
    rotation.m[2][1] = back.y;
    rotation.m[2][2] = back.z;

    // The look rotation is a WORLD rotation; localRotation is relative to the parent
    // (world = parentWorld * local, see TransformSystem::ComputeWorldTransform), so
    // local = inverse(parentWorld) * desiredWorld. The parent's current world rotation
    // is taken from the entity's own current world transform (parentWorld =
    // world * inverse(local)); that applies exactly the parent rules of
    // ComputeCurrentWorldTransform without repeating them here. Unit quaternions:
    // inverse = conjugate.
    const engine::math::Quat desiredWorld = QuatFromRotationMatrix(rotation).Normalized();
    if (world->Has<engine::ParentComponent>(entity) && world->Has<engine::WorldTransformComponent>(entity))
    {
        const engine::WorldTransformComponent current =
            engine::TransformSystem::ComputeCurrentWorldTransform(*world, entity);
        const engine::math::Quat parentWorld = (current.rotation * transform->localRotation.Conjugate()).Normalized();
        transform->localRotation = (parentWorld.Conjugate() * desiredWorld).Normalized();
    }
    else
    {
        transform->localRotation = desiredWorld;
    }
    transform->dirty = true;
    return true;
}

bool LookAt(engine::ecs::World* world, LPENTITY entity, LPENTITY targetEntity, const engine::math::Vec3& up)
{
    return LookAt(world, entity, ResolveEntityPosition(world, targetEntity), up);
}

LPENTITY CreateCamera(engine::ecs::World* world,
                      const char* name,
                      const engine::math::Vec3& position,
                      float fovYDeg,
                      bool isMainCamera)
{
    const LPENTITY entity = CreateEntity(world, name);
    if (!entity.IsValid())
        return NULL_LPENTITY;

    SetPosition(world, entity, position);
    SetRotationEulerDeg(world, entity, -10.0f, 0.0f, 0.0f);

    engine::CameraComponent camera{};
    camera.projection = engine::ProjectionType::Perspective;
    camera.fovYDeg = fovYDeg;
    camera.nearPlane = 0.05f;
    camera.farPlane = 1000.0f;
    camera.isMainCamera = isMainCamera;
    world->Add<engine::CameraComponent>(entity, camera);
    return entity;
}

LPENTITY CreateDirectionalLight(engine::ecs::World* world,
                                const char* name,
                                const engine::math::Vec3& position,
                                const engine::math::Vec3& eulerDeg,
                                const engine::math::Vec3& color,
                                float intensity)
{
    const LPENTITY entity = CreateEntity(world, name);
    if (!entity.IsValid())
        return NULL_LPENTITY;

    SetPosition(world, entity, position);
    SetRotationEulerDeg(world, entity, eulerDeg.x, eulerDeg.y, eulerDeg.z);

    engine::LightComponent light{};
    light.type = engine::LightType::Directional;
    light.color = color;
    light.intensity = intensity;
    light.castShadows = true;
    light.shadowSettings.enabled = true;
    light.shadowSettings.resolution = 2048u;
    light.shadowSettings.bias = 0.0015f;
    engine::ApplyDirectionalShadowDefaults(light.shadowSettings);
    world->Add<engine::LightComponent>(entity, light);
    return entity;
}

LPENTITY CreatePointLight(engine::ecs::World* world,
                          const char* name,
                          const engine::math::Vec3& position,
                          const engine::math::Vec3& color,
                          float intensity,
                          float range)
{
    const LPENTITY entity = CreateEntity(world, name);
    if (!entity.IsValid())
        return NULL_LPENTITY;

    SetPosition(world, entity, position);

    engine::LightComponent light{};
    light.type = engine::LightType::Point;
    light.color = color;
    light.intensity = intensity;
    light.range = range;
    light.castShadows = true;
    light.shadowSettings.enabled = true;
    light.shadowSettings.resolution = 1024u;
    light.shadowSettings.bias = 0.0015f;
    light.shadowSettings.maxDistance = range;
    world->Add<engine::LightComponent>(entity, light);
    return entity;
}

LPENTITY CreateSpotLight(engine::ecs::World* world,
                         const char* name,
                         const engine::math::Vec3& position,
                         const engine::math::Vec3& eulerDeg,
                         const engine::math::Vec3& color,
                         float intensity,
                         float range,
                         float innerDeg,
                         float outerDeg)
{
    const LPENTITY entity = CreateEntity(world, name);
    if (!entity.IsValid())
        return NULL_LPENTITY;

    SetPosition(world, entity, position);
    SetRotationEulerDeg(world, entity, eulerDeg.x, eulerDeg.y, eulerDeg.z);

    engine::LightComponent light{};
    light.type = engine::LightType::Spot;
    light.color = color;
    light.intensity = intensity;
    light.range = range;
    light.spotInnerDeg = innerDeg;
    light.spotOuterDeg = outerDeg;
    light.castShadows = true;
    light.shadowSettings.enabled = true;
    light.shadowSettings.resolution = 1024u;
    light.shadowSettings.bias = 0.0015f;
    light.shadowSettings.maxDistance = range;
    world->Add<engine::LightComponent>(entity, light);
    return entity;
}

LPENTITY FindEntity(engine::ecs::World* world, const char* name)
{
    if (!world || !name)
        return NULL_LPENTITY;

    LPENTITY found = NULL_LPENTITY;
    world->ForEachAlive([&](engine::EntityID id)
    {
        if (found.IsValid()) return;
        if (const auto* nc = world->Get<engine::NameComponent>(id))
            if (nc->name == name)
                found = id;
    });
    return found;
}

LPENTITY GetMainCamera(engine::ecs::World* world)
{
    if (!world)
        return NULL_LPENTITY;

    LPENTITY mainCamera = NULL_LPENTITY;
    LPENTITY fallbackCamera = NULL_LPENTITY;
    world->View<engine::CameraComponent>(
        [&](LPENTITY entity, const engine::CameraComponent& camera)
        {
            if (!fallbackCamera.IsValid())
                fallbackCamera = entity;
            if (camera.isMainCamera && !mainCamera.IsValid())
                mainCamera = entity;
        });

    return mainCamera.IsValid() ? mainCamera : fallbackCamera;
}

} // namespace Krom::internal
