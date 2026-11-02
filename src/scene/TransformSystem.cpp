// =============================================================================
// KROM Engine - src/scene/TransformSystem.cpp
// =============================================================================
#include "scene/TransformSystem.hpp"
#include "core/Debug.hpp"
#include <queue>
#include <cassert>
#include <unordered_map>

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
                if (parentLocal) parentDirty = (parentLocal->worldVersion != local->parentWorldVersion);
            }
        }

        if (!local->dirty && !parentDirty) continue;

        ComputeWorldTransform(*local, parentWtc, *wtc);
        local->dirty = false;
        ++local->worldVersion;
        if (world.Has<ParentComponent>(id))
        {
            const EntityID parentId = world.Get<ParentComponent>(id)->parent;
            if (const TransformComponent* parentLocal = world.Get<TransformComponent>(parentId))
                local->parentWorldVersion = parentLocal->worldVersion;
            else
                local->parentWorldVersion = 0u;
        }
        else
        {
            local->parentWorldVersion = 0u;
        }
        ++m_lastUpdateCount;
    }
}

void TransformSystem::RebuildSortedList(ecs::World& world)
{
    m_sortedEntities.clear();

    // ParentComponent is the authoritative hierarchy, just as in the outliner.
    // ChildrenComponent is an optional reverse cache and can be incomplete or
    // stale after loading/reparenting. Never omit a sibling because of that cache.
    std::unordered_map<uint32_t, std::vector<EntityID>> childrenByParent;
    std::queue<EntityID> bfsQueue;

    world.View<TransformComponent>([&](EntityID id, TransformComponent&) {
        const auto* parent = world.Get<ParentComponent>(id);
        if (parent && parent->parent != id && world.IsAlive(parent->parent) &&
            world.Has<TransformComponent>(parent->parent))
            childrenByParent[parent->parent.value].push_back(id);
        else
            bfsQueue.push(id);
    });

    m_sortedEntities.reserve(world.EntityCount());

    while (!bfsQueue.empty())
    {
        EntityID current = bfsQueue.front();
        bfsQueue.pop();

        if (!world.IsAlive(current)) continue;
        m_sortedEntities.push_back(current);

        if (const auto it = childrenByParent.find(current.value); it != childrenByParent.end())
            for (const auto child : it->second) bfsQueue.push(child);
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
    if (parentWorld)
    {
        const math::Vec3 localOffset = local.inheritParentScale
            ? math::Vec3{
                parentWorld->scale.x * local.localPosition.x,
                parentWorld->scale.y * local.localPosition.y,
                parentWorld->scale.z * local.localPosition.z
              }
            : local.localPosition;

        outWorld.position = parentWorld->position + parentWorld->rotation.Rotate(localOffset);
        outWorld.rotation = (parentWorld->rotation * local.localRotation).Normalized();
        outWorld.scale = local.inheritParentScale
            ? math::Vec3{
                parentWorld->scale.x * local.localScale.x,
                parentWorld->scale.y * local.localScale.y,
                parentWorld->scale.z * local.localScale.z
              }
            : local.localScale;
    }
    else
    {
        outWorld.position = local.localPosition;
        outWorld.rotation = local.localRotation;
        outWorld.scale    = local.localScale;
    }

    outWorld.matrix = math::Mat4::TRS(outWorld.position, outWorld.rotation, outWorld.scale);

    // InverseAffine() ist nur korrekt für TRS-Matrizen OHNE Scherung
    // (d.h. uniforme Skalierung oder keine Rotation).
    // Bei nicht-uniformer Skalierung + Rotation – sei es in einer einzelnen
    // Entity oder durch einen nicht-uniform-skalierten Parent mit rotiertem Child –
    // enthält die Weltmatrix Scherung, und InverseAffine liefert ein falsches
    // Ergebnis. Das führt zu falsch transformierten Normalen im Shader (Verzerrung).
    // Inverse() verwendet die allgemeine Cramer-Regel und ist für alle Fälle korrekt.
    outWorld.inverse = outWorld.matrix.InverseAffine();
}

} // namespace engine
