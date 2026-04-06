// =============================================================================
// KROM Engine - src/ecs/Archetype.cpp
// Archetype/Chunk-Implementierung.
// =============================================================================
#include "ecs/Archetype.hpp"
#include "core/Debug.hpp"
#include <cassert>
#include <algorithm>

namespace engine::ecs {

// =============================================================================
// ChunkLayout
// =============================================================================

size_t ChunkLayout::GetComponentOffset(uint32_t typeId) const noexcept
{
    for (const auto& s : slots)
        if (s.typeId == typeId) return s.offsetInChunk;
    return SIZE_MAX;
}

ChunkLayout ChunkLayout::Build(const std::vector<uint32_t>& typeIds)
{
    const ComponentMetaRegistry& reg = ComponentMetaRegistry::Instance();
    ChunkLayout layout;

    // Schritt 1: Konservative Kapazitäts-Schätzung (ohne Alignment-Padding)
    size_t rowSize = sizeof(EntityID);
    for (uint32_t tid : typeIds)
    {
        const ComponentMeta* meta = reg.Get(tid);
        assert(meta && "Archetype::Build: unregistered component type - call RegisterComponent<T>() first");
        rowSize += meta->size;
    }
    size_t capacity = (rowSize > 0u) ? (CHUNK_SIZE / rowSize) : 0u;
    if (capacity == 0u) capacity = 1u;

    // Schritt 2: Exaktes SoA-Layout mit Alignment-Padding
    // Wird iterativ angepasst wenn Padding die Kapazität unterschreitet
    bool layoutOk = false;
    while (!layoutOk)
    {
        layout.slots.clear();
        layout.entityIdOffset = 0u;
        size_t offset = sizeof(EntityID) * capacity;

        bool fits = true;
        for (uint32_t tid : typeIds)
        {
            const ComponentMeta* meta = reg.Get(tid);
            const size_t align = meta->alignment;

            // Alignment-Padding
            offset = (offset + align - 1u) & ~(align - 1u);

            if (offset + meta->size * capacity > CHUNK_SIZE)
            {
                // Passt nicht - Kapazität reduzieren und neu versuchen
                if (capacity > 1u) { --capacity; fits = false; break; }
                // Kapazität 1 ist Minimum - trotzdem eintragen
            }

            ChunkComponentSlot slot;
            slot.typeId        = tid;
            slot.typeSize      = meta->size;
            slot.typeAlign     = meta->alignment;
            slot.offsetInChunk = offset;
            layout.slots.push_back(slot);
            offset += meta->size * capacity;
        }

        if (fits) layoutOk = true;
    }

    layout.entityCapacity = capacity;

    Debug::LogVerbose("Archetype.cpp: ChunkLayout::Build - %zu types, capacity=%zu, entityIdOffset=0",
        typeIds.size(), capacity);

    return layout;
}

// =============================================================================
// Chunk
// =============================================================================

EntityID* Chunk::EntityIDs(size_t offset) noexcept
{
    return reinterpret_cast<EntityID*>(data + offset);
}

const EntityID* Chunk::EntityIDs(size_t offset) const noexcept
{
    return reinterpret_cast<const EntityID*>(data + offset);
}

uint8_t* Chunk::ComponentPtr(size_t off, uint32_t slot, size_t sz) noexcept
{
    return data + off + slot * sz;
}

const uint8_t* Chunk::ComponentPtr(size_t off, uint32_t slot, size_t sz) const noexcept
{
    return data + off + slot * sz;
}

// =============================================================================
// Archetype
// =============================================================================

Archetype::Archetype(const ComponentSignature& sig,
                     const std::vector<uint32_t>& typeIds)
    : m_signature(sig)
    , m_layout(ChunkLayout::Build(typeIds))
    , m_typeIds(typeIds)
{
    Debug::LogVerbose("Archetype.cpp: created, %zu component types, chunk capacity=%zu",
        typeIds.size(), m_layout.entityCapacity);
}

Archetype::~Archetype()
{
    DestroyAllComponents();
    for (Chunk* c : m_chunks)
        delete c;
    m_chunks.clear();
}

bool Archetype::HasComponent(uint32_t typeId) const noexcept
{
    return m_signature.Test(typeId);
}

uint32_t Archetype::ChunkCount() const noexcept
{
    return static_cast<uint32_t>(m_chunks.size());
}

uint32_t Archetype::ChunkCapacity() const noexcept
{
    return static_cast<uint32_t>(m_layout.entityCapacity);
}

Chunk* Archetype::GetChunk(uint32_t index) noexcept
{
    assert(index < m_chunks.size());
    return m_chunks[index];
}

const Chunk* Archetype::GetChunk(uint32_t index) const noexcept
{
    assert(index < m_chunks.size());
    return m_chunks[index];
}

uint8_t* Archetype::GetComponentPtr(uint32_t chunkIndex,
                                     uint32_t slotIndex,
                                     uint32_t typeId) noexcept
{
    const size_t off = m_layout.GetComponentOffset(typeId);
    if (off == SIZE_MAX) return nullptr;
    const size_t idx = GetSlotIndex(typeId);
    if (idx == SIZE_MAX) return nullptr;
    return m_chunks[chunkIndex]->ComponentPtr(off, slotIndex, m_layout.slots[idx].typeSize);
}

const uint8_t* Archetype::GetComponentPtr(uint32_t chunkIndex,
                                           uint32_t slotIndex,
                                           uint32_t typeId) const noexcept
{
    const size_t off = m_layout.GetComponentOffset(typeId);
    if (off == SIZE_MAX) return nullptr;
    const size_t idx = GetSlotIndex(typeId);
    if (idx == SIZE_MAX) return nullptr;
    return m_chunks[chunkIndex]->ComponentPtr(off, slotIndex, m_layout.slots[idx].typeSize);
}

Archetype::Slot Archetype::Allocate(EntityID id)
{
    const uint32_t chunkIdx = FindOrAllocateChunk();
    Chunk* chunk = m_chunks[chunkIdx];
    const uint32_t slot = chunk->count;
    ++chunk->count;
    ++m_entityCount;

    // EntityID schreiben
    chunk->EntityIDs(m_layout.entityIdOffset)[slot] = id;

    // Default-Konstruktoren aufrufen
    const ComponentMetaRegistry& reg = ComponentMetaRegistry::Instance();
    for (const auto& s : m_layout.slots)
    {
        const ComponentMeta* meta = reg.Get(s.typeId);
        assert(meta && meta->defaultConstruct);
        uint8_t* ptr = chunk->ComponentPtr(s.offsetInChunk, slot, s.typeSize);
        meta->defaultConstruct(ptr);
    }

    return { chunkIdx, slot };
}

Archetype::SwapResult Archetype::Free(uint32_t chunkIndex, uint32_t slotIndex)
{
    SwapResult result{};
    Chunk* chunk = m_chunks[chunkIndex];
    assert(slotIndex < chunk->count);

    const uint32_t lastSlot = chunk->count - 1u;
    const bool needsSwap    = (slotIndex != lastSlot);
    const ComponentMetaRegistry& reg = ComponentMetaRegistry::Instance();

    // Destruktoren des zu entfernenden Slots
    for (const auto& s : m_layout.slots)
    {
        const ComponentMeta* meta = reg.Get(s.typeId);
        uint8_t* ptr = chunk->ComponentPtr(s.offsetInChunk, slotIndex, s.typeSize);
        meta->destruct(ptr);
    }

    if (needsSwap)
    {
        // Letzte Entity per move an die leere Stelle
        EntityID* ids       = chunk->EntityIDs(m_layout.entityIdOffset);
        result.movedEntity  = ids[lastSlot];
        ids[slotIndex]      = ids[lastSlot];

        for (const auto& s : m_layout.slots)
        {
            const ComponentMeta* meta = reg.Get(s.typeId);
            uint8_t* dst = chunk->ComponentPtr(s.offsetInChunk, slotIndex, s.typeSize);
            uint8_t* src = chunk->ComponentPtr(s.offsetInChunk, lastSlot, s.typeSize);
            meta->moveConstruct(dst, src);
        }

        result.swapped       = true;
        result.newChunkIndex = chunkIndex;
        result.newSlotIndex  = slotIndex;
    }

    --chunk->count;
    --m_entityCount;
    return result;
}

// --- Private -----------------------------------------------------------------

uint32_t Archetype::FindOrAllocateChunk()
{
    const uint32_t cap = static_cast<uint32_t>(m_layout.entityCapacity);
    for (int i = static_cast<int>(m_chunks.size()) - 1; i >= 0; --i)
    {
        if (!m_chunks[i]->IsFull(cap))
            return static_cast<uint32_t>(i);
    }
    m_chunks.push_back(new Chunk{});
    Debug::LogVerbose("Archetype.cpp: new chunk #%zu allocated", m_chunks.size() - 1u);
    return static_cast<uint32_t>(m_chunks.size() - 1u);
}

size_t Archetype::GetSlotIndex(uint32_t typeId) const noexcept
{
    for (size_t i = 0; i < m_layout.slots.size(); ++i)
        if (m_layout.slots[i].typeId == typeId) return i;
    return SIZE_MAX;
}

void Archetype::DestroyAllComponents()
{
    const ComponentMetaRegistry& reg = ComponentMetaRegistry::Instance();
    for (Chunk* chunk : m_chunks)
    {
        for (uint32_t s = 0; s < chunk->count; ++s)
        {
            for (const auto& slot : m_layout.slots)
            {
                const ComponentMeta* meta = reg.Get(slot.typeId);
                if (meta && meta->destruct)
                    meta->destruct(chunk->ComponentPtr(slot.offsetInChunk, s, slot.typeSize));
            }
        }
    }
}

} // namespace engine::ecs
