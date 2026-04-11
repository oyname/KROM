#include "Win32Internal.hpp"

namespace engine::platform::win32 {

#ifdef _WIN32

Win32Platform::~Win32Platform() = default;

bool Win32Platform::Initialize()
{
    m_input = std::make_unique<Win32Input>();
    m_threadFactory = std::make_unique<Win32ThreadFactory>();
    m_start = std::chrono::steady_clock::now();
    return true;
}

void Win32Platform::Shutdown()
{
    m_windows.clear();
    m_threadFactory.reset();
    m_input.reset();
}

void Win32Platform::PumpEvents()
{
    if (!m_input)
        return;

    m_input->BeginFrame();

    MSG msg{};
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE))
    {
        if (msg.message == WM_QUIT)
        {
            for (auto& window : m_windows)
            {
                if (window)
                    window->RequestClose();
            }
            continue;
        }

        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
}

double Win32Platform::GetTimeSeconds() const
{
    const auto now = std::chrono::steady_clock::now();
    return std::chrono::duration<double>(now - m_start).count();
}

IWindow* Win32Platform::CreateWindow(const WindowDesc& desc)
{
    auto w = std::make_unique<Win32Window>();
    if (!w->Create(desc))
        return nullptr;
    if (auto* input = dynamic_cast<Win32Input*>(m_input.get()))
    {
        input->AttachWindow(static_cast<HWND>(w->GetNativeHandle()));
        w->AttachInput(input);
    }
    auto* out = w.get();
    m_windows.push_back(std::move(w));
    return out;
}

IInput* Win32Platform::GetInput()
{
    return m_input.get();
}

IThreadFactory* Win32Platform::GetThreadFactory()
{
    return m_threadFactory.get();
}

#endif

} // namespace engine::platform::win32
