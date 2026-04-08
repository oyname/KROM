#include "Win32Internal.hpp"

#include "core/Debug.hpp"
#include <mutex>

namespace engine::platform::win32 {

#ifdef _WIN32

namespace {
constexpr wchar_t kWindowClassName[] = L"KromEngineWin32Window";

std::wstring WidenUtf8(const char* text)
{
    if (!text)
        return {};
    const int count = MultiByteToWideChar(CP_UTF8, 0, text, -1, nullptr, 0);
    if (count <= 0)
        return {};
    std::wstring out(static_cast<size_t>(count), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text, -1, out.data(), count);
    if (!out.empty() && out.back() == L'\0')
        out.pop_back();
    return out;
}

Key VkToKey(WPARAM vk, LPARAM lp)
{
    const UINT sc = (lp >> 16) & 0xFF;
    const bool extended = (lp & (1u << 24)) != 0;
    switch (vk) {
    case VK_ESCAPE: return Key::Escape;
    case VK_SPACE: return Key::Space;
    case VK_RETURN: return Key::Enter;
    case VK_TAB: return Key::Tab;
    case VK_BACK: return Key::Backspace;
    case VK_SHIFT: return MapVirtualKeyW(sc, MAPVK_VSC_TO_VK_EX) == VK_RSHIFT ? Key::RightShift : Key::LeftShift;
    case VK_CONTROL: return extended ? Key::RightCtrl : Key::LeftCtrl;
    case VK_MENU: return extended ? Key::RightAlt : Key::LeftAlt;
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
    case VK_OEM_PLUS: return Key::Plus;
    case VK_OEM_MINUS: return Key::Minus;
    case VK_F1: return Key::F1; case VK_F2: return Key::F2; case VK_F3: return Key::F3; case VK_F4: return Key::F4;
    case VK_F5: return Key::F5; case VK_F6: return Key::F6; case VK_F7: return Key::F7; case VK_F8: return Key::F8;
    case VK_F9: return Key::F9; case VK_F10: return Key::F10; case VK_F11: return Key::F11; case VK_F12: return Key::F12;
    default: break;
    }
    if (vk >= 'A' && vk <= 'Z') return static_cast<Key>(static_cast<uint16_t>(Key::A) + static_cast<uint16_t>(vk - 'A'));
    if (vk >= '0' && vk <= '9') return static_cast<Key>(static_cast<uint16_t>(Key::Num0) + static_cast<uint16_t>(vk - '0'));
    return Key::Unknown;
}

MouseButton ToMouseButton(UINT msg)
{
    switch (msg) {
    case WM_LBUTTONDOWN: case WM_LBUTTONUP: return MouseButton::Left;
    case WM_RBUTTONDOWN: case WM_RBUTTONUP: return MouseButton::Right;
    case WM_MBUTTONDOWN: case WM_MBUTTONUP: return MouseButton::Middle;
    default: return MouseButton::Left;
    }
}
}

void Win32Window::RegisterClassOnce()
{
    static std::once_flag once;
    std::call_once(once, []() {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.style = CS_HREDRAW | CS_VREDRAW | CS_OWNDC;
        wc.lpfnWndProc = &Win32Window::WndProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        wc.lpszClassName = kWindowClassName;
        RegisterClassExW(&wc);
    });
}

bool Win32Window::Create(const WindowDesc& desc)
{
    RegisterClassOnce();
    m_width = desc.width;
    m_height = desc.height;
    m_title = desc.title;
    RECT rect{0, 0, static_cast<LONG>(desc.width), static_cast<LONG>(desc.height)};
    AdjustWindowRect(&rect, WS_OVERLAPPEDWINDOW, FALSE);
    const auto titleW = WidenUtf8(desc.title.c_str());
    m_hwnd = CreateWindowExW(0, kWindowClassName, titleW.c_str(), WS_OVERLAPPEDWINDOW,
                             CW_USEDEFAULT, CW_USEDEFAULT,
                             rect.right - rect.left, rect.bottom - rect.top,
                             nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!m_hwnd)
        return false;
    SetWindowLongPtrW(m_hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
    if (desc.visible)
        ShowWindow(m_hwnd, SW_SHOW);
    if (m_hwnd)
        UpdateWindow(m_hwnd);
    Debug::Log("Win32Window::Create: hwnd=%p visible=%d size=%ux%u", static_cast<void*>(m_hwnd), desc.visible ? 1 : 0, desc.width, desc.height);
    m_open = true;
    m_closeReq = false;
    return true;
}

void Win32Window::Destroy()
{
    if (m_hwnd)
        DestroyWindow(m_hwnd);
    m_hwnd = nullptr;
    m_open = false;
}

WindowEventState Win32Window::PumpEvents(IInput& input)
{
    m_input = dynamic_cast<Win32Input*>(&input);
    if (m_input && m_hwnd)
        m_input->AttachWindow(m_hwnd);
    MSG msg{};
    while (PeekMessageW(&msg, m_hwnd, 0, 0, PM_REMOVE)) {
        if (msg.message == WM_QUIT) {
            m_closeReq = true;
            m_open = false;
        }
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    WindowEventState state{};
    state.quitRequested = m_closeReq;
    state.resized = m_resizePending;
    state.width = m_width;
    state.height = m_height;
    state.framebufferWidth = m_width;
    state.framebufferHeight = m_height;
    m_resizePending = false;
    return state;
}

bool Win32Window::IsOpen() const { return m_open; }
void Win32Window::RequestClose() { m_closeReq = true; m_open = false; }
void Win32Window::Resize(uint32_t w, uint32_t h) { if (m_hwnd) SetWindowPos(m_hwnd, nullptr, 0, 0, static_cast<int>(w), static_cast<int>(h), SWP_NOMOVE | SWP_NOZORDER); }
void Win32Window::SetTitle(const char* title) { m_title = title ? title : ""; if (m_hwnd) { const auto w = WidenUtf8(m_title.c_str()); SetWindowTextW(m_hwnd, w.c_str()); } }
void* Win32Window::GetNativeHandle() const { return m_hwnd; }
uint32_t Win32Window::GetWidth() const { return m_width; }
uint32_t Win32Window::GetHeight() const { return m_height; }
const char* Win32Window::GetBackendName() const { return "Win32Window"; }

LRESULT CALLBACK Win32Window::WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    auto* self = reinterpret_cast<Win32Window*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (self) {
        switch (msg) {
        case WM_KEYDOWN:
        case WM_SYSKEYDOWN:
            if (self->m_input)
                self->m_input->PostKeyEvent(wParam, true, (lParam & (1u << 30)) != 0);
            return 0;

        case WM_KEYUP:
        case WM_SYSKEYUP:
            if (self->m_input)
                self->m_input->PostKeyEvent(wParam, false, false);
            return 0;
        case WM_MOUSEMOVE:
            if (self->m_input) self->m_input->PostMouseMove(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
            return 0;
        case WM_LBUTTONDOWN: case WM_RBUTTONDOWN: case WM_MBUTTONDOWN:
            if (self->m_input) self->m_input->PostMouseButton(ToMouseButton(msg), true, GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
            return 0;
        case WM_LBUTTONUP: case WM_RBUTTONUP: case WM_MBUTTONUP:
            if (self->m_input) self->m_input->PostMouseButton(ToMouseButton(msg), false, GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
            return 0;
        case WM_MOUSEWHEEL:
            if (self->m_input) self->m_input->PostMouseScroll(static_cast<float>(GET_WHEEL_DELTA_WPARAM(wParam)) / 120.0f);
            return 0;
        case WM_SIZE:
            self->m_width = static_cast<uint32_t>(LOWORD(lParam));
            self->m_height = static_cast<uint32_t>(HIWORD(lParam));
            self->m_resizePending = true;
            return 0;
        case WM_PAINT: {
            PAINTSTRUCT ps{};
            BeginPaint(hwnd, &ps);
            EndPaint(hwnd, &ps);
            Debug::Log("Win32Window::WndProc: WM_PAINT hwnd=%p", static_cast<void*>(hwnd));
            return 0;
        }
        case WM_ERASEBKGND:
            Debug::Log("Win32Window::WndProc: WM_ERASEBKGND hwnd=%p", static_cast<void*>(hwnd));
            return 1;
        case WM_CLOSE:
            self->m_closeReq = true;
            self->m_open = false;
            return 0;
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
        default: break;
        }
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

#endif

} // namespace engine::platform::win32
