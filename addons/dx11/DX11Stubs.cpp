#include "DX11Device.hpp"
#include "core/Debug.hpp"

namespace engine::renderer::dx11 {
namespace {

std::unique_ptr<IDevice> CreateDX11DeviceStub()
{
    Debug::LogError("DX11 backend stub active: DirectX11 unavailable on this platform/build.");
    return nullptr;
}

struct AutoRegister
{
    AutoRegister()
    {
        static DeviceFactory::Registrar registrar(
            DeviceFactory::BackendType::DirectX11,
            &CreateDX11DeviceStub,
            nullptr,
            true);
        (void)registrar;
    }
};
static AutoRegister s_autoRegister;

} // namespace
} // namespace engine::renderer::dx11
