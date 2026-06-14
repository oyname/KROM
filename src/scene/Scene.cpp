// =============================================================================
// KROM Engine - src/scene/Scene.cpp
// Scene-System: Implementierung.
// =============================================================================
#include "scene/Scene.hpp"
#include "core/Debug.hpp"
#include "jobs/FrameScheduler.hpp"
#include <cassert>
#include <algorithm>
#include <cmath>

namespace engine {

namespace {

math::Quat ComputeWorldRotation(const ecs::World& world, EntityID entity) noexcept
{
    const auto* transform = world.Get<TransformComponent>(entity);
    if (!transform)
        return math::Quat::Identity();

    const auto* parent = world.Get<ParentComponent>(entity);
    if (!parent || !parent->parent.IsValid() || !world.IsAlive(parent->parent))
        return transform->localRotation;

    return (ComputeWorldRotation(world, parent->parent) * transform->localRotation).Normalized();
}

math::Vec3 ComputeWorldScale(const ecs::World& world, EntityID entity) noexcept
{
    const auto* transform = world.Get<TransformComponent>(entity);
    if (!transform)
        return math::Vec3::One();

    const auto* parent = world.Get<ParentComponent>(entity);
    if (!parent || !parent->parent.IsValid() || !world.IsAlive(parent->parent))
        return transform->localScale;

    if (!transform->inheritParentScale)
        return transform->localScale;

    const math::Vec3 parentScale = ComputeWorldScale(world, parent->parent);
    return {
        parentScale.x * transform->localScale.x,
        parentScale.y * transform->localScale.y,
        parentScale.z * transform->localScale.z
    };
}

math::Vec3 ComputeWorldPosition(const ecs::World& world, EntityID entity) noexcept
{
    if (const auto* worldTransform = world.Get<WorldTransformComponent>(entity))
        return worldTransform->matrix.TransformPoint(math::Vec3::Zero());
    return math::Vec3::Zero();
}

math::Vec3 ComputeLocalScaleFromWorld(const math::Vec3& worldScale,
                                      const math::Vec3& parentWorldScale) noexcept
{
    return {
        std::abs(parentWorldScale.x) > math::EPSILON ? worldScale.x / parentWorldScale.x : worldScale.x,
        std::abs(parentWorldScale.y) > math::EPSILON ? worldScale.y / parentWorldScale.y : worldScale.y,
        std::abs(parentWorldScale.z) > math::EPSILON ? worldScale.z / parentWorldScale.z : worldScale.z
    };
}

} // namespace

EntityID Scene::CreateEntity(std::string_view name)
{
    EntityID id = m_world.CreateEntity();
    m_world.Add<NameComponent>(id, std::string(name));
    m_world.Add<ActiveComponent>(id);
    m_world.Add<TransformComponent>(id);
    m_world.Add<WorldTransformComponent>(id);
    m_rootEntities.push_back(id);
    return id;
}

void Scene::SetParent(EntityID child, EntityID parent)
{
    assert(m_world.IsAlive(child)  && "Scene::SetParent: child not alive");
    assert(m_world.IsAlive(parent) && "Scene::SetParent: parent not alive");
    assert(child != parent         && "Scene::SetParent: self-parenting");

    PropagateTransforms();

    const math::Vec3 childWorldPosition = ComputeWorldPosition(m_world, child);
    const math::Quat childWorldRotation = ComputeWorldRotation(m_world, child);
    const math::Vec3 childWorldScale    = ComputeWorldScale(m_world, child);

    // Zykluserkennung: prüfe ob 'parent' bereits ein Descendant von 'child' ist.
    // Wenn ja, würde SetParent einen Zyklus in der Hierarchie erzeugen.
    {
        EntityID cursor = parent;
        uint32_t depth  = 0u;
        static constexpr uint32_t MAX_DEPTH = 1024u; // Schutz vor Endlosschleife
        while (cursor.IsValid() && depth < MAX_DEPTH)
        {
            if (cursor == child)
            {
                Debug::LogError("Scene.cpp: SetParent - Zyklus erkannt: "
                    "entity %u ist bereits Vorfahre von %u",
                    child.value, parent.value);
                return; // Abbruch - kein Zyklus erzeugen
            }
            const ParentComponent* pc = m_world.Get<ParentComponent>(cursor);
            if (!pc || !pc->parent.IsValid()) break;
            cursor = pc->parent;
            ++depth;
        }
        if (depth >= MAX_DEPTH)
        {
            Debug::LogError("Scene.cpp: SetParent - Hierarchietiefe > %u, "
                "möglicher Zyklus, Abbruch", MAX_DEPTH);
            return;
        }
    }

    // Alten Parent lösen
    if (m_world.Has<ParentComponent>(child))
    {
        const EntityID oldParent = m_world.Get<ParentComponent>(child)->parent;
        if (oldParent == parent) return;
        if (m_world.IsAlive(oldParent) && m_world.Has<ChildrenComponent>(oldParent))
            m_world.Get<ChildrenComponent>(oldParent)->Remove(child);
        m_world.Remove<ParentComponent>(child);
    }
    else
    {
        RemoveFromRoots(child);
    }

    // Neuen Parent setzen
    m_world.Add<ParentComponent>(child, parent);

    if (!m_world.Has<ChildrenComponent>(parent))
        m_world.Add<ChildrenComponent>(parent);
    m_world.Get<ChildrenComponent>(parent)->Add(child);

    if (auto* tc = m_world.Get<TransformComponent>(child))
    {
        if (const auto* parentWorld = m_world.Get<WorldTransformComponent>(parent))
        {
            tc->localPosition = parentWorld->rotation.Conjugate().Normalized().Rotate(
                childWorldPosition - parentWorld->position);
            if (tc->inheritParentScale)
            {
                tc->localPosition = {
                    std::abs(parentWorld->scale.x) > math::EPSILON ? tc->localPosition.x / parentWorld->scale.x : tc->localPosition.x,
                    std::abs(parentWorld->scale.y) > math::EPSILON ? tc->localPosition.y / parentWorld->scale.y : tc->localPosition.y,
                    std::abs(parentWorld->scale.z) > math::EPSILON ? tc->localPosition.z / parentWorld->scale.z : tc->localPosition.z
                };
            }
        }
        else
            tc->localPosition = childWorldPosition;

        const math::Quat parentWorldRotation = ComputeWorldRotation(m_world, parent);
        tc->localRotation = (parentWorldRotation.Conjugate().Normalized() * childWorldRotation).Normalized();
        tc->localScale = tc->inheritParentScale
            ? ComputeLocalScaleFromWorld(childWorldScale, ComputeWorldScale(m_world, parent))
            : childWorldScale;
        tc->dirty = true;
    }

    m_transformSys.Invalidate();
}

void Scene::DetachFromParent(EntityID child)
{
    if (!m_world.Has<ParentComponent>(child)) return;

    PropagateTransforms();

    const math::Vec3 childWorldPosition = ComputeWorldPosition(m_world, child);
    const math::Quat childWorldRotation = ComputeWorldRotation(m_world, child);
    const math::Vec3 childWorldScale    = ComputeWorldScale(m_world, child);

    const EntityID parent = m_world.Get<ParentComponent>(child)->parent;
    if (m_world.IsAlive(parent) && m_world.Has<ChildrenComponent>(parent))
        m_world.Get<ChildrenComponent>(parent)->Remove(child);
    m_world.Remove<ParentComponent>(child);
    m_rootEntities.push_back(child);

    if (auto* tc = m_world.Get<TransformComponent>(child))
    {
        tc->localPosition = childWorldPosition;
        tc->localRotation = childWorldRotation;
        tc->localScale    = childWorldScale;
        tc->dirty = true;
    }

    m_transformSys.Invalidate();
}

void Scene::Update(jobs::JobSystem& js)
{
    jobs::FrameScheduler scheduler;

    auto transform = scheduler.RegisterStage(
        "TransformUpdate",
        {},
        [this]() -> jobs::TaskResult {
            m_transformSys.Update(m_world);
            return jobs::TaskResult::Ok();
        },
        { jobs::FrameTags::Transform },
        {}
    );

    scheduler.RegisterStage(
        "BoundsUpdate",
        { transform },
        [this, &js]() -> jobs::TaskResult {
            m_boundsSys.Update(m_world, js);
            return jobs::TaskResult::Ok();
        },
        { "Bounds" },
        { jobs::FrameTags::Transform }
    );

    if (!scheduler.Build())
    {
        Debug::LogError("Scene::Update: FrameScheduler Build fehlgeschlagen");
        return;
    }
    scheduler.Execute(js);
}

void Scene::PropagateTransforms()
{
    // Delegiert an TransformSystem (BFS, topologisch korrekt).
    // Die alte rekursive Implementierung bleibt als private Fallback
    // für einzelne Subtrees (z.B. nach SetParent auf einen Teilbaum).
    m_transformSys.Update(m_world);
}

void Scene::DestroyEntity(EntityID id)
{
    if (!m_world.IsAlive(id)) return;

    // Kinder zuerst (Kopie der Liste da Rekursion modifiziert)
    if (m_world.Has<ChildrenComponent>(id))
    {
        const auto children = m_world.Get<ChildrenComponent>(id)->children;
        for (EntityID child : children)
            DestroyEntity(child);
    }

    // Aus Parent-ChildrenComponent entfernen
    if (m_world.Has<ParentComponent>(id))
    {
        const EntityID parent = m_world.Get<ParentComponent>(id)->parent;
        if (m_world.IsAlive(parent) && m_world.Has<ChildrenComponent>(parent))
            m_world.Get<ChildrenComponent>(parent)->Remove(id);
    }

    RemoveFromRoots(id);
    m_world.DestroyEntity(id);
}

EntityID Scene::FindByName(std::string_view name) const
{
    EntityID found = NULL_ENTITY;
    m_world.View<NameComponent>([&](EntityID id, const NameComponent& nc) {
        if (nc.name == name) found = id;
    });
    return found;
}

void Scene::SetLocalPosition(EntityID id, const math::Vec3& pos) noexcept
{
    if (auto* tc = m_world.Get<TransformComponent>(id))
    { tc->localPosition = pos; tc->dirty = true; }
}

void Scene::SetLocalRotation(EntityID id, const math::Quat& rot) noexcept
{
    if (auto* tc = m_world.Get<TransformComponent>(id))
    { tc->localRotation = rot; tc->dirty = true; }
}

void Scene::SetLocalScale(EntityID id, const math::Vec3& scale) noexcept
{
    if (auto* tc = m_world.Get<TransformComponent>(id))
    { tc->localScale = scale; tc->dirty = true; }
}

// ---------------------------------------------------------------------------
// Private
// ---------------------------------------------------------------------------

void Scene::RemoveFromRoots(EntityID id)
{
    auto it = std::find(m_rootEntities.begin(), m_rootEntities.end(), id);
    if (it != m_rootEntities.end()) m_rootEntities.erase(it);
}

void Scene::PropagateTransformRecursive(EntityID id,
                                         const math::Mat4* parentWorld,
                                         bool parentDirty)
{
    TransformComponent*      local = m_world.Get<TransformComponent>(id);
    WorldTransformComponent* world = m_world.Get<WorldTransformComponent>(id);
    if (!local || !world) return;

    const bool needsUpdate = local->dirty || parentDirty;
    if (needsUpdate)
    {
        const math::Mat4 localMat = math::Mat4::TRS(
            local->localPosition, local->localRotation, local->localScale);

        world->matrix  = parentWorld ? (*parentWorld * localMat) : localMat;
        world->inverse = world->matrix.InverseAffine();
        local->dirty   = false;
        ++local->worldVersion;
    }

    if (m_world.Has<ChildrenComponent>(id))
    {
        for (EntityID child : m_world.Get<ChildrenComponent>(id)->children)
            if (m_world.IsAlive(child))
                PropagateTransformRecursive(child, &world->matrix, needsUpdate);
    }
}

} // namespace engine
