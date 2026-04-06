// =============================================================================
// KROM Engine - src/scene/Scene.cpp
// Scene-System: Implementierung.
// =============================================================================
#include "scene/Scene.hpp"
#include "core/Debug.hpp"
#include <cassert>
#include <algorithm>

namespace engine {

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
        tc->dirty = true;
}

void Scene::DetachFromParent(EntityID child)
{
    if (!m_world.Has<ParentComponent>(child)) return;
    const EntityID parent = m_world.Get<ParentComponent>(child)->parent;
    if (m_world.IsAlive(parent) && m_world.Has<ChildrenComponent>(parent))
        m_world.Get<ChildrenComponent>(parent)->Remove(child);
    m_world.Remove<ParentComponent>(child);
    m_rootEntities.push_back(child);
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
