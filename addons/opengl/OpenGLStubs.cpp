#include "renderer/IDevice.hpp"
#include "core/Debug.hpp"

namespace engine::renderer::opengl {
namespace {

std::unique_ptr<IDevice> CreateOpenGLDeviceStub()
{
    Debug::LogError("OpenGL backend stub active: OpenGL unavailable in this build.");
    return nullptr;
}

struct AutoRegister
{
    AutoRegister()
    {
        static DeviceFactory::Registrar registrar(
            DeviceFactory::BackendType::OpenGL,
            &CreateOpenGLDeviceStub,
            nullptr,
            true);
        (void)registrar;
    }
};
static AutoRegister s_autoRegister;

} // namespace
} // namespace engine::renderer::opengl
