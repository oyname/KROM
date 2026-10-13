#include "renderer/IDevice.hpp"
#include "core/Debug.hpp"

namespace engine::renderer::vulkan {
namespace {

std::unique_ptr<IDevice> CreateVulkanDeviceStub()
{
    Debug::LogError("Vulkan backend stub active: Vulkan unavailable on this platform/build.");
    return nullptr;
}

struct AutoRegister
{
    AutoRegister()
    {
        static DeviceFactory::Registrar registrar(
            DeviceFactory::BackendType::Vulkan,
            &CreateVulkanDeviceStub,
            nullptr,
            true);
        (void)registrar;
    }
};
static AutoRegister s_autoRegister;
} // namespace
} // namespace engine::renderer::vulkan
