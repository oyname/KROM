#include "scene/BoundsSystem.hpp"
#include "ecs/Components.hpp"
#include "jobs/JobSystem.hpp"
#include "scene/TransformSystem.hpp"
#include <cmath>
#include <vector>

namespace engine {

void BoundsSystem::TransformAABB(const math::Vec3& localCenter,
                                 const math::Vec3& localExtents,
                                 const math::Mat4& worldMatrix,
                                 math::Vec3& outWorldCenter,
                                 math::Vec3& outWorldExtents) noexcept
{
    outWorldCenter = worldMatrix.TransformPoint(localCenter);

    const math::Mat4& m = worldMatrix;
    outWorldExtents.x =
        std::abs(m.m[0][0]) * localExtents.x +
        std::abs(m.m[1][0]) * localExtents.y +
        std::abs(m.m[2][0]) * localExtents.z;
    outWorldExtents.y =
        std::abs(m.m[0][1]) * localExtents.x +
        std::abs(m.m[1][1]) * localExtents.y +
        std::abs(m.m[2][1]) * localExtents.z;
    outWorldExtents.z =
        std::abs(m.m[0][2]) * localExtents.x +
        std::abs(m.m[1][2]) * localExtents.y +
        std::abs(m.m[2][2]) * localExtents.z;
}

void BoundsSystem::Update(ecs::World& world)
{
    world.View<WorldTransformComponent, BoundsComponent, TransformComponent>(
        [&](EntityID,
            const WorldTransformComponent& worldTransform,
            BoundsComponent& bounds,
            const TransformComponent& transform)
        {
            if (transform.worldVersion == bounds.lastTransformVersion && !bounds.localDirty)
                return;

            TransformAABB(bounds.centerLocal,
                          bounds.extentsLocal,
                          worldTransform.matrix,
                          bounds.centerWorld,
                          bounds.extentsWorld);

            bounds.boundingSphere = bounds.extentsWorld.Length();
            bounds.lastTransformVersion = transform.worldVersion;
            bounds.localDirty = false;
        });
}

void BoundsSystem::Update(ecs::World& world, jobs::JobSystem& js)
{
    std::vector<EntityID> entities;
    world.View<WorldTransformComponent, BoundsComponent, TransformComponent>(
        [&](EntityID id, const WorldTransformComponent&, BoundsComponent&, const TransformComponent&)
        {
            entities.push_back(id);
        });

    if (entities.empty())
        return;

    const auto result = js.ParallelFor(entities.size(), [&](size_t begin, size_t end)
    {
        for (size_t i = begin; i < end; ++i)
        {
            const EntityID id = entities[i];
            const auto* wt     = world.Get<WorldTransformComponent>(id);
            auto*       bounds = world.Get<BoundsComponent>(id);
            const auto* tc     = world.Get<TransformComponent>(id);
            if (!wt || !bounds || !tc) continue;

            if (tc->worldVersion == bounds->lastTransformVersion && !bounds->localDirty)
                continue;

            TransformAABB(bounds->centerLocal, bounds->extentsLocal, wt->matrix,
                          bounds->centerWorld, bounds->extentsWorld);

            bounds->boundingSphere      = bounds->extentsWorld.Length();
            bounds->lastTransformVersion = tc->worldVersion;
            bounds->localDirty          = false;
        }
    });
    if (!result.Succeeded())
        Debug::LogError("BoundsSystem::Update parallel pass failed");
}

namespace {

// True when the stored WorldTransformComponent of `entity` is exactly what
// TransformSystem::ComputeCurrentWorldTransform() would compute now: no entity
// in the parent chain is dirty or behind its parent's world version (the
// conditions under which TransformSystem::Update() would recompute it), and
// every link follows the parent rules of ComputeCurrentWorldTransform().
// Anything unusual (self-parent, parent without transform components, a
// cycle, a never-updated entity) answers false and takes the full path.
bool StoredWorldTransformIsCurrent(const ecs::World& world, EntityID entity) noexcept
{
    EntityID cursor = entity;
    for (uint32_t depth = 0u; depth < 1024u; ++depth)
    {
        const TransformComponent* local = world.Get<TransformComponent>(cursor);
        if (!local || !world.Has<WorldTransformComponent>(cursor) ||
            local->dirty || local->worldVersion == 0u)
            return false;

        EntityID parent = NULL_ENTITY;
        if (const auto* link = world.Get<ParentComponent>(cursor);
            link && world.IsAlive(link->parent))
        {
            // Update() and ComputeCurrentWorldTransform() treat these differently.
            if (link->parent == cursor ||
                !world.Has<TransformComponent>(link->parent) ||
                !world.Has<WorldTransformComponent>(link->parent))
                return false;
            parent = link->parent;
        }

        if (!parent.IsValid())
            // Root now. A non-zero parent version means it was last computed as a
            // child (detached without dirty): stored value is not the root value.
            return local->parentWorldVersion == 0u;

        if (local->parentWorldVersion != world.Get<TransformComponent>(parent)->worldVersion)
            return false;
        cursor = parent;
    }
    return false;   // parent cycle
}

} // namespace

bool BoundsSystem::ComputeCurrentWorldBounds(const ecs::World& world, EntityID id,
                                             math::Vec3& outCenter, math::Vec3& outExtents) noexcept
{
    const BoundsComponent* bounds = world.Get<BoundsComponent>(id);   // null when dead
    if (!bounds)
        return false;

    const TransformComponent* transform = world.Get<TransformComponent>(id);
    const WorldTransformComponent* worldTransform = world.Get<WorldTransformComponent>(id);
    if (transform && worldTransform)
    {
        if (!bounds->localDirty && bounds->lastTransformVersion == transform->worldVersion &&
            StoredWorldTransformIsCurrent(world, id))
        {
            outCenter = bounds->centerWorld;
            outExtents = bounds->extentsWorld;
            return true;
        }
        const WorldTransformComponent current = TransformSystem::ComputeCurrentWorldTransform(world, id);
        TransformAABB(bounds->centerLocal, bounds->extentsLocal, current.matrix, outCenter, outExtents);
        return true;
    }
    if (worldTransform)
    {
        TransformAABB(bounds->centerLocal, bounds->extentsLocal, worldTransform->matrix, outCenter, outExtents);
        return true;
    }
    outCenter = bounds->centerWorld;
    outExtents = bounds->extentsWorld;
    return true;
}

void BoundsSystem::ComputeBoundsForEntity(ecs::World& world,
                                          EntityID id) noexcept
{
    if (!world.IsAlive(id))
        return;

    const auto* worldTransform = world.Get<WorldTransformComponent>(id);
    auto* bounds = world.Get<BoundsComponent>(id);
    const auto* transform = world.Get<TransformComponent>(id);
    if (!worldTransform || !bounds || !transform)
        return;

    TransformAABB(bounds->centerLocal,
                  bounds->extentsLocal,
                  worldTransform->matrix,
                  bounds->centerWorld,
                  bounds->extentsWorld);

    bounds->boundingSphere = bounds->extentsWorld.Length();
    bounds->lastTransformVersion = transform->worldVersion;
    bounds->localDirty = false;
}

} // namespace engine
