#pragma once
// =============================================================================
// KROM Engine - ecs/EntityCommandBuffer.hpp
// Puffert strukturelle ECS-Änderungen (Add/Remove/Create/Destroy) während
// einer Iteration. Erst nach Commit werden sie auf die World angewendet.
//
// Thread-sicheres Aufzeichnen (mehrere Threads schreiben gleichzeitig),
// single-threaded Commit auf dem Haupt-Thread.
//
// Typische Nutzung in einem System:
//   EntityCommandBuffer ecb;
//   world.View<Foo, Bar>([&](EntityID id, Foo& f, Bar& b) {
//       if (f.shouldExplode)
//           ecb.DestroyEntity(id);
//       if (b.needsLight)
//           ecb.AddComponent<LightComponent>(id, LightComponent{...});
//   });
//   ecb.Commit(world);    // strukturelle Änderungen jetzt anwenden
// =============================================================================
#include "ecs/World.hpp"
#include <vector>
#include <mutex>
#include <functional>
#include <variant>

namespace engine::ecs {

class EntityCommandBuffer
{
public:
    EntityCommandBuffer()  = default;
    ~EntityCommandBuffer() = default;

    // Kein Copy - die gespeicherten Lambdas könnten Referenzen halten
    EntityCommandBuffer(const EntityCommandBuffer&) = delete;
    EntityCommandBuffer& operator=(const EntityCommandBuffer&) = delete;

    // Move erlaubt (für temporäre Puffer)
    EntityCommandBuffer(EntityCommandBuffer&&)            = default;
    EntityCommandBuffer& operator=(EntityCommandBuffer&&) = default;

    // -------------------------------------------------------------------------
    // Aufzeichnungs-API (thread-sicher via Mutex)
    // -------------------------------------------------------------------------

    void CreateEntity(std::function<void(EntityID)> onCreated = nullptr)
    {
        std::lock_guard lock(m_mutex);
        m_commands.push_back(CreateCmd{ std::move(onCreated) });
    }

    void DestroyEntity(EntityID id)
    {
        std::lock_guard lock(m_mutex);
        m_commands.push_back(DestroyCmd{ id });
    }

    template<typename T, typename... Args>
    void AddComponent(EntityID id, Args&&... args)
    {
        // Bindet Argumente in einen Lambda - erfordert kopierbare Typen
        // Für move-only Typen: AddComponent(id, T{...}) mit explizitem Wert
        auto value = T(std::forward<Args>(args)...);
        std::lock_guard lock(m_mutex);
        m_commands.push_back(ComponentCmd{
            id,
            ComponentCmd::Add,
            [v = std::move(value)](World& w, EntityID eid) mutable {
                if (w.IsAlive(eid) && !w.Has<T>(eid))
                    w.Add<T>(eid, std::move(v));
            }
        });
    }

    template<typename T>
    void RemoveComponent(EntityID id)
    {
        std::lock_guard lock(m_mutex);
        m_commands.push_back(ComponentCmd{
            id,
            ComponentCmd::Remove,
            [](World& w, EntityID eid) {
                if (w.IsAlive(eid) && w.Has<T>(eid))
                    w.Remove<T>(eid);
            }
        });
    }

    // Generischer Befehl für komplexere Operationen
    void Custom(std::function<void(World&)> fn)
    {
        std::lock_guard lock(m_mutex);
        m_commands.push_back(CustomCmd{ std::move(fn) });
    }

    // -------------------------------------------------------------------------
    // Commit: alle gepufferten Befehle auf World anwenden
    // Muss auf dem Haupt-Thread aufgerufen werden.
    // -------------------------------------------------------------------------
    void Commit(World& world)
    {
        // Kein Lock nötig - Commit ist single-threaded
        for (auto& cmd : m_commands)
        {
            std::visit([&](auto& c) { Apply(c, world); }, cmd);
        }
        m_commands.clear();
    }

    void Clear() noexcept { m_commands.clear(); }

    [[nodiscard]] size_t  PendingCount() const noexcept { return m_commands.size(); }
    [[nodiscard]] bool    Empty()        const noexcept { return m_commands.empty(); }

private:
    struct CreateCmd
    {
        std::function<void(EntityID)> onCreated;
    };

    struct DestroyCmd
    {
        EntityID entity;
    };

    struct ComponentCmd
    {
        enum Op : uint8_t { Add, Remove } op;
        EntityID entity;
        std::function<void(World&, EntityID)> apply;

        ComponentCmd(EntityID id, Op o, std::function<void(World&, EntityID)> fn)
            : op(o), entity(id), apply(std::move(fn)) {}
    };

    struct CustomCmd
    {
        std::function<void(World&)> fn;
    };

    using Command = std::variant<CreateCmd, DestroyCmd, ComponentCmd, CustomCmd>;

    void Apply(CreateCmd& cmd, World& world)
    {
        EntityID id = world.CreateEntity();
        if (cmd.onCreated) cmd.onCreated(id);
    }

    void Apply(DestroyCmd& cmd, World& world)
    {
        if (world.IsAlive(cmd.entity))
            world.DestroyEntity(cmd.entity);
    }

    void Apply(ComponentCmd& cmd, World& world)
    {
        cmd.apply(world, cmd.entity);
    }

    void Apply(CustomCmd& cmd, World& world)
    {
        cmd.fn(world);
    }

    mutable std::mutex    m_mutex;
    std::vector<Command>  m_commands;
};

} // namespace engine::ecs
