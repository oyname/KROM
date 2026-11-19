#pragma once
#include "ecs/Components.hpp"
#include "ecs/World.hpp"

namespace engine {

// Evaluate local state without overwriting descendants. Also bounds malformed
// parent chains so loading a broken hierarchy cannot hang a frame.
template<class Predicate>
[[nodiscard]] bool AllHierarchyAncestors(const ecs::World& world, EntityID entity,
                                         Predicate accepts) noexcept
{
    for (uint32_t depth = 0; depth < 1024u && world.IsAlive(entity); ++depth)
    {
        if (!accepts(entity))
            return false;
        const auto* parent = world.Get<ParentComponent>(entity);
        if (!parent || !world.IsAlive(parent->parent))
            return true;
        entity = parent->parent;
    }
    return false;
}

[[nodiscard]] inline bool IsEntityActiveInHierarchy(const ecs::World& world, EntityID entity) noexcept
{
    return AllHierarchyAncestors(world, entity, [&](EntityID id) {
        const auto* active = world.Get<ActiveComponent>(id);
        return !active || active->active;
    });
}

[[nodiscard]] inline bool IsEntityVisibleInHierarchy(const ecs::World& world, EntityID entity) noexcept
{
    return AllHierarchyAncestors(world, entity, [&](EntityID id) {
        const auto* visible = world.Get<VisibilityComponent>(id);
        return !visible || visible->visible;
    });
}

} // namespace engine
