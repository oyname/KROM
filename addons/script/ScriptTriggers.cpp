#include "addons/script/ScriptTriggers.hpp"
#include "collision/SceneQueries.hpp"

namespace engine::script {

namespace {

EntityID RootOf(const ecs::World& world, EntityID entity)
{
    EntityID cursor = entity;
    for (uint32_t depth = 0; depth < 1024u; ++depth)
    {
        const auto* parent = world.Get<ParentComponent>(cursor);
        if (!parent || !world.IsAlive(parent->parent))
            break;
        cursor = parent->parent;
    }
    return cursor;
}

struct Collider
{
    EntityID        root;
    collision::AABB aabb;
};

void Notify(ecs::World& world, bool enter, EntityID self, EntityID other, const ScriptServices& services)
{
    if (!world.IsAlive(self))
        return;
    if (auto* scripts = world.Get<ScriptList>(self))
        scripts->NotifyTrigger(enter, self, other, services);
}

} // namespace

void ScriptTriggerSystem::Update(ecs::World& world, const ScriptServices& services)
{
    // Trigger-Volumen sammeln
    std::vector<std::pair<EntityID, collision::AABB>> triggers;
    world.View<OBBComponent>([&](EntityID id, const OBBComponent& obb) {
        if (!obb.isTrigger)
            return;
        if (const auto* transform = world.Get<WorldTransformComponent>(id))
            triggers.emplace_back(id, collision::ComputeWorldAABB(obb, *transform));
    });

    std::set<Contact, ContactLess> current;
    if (!triggers.empty())
    {
        // Gegenseite: Box-Kollisionen haben Vorrang vor Mesh-Bounds (wie SceneQueries::BuildColliders)
        std::vector<Collider> colliders;
        world.View<BoundsComponent>([&](EntityID id, const BoundsComponent& bounds) {
            if (world.Has<OBBComponent>(id))
                return;
            colliders.push_back({RootOf(world, id),
                {bounds.centerWorld - bounds.extentsWorld, bounds.centerWorld + bounds.extentsWorld}});
        });
        world.View<OBBComponent>([&](EntityID id, const OBBComponent& obb) {
            if (obb.isTrigger)
                return;
            if (const auto* transform = world.Get<WorldTransformComponent>(id))
                colliders.push_back({RootOf(world, id), collision::ComputeWorldAABB(obb, *transform)});
        });

        for (const auto& [trigger, triggerBox] : triggers)
        {
            const EntityID triggerRoot = RootOf(world, trigger);
            for (const Collider& collider : colliders)
            {
                if (collider.root == triggerRoot)
                    continue;
                if (collision::SceneQueries::Intersects(triggerBox, collider.aabb))
                    current.insert({trigger, collider.root});
            }
        }
    }

    // Callbacks erst nach der Erkennung — Scripts duerfen dabei die World veraendern.
    for (const Contact& contact : current)
    {
        if (m_active.count(contact))
            continue;
        Notify(world, true, contact.first, contact.second, services);
        Notify(world, true, contact.second, contact.first, services);
    }
    for (const Contact& contact : m_active)
    {
        if (current.count(contact))
            continue;
        Notify(world, false, contact.first, contact.second, services);
        Notify(world, false, contact.second, contact.first, services);
    }
    m_active = std::move(current);
}

void DispatchCollision(ecs::World& world, bool enter, EntityID a, EntityID b,
                       const ScriptServices& services)
{
    auto notify = [&](EntityID self, EntityID other) {
        if (!world.IsAlive(self))
            return;
        const EntityID otherRoot = world.IsAlive(other) ? RootOf(world, other) : other;
        if (auto* scripts = world.Get<ScriptList>(self))
            scripts->NotifyCollision(enter, self, otherRoot, services);
    };
    notify(a, b);
    notify(b, a);
}

} // namespace engine::script
