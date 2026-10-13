#include "renderer/IDevice.hpp"
#include "core/Debug.hpp"

namespace engine::renderer {

namespace {

std::unordered_map<DeviceFactory::BackendType, DeviceFactory::BackendEntry>& BackendCatalog()
{
    static std::unordered_map<DeviceFactory::BackendType, DeviceFactory::BackendEntry> catalog;
    return catalog;
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

DeviceFactory::Registry::Registry()
    : m_entries(BackendCatalog())
{
}

void DeviceFactory::Registry::Register(BackendType backend, FactoryFn fn, EnumerateFn enumFn, bool isStub)
{
    if (fn)
        m_entries[backend] = BackendEntry{ fn, enumFn, isStub };
    else
        m_entries.erase(backend);
}

void DeviceFactory::Registry::Unregister(BackendType backend)
{
    m_entries.erase(backend);
}

bool DeviceFactory::Registry::IsRegistered(BackendType backend) const
{
    return m_entries.find(backend) != m_entries.end();
}

bool DeviceFactory::Registry::IsAvailable(BackendType backend) const
{
    auto it = m_entries.find(backend);
    return it != m_entries.end() && !it->second.isStub;
}

std::unique_ptr<IDevice> DeviceFactory::Registry::Create(BackendType backend) const
{
    auto it = m_entries.find(backend);
    if (it == m_entries.end())
    {
        Debug::LogError("DeviceFactory.cpp: backend '%s' is not registered", BackendName(backend));
        return nullptr;
    }

    const FactoryFn fn = it->second.factory;
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

std::vector<AdapterInfo> DeviceFactory::Registry::EnumerateAdapters(BackendType backend) const
{
    auto it = m_entries.find(backend);
    const EnumerateFn fn = it != m_entries.end() ? it->second.enumerate : nullptr;

    if (fn)
        return fn();

    Debug::LogWarning("DeviceFactory.cpp: EnumerateAdapters - backend '%s' hat keine EnumerateFn",
        BackendName(backend));
    return {};
}

void DeviceFactory::Registry::CopyFrom(const Registry& other)
{
    if (this == &other)
        return;
    m_entries = other.m_entries;
}

void DeviceFactory::RegisterBackend(BackendType backend, FactoryFn fn, EnumerateFn enumFn, bool isStub)
{
    if (fn)
        BackendCatalog()[backend] = BackendEntry{ fn, enumFn, isStub };
    else
        BackendCatalog().erase(backend);
}

void DeviceFactory::UnregisterBackend(BackendType backend)
{
    BackendCatalog().erase(backend);
}

uint32_t DeviceFactory::FindBestAdapter(const std::vector<AdapterInfo>& adapterInfos)
{
    if (adapterInfos.empty())
        return 0u;

    uint32_t bestIdx   = 0u;
    int      bestFL    = -1;
    bool     bestDiscr = false;

    for (const auto& a : adapterInfos)
    {
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
