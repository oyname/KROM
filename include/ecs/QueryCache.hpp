#pragma once
// =============================================================================
// KROM Engine - ecs/QueryCache.hpp
// Gecachte Archetype-Matching-Ergebnisse pro ComponentSignature-Query.
//
// Problem ohne Cache:
//   world.View<A,B,C>() iteriert bei jedem Aufruf über ALLE Archetypes
//   und prüft ob sie A+B+C enthalten. O(n_archetypes) pro System-Tick.
//
// Mit QueryCache:
//   Beim ersten Aufruf wird die passende Archetype-Menge berechnet und
//   hinter der Signature gecacht. Bei jedem weiteren Aufruf O(cache_hit).
//   Invalidierung: wenn World::StructureVersion sich ändert (neuer Archetype
//   wurde angelegt), wird der Cache für betroffene Queries ungültig.
//
// Nutzung:
//   QueryCache cache;
//   // In World::View<Ts...> eingehängt oder direkt:
//   cache.Query(world, query, [](Archetype& arch) { ... });
// =============================================================================
#include "ecs/Archetype.hpp"
#include <vector>
#include <unordered_map>

namespace engine::ecs {

class World;  // forward

struct QueryResult
{
    std::vector<Archetype*> archetypes;
    uint64_t                structureVersion = 0ull; // bei welcher Version gecacht
};

class QueryCache
{
public:
    // Führt Query aus - benutzt Cache oder baut ihn neu auf.
    // Callback: void(Archetype&)
    template<typename Func>
    void Query(World& world, const ComponentSignature& sig, Func&& func);

    // Explizite Cache-Invalidierung (optional - normalerweise automatisch)
    void Invalidate() noexcept { m_cache.clear(); }

    [[nodiscard]] size_t EntryCount() const noexcept { return m_cache.size(); }

private:
    std::unordered_map<ComponentSignature, QueryResult> m_cache;

    // Baut Ergebnis neu auf und cached es
    const QueryResult& Rebuild(World& world, const ComponentSignature& sig);
};

} // namespace engine::ecs

// --- Implementierung (inline da template-abhängig) ---
#include "ecs/World.hpp"

namespace engine::ecs {

inline const QueryResult& QueryCache::Rebuild(World& world, const ComponentSignature& sig)
{
    QueryResult& result = m_cache[sig];
    result.archetypes.clear();
    result.structureVersion = world.StructureVersion();

    world.ForEachMatchingArchetype(sig, [&](Archetype& arch) {
        result.archetypes.push_back(&arch);
    });
    return result;
}

template<typename Func>
void QueryCache::Query(World& world, const ComponentSignature& sig, Func&& func)
{
    auto it = m_cache.find(sig);
    bool needsRebuild = (it == m_cache.end())
                     || (it->second.structureVersion != world.StructureVersion());

    const QueryResult& result = needsRebuild
        ? Rebuild(world, sig)
        : it->second;

    for (Archetype* arch : result.archetypes)
        func(*arch);
}

} // namespace engine::ecs
