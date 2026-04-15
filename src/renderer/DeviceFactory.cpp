#include "renderer/IDevice.hpp"
#include "core/Debug.hpp"
#include <mutex>
#include <unordered_map>

namespace engine::renderer {

namespace {

using Registry = std::unordered_map<DeviceFactory::BackendType, DeviceFactory::BackendEntry>;

Registry& GetRegistry()
{
    static Registry registry;
    return registry;
}

std::mutex& GetRegistryMutex()
{
    static std::mutex m;
    return m;
}


const char* BackendName(DeviceFactory::BackendType backend)
{
    switch (backend)
    {
    case DeviceFactory::BackendType::Null:      return "Null";
    case DeviceFactory::BackendType::DirectX11: return "DirectX11";
    case DeviceFactory::BackendType::DirectX12: return "DirectX12";
    case DeviceFactory::BackendType::OpenGL:    return "OpenGL";
    case DeviceFactory::BackendType::Vulkan:    return "Vulkan";
    default:                                    return "Unknown";
    }
}
} // namespace

void DeviceFactory::Register(BackendType backend, FactoryFn fn, EnumerateFn enumFn, bool isStub)
{
    std::scoped_lock lock(GetRegistryMutex());
    if (fn)
        GetRegistry()[backend] = BackendEntry{ fn, enumFn, isStub };
    else
        GetRegistry().erase(backend);
}

void DeviceFactory::Unregister(BackendType backend)
{
    std::scoped_lock lock(GetRegistryMutex());
    GetRegistry().erase(backend);
}

bool DeviceFactory::IsRegistered(BackendType backend)
{
    std::scoped_lock lock(GetRegistryMutex());
    return GetRegistry().find(backend) != GetRegistry().end();
}

bool DeviceFactory::IsAvailable(BackendType backend)
{
    std::scoped_lock lock(GetRegistryMutex());
    auto it = GetRegistry().find(backend);
    return it != GetRegistry().end() && !it->second.isStub;
}

std::unique_ptr<IDevice> DeviceFactory::Create(BackendType backend)
{
    FactoryFn fn = nullptr;
    {
        std::scoped_lock lock(GetRegistryMutex());
        auto it = GetRegistry().find(backend);
        if (it == GetRegistry().end())
        {
            Debug::LogError("DeviceFactory.cpp: backend '%s' is not registered", BackendName(backend));
            return nullptr;
        }

        fn = it->second.factory;
    }

    if (!fn)
    {
        Debug::LogError("DeviceFactory.cpp: backend '%s' has no factory function", BackendName(backend));
        return nullptr;
    }

    auto device = fn();
    if (!device)
        Debug::LogError("DeviceFactory.cpp: backend '%s' factory returned nullptr", BackendName(backend));

    return device;
}

std::vector<AdapterInfo> DeviceFactory::EnumerateAdapters(BackendType backend)
{
    EnumerateFn fn = nullptr;
    {
        std::scoped_lock lock(GetRegistryMutex());
        auto it = GetRegistry().find(backend);
        if (it != GetRegistry().end())
            fn = it->second.enumerate;
    }

    if (fn)
        return fn();

    Debug::LogWarning("DeviceFactory.cpp: EnumerateAdapters - backend '%s' hat keine EnumerateFn",
        BackendName(backend));
    return {};
}

uint32_t DeviceFactory::FindBestAdapter(const std::vector<AdapterInfo>& adapters)
{
    if (adapters.empty())
        return 0u;

    uint32_t bestIdx   = 0u;
    int      bestFL    = -1;
    bool     bestDiscr = false;

    for (const auto& a : adapters)
    {
        // Höherer Feature Level gewinnt; bei Gleichstand gewinnt diskrete GPU.
        const bool better = (a.featureLevel > bestFL) ||
                            (a.featureLevel == bestFL && a.isDiscrete && !bestDiscr);
        if (better)
        {
            bestIdx   = a.index;
            bestFL    = a.featureLevel;
            bestDiscr = a.isDiscrete;
        }
    }
    return bestIdx;
}

} // namespace engine::renderer
