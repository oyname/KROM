#pragma once
// =============================================================================
// KROM Engine - ecs/World.hpp
// ECS-World: Entity-Verwaltung, Archetype-Migration, Query-Dispatch.
// Nicht-template-Methoden → src/ecs/World.cpp
// Template-Methoden (Add, Remove, Get, Has, View) bleiben im Header.
// =============================================================================
#include "ecs/Archetype.hpp"
#include <unordered_map>
#include <memory>
#include <functional>
#include <utility>
#include <cassert>
#include <cstdint>

namespace engine::ecs {

struct EntityRecord
{
    Archetype* archetype  = nullptr;
    uint32_t   chunkIndex = 0u;
    uint32_t   slotIndex  = 0u;
};

class World
{
public:
    class ScopedReadPhase
    {
    public:
        explicit ScopedReadPhase(const World& world) noexcept
            : m_world(&world)
        {
            m_world->BeginReadPhase();
        }

        ~ScopedReadPhase()
        {
            if (m_world)
                m_world->EndReadPhase();
        }

        ScopedReadPhase(const ScopedReadPhase&)            = delete;
        ScopedReadPhase& operator=(const ScopedReadPhase&) = delete;

        ScopedReadPhase(ScopedReadPhase&& other) noexcept
            : m_world(other.m_world)
        {
            other.m_world = nullptr;
        }

        ScopedReadPhase& operator=(ScopedReadPhase&& other) noexcept
        {
            if (this == &other) return *this;

            if (m_world)
                m_world->EndReadPhase();

            m_world       = other.m_world;
            other.m_world = nullptr;
            return *this;
        }

    private:
        const World* m_world = nullptr;
    };

     World();
    ~World() = default;
    World(const World&)            = delete;
    World& operator=(const World&) = delete;

    // -------------------------------------------------------------------------
    // Entity-Lifecycle (impl in World.cpp)
    // -------------------------------------------------------------------------
    [[nodiscard]] EntityID CreateEntity();
    void                   DestroyEntity(EntityID id);
    [[nodiscard]] bool     IsAlive(EntityID id) const noexcept;
    [[nodiscard]] size_t   EntityCount()        const noexcept { return m_aliveCount; }
    [[nodiscard]] uint64_t StructureVersion()   const noexcept { return m_structureVersion; }
    [[nodiscard]] size_t   ArchetypeCount()     const noexcept { return m_archetypes.size(); }

    void BeginReadPhase() const noexcept;
    void EndReadPhase() const noexcept;
    [[nodiscard]] bool IsReadPhaseActive() const noexcept { return m_readPhaseDepth != 0u; }
    [[nodiscard]] uint32_t ReadPhaseDepth() const noexcept { return m_readPhaseDepth; }
    [[nodiscard]] ScopedReadPhase ReadPhaseScope() const noexcept { return ScopedReadPhase(*this); }

    // Iteriert alle lebendigen Entities - unabhängig von Komponenten.
    // Wird vom Serializer genutzt um keine Entity zu überspringen.
    template<typename Func>
    void ForEachAlive(Func&& func) const
    {
        for (uint32_t i = 0u; i < static_cast<uint32_t>(m_alive.size()); ++i)
        {
            if (m_alive[i] == 0u) continue;
            const EntityID id = EntityID::Make(i, m_generations[i]);
            func(id);
        }
    }

    // -------------------------------------------------------------------------
    // Komponenten - template, bleibt im Header
    // -------------------------------------------------------------------------
    template<typename T, typename... Args>
    T& Add(EntityID id, Args&&... args)
    {
        AssertStructuralMutationAllowed("World::Add");
        assert(IsAlive(id) && "World::Add: entity not alive");
        ++m_structureVersion;
        const uint32_t typeId = ComponentTypeID<T>::value;
        EntityRecord&  rec    = m_records[id.Index()];

        ComponentSignature oldSig = rec.archetype ? rec.archetype->GetSignature()
                                                  : ComponentSignature{};
        assert(!oldSig.Test(typeId) && "World::Add: component already present");

        ComponentSignature newSig  = oldSig.With(typeId);
        Archetype*         newArch = GetOrCreateArchetype(newSig);
        Archetype::Slot    newSlot = MigrateEntity(id, rec, oldSig, newArch);

        // Default-konstruierte Instanz mit übergebenen Werten überschreiben
        uint8_t* ptr = newArch->GetComponentPtr(newSlot.chunkIndex, newSlot.slotIndex, typeId);
        static_cast<T*>(static_cast<void*>(ptr))->~T();
        new(ptr) T(std::forward<Args>(args)...);

        rec.archetype  = newArch;
        rec.chunkIndex = newSlot.chunkIndex;
        rec.slotIndex  = newSlot.slotIndex;
        return *static_cast<T*>(static_cast<void*>(ptr));
    }

    template<typename T>
    void Remove(EntityID id)
    {
        AssertStructuralMutationAllowed("World::Remove");
        assert(IsAlive(id) && "World::Remove: entity not alive");
        ++m_structureVersion;
        const uint32_t typeId = ComponentTypeID<T>::value;
        EntityRecord&  rec    = m_records[id.Index()];
        assert(rec.archetype && rec.archetype->GetSignature().Test(typeId));

        ComponentSignature oldSig  = rec.archetype->GetSignature();
        ComponentSignature newSig  = oldSig.Without(typeId);
        Archetype*         newArch = GetOrCreateArchetype(newSig);
        Archetype::Slot    newSlot = MigrateEntity(id, rec, oldSig, newArch, typeId);

        rec.archetype  = newArch;
        rec.chunkIndex = newSlot.chunkIndex;
        rec.slotIndex  = newSlot.slotIndex;
    }

    template<typename T>
    [[nodiscard]] T* Get(EntityID id) noexcept
    {
        if (!IsAlive(id)) return nullptr;
        const EntityRecord& rec = m_records[id.Index()];
        if (!rec.archetype) return nullptr;
        uint8_t* p = rec.archetype->GetComponentPtr(
            rec.chunkIndex, rec.slotIndex, ComponentTypeID<T>::value);
        return p ? static_cast<T*>(static_cast<void*>(p)) : nullptr;
    }

    template<typename T>
    [[nodiscard]] const T* Get(EntityID id) const noexcept
    {
        if (!IsAlive(id)) return nullptr;
        const EntityRecord& rec = m_records[id.Index()];
        if (!rec.archetype) return nullptr;
        const uint8_t* p = rec.archetype->GetComponentPtr(
            rec.chunkIndex, rec.slotIndex, ComponentTypeID<T>::value);
        return p ? static_cast<const T*>(static_cast<const void*>(p)) : nullptr;
    }

    template<typename T>
    [[nodiscard]] bool Has(EntityID id) const noexcept
    {
        if (!IsAlive(id)) return false;
        const EntityRecord& rec = m_records[id.Index()];
        return rec.archetype && rec.archetype->GetSignature().Test(ComponentTypeID<T>::value);
    }

    // -------------------------------------------------------------------------
    // View<Ts...> - iteriert alle passenden Archetypes
    // -------------------------------------------------------------------------
    template<typename... Ts, typename Func>
    void View(Func&& func)
    {
        ComponentSignature query{};
        (query.Set(ComponentTypeID<Ts>::value), ...);
        for (auto& [sig, arch] : m_archetypes)
            if (sig.Contains(query))
                IterateArchetype<Ts...>(*arch, std::forward<Func>(func));
    }

    template<typename... Ts, typename Func>
    void View(Func&& func) const
    {
        ComponentSignature query{};
        (query.Set(ComponentTypeID<Ts>::value), ...);
        for (const auto& [sig, arch] : m_archetypes)
            if (sig.Contains(query))
                IterateArchetypeConst<Ts...>(*arch, std::forward<Func>(func));
    }

    // Für QueryCache: iteriert alle Archetypes die die Signatur erfüllen
    void ForEachMatchingArchetype(const ComponentSignature& sig,
                                   const std::function<void(Archetype&)>& fn)
    {
        for (auto& [s, arch] : m_archetypes)
            if (s.Contains(sig)) fn(*arch);
    }

private:
    void AssertStructuralMutationAllowed(const char* operation) const noexcept;
    [[noreturn]] void FatalReadPhaseViolation(const char* operation, const char* detail) const noexcept;

    std::vector<EntityGeneration> m_generations;
    std::vector<uint8_t>          m_alive;
    std::vector<EntityRecord>     m_records;
    std::vector<EntityIndex>      m_freeList;
    size_t                        m_aliveCount       = 0u;
    uint64_t                      m_structureVersion = 0ull;
    mutable uint32_t              m_readPhaseDepth   = 0u;

    std::unordered_map<ComponentSignature, std::unique_ptr<Archetype>> m_archetypes;

    // Non-template - deklariert, impl in World.cpp
    Archetype*      GetOrCreateArchetype(const ComponentSignature& sig);
    Archetype::Slot MigrateEntity(EntityID id, const EntityRecord& oldRec,
                                   const ComponentSignature& oldSig,
                                   Archetype* newArch,
                                   uint32_t skipTypeId = UINT32_MAX);
    void            FreeWithoutDestruct(const EntityRecord& rec,
                                         uint32_t skipTypeId);

    // -------------------------------------------------------------------------
    // Template-Iteration (muss im Header bleiben)
    // -------------------------------------------------------------------------
    template<typename... Ts, size_t... Is, typename Func>
    void IterateArchetypeImpl(Archetype& arch, std::index_sequence<Is...>, Func&& func)
    {
        const ChunkLayout& layout = arch.GetLayout();
        const size_t offsets[sizeof...(Ts)] = {
            layout.GetComponentOffset(ComponentTypeID<Ts>::value)...
        };
        const uint32_t numChunks = arch.ChunkCount();
        for (uint32_t ci = 0; ci < numChunks; ++ci)
        {
            Chunk*         chunk = arch.GetChunk(ci);
            const uint32_t n     = chunk->count;
            EntityID*      ids   = chunk->EntityIDs(layout.entityIdOffset);
            for (uint32_t si = 0; si < n; ++si)
                func(ids[si], *GetSlot<Ts>(chunk, offsets[Is], si)...);
        }
    }

    template<typename... Ts, typename Func>
    void IterateArchetype(Archetype& arch, Func&& func)
    {
        IterateArchetypeImpl<Ts...>(arch,
            std::index_sequence_for<Ts...>{},
            std::forward<Func>(func));
    }

    template<typename... Ts, size_t... Is, typename Func>
    void IterateArchetypeImplConst(const Archetype& arch, std::index_sequence<Is...>, Func&& func) const
    {
        const ChunkLayout& layout = arch.GetLayout();
        const size_t offsets[sizeof...(Ts)] = {
            layout.GetComponentOffset(ComponentTypeID<Ts>::value)...
        };
        const uint32_t numChunks = arch.ChunkCount();
        for (uint32_t ci = 0; ci < numChunks; ++ci)
        {
            const Chunk*   chunk = arch.GetChunk(ci);
            const uint32_t n     = chunk->count;
            const EntityID* ids  = chunk->EntityIDs(layout.entityIdOffset);
            for (uint32_t si = 0; si < n; ++si)
                func(ids[si], *GetSlotConst<Ts>(chunk, offsets[Is], si)...);
        }
    }

    template<typename... Ts, typename Func>
    void IterateArchetypeConst(const Archetype& arch, Func&& func) const
    {
        IterateArchetypeImplConst<Ts...>(arch,
            std::index_sequence_for<Ts...>{},
            std::forward<Func>(func));
    }

    template<typename T>
    static T* GetSlot(Chunk* chunk, size_t offset, uint32_t slot) noexcept
    {
        return reinterpret_cast<T*>(chunk->data + offset + slot * sizeof(T));
    }

    template<typename T>
    static const T* GetSlotConst(const Chunk* chunk, size_t offset, uint32_t slot) noexcept
    {
        return reinterpret_cast<const T*>(chunk->data + offset + slot * sizeof(T));
    }
};

} // namespace engine::ecs
