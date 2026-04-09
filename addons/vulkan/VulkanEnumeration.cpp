#include "VulkanDevice.hpp"
#include "core/Debug.hpp"
#include <memory>

namespace engine::renderer::vulkan {

std::vector<AdapterInfo> VulkanDevice::EnumerateAdaptersImpl()
{
    std::vector<AdapterInfo> result;

    VkInstance instance = VK_NULL_HANDLE;
    VkApplicationInfo appInfo{ VK_STRUCTURE_TYPE_APPLICATION_INFO };
    appInfo.pApplicationName = "KROM Vulkan Enumeration";
    appInfo.apiVersion = VK_API_VERSION_1_2;

    VkInstanceCreateInfo ci{ VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO };
    ci.pApplicationInfo = &appInfo;
#ifdef _WIN32
    const char* exts[] = { VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_WIN32_SURFACE_EXTENSION_NAME };
    ci.enabledExtensionCount = 2u;
    ci.ppEnabledExtensionNames = exts;
#endif

    if (vkCreateInstance(&ci, nullptr, &instance) != VK_SUCCESS)
        return result;

    uint32_t count = 0u;
    vkEnumeratePhysicalDevices(instance, &count, nullptr);
    std::vector<VkPhysicalDevice> devices(count);
    vkEnumeratePhysicalDevices(instance, &count, devices.data());

    for (uint32_t i = 0u; i < count; ++i)
    {
        VkPhysicalDeviceProperties props{};
        VkPhysicalDeviceMemoryProperties mem{};
        vkGetPhysicalDeviceProperties(devices[i], &props);
        vkGetPhysicalDeviceMemoryProperties(devices[i], &mem);

        size_t dedicatedBytes = 0u;
        for (uint32_t heap = 0u; heap < mem.memoryHeapCount; ++heap)
        {
            if ((mem.memoryHeaps[heap].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) != 0u)
                dedicatedBytes = std::max(dedicatedBytes, static_cast<size_t>(mem.memoryHeaps[heap].size));
        }

        AdapterInfo info{};
        info.index = i;
        info.name = props.deviceName;
        info.dedicatedVRAM = dedicatedBytes;
        info.isDiscrete = props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU;
        info.featureLevel = static_cast<int>(VK_API_VERSION_MAJOR(props.apiVersion) * 10 + VK_API_VERSION_MINOR(props.apiVersion));
        result.push_back(std::move(info));
    }

    vkDestroyInstance(instance, nullptr);
    return result;
}

namespace {
std::unique_ptr<IDevice> CreateVulkanDeviceInstance()
{
    return std::make_unique<VulkanDevice>();
}

struct AutoRegister
{
    AutoRegister()
    {
        static DeviceFactory::Registrar registrar(
            DeviceFactory::BackendType::Vulkan,
            &CreateVulkanDeviceInstance,
            &VulkanDevice::EnumerateAdaptersImpl);
        (void)registrar;
    }
};

static AutoRegister s_autoRegister;
} // namespace

} // namespace engine::renderer::vulkan
