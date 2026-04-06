#include "GLFWInternal.hpp"

#if __has_include(<GLFW/glfw3.h>)
#include <GLFW/glfw3.h>
#endif

namespace engine::platform {

void GLFWInput::AttachWindow(GLFWwindow* window)
{
    m_window = window;
}

void GLFWInput::OnKeyEvent(const InputKeyEvent& e)
{
    StandardInput::OnKeyEvent(e);
    PushEvent([&](InputEvent& out) {
        out.type = InputEventType::Key;
        out.key = e;
    });
}

void GLFWInput::OnMouseButtonEvent(const InputMouseButtonEvent& e)
{
    StandardInput::OnMouseButtonEvent(e);
    PushEvent([&](InputEvent& out) {
        out.type = InputEventType::MouseButton;
        out.mouseButton = e;
    });
}

void GLFWInput::OnMouseMoveEvent(const InputMouseMoveEvent& e)
{
    StandardInput::OnMouseMoveEvent(e);
    PushEvent([&](InputEvent& out) {
        out.type = InputEventType::MouseMove;
        out.mouseMove = e;
    });
}

void GLFWInput::OnMouseScrollEvent(const InputMouseScrollEvent& e)
{
    StandardInput::OnMouseScrollEvent(e);
    PushEvent([&](InputEvent& out) {
        out.type = InputEventType::MouseScroll;
        out.mouseScroll = e;
    });
}

bool GLFWInput::PollEvent(InputEvent& outEvent)
{
    std::scoped_lock lock(m_mutex);
    if (m_events.empty())
    {
        outEvent = {};
        return false;
    }

    outEvent = m_events.front();
    m_events.pop();
    return true;
}

void GLFWInput::SetCursorMode(bool captured, bool hidden)
{
#if __has_include(<GLFW/glfw3.h>)
    if (!m_window)
        return;
    if (captured)
        glfwSetInputMode(m_window, GLFW_CURSOR, GLFW_CURSOR_DISABLED);
    else if (hidden)
        glfwSetInputMode(m_window, GLFW_CURSOR, GLFW_CURSOR_HIDDEN);
    else
        glfwSetInputMode(m_window, GLFW_CURSOR, GLFW_CURSOR_NORMAL);
#else
    (void)captured;
    (void)hidden;
#endif
}

MousePosition GLFWInput::GetMousePosition() const
{
    return MousePosition{static_cast<double>(MouseX()), static_cast<double>(MouseY())};
}

} // namespace engine::platform
