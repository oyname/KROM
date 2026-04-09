#include "renderer/IDevice.hpp"
#include "core/Debug.hpp"

namespace engine::renderer::vulkan {
namespace {
std::unique_ptr<IDevice> CreateVulkanDeviceStub() { return nullptr; }
struct AutoRegister
{
    AutoRegister()
    {
        static DeviceFactory::Registrar registrar(
            DeviceFactory::BackendType::Vulkan,
            &CreateVulkanDeviceStub,
            nullptr);
        (void)registrar;
    }
};
static AutoRegister s_autoRegister;
} // namespace
} // namespace engine::renderer::vulkan
