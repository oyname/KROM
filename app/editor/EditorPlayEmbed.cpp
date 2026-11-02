// =============================================================================
// KROM Engine - app/editor/EditorPlayEmbed.cpp
// =============================================================================
#include "app/editor/EditorPlayEmbed.hpp"

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#include <string>

namespace engine::app {

#if defined(_WIN32)
namespace {

// Wie lange ein Spiel nach WM_CLOSE Zeit zum Beenden bekommt.
constexpr uint64_t kStopGraceMs = 3000u;

struct ChildSearch
{
    DWORD processId = 0u;
    HWND found = nullptr;
};

BOOL CALLBACK FindProcessChild(HWND hwnd, LPARAM param)
{
    auto* search = reinterpret_cast<ChildSearch*>(param);
    DWORD pid = 0u;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid != search->processId)
        return TRUE;
    search->found = hwnd;
    return FALSE;
}

std::wstring Quoted(const std::wstring& value)
{
    return L"\"" + value + L"\"";
}

} // namespace
#endif

EmbeddedPlayProcess::~EmbeddedPlayProcess()
{
    Terminate();
}

bool EmbeddedPlayProcess::Launch(const std::filesystem::path& exePath,
                                 const std::filesystem::path& projectFile,
                                 platform::NativeWindowHandle parent,
                                 uint32_t width, uint32_t height)
{
#if defined(_WIN32)
    Terminate();
    if (parent.backend != platform::NativeWindowBackend::Win32 || !parent.native)
        return false;
    std::error_code ec;
    if (!std::filesystem::exists(exePath, ec) || !std::filesystem::exists(projectFile, ec))
        return false;

    const HWND parentHwnd = static_cast<HWND>(parent.native);
    // Der Editor zeichnet sonst ueber die Flaeche des Spielfensters.
    const LONG_PTR style = GetWindowLongPtrW(parentHwnd, GWL_STYLE);
    if ((style & WS_CLIPCHILDREN) == 0)
        SetWindowLongPtrW(parentHwnd, GWL_STYLE, style | WS_CLIPCHILDREN);
    std::wstring commandLine = Quoted(exePath.wstring()) + L" " + Quoted(projectFile.wstring()) +
        L" 1 --editor-live-state --parent-hwnd " +
        std::to_wstring(reinterpret_cast<std::uintptr_t>(parentHwnd)) +
        L" --size " + std::to_wstring(width > 0u ? width : 1280u) + L" " +
        std::to_wstring(height > 0u ? height : 720u);
    const std::wstring workingDir = exePath.parent_path().wstring();

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION info{};
    // Keine Konsole: das Spiel erscheint nur im Game-Reiter.
    if (!CreateProcessW(exePath.c_str(), commandLine.data(), nullptr, nullptr, FALSE,
                        CREATE_NO_WINDOW, nullptr, workingDir.c_str(), &startup, &info))
        return false;
    CloseHandle(info.hThread);
    m_process = info.hProcess;
    m_processId = info.dwProcessId;
    m_parent = parentHwnd;
    return true;
#else
    (void)exePath;
    (void)projectFile;
    (void)parent;
    (void)width;
    (void)height;
    return false;
#endif
}

void EmbeddedPlayProcess::Update(bool visible, int x, int y, int width, int height)
{
#if defined(_WIN32)
    if (!m_process)
        return;
    if (WaitForSingleObject(static_cast<HANDLE>(m_process), 0) == WAIT_OBJECT_0)
    {
        Reset();
        return;
    }
    if (m_stopRequestedMs != 0u && GetTickCount64() - m_stopRequestedMs > kStopGraceMs)
    {
        Terminate();
        return;
    }

    HWND window = static_cast<HWND>(m_window);
    if (!window || !IsWindow(window))
    {
        ChildSearch search{};
        search.processId = m_processId;
        EnumChildWindows(static_cast<HWND>(m_parent), &FindProcessChild, reinterpret_cast<LPARAM>(&search));
        window = search.found;
        m_window = window;
        m_shown = false;
        if (!window)
            return;
    }

    if (!visible || width <= 0 || height <= 0)
    {
        if (m_shown)
            ShowWindowAsync(window, SW_HIDE);
        m_shown = false;
        return;
    }
    if (!m_shown || x != m_x || y != m_y || width != m_width || height != m_height)
    {
        // Asynchron: das Fenster gehoert einem anderen Prozess. Synchron wuerde der
        // Editor blockieren, solange das Spiel keine Nachrichten verarbeitet (Laden).
        SetWindowPos(window, HWND_TOP, x, y, width, height,
                     SWP_NOACTIVATE | SWP_SHOWWINDOW | SWP_ASYNCWINDOWPOS);
        m_x = x;
        m_y = y;
        m_width = width;
        m_height = height;
        m_shown = true;
    }
#else
    (void)visible;
    (void)x;
    (void)y;
    (void)width;
    (void)height;
#endif
}

void EmbeddedPlayProcess::Stop()
{
#if defined(_WIN32)
    if (!m_process || m_stopRequestedMs != 0u)
        return;
    m_stopRequestedMs = GetTickCount64();
    if (m_window && IsWindow(static_cast<HWND>(m_window)))
        PostMessageW(static_cast<HWND>(m_window), WM_CLOSE, 0, 0);
    else
        Terminate();
#endif
}

void EmbeddedPlayProcess::Terminate()
{
#if defined(_WIN32)
    if (m_process)
    {
        HANDLE process = static_cast<HANDLE>(m_process);
        if (WaitForSingleObject(process, 0) != WAIT_OBJECT_0)
            TerminateProcess(process, 0u);
    }
#endif
    Reset();
}

void EmbeddedPlayProcess::Reset()
{
#if defined(_WIN32)
    if (m_process)
        CloseHandle(static_cast<HANDLE>(m_process));
#endif
    m_process = nullptr;
    m_processId = 0u;
    m_window = nullptr;
    m_shown = false;
    m_x = m_y = m_width = m_height = 0;
    m_stopRequestedMs = 0u;
}

} // namespace engine::app
