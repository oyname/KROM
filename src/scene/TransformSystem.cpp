// =============================================================================
// KROM Engine - src/scene/TransformSystem.cpp
// =============================================================================
#include "scene/TransformSystem.hpp"
#include "core/Debug.hpp"
#include <queue>
#include <cassert>

namespace engine {

void TransformSystem::Update(ecs::World& world)
{
    // Topologie-Liste bei Hierarchie-Änderung neu aufbauen
    if (world.StructureVersion() != m_cachedVersion)
        RebuildSortedList(world);

    m_lastUpdateCount = 0u;

    for (EntityID id : m_sortedEntities)
    {
        if (!world.IsAlive(id)) continue;

        TransformComponent*      local = world.Get<TransformComponent>(id);
        WorldTransformComponent* wtc   = world.Get<WorldTransformComponent>(id);
        if (!local || !wtc) continue;

        // Eltern-WorldTransform holen (falls vorhanden)
        const WorldTransformComponent* parentWtc = nullptr;
        bool parentDirty = false;

        if (world.Has<ParentComponent>(id))
        {
            const EntityID parentId = world.Get<ParentComponent>(id)->parent;
            if (world.IsAlive(parentId))
            {
                parentWtc = world.Get<WorldTransformComponent>(parentId);
                // Parent-dirty-Flag mitziehen: wenn Elternteil sich geändert hat,
                // müssen wir auch dann neu rechnen wenn eigenes dirty nicht gesetzt
                const TransformComponent* parentLocal = world.Get<TransformComponent>(parentId);
                if (parentLocal) parentDirty = (parentLocal->worldVersion != local->worldVersion);
            }
        }

        if (!local->dirty && !parentDirty) continue;

        ComputeWorldTransform(*local, parentWtc, *wtc);
        local->dirty = false;
        ++local->worldVersion;
        ++m_lastUpdateCount;
    }
}

void TransformSystem::RebuildSortedList(ecs::World& world)
{
    m_sortedEntities.clear();

    // BFS vom Root: alle Entities ohne Parent sind Roots
    // Ebene 0 = Roots, Ebene 1 = deren Kinder, usw.
    // Damit ist garantiert: Eltern immer vor Kindern in der Liste.

    std::queue<EntityID> bfsQueue;

    // Seed: alle Entities ohne ParentComponent
    world.View<TransformComponent>([&](EntityID id, TransformComponent&) {
        if (!world.Has<ParentComponent>(id))
            bfsQueue.push(id);
    });

    m_sortedEntities.reserve(world.EntityCount());

    while (!bfsQueue.empty())
    {
        EntityID current = bfsQueue.front();
        bfsQueue.pop();

        if (!world.IsAlive(current)) continue;
        m_sortedEntities.push_back(current);

        // Kinder in Queue einreihen
        if (world.Has<ChildrenComponent>(current))
        {
            for (EntityID child : world.Get<ChildrenComponent>(current)->children)
            {
                if (world.IsAlive(child) && world.Has<TransformComponent>(child))
                    bfsQueue.push(child);
            }
        }
    }

    m_cachedVersion = world.StructureVersion();

    Debug::LogVerbose("TransformSystem.cpp: RebuildSortedList - %zu entities "
        "in topological order", m_sortedEntities.size());
}

void TransformSystem::ComputeWorldTransform(
    const TransformComponent&      local,
    const WorldTransformComponent* parentWorld,
    WorldTransformComponent&       outWorld) noexcept
{
    const math::Mat4 localMat = math::Mat4::TRS(
        local.localPosition, local.localRotation, local.localScale);

    if (parentWorld)
        outWorld.matrix = parentWorld->matrix * localMat;
    else
        outWorld.matrix = localMat;

    outWorld.inverse = outWorld.matrix.InverseAffine();
}

} // namespace engine
