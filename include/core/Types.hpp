#pragma once
// =============================================================================
// KROM Engine - core/Types.hpp
// Fundamentale Typen: Handle<Tag>, EntityID, ComponentTypeID
// Kein Grafik-API-Include, keine externen Abhängigkeiten.
// =============================================================================

#include <cstdint>
#include <cstddef>
#include <cassert>
#include <functional>
#include <limits>

namespace engine {

// -----------------------------------------------------------------------------
// Handle<Tag>
// Typisierter, generationssicherer 32-Bit-Ressourcen-Handle.
// Layout: Bits 31-12 = Index (20 Bit), Bits 11-0 = Generation (12 Bit)
// Direkt aus KROM-Engine übernommen und leicht erweitert.
// -----------------------------------------------------------------------------
template<typename Tag>
struct Handle
{
    static constexpr uint32_t INDEX_BITS = 20u;
    static constexpr uint32_t GEN_BITS   = 12u;
    static constexpr uint32_t INDEX_MASK = (1u << INDEX_BITS) - 1u;
    static constexpr uint32_t GEN_MASK   = (1u << GEN_BITS)  - 1u;
    static constexpr uint32_t MAX_INDEX  = INDEX_MASK;

    uint32_t value = 0u;

    Handle() = default;
    constexpr explicit Handle(uint32_t raw) : value(raw) {}

    static Handle Make(uint32_t index, uint32_t generation) noexcept
    {
        assert((index & ~INDEX_MASK) == 0 && "Handle::Make: index overflow");
        return Handle{ ((index & INDEX_MASK) << GEN_BITS) | (generation & GEN_MASK) };
    }

    [[nodiscard]] uint32_t Index()      const noexcept { return (value >> GEN_BITS) & INDEX_MASK; }
    [[nodiscard]] uint32_t Generation() const noexcept { return value & GEN_MASK; }
    [[nodiscard]] bool     IsValid()    const noexcept { return value != 0u; }

    explicit operator bool() const noexcept { return IsValid(); }

    bool operator==(const Handle& o) const noexcept { return value == o.value; }
    bool operator!=(const Handle& o) const noexcept { return value != o.value; }

    static Handle Invalid() noexcept { return Handle{}; }
};

// Vordefinierte Handle-Tags für Asset-Typen
struct MeshTag         {};
struct TextureTag      {};
struct ShaderTag       {};
struct MaterialTag     {};
struct RenderTargetTag {};
struct BufferTag       {};
struct PipelineTag     {};

using MeshHandle         = Handle<MeshTag>;
using TextureHandle      = Handle<TextureTag>;
using ShaderHandle       = Handle<ShaderTag>;
using MaterialHandle     = Handle<MaterialTag>;
using RenderTargetHandle = Handle<RenderTargetTag>;
using BufferHandle       = Handle<BufferTag>;
using PipelineHandle     = Handle<PipelineTag>;

} // namespace engine

// Hash-Support für std::unordered_map
namespace std {
    template<typename Tag>
    struct hash<engine::Handle<Tag>> {
        size_t operator()(const engine::Handle<Tag>& h) const noexcept {
            return std::hash<uint32_t>{}(h.value);
        }
    };
}

namespace engine {

// -----------------------------------------------------------------------------
// EntityID
// 32-Bit-Handle: Bits 31-8 = Index (24 Bit), Bits 7-0 = Generation (8 Bit)
// Generation 0 ist für NULL_ENTITY reserviert.
// Ident mit KROM-EntityID - bewährte Produktion.
// -----------------------------------------------------------------------------
using EntityIndex      = uint32_t;
using EntityGeneration = uint8_t;

struct EntityID
{
    static constexpr uint32_t INDEX_BITS = 24u;
    static constexpr uint32_t GEN_BITS   = 8u;
    static constexpr uint32_t INDEX_MASK = (1u << INDEX_BITS) - 1u;
    static constexpr uint32_t GEN_MASK   = (1u << GEN_BITS)   - 1u;
    static constexpr uint32_t MAX_ENTITIES = INDEX_MASK; // ~16M

    uint32_t value = 0u;

    EntityID() = default;
    constexpr explicit EntityID(uint32_t raw) : value(raw) {}

    static EntityID Make(EntityIndex index, EntityGeneration gen) noexcept
    {
        assert(gen != 0u && "EntityID::Make: generation 0 reserved for NULL_ENTITY");
        return EntityID{ (index << GEN_BITS) | (gen & GEN_MASK) };
    }

    [[nodiscard]] EntityIndex      Index()      const noexcept { return (value >> GEN_BITS) & INDEX_MASK; }
    [[nodiscard]] EntityGeneration Generation() const noexcept { return static_cast<EntityGeneration>(value & GEN_MASK); }
    [[nodiscard]] bool             IsValid()    const noexcept { return value != 0u && Generation() != 0u; }

    explicit operator bool() const noexcept { return IsValid(); }

    bool operator==(const EntityID& o) const noexcept { return value == o.value; }
    bool operator!=(const EntityID& o) const noexcept { return value != o.value; }
    bool operator< (const EntityID& o) const noexcept { return value <  o.value; }
};

inline constexpr EntityID NULL_ENTITY{ 0u };

} // namespace engine

namespace std {
    template<>
    struct hash<engine::EntityID> {
        size_t operator()(const engine::EntityID& e) const noexcept {
            return std::hash<uint32_t>{}(e.value);
        }
    };
}

namespace engine {

// -----------------------------------------------------------------------------
// ComponentTypeID<T>
// Globaler, compile-time-stabiler Integer-ID pro Komponententyp.
// Direkter O(1)-Pool-Index ohne Hash.
// -----------------------------------------------------------------------------
struct ComponentTypeIDCounter
{
    static inline uint32_t next = 0u;
};

template<typename T>
struct ComponentTypeID
{
    // Wird beim ersten ODR-Zugriff initialisiert - Thread-safe in C++11+
    static const uint32_t value;
};

template<typename T>
const uint32_t ComponentTypeID<T>::value = ComponentTypeIDCounter::next++;

// Maximale Anzahl registrierbarer Komponententypen (für Signature-Bitset)
static constexpr uint32_t MAX_COMPONENT_TYPES = 128u;

// -----------------------------------------------------------------------------
// ComponentSignature
// 2× uint64_t = 128-Bit-Bitset für Archetype-Zugehörigkeit.
// Kein std::bitset<N> um platformübergreifende Effizienz zu kontrollieren.
// -----------------------------------------------------------------------------
struct ComponentSignature
{
    uint64_t bits[2] = { 0ull, 0ull };

    void Set(uint32_t id) noexcept
    {
        assert(id < MAX_COMPONENT_TYPES);
        bits[id >> 6] |= (1ull << (id & 63u));
    }

    void Clear(uint32_t id) noexcept
    {
        assert(id < MAX_COMPONENT_TYPES);
        bits[id >> 6] &= ~(1ull << (id & 63u));
    }

    bool Test(uint32_t id) const noexcept
    {
        assert(id < MAX_COMPONENT_TYPES);
        return (bits[id >> 6] & (1ull << (id & 63u))) != 0ull;
    }

    // Prüft ob alle Bits von 'other' in this gesetzt sind
    bool Contains(const ComponentSignature& other) const noexcept
    {
        return ((bits[0] & other.bits[0]) == other.bits[0]) &&
               ((bits[1] & other.bits[1]) == other.bits[1]);
    }

    ComponentSignature With(uint32_t id) const noexcept
    {
        ComponentSignature s = *this;
        s.Set(id);
        return s;
    }

    ComponentSignature Without(uint32_t id) const noexcept
    {
        ComponentSignature s = *this;
        s.Clear(id);
        return s;
    }

    bool operator==(const ComponentSignature& o) const noexcept
    {
        return bits[0] == o.bits[0] && bits[1] == o.bits[1];
    }
    bool operator!=(const ComponentSignature& o) const noexcept { return !(*this == o); }
};

} // namespace engine

namespace std {
    template<>
    struct hash<engine::ComponentSignature> {
        size_t operator()(const engine::ComponentSignature& s) const noexcept {
            size_t h = std::hash<uint64_t>{}(s.bits[0]);
            h ^= std::hash<uint64_t>{}(s.bits[1]) + 0x9e3779b9 + (h << 6) + (h >> 2);
            return h;
        }
    };
}
