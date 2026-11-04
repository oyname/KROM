#include "GLFWInternal.hpp"
#include <iterator>

#if __has_include(<GLFW/glfw3.h>)
#include <GLFW/glfw3.h>
#endif

namespace engine::platform {

bool GLFWInput::GamepadDown(GamepadButton button) const
{
#if defined(GLFW_GAMEPAD_BUTTON_A)
    constexpr int buttons[] = {-1, GLFW_GAMEPAD_BUTTON_A, GLFW_GAMEPAD_BUTTON_B, GLFW_GAMEPAD_BUTTON_X,
        GLFW_GAMEPAD_BUTTON_Y, GLFW_GAMEPAD_BUTTON_LEFT_BUMPER, GLFW_GAMEPAD_BUTTON_RIGHT_BUMPER,
        GLFW_GAMEPAD_BUTTON_BACK, GLFW_GAMEPAD_BUTTON_START, GLFW_GAMEPAD_BUTTON_LEFT_THUMB,
        GLFW_GAMEPAD_BUTTON_RIGHT_THUMB, GLFW_GAMEPAD_BUTTON_DPAD_UP, GLFW_GAMEPAD_BUTTON_DPAD_RIGHT,
        GLFW_GAMEPAD_BUTTON_DPAD_DOWN, GLFW_GAMEPAD_BUTTON_DPAD_LEFT};
    const auto index = static_cast<size_t>(button);
    if (!m_window || !glfwGetWindowAttrib(m_window, GLFW_FOCUSED) || index == 0 || index >= std::size(buttons))
        return false;
    GLFWgamepadstate state{};
    for (int pad = GLFW_JOYSTICK_1; pad <= GLFW_JOYSTICK_LAST; ++pad)
        if (glfwGetGamepadState(pad, &state) && state.buttons[buttons[index]] == GLFW_PRESS)
            return true;
#else
    (void)button;
#endif
    return false;
}

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
