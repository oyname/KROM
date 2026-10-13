#include "addons/script/IScriptContext.hpp"

#include <algorithm>

namespace engine::script {

TransformComponent* IScriptContext::EditTransform(EntityID entity)
{
    TransformComponent* transform = Get<TransformComponent>(entity);
    if (transform)
    {
        transform->dirty = true;
        ++transform->localVersion;
    }
    return transform;
}

EntityID IScriptContext::FindEntity(std::string_view name)
{
    ecs::World* world = GetWorld();
    if (!world)
        return NULL_ENTITY;

    EntityID found = NULL_ENTITY;
    world->ForEachAlive([&](EntityID id)
    {
        if (found.IsValid())
            return;
        if (const auto* n = world->Get<NameComponent>(id); n && n->name == name)
            found = id;
    });
    return found;
}

EntityID IScriptContext::GetRoot(EntityID entity)
{
    ecs::World* world = GetWorld();
    EntityID cursor = Resolve(entity);
    if (!world)
        return cursor;

    for (uint32_t depth = 0; depth < 1024u && world->IsAlive(cursor); ++depth)
    {
        const auto* parent = world->Get<ParentComponent>(cursor);
        if (!parent || !world->IsAlive(parent->parent))
            break;
        cursor = parent->parent;
    }
    return cursor;
}

std::vector<EntityID> IScriptContext::FilterRoots(const std::vector<EntityID>& entities, bool ignoreSelf)
{
    const EntityID selfRoot = ignoreSelf ? GetRoot() : NULL_ENTITY;
    std::vector<EntityID> roots;
    for (const EntityID e : entities)
    {
        const EntityID root = GetRoot(e);
        if (ignoreSelf && root == selfRoot)
            continue;
        if (std::find(roots.begin(), roots.end(), root) == roots.end())
            roots.push_back(root);
    }
    return roots;
}

// ── Kollisionsabfragen ───────────────────────────────────────────────────────

bool IScriptContext::Raycast(const math::Vec3& origin, const math::Vec3& direction, float maxDistance,
                             collision::RaycastHit& outHit, bool ignoreSelf)
{
    const collision::SceneQueries* queries = GetSceneQueries();
    if (!queries || direction.LengthSq() <= math::EPSILON)
        return false;

    const collision::Ray ray{origin, direction.Normalized()};
    const EntityID selfRoot = ignoreSelf ? GetRoot() : NULL_ENTITY;
    // Kandidaten sind nach Distanz sortiert — erster Treffer ausserhalb der eigenen Hierarchie gewinnt.
    for (const collision::RaycastCandidate& candidate : queries->CollectRaycastCandidates(ray, maxDistance))
    {
        const EntityID root = GetRoot(candidate.entity);
        if (ignoreSelf && root == selfRoot)
            continue;
        outHit.entity   = root;
        outHit.distance = candidate.boundsDistance;
        outHit.position = ray.origin + ray.direction * candidate.boundsDistance;
        outHit.normal   = (outHit.position - candidate.sphere.center).Normalized();
        return true;
    }
    return false;
}

std::vector<EntityID> IScriptContext::OverlapSphere(const math::Vec3& center, float radius, bool ignoreSelf)
{
    const collision::SceneQueries* queries = GetSceneQueries();
    if (!queries)
        return {};
    return FilterRoots(queries->OverlapSphere({center, radius}), ignoreSelf);
}

std::vector<EntityID> IScriptContext::OverlapBox(const math::Vec3& center, const math::Vec3& halfExtents,
                                                 bool ignoreSelf)
{
    const collision::SceneQueries* queries = GetSceneQueries();
    if (!queries)
        return {};
    return FilterRoots(queries->OverlapAABB({center - halfExtents, center + halfExtents}), ignoreSelf);
}

// ── Transform ────────────────────────────────────────────────────────────────

math::Vec3 IScriptContext::GetPosition(EntityID entity)
{
    const TransformComponent* t = Get<TransformComponent>(entity);
    return t ? t->localPosition : math::Vec3::Zero();
}

void IScriptContext::SetPosition(const math::Vec3& position, EntityID entity)
{
    if (TransformComponent* t = EditTransform(entity))
        t->localPosition = position;
}

void IScriptContext::Translate(const math::Vec3& delta, Space space, EntityID entity)
{
    if (TransformComponent* t = EditTransform(entity))
        t->localPosition += (space == Space::Local) ? t->localRotation.Rotate(delta) : delta;
}

math::Quat IScriptContext::GetRotation(EntityID entity)
{
    const TransformComponent* t = Get<TransformComponent>(entity);
    return t ? t->localRotation : math::Quat::Identity();
}

void IScriptContext::SetRotation(const math::Quat& rotation, EntityID entity)
{
    if (TransformComponent* t = EditTransform(entity))
        t->localRotation = rotation.Normalized();
}

void IScriptContext::SetRotationEulerDeg(float pitch, float yaw, float roll, EntityID entity)
{
    if (TransformComponent* t = EditTransform(entity))
        t->SetEulerDeg(pitch, yaw, roll);
}

void IScriptContext::RotateEulerDeg(float pitch, float yaw, float roll, Space space, EntityID entity)
{
    TransformComponent* t = EditTransform(entity);
    if (!t)
        return;
    if (space == Space::Local)
        t->RotateLocalEulerDeg(pitch, yaw, roll);
    else
        t->RotateWorldEulerDeg(pitch, yaw, roll);
}

math::Vec3 IScriptContext::GetScale(EntityID entity)
{
    const TransformComponent* t = Get<TransformComponent>(entity);
    return t ? t->localScale : math::Vec3::One();
}

void IScriptContext::SetScale(const math::Vec3& scale, EntityID entity)
{
    if (TransformComponent* t = EditTransform(entity))
        t->localScale = scale;
}

math::Vec3 IScriptContext::Forward(EntityID entity)
{
    return GetRotation(entity).Rotate(math::Vec3::Forward());
}

math::Vec3 IScriptContext::Right(EntityID entity)
{
    return GetRotation(entity).Rotate(math::Vec3::Right());
}

math::Vec3 IScriptContext::Up(EntityID entity)
{
    return GetRotation(entity).Rotate(math::Vec3::Up());
}

math::Vec3 IScriptContext::GetWorldPosition(EntityID entity)
{
    if (const WorldTransformComponent* w = Get<WorldTransformComponent>(entity))
        return w->position;
    return GetPosition(entity);
}

// ── Input ────────────────────────────────────────────────────────────────────

bool IScriptContext::KeyDown(platform::Key key) const
{
    const platform::IInput* input = GetInput();
    return input && input->KeyDown(key);
}

bool IScriptContext::KeyHit(platform::Key key) const
{
    const platform::IInput* input = GetInput();
    return input && input->KeyHit(key);
}

bool IScriptContext::KeyReleased(platform::Key key) const
{
    const platform::IInput* input = GetInput();
    return input && input->KeyReleased(key);
}

bool IScriptContext::MouseButtonDown(platform::MouseButton button) const
{
    const platform::IInput* input = GetInput();
    return input && input->MouseButtonDown(button);
}

bool IScriptContext::MouseButtonHit(platform::MouseButton button) const
{
    const platform::IInput* input = GetInput();
    return input && input->MouseButtonHit(button);
}

math::Vec2 IScriptContext::MouseDelta() const
{
    const platform::IInput* input = GetInput();
    if (!input)
        return math::Vec2::Zero();
    return {static_cast<float>(input->MouseDeltaX()), static_cast<float>(input->MouseDeltaY())};
}

float IScriptContext::MouseScroll() const
{
    const platform::IInput* input = GetInput();
    return input ? input->MouseScrollDelta() : 0.f;
}

float IScriptContext::Axis(platform::Key negative, platform::Key positive) const
{
    float value = 0.f;
    if (KeyDown(negative)) value -= 1.f;
    if (KeyDown(positive)) value += 1.f;
    return value;
}

} // namespace engine::script
