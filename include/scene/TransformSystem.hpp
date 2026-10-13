#pragma once
// =============================================================================
// KROM Engine - scene/TransformSystem.hpp
// Breadth-first Transform-Propagation mit Dirty-Tracking.
//
// Probleme der naiven rekursiven Lösung in Scene.cpp:
//   - Keine Guarantie dass Eltern vor Kindern verarbeitet werden wenn
//     mehrere Roots existieren und sie verschieden tief sind
//   - Kein explizites Dirty-Subtree-Management (ganzer Baum immer rekursiert)
//   - Keine Grundlage für Parallelisierung
//
// Diese Lösung:
//   1. Baut einmalig eine Topologie-sortierte Entities-Liste (Ebene-für-Ebene)
//   2. Invalidiert nur die Liste wenn sich Hierarchie ändert (StructureVersion)
//   3. Propagiert in exakt einem Vorwärts-Durchlauf (kein Rekursion)
//   4. Überspringt nicht-dirty Subtrees
//
// Deklaration. Implementierung: src/scene/TransformSystem.cpp
// =============================================================================
#include "ecs/World.hpp"
#include "ecs/Components.hpp"
#include <vector>
#include <cstdint>

namespace engine {

class TransformSystem
{
public:
    TransformSystem() = default;

    // Hauptmethode: propagiert alle schmutzigen Transforms.
    // Baut die Sorted-Entities-Liste neu auf wenn Hierarchie sich geändert hat.
    void Update(ecs::World& world);

    // Zwingt Neuaufbau der Topologie beim nächsten Update()
    void Invalidate() noexcept { m_cachedVersion = UINT64_MAX; }

    [[nodiscard]] size_t  SortedEntityCount() const noexcept { return m_sortedEntities.size(); }
    [[nodiscard]] uint32_t UpdateCount()      const noexcept { return m_lastUpdateCount; }

private:
    // Sortierte Entity-Reihenfolge: Eltern immer vor Kindern
    std::vector<EntityID> m_sortedEntities;
    uint64_t              m_cachedVersion   = UINT64_MAX;
    uint32_t              m_lastUpdateCount = 0u;

    // Topologie-Sortierung: Breiten-zuerst-Traversal aller Hierarchien
    void RebuildSortedList(ecs::World& world);

    // Berechnet lokale → World-Matrix für eine Entity
    static void ComputeWorldTransform(
        const TransformComponent& local,
        const WorldTransformComponent* parentWorld,
        WorldTransformComponent& outWorld) noexcept;
};

} // namespace engine
