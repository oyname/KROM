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

bool IsGLFWPlatformAvailable() noexcept
{
    return true;
}

} // namespace engine::platform
