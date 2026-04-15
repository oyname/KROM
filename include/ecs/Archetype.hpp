#pragma once
// =============================================================================
// KROM Engine - ecs/Archetype.hpp
// Archetype/Chunk-basiertes ECS - reine Deklaration.
// Implementierung: src/ecs/Archetype.cpp
// =============================================================================
#include "core/Types.hpp"
#include "ecs/ComponentMeta.hpp"
#include <vector>
#include <cstddef>

namespace engine::ecs {

// ---------------------------------------------------------------------------
// ChunkLayout
// ---------------------------------------------------------------------------
struct ChunkComponentSlot
{
    uint32_t typeId;
    size_t   typeSize;
    size_t   typeAlign;
    size_t   offsetInChunk;
};

struct ChunkLayout
{
    static constexpr size_t CHUNK_SIZE = 16384u;

    std::vector<ChunkComponentSlot> slots;
    size_t entityCapacity = 0u;
    size_t entityIdOffset = 0u;

    [[nodiscard]] size_t GetComponentOffset(uint32_t typeId) const noexcept;

    static uint8_t* ComponentPtr(uint8_t* base, size_t offset,
                                  size_t slotIndex, size_t compSize) noexcept
    {
        return base + offset + slotIndex * compSize;
    }

    static ChunkLayout Build(const std::vector<uint32_t>& typeIds);
};

// ---------------------------------------------------------------------------
// Chunk
// ---------------------------------------------------------------------------
struct alignas(64) Chunk
{
    uint8_t  data[ChunkLayout::CHUNK_SIZE]{};
    uint32_t count = 0u;
    uint8_t  reserved[60]{};

    static_assert((sizeof(data) + sizeof(count) + sizeof(reserved)) % 64u == 0u,
                  "Chunk storage must be an exact multiple of cache-line alignment.");

    [[nodiscard]] bool IsFull(uint32_t cap) const noexcept { return count >= cap; }
    [[nodiscard]] bool IsEmpty()            const noexcept { return count == 0u; }

    [[nodiscard]] EntityID*       EntityIDs(size_t offset) noexcept;
    [[nodiscard]] const EntityID* EntityIDs(size_t offset) const noexcept;

    [[nodiscard]] uint8_t*       ComponentPtr(size_t off, uint32_t slot, size_t sz) noexcept;
    [[nodiscard]] const uint8_t* ComponentPtr(size_t off, uint32_t slot, size_t sz) const noexcept;
};

// ---------------------------------------------------------------------------
// Archetype
// ---------------------------------------------------------------------------
class Archetype
{
public:
    explicit Archetype(const ComponentSignature& sig,
                       const std::vector<uint32_t>& typeIds);
    ~Archetype();

    Archetype(const Archetype&)            = delete;
    Archetype& operator=(const Archetype&) = delete;

    [[nodiscard]] const ComponentSignature&    GetSignature()  const noexcept { return m_signature; }
    [[nodiscard]] const ChunkLayout&           GetLayout()     const noexcept { return m_layout; }
    [[nodiscard]] const std::vector<uint32_t>& GetTypeIds()    const noexcept { return m_typeIds; }
    [[nodiscard]] bool     HasComponent(uint32_t typeId)       const noexcept;
    [[nodiscard]] uint32_t ChunkCount()                        const noexcept;
    [[nodiscard]] uint32_t EntityCount()                       const noexcept { return m_entityCount; }
    [[nodiscard]] uint32_t ChunkCapacity()                     const noexcept;

    [[nodiscard]] Chunk*       GetChunk(uint32_t index) noexcept;
    [[nodiscard]] const Chunk* GetChunk(uint32_t index) const noexcept;

    [[nodiscard]] uint8_t*       GetComponentPtr(uint32_t chunk, uint32_t slot, uint32_t typeId) noexcept;
    [[nodiscard]] const uint8_t* GetComponentPtr(uint32_t chunk, uint32_t slot, uint32_t typeId) const noexcept;

    struct Slot       { uint32_t chunkIndex; uint32_t slotIndex; };
    struct SwapResult { bool swapped = false; EntityID movedEntity;
                        uint32_t newChunkIndex = 0u; uint32_t newSlotIndex = 0u; };

    Slot       Allocate(EntityID id);
    SwapResult Free(uint32_t chunkIndex, uint32_t slotIndex);

private:
    ComponentSignature    m_signature;
    ChunkLayout           m_layout;
    std::vector<uint32_t> m_typeIds;
    std::vector<Chunk*>   m_chunks;
    uint32_t              m_entityCount = 0u;

    uint32_t FindOrAllocateChunk();
    size_t   GetSlotIndex(uint32_t typeId) const noexcept;
    void     DestroyAllComponents();
};

} // namespace engine::ecs
