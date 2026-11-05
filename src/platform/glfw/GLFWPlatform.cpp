#include "GLFWInternal.hpp"

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

namespace engine::platform {

GLFWPlatform::GLFWPlatform() = default;
GLFWPlatform::~GLFWPlatform()
{
    Shutdown();
}

bool GLFWPlatform::Initialize()
{
    if (m_initialized)
        return true;

    if (!glfwInit())
        return false;

#if defined(__APPLE__)
    glfwWindowHint(GLFW_COCOA_RETINA_FRAMEBUFFER, GLFW_TRUE);
#endif

    m_input = std::make_unique<GLFWInput>();
    m_threadFactory = std::make_unique<GLFWThreadFactory>();
    m_timeOffset = glfwGetTime();
    m_initialized = true;
    return true;
}

void GLFWPlatform::Shutdown()
{
    m_windows.clear();
    m_threadFactory.reset();
    m_input.reset();
    if (m_initialized)
    {
        glfwTerminate();
        m_initialized = false;
    }
}

void GLFWPlatform::PumpEvents()
{
    if (!m_initialized)
        return;
    if (m_input)
        m_input->BeginFrame();
    glfwPollEvents();
}

double GLFWPlatform::GetTimeSeconds() const
{
    return m_initialized ? (glfwGetTime() - m_timeOffset) : 0.0;
}

IWindow* GLFWPlatform::CreateWindow(const WindowDesc& desc)
{
    if (!m_initialized || !m_input)
        return nullptr;

    auto window = std::make_unique<GLFWWindow>(*m_input);
    if (!window->Create(desc))
        return nullptr;

    auto* out = window.get();
    m_windows.push_back(std::move(window));
    return out;
}

IInput* GLFWPlatform::GetInput()
{
    return m_input.get();
}

IThreadFactory* GLFWPlatform::GetThreadFactory()
{
    return m_threadFactory.get();
}

void GLFWPlatform::GetPrimaryMonitorSize(uint32_t& outWidth, uint32_t& outHeight) const
{
    outWidth = 0u;
    outHeight = 0u;
    if (!m_initialized)
        return;
    GLFWmonitor* monitor = glfwGetPrimaryMonitor();
    const GLFWvidmode* mode = monitor ? glfwGetVideoMode(monitor) : nullptr;
    if (mode && mode->width > 0 && mode->height > 0)
    {
        outWidth = static_cast<uint32_t>(mode->width);
        outHeight = static_cast<uint32_t>(mode->height);
    }
}

bool GLFWPlatform::GetStartupMonitor(MonitorRect& outMonitor) const
{
    // GLFW has no window-independent cursor query, so this is the primary monitor.
    outMonitor = MonitorRect{};
    if (!m_initialized)
        return false;
    GLFWmonitor* monitor = glfwGetPrimaryMonitor();
    const GLFWvidmode* mode = monitor ? glfwGetVideoMode(monitor) : nullptr;
    if (!mode || mode->width <= 0 || mode->height <= 0)
        return false;
    int x = 0, y = 0;
    glfwGetMonitorPos(monitor, &x, &y);
    int workX = x, workY = y, workW = mode->width, workH = mode->height;
    glfwGetMonitorWorkarea(monitor, &workX, &workY, &workW, &workH);
    outMonitor.x = x;
    outMonitor.y = y;
    outMonitor.width = static_cast<uint32_t>(mode->width);
    outMonitor.height = static_cast<uint32_t>(mode->height);
    outMonitor.workX = workX;
    outMonitor.workY = workY;
    outMonitor.workWidth = static_cast<uint32_t>(workW > 0 ? workW : mode->width);
    outMonitor.workHeight = static_cast<uint32_t>(workH > 0 ? workH : mode->height);
    return true;
}

bool IsGLFWPlatformAvailable() noexcept
{
    return true;
}

} // namespace engine::platform
