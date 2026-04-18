#pragma once
// =============================================================================
// KROM Engine - ecs/ComponentMeta.hpp
// Laufzeit-Typinformation pro Komponente.
// Nötig für das Archetype/Chunk-System: sizeof, construct, destruct, move.
// Registrierung über RegisterComponent<T>() vor World::Create().
// =============================================================================
#include "core/Types.hpp"
#include <vector>
#include <string>
#include <cstring>

namespace engine::ecs {

// Pro Komponententyp gespeicherte Metadaten
struct ComponentMeta
{
    uint32_t    typeId    = 0u;
    size_t      size      = 0u;
    size_t      alignment = 0u;
    const char* name      = "";

    // Lifecycle-Funktionen (zeigen auf statische Template-Instanzen)
    void (*defaultConstruct)(void* ptr)             = nullptr; // placement new default
    void (*destruct)        (void* ptr)             = nullptr; // explicit destructor
    void (*moveConstruct)   (void* dst, void* src)  = nullptr; // move-construct + destruct src
    void (*copyConstruct)   (void* dst, const void* src) = nullptr;
    void (*moveAssign)      (void* dst, void* src)  = nullptr;
};

// Globale Metadaten-Tabelle - indexed by ComponentTypeID<T>::value
class ComponentMetaRegistry
{
public:
    void Register(const ComponentMeta& meta)
    {
        if (meta.typeId >= m_metas.size())
            m_metas.resize(meta.typeId + 1u);
        m_metas[meta.typeId] = meta;
    }

    const ComponentMeta* Get(uint32_t typeId) const noexcept
    {
        if (typeId >= m_metas.size()) return nullptr;
        return m_metas[typeId].size > 0 ? &m_metas[typeId] : nullptr;
    }

    template<typename T>
    const ComponentMeta* Get() const noexcept
    {
        return Get(ComponentTypeID<T>::value);
    }

    size_t Count() const noexcept { return m_metas.size(); }
    void Clear() noexcept { m_metas.clear(); }

private:
    std::vector<ComponentMeta> m_metas;
};

// Template-Hilfsfunktionen für Lifecycle
namespace detail {

    template<typename T>
    void DefaultConstruct(void* ptr) { new(ptr) T(); }

    template<typename T>
    void Destruct(void* ptr) { static_cast<T*>(ptr)->~T(); }

    template<typename T>
    void MoveConstruct(void* dst, void* src)
    {
        new(dst) T(std::move(*static_cast<T*>(src)));
        static_cast<T*>(src)->~T();
    }

    template<typename T>
    void CopyConstruct(void* dst, const void* src)
    {
        new(dst) T(*static_cast<const T*>(src));
    }

    template<typename T>
    void MoveAssign(void* dst, void* src)
    {
        *static_cast<T*>(dst) = std::move(*static_cast<T*>(src));
    }

} // namespace detail

// Registriert Komponente T und gibt ihre ID zurück
template<typename T>
uint32_t RegisterComponent(ComponentMetaRegistry& registry, const char* name = nullptr)
{
    const uint32_t id = ComponentTypeID<T>::value;
    ComponentMeta meta;
    meta.typeId    = id;
    meta.size      = sizeof(T);
    meta.alignment = alignof(T);
    meta.name      = name ? name : typeid(T).name();
    meta.defaultConstruct = detail::DefaultConstruct<T>;
    meta.destruct         = detail::Destruct<T>;
    meta.moveConstruct    = detail::MoveConstruct<T>;
    meta.copyConstruct    = detail::CopyConstruct<T>;
    meta.moveAssign       = detail::MoveAssign<T>;
    registry.Register(meta);
    return id;
}

} // namespace engine::ecs
