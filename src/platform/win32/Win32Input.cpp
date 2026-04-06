#include "Win32Internal.hpp"

namespace engine::platform::win32 {

#ifdef _WIN32

namespace {
[[nodiscard]] Key VkToKeySimple(WPARAM vk)
{
    if (vk >= 'A' && vk <= 'Z')
        return static_cast<Key>(static_cast<uint16_t>(Key::A) + static_cast<uint16_t>(vk - 'A'));
    if (vk >= '0' && vk <= '9')
        return static_cast<Key>(static_cast<uint16_t>(Key::Num0) + static_cast<uint16_t>(vk - '0'));
    switch (vk) {
    case VK_ESCAPE: return Key::Escape;
    case VK_SPACE: return Key::Space;
    case VK_RETURN: return Key::Enter;
    case VK_TAB: return Key::Tab;
    case VK_BACK: return Key::Backspace;
    case VK_LEFT: return Key::Left;
    case VK_RIGHT: return Key::Right;
    case VK_UP: return Key::Up;
    case VK_DOWN: return Key::Down;
    case VK_HOME: return Key::Home;
    case VK_END: return Key::End;
    case VK_PRIOR: return Key::PageUp;
    case VK_NEXT: return Key::PageDown;
    case VK_INSERT: return Key::Insert;
    case VK_DELETE: return Key::Delete;
    case VK_F1: return Key::F1;
    case VK_F2: return Key::F2;
    case VK_F3: return Key::F3;
    case VK_F4: return Key::F4;
    case VK_F5: return Key::F5;
    case VK_F6: return Key::F6;
    case VK_F7: return Key::F7;
    case VK_F8: return Key::F8;
    case VK_F9: return Key::F9;
    case VK_F10: return Key::F10;
    case VK_F11: return Key::F11;
    case VK_F12: return Key::F12;
    case VK_OEM_PLUS: return Key::Plus;
    case VK_OEM_MINUS: return Key::Minus;
    default: return Key::Unknown;
    }
}
}

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

void Win32Input::PostKeyEvent(WPARAM vk, bool pressed, bool repeat)
{
    Key key = VkToKeySimple(vk);
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
