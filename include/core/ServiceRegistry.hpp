#pragma once

#include <cassert>
#include <type_traits>
#include <typeindex>
#include <unordered_map>

namespace engine {

class ServiceRegistry
{
public:
    template<typename T>
    void Register(std::remove_cv_t<std::remove_reference_t<T>>* service) noexcept
    {
        using StoredType = std::remove_cv_t<std::remove_reference_t<T>>;
        assert(service != nullptr);
        m_services[std::type_index(typeid(StoredType))] = service;
    }

    template<typename T>
    [[nodiscard]] T* Get() const noexcept
    {
        T* service = TryGet<T>();
        assert(service != nullptr);
        return service;
    }

    template<typename T>
    [[nodiscard]] T* TryGet() const noexcept
    {
        using StoredType = std::remove_cv_t<std::remove_reference_t<T>>;
        const auto it = m_services.find(std::type_index(typeid(StoredType)));
        if (it == m_services.end())
            return nullptr;

        return static_cast<T*>(it->second);
    }

    template<typename T>
    [[nodiscard]] bool Has() const noexcept
    {
        return TryGet<T>() != nullptr;
    }

    void Clear() noexcept
    {
        m_services.clear();
    }

private:
    std::unordered_map<std::type_index, void*> m_services;
};

using IServiceRegistry = ServiceRegistry;

} // namespace engine
