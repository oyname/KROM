#include "GLFWInternal.hpp"

#include <cassert>

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>
#if defined(_WIN32)
#include <GLFW/glfw3native.h>
#elif defined(__APPLE__)
#include <GLFW/glfw3native.h>
#elif defined(__linux__)
#include <GLFW/glfw3native.h>
#endif

namespace engine::platform {

namespace {

Key TranslateKey(int key)
{
    switch (key)
    {
    case GLFW_KEY_ESCAPE: return Key::Escape;
    case GLFW_KEY_SPACE: return Key::Space;
    case GLFW_KEY_ENTER: return Key::Enter;
    case GLFW_KEY_TAB: return Key::Tab;
    case GLFW_KEY_BACKSPACE: return Key::Backspace;
    case GLFW_KEY_LEFT_SHIFT: return Key::LeftShift;
    case GLFW_KEY_RIGHT_SHIFT: return Key::RightShift;
    case GLFW_KEY_LEFT_CONTROL: return Key::LeftCtrl;
    case GLFW_KEY_RIGHT_CONTROL: return Key::RightCtrl;
    case GLFW_KEY_LEFT_ALT: return Key::LeftAlt;
    case GLFW_KEY_RIGHT_ALT: return Key::RightAlt;
    case GLFW_KEY_LEFT: return Key::Left;
    case GLFW_KEY_RIGHT: return Key::Right;
    case GLFW_KEY_UP: return Key::Up;
    case GLFW_KEY_DOWN: return Key::Down;
    case GLFW_KEY_A: return Key::A;
    case GLFW_KEY_D: return Key::D;
    case GLFW_KEY_S: return Key::S;
    case GLFW_KEY_W: return Key::W;
    default: return Key::Unknown;
    }
}

MouseButton TranslateMouseButton(int button)
{
    switch (button)
    {
    case GLFW_MOUSE_BUTTON_LEFT: return MouseButton::Left;
    case GLFW_MOUSE_BUTTON_RIGHT: return MouseButton::Right;
    case GLFW_MOUSE_BUTTON_MIDDLE: return MouseButton::Middle;
    case GLFW_MOUSE_BUTTON_4: return MouseButton::X1;
    case GLFW_MOUSE_BUTTON_5: return MouseButton::X2;
    default: return MouseButton::Left;
    }
}

float ComputeDPIScale(GLFWwindow* handle, int width, int fbWidth)
{
    if (width <= 0)
        return 1.0f;

#if defined(__APPLE__)
    return static_cast<float>(fbWidth) / static_cast<float>(width);
#else
    float xscale = 1.0f;
    float yscale = 1.0f;
    glfwGetWindowContentScale(handle, &xscale, &yscale);
    return xscale > 0.0f ? xscale : 1.0f;
#endif
}

} // namespace

GLFWWindow::GLFWWindow(GLFWInput& input)
    : m_input(input)
{
}

GLFWWindow::~GLFWWindow()
{
    Destroy();
}

bool GLFWWindow::Create(const WindowDesc& desc)
{
    glfwDefaultWindowHints();

    if (desc.openglContext)
    {
        glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, desc.openglMajor);
        glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, desc.openglMinor);
        glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
        glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GLFW_TRUE);
        if (desc.openglDebugContext)
            glfwWindowHint(GLFW_OPENGL_DEBUG_CONTEXT, GLFW_TRUE);
    }
    else
    {
        glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    }
    glfwWindowHint(GLFW_VISIBLE, desc.visible ? GLFW_TRUE : GLFW_FALSE);
    glfwWindowHint(GLFW_RESIZABLE, desc.resizable ? GLFW_TRUE : GLFW_FALSE);
#if defined(__APPLE__)
    glfwWindowHint(GLFW_COCOA_RETINA_FRAMEBUFFER, GLFW_TRUE);
#endif

    GLFWmonitor* monitor = desc.fullscreen ? glfwGetPrimaryMonitor() : nullptr;
    m_handle = glfwCreateWindow(static_cast<int>(desc.width), static_cast<int>(desc.height), desc.title.c_str(), monitor, nullptr);
    if (!m_handle)
        return false;

    m_title = desc.title;
    m_hasGLContext = desc.openglContext;
    glfwSetWindowUserPointer(m_handle, this);
    m_input.AttachWindow(m_handle);

    glfwGetWindowSize(m_handle, reinterpret_cast<int*>(&m_width), reinterpret_cast<int*>(&m_height));
    glfwGetFramebufferSize(m_handle, reinterpret_cast<int*>(&m_fbWidth), reinterpret_cast<int*>(&m_fbHeight));
    m_dpiScale = ComputeDPIScale(m_handle, static_cast<int>(m_width), static_cast<int>(m_fbWidth));

    glfwSetFramebufferSizeCallback(m_handle, [](GLFWwindow* window, int w, int h) {
        auto* self = static_cast<GLFWWindow*>(glfwGetWindowUserPointer(window));
        if (self)
            self->HandleFramebufferResize(w, h);
    });
    glfwSetWindowSizeCallback(m_handle, [](GLFWwindow* window, int w, int h) {
        auto* self = static_cast<GLFWWindow*>(glfwGetWindowUserPointer(window));
        if (self)
            self->HandleWindowResize(w, h);
    });
    glfwSetKeyCallback(m_handle, [](GLFWwindow* window, int key, int, int action, int) {
        auto* self = static_cast<GLFWWindow*>(glfwGetWindowUserPointer(window));
        if (!self)
            return;
        const bool pressed = action != GLFW_RELEASE;
        self->m_input.OnKeyEvent(InputKeyEvent{TranslateKey(key), pressed, action == GLFW_REPEAT});
    });
    glfwSetCursorPosCallback(m_handle, [](GLFWwindow* window, double x, double y) {
        auto* self = static_cast<GLFWWindow*>(glfwGetWindowUserPointer(window));
        if (!self)
            return;
        const int32_t xi = static_cast<int32_t>(x);
        const int32_t yi = static_cast<int32_t>(y);
        const int32_t dx = xi - self->m_input.MouseX();
        const int32_t dy = yi - self->m_input.MouseY();
        self->m_input.OnMouseMoveEvent(InputMouseMoveEvent{xi, yi, dx, dy});
    });
    glfwSetMouseButtonCallback(m_handle, [](GLFWwindow* window, int button, int action, int) {
        auto* self = static_cast<GLFWWindow*>(glfwGetWindowUserPointer(window));
        if (!self)
            return;
        double x = 0.0;
        double y = 0.0;
        glfwGetCursorPos(window, &x, &y);
        self->m_input.OnMouseButtonEvent(InputMouseButtonEvent{
            TranslateMouseButton(button),
            action == GLFW_PRESS,
            static_cast<int32_t>(x),
            static_cast<int32_t>(y)
        });
    });
    glfwSetScrollCallback(m_handle, [](GLFWwindow* window, double, double yoffset) {
        auto* self = static_cast<GLFWWindow*>(glfwGetWindowUserPointer(window));
        if (!self)
            return;
        self->m_input.OnMouseScrollEvent(InputMouseScrollEvent{static_cast<float>(yoffset)});
    });

    return true;
}

void GLFWWindow::Destroy()
{
    if (m_handle)
    {
        glfwDestroyWindow(m_handle);
        m_handle = nullptr;
    }
}

WindowEventState GLFWWindow::PumpEvents(IInput& input)
{
    input.BeginFrame();
    const WindowEventState state{
        .quitRequested = ShouldClose(),
        .resized = m_resizePending,
        .width = m_width,
        .height = m_height,
        .framebufferWidth = m_fbWidth,
        .framebufferHeight = m_fbHeight
    };
    m_resizePending = false;
    return state;
}

bool GLFWWindow::IsOpen() const
{
    return m_handle != nullptr;
}

bool GLFWWindow::ShouldClose() const
{
    return m_handle == nullptr || glfwWindowShouldClose(m_handle) != 0;
}

void GLFWWindow::RequestClose()
{
    if (m_handle)
        glfwSetWindowShouldClose(m_handle, GLFW_TRUE);
}

void GLFWWindow::Resize(uint32_t width, uint32_t height)
{
    if (m_handle)
        glfwSetWindowSize(m_handle, static_cast<int>(width), static_cast<int>(height));
}

void GLFWWindow::SetTitle(const char* title)
{
    m_title = title ? title : "";
    if (m_handle)
        glfwSetWindowTitle(m_handle, m_title.c_str());
}


void* GLFWWindow::GetNativeHandle() const
{
    if (!m_handle)
        return nullptr;
    if (m_hasGLContext)
        return static_cast<void*>(m_handle);
#if defined(_WIN32)
    return glfwGetWin32Window(m_handle);
#elif defined(__linux__)
    return reinterpret_cast<void*>(static_cast<uintptr_t>(glfwGetX11Window(m_handle)));
#else
    return static_cast<void*>(m_handle);
#endif
}

void GLFWWindow::HandleFramebufferResize(int w, int h)
{
    m_fbWidth = static_cast<uint32_t>(w);
    m_fbHeight = static_cast<uint32_t>(h);
    m_dpiScale = ComputeDPIScale(m_handle, static_cast<int>(m_width), w);
    m_resizePending = true;
}

void GLFWWindow::HandleWindowResize(int w, int h)
{
    m_width = static_cast<uint32_t>(w);
    m_height = static_cast<uint32_t>(h);
    m_dpiScale = ComputeDPIScale(m_handle, w, static_cast<int>(m_fbWidth));
    m_resizePending = true;
}

} // namespace engine::platform
