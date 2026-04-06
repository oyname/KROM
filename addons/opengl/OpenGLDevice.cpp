#include "OpenGLDevice.hpp"
#include "core/Debug.hpp"

#ifdef KROM_OPENGL_BACKEND
#   if defined(_WIN32)
#       ifndef WIN32_LEAN_AND_MEAN
#           define WIN32_LEAN_AND_MEAN
#       endif
#       ifndef NOMINMAX
#           define NOMINMAX
#       endif
#       include <windows.h>
#   endif
#   if defined(_WIN32)
#       include "OpenGLWin32Loader.hpp"
#   endif
#   if defined(__APPLE__)
#       include <OpenGL/gl3.h>
#   else
#       include <GL/gl.h>
#       include "glext.h"
#   endif
#endif

namespace engine::renderer::opengl {

OpenGLDevice::~OpenGLDevice() { Shutdown(); }

bool OpenGLDevice::Initialize(const DeviceDesc& desc)
{
    // GL-Kontext wird von OpenGLSwapchain (gladLoadGLLoader) aktiviert.
    // Device-Initialisierung ist plattformneutral - kein GL-Aufruf hier nötig.
    (void)desc;
    m_initialized   = true;
    m_frameIndex    = 0u;
    m_totalDrawCalls = 0u;
    Debug::Log("OpenGLDevice.cpp: Initialize - Kontext-Aktivierung erfolgt in CreateSwapchain");
    return true;
}

// Shutdown ist in OpenGLResources.cpp implementiert (GL-Objekte freigeben).

void OpenGLDevice::WaitIdle()
{
#ifdef KROM_OPENGL_BACKEND
    glFinish();
#endif
}

std::unique_ptr<ISwapchain> OpenGLDevice::CreateSwapchain(const SwapchainDesc& desc)
{
    if (!desc.nativeWindowHandle) {
        Debug::LogError("OpenGLDevice.cpp: CreateSwapchain - nativeWindowHandle ist null");
        return nullptr;
    }
    // nativeWindowHandle = HWND auf Win32, GLFWwindow* auf GLFW-Plattformen
    return std::make_unique<OpenGLSwapchain>(
        desc.nativeWindowHandle, m_resources, desc.width, desc.height,
        desc.openglMajor, desc.openglMinor, desc.openglDebugContext);
}

std::unique_ptr<ICommandList> OpenGLDevice::CreateCommandList(QueueType)
{
    return std::make_unique<OpenGLCommandList>(m_resources, &m_totalDrawCalls);
}

std::unique_ptr<IFence> OpenGLDevice::CreateFence(uint64_t initialValue)
{
    return std::make_unique<OpenGLFence>(initialValue);
}

void OpenGLDevice::BeginFrame()
{
    ++m_frameIndex;
#ifdef KROM_OPENGL_BACKEND
#   if defined(_WIN32)
    const HGLRC currentRc = wglGetCurrentContext();
    const HDC currentDc = wglGetCurrentDC();
    Debug::Log("OpenGLDevice::BeginFrame[%llu]: currentRC=%p currentDC=%p",
               static_cast<unsigned long long>(m_frameIndex),
               static_cast<void*>(currentRc),
               static_cast<void*>(currentDc));
#   endif
#endif
}
void OpenGLDevice::EndFrame()   {}

bool OpenGLDevice::SupportsFeature(const char* feature) const
{
    if (!feature) return false;
    const std::string f(feature);
    if (f == "ShaderVariants") return true;
    if (f == "compute")        return true;  // GL 4.3+ - aber wir binden GLAD 4.1
    return false;
}

namespace {
std::unique_ptr<IDevice> CreateOpenGLDeviceInstance()
{
    return std::make_unique<OpenGLDevice>();
}

// Registriert sich automatisch beim Linken — Core muss AddOn nicht kennen.
struct AutoRegister
{
    AutoRegister()
    {
        static DeviceFactory::Registrar registrar(
            DeviceFactory::BackendType::OpenGL,
            &CreateOpenGLDeviceInstance,
            &OpenGLDevice::EnumerateAdaptersImpl);
        (void)registrar;
    }
};
static AutoRegister s_autoRegister;
} // namespace

} // namespace engine::renderer::opengl
