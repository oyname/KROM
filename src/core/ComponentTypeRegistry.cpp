#include "core/Types.hpp"
#include <atomic>
#include <mutex>
#include <stdexcept>
#include <string>
#include <unordered_map>

namespace engine {
namespace {
struct Registry
{
    std::mutex mutex;
    std::unordered_map<std::string, uint32_t> ids;
};
Registry& LocalRegistry() { static Registry registry; return registry; }
std::atomic<ComponentTypeResolver> hostResolver{nullptr};
}

uint32_t ResolveComponentTypeId(const char* name)
{
    if (const auto resolver = hostResolver.load(std::memory_order_acquire))
        return resolver(name);
    auto& registry = LocalRegistry();
    std::lock_guard lock(registry.mutex);
    if (auto it = registry.ids.find(name); it != registry.ids.end())
        return it->second;
    if (registry.ids.size() >= MAX_COMPONENT_TYPES)
        throw std::runtime_error("KROM: too many component types");
    const auto id = static_cast<uint32_t>(registry.ids.size());
    registry.ids.emplace(name, id);
    return id;
}

void SetComponentTypeResolver(ComponentTypeResolver resolver) noexcept
{
    hostResolver.store(resolver, std::memory_order_release);
}

bool HasLocalComponentTypeIds() noexcept
{
    auto& registry = LocalRegistry();
    std::lock_guard lock(registry.mutex);
    return !registry.ids.empty();
}
}
