#include "Win32Internal.hpp"

namespace engine::platform::win32 {

#ifdef _WIN32


void Win32Input::SetCursorMode(bool captured, bool hidden)
{
    ShowCursor(hidden ? FALSE : TRUE);
    if (captured && m_hwnd)
        SetCapture(m_hwnd);
    else
        ReleaseCapture();
}

MousePosition Win32Input::GetMousePosition() const
{
    if (!m_hwnd)
        return StandardInput::GetMousePosition();
    POINT p{};
    GetCursorPos(&p);
    ScreenToClient(m_hwnd, &p);
    return MousePosition{static_cast<double>(p.x), static_cast<double>(p.y)};
}

bool Win32Input::PollEvent(InputEvent& out)
{
    std::scoped_lock lock(m_mutex);
    if (m_queue.empty()) {
        out = {};
        return false;
    }
    out = m_queue.front();
    m_queue.pop();
    return true;
}

void Win32Input::PostKeyEvent(Key key, bool pressed, bool repeat)
{
    if (key == Key::Unknown)
        return;
    InputKeyEvent e{key, pressed, repeat};
    OnKeyEvent(e);
    std::scoped_lock lock(m_mutex);
    InputEvent ev{};
    ev.type = InputEventType::Key;
    ev.key = e;
    m_queue.push(ev);
}

void Win32Input::PostMouseMove(int32_t x, int32_t y)
{
    InputMouseMoveEvent e{x, y, x - m_lastX, y - m_lastY};
    m_lastX = x;
    m_lastY = y;
    OnMouseMoveEvent(e);
    std::scoped_lock lock(m_mutex);
    InputEvent ev{};
    ev.type = InputEventType::MouseMove;
    ev.mouseMove = e;
    m_queue.push(ev);
}

void Win32Input::PostMouseButton(MouseButton btn, bool pressed, int32_t x, int32_t y)
{
    InputMouseButtonEvent e{btn, pressed, x, y};
    OnMouseButtonEvent(e);
    std::scoped_lock lock(m_mutex);
    InputEvent ev{};
    ev.type = InputEventType::MouseButton;
    ev.mouseButton = e;
    m_queue.push(ev);
}

void Win32Input::PostMouseScroll(float delta)
{
    InputMouseScrollEvent e{delta};
    OnMouseScrollEvent(e);
    std::scoped_lock lock(m_mutex);
    InputEvent ev{};
    ev.type = InputEventType::MouseScroll;
    ev.mouseScroll = e;
    m_queue.push(ev);
}

#endif

} // namespace engine::platform::win32
