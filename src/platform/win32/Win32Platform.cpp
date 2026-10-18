#include "Win32Internal.hpp"

namespace engine::platform::win32 {

#ifdef _WIN32

Win32Platform::~Win32Platform() = default;

namespace {

// Per-Monitor-V2 DPI-Awareness, bevor Fenster entstehen oder Monitore abgefragt
// werden. Ohne sie ueberspringt Windows bei maximierten Fenstern unter einer oben
// angedockten Taskleiste einen Streifen der Titelleiste beim Treffertest (Ziehen
// und Minimieren gehen dann nicht) und skaliert auf Monitoren > 100 % unscharf.
// Per GetProcAddress, damit aeltere Windows-Versionen ohne die Funktion starten.
void EnableDpiAwareness()
{
    using SetContextFn = BOOL(WINAPI*)(HANDLE);
    if (HMODULE user32 = GetModuleHandleW(L"user32.dll"))
    {
        if (auto setContext = reinterpret_cast<SetContextFn>(
                reinterpret_cast<void*>(GetProcAddress(user32, "SetProcessDpiAwarenessContext"))))
        {
            // DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2 == (HANDLE)-4; schlaegt fehl,
            // wenn schon gesetzt (Manifest, zweite Plattform-Instanz) — dann gilt das.
            if (setContext(reinterpret_cast<HANDLE>(static_cast<intptr_t>(-4))))
                return;
            if (GetLastError() == ERROR_ACCESS_DENIED)
                return;
        }
    }
    SetProcessDPIAware();   // Fallback vor Windows 10 1703: System-Aware
}

} // namespace

bool Win32Platform::Initialize()
{
    EnableDpiAwareness();
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
        input->AttachWindow(static_cast<HWND>(w->GetNativeWindowHandle().native));
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

void Win32Platform::GetPrimaryMonitorSize(uint32_t& outWidth, uint32_t& outHeight) const
{
    HMONITOR    mon = MonitorFromPoint({0, 0}, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    if (GetMonitorInfoW(mon, &mi))
    {
        outWidth  = static_cast<uint32_t>(mi.rcMonitor.right  - mi.rcMonitor.left);
        outHeight = static_cast<uint32_t>(mi.rcMonitor.bottom - mi.rcMonitor.top);
    }
    else
    {
        outWidth  = 0u;
        outHeight = 0u;
    }
}

IThreadFactory* Win32Platform::GetThreadFactory()
{
    return m_threadFactory.get();
}

#endif

} // namespace engine::platform::win32
