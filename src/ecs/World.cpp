// =============================================================================
// KROM Engine - src/ecs/World.cpp
// ECS-World: nicht-template Implementierung.
// =============================================================================
#include "ecs/World.hpp"
#include "core/Debug.hpp"
#include <cassert>
#include <cstdlib>
#include <limits>

namespace engine::ecs {

World::World(ComponentMetaRegistry& componentMetaRegistry)
    : m_componentMetaRegistry(&componentMetaRegistry)
{
    // Reserve für typische Szenengrößen - vermeidet frühe Reallocations
    m_generations.reserve(1024u);
    m_alive.reserve(1024u);
    m_records.reserve(1024u);
    m_freeList.reserve(256u);
}

void World::BeginReadPhase() const noexcept
{
    if (m_readPhaseDepth == std::numeric_limits<uint32_t>::max())
        FatalReadPhaseViolation("World::BeginReadPhase", "read phase depth overflow");

    ++m_readPhaseDepth;
}

void World::EndReadPhase() const noexcept
{
    if (m_readPhaseDepth == 0u)
        FatalReadPhaseViolation("World::EndReadPhase", "read phase underflow");

    --m_readPhaseDepth;
}

void World::AssertStructuralMutationAllowed(const char* operation) const noexcept
{
    if (m_readPhaseDepth != 0u)
        FatalReadPhaseViolation(operation, "structural mutation during active read phase");
}

[[noreturn]] void World::FatalReadPhaseViolation(const char* operation, const char* detail) const noexcept
{
    Debug::LogError("World.cpp: %s: %s (depth=%u)", operation, detail, m_readPhaseDepth);
#ifndef NDEBUG
    assert(false && "World read phase violation");
#endif
    std::abort();
}

EntityID World::CreateEntity()
{
    AssertStructuralMutationAllowed("World::CreateEntity");
    ++m_structureVersion;
    ++m_aliveCount;

    EntityIndex idx;
    if (!m_freeList.empty())
    {
        idx = m_freeList.back();
        m_freeList.pop_back();
        EntityGeneration newGen = static_cast<EntityGeneration>(m_generations[idx] + 1u);
        if (newGen == 0u) newGen = 1u; // Generation 0 ist NULL_ENTITY-Sentinel
        m_generations[idx] = newGen;
        m_alive[idx]       = 1u;
        m_records[idx]     = EntityRecord{};
    }
    else
    {
        idx = static_cast<EntityIndex>(m_generations.size());
        m_generations.push_back(1u);
        m_alive.push_back(1u);
        m_records.emplace_back();
    }

    return EntityID::Make(idx, m_generations[idx]);
}

void World::DestroyEntity(EntityID id)
{
    AssertStructuralMutationAllowed("World::DestroyEntity");
    if (!IsAlive(id)) return;
    ++m_structureVersion;

    const EntityIndex idx = id.Index();
    EntityRecord& rec     = m_records[idx];

    if (rec.archetype)
    {
        auto swap = rec.archetype->Free(rec.chunkIndex, rec.slotIndex);
        if (swap.swapped)
        {
            EntityRecord& movedRec  = m_records[swap.movedEntity.Index()];
            movedRec.chunkIndex     = swap.newChunkIndex;
            movedRec.slotIndex      = swap.newSlotIndex;
        }
    }

    rec            = EntityRecord{};
    m_alive[idx]   = 0u;
    --m_aliveCount;
    m_freeList.push_back(idx);
}

bool World::IsAlive(EntityID id) const noexcept
{
    const EntityIndex idx = id.Index();
    if (idx >= m_generations.size()) return false;
    return m_alive[idx] != 0u && m_generations[idx] == id.Generation();
}

// ---------------------------------------------------------------------------
// Archetype-Verwaltung
// ---------------------------------------------------------------------------

Archetype* World::GetOrCreateArchetype(const ComponentSignature& sig)
{
    auto it = m_archetypes.find(sig);
    if (it != m_archetypes.end()) return it->second.get();

    // Typen-IDs aus der Signatur ableiten
    const ComponentMetaRegistry& reg = *m_componentMetaRegistry;
    std::vector<uint32_t> typeIds;
    typeIds.reserve(16u);
    for (uint32_t i = 0; i < MAX_COMPONENT_TYPES; ++i)
        if (sig.Test(i) && reg.Get(i)) typeIds.push_back(i);

    auto arch = std::make_unique<Archetype>(sig, typeIds, *m_componentMetaRegistry);
    Archetype* ptr = arch.get();
    m_archetypes.emplace(sig, std::move(arch));

    Debug::LogVerbose("World.cpp: new Archetype created (%zu types)", typeIds.size());
    return ptr;
}

// ---------------------------------------------------------------------------
// Entity-Migration zwischen Archetypes
// ---------------------------------------------------------------------------

Archetype::Slot World::MigrateEntity(EntityID id,
                                      const EntityRecord& oldRec,
                                      const ComponentSignature& /*oldSig*/,
                                      Archetype* newArch,
                                      uint32_t skipTypeId)
{
    // Slot im Ziel-Archetype allozieren
    Archetype::Slot newSlot = newArch->Allocate(id);

    if (oldRec.archetype)
    {
        // Alle gemeinsamen Komponenten move-constructen
        const ComponentSignature& newSig    = newArch->GetSignature();
        const ComponentMetaRegistry& reg    = *m_componentMetaRegistry;
        const ChunkLayout&           oldLay = oldRec.archetype->GetLayout();

        for (const auto& s : oldLay.slots)
        {
            if (s.typeId == skipTypeId)   continue; // bei Remove: nicht kopieren
            if (!newSig.Test(s.typeId))   continue; // neuer Archetype hat den Typ nicht

            const ComponentMeta* meta = reg.Get(s.typeId);
            uint8_t* src = oldRec.archetype->GetComponentPtr(
                oldRec.chunkIndex, oldRec.slotIndex, s.typeId);
            uint8_t* dst = newArch->GetComponentPtr(
                newSlot.chunkIndex, newSlot.slotIndex, s.typeId);

            // dst ist default-konstruiert → erst destrukten, dann move-construct
            meta->destruct(dst);
            meta->moveConstruct(dst, src);
        }

        // Alten Slot freigeben (migrierte Komponenten sind bereits leer)
        FreeWithoutDestruct(oldRec, skipTypeId);
    }

    return newSlot;
}

void World::FreeWithoutDestruct(const EntityRecord& rec, uint32_t skipTypeId)
{
    const auto swap = rec.archetype->FreeMigrated(rec.chunkIndex, rec.slotIndex, skipTypeId);
    if (swap.swapped)
    {
        // Record der verschobenen Entity aktualisieren
        EntityRecord& movedRec = m_records[swap.movedEntity.Index()];
        movedRec.chunkIndex    = swap.newChunkIndex;
        movedRec.slotIndex     = swap.newSlotIndex;
    }
}

} // namespace engine::ecs
