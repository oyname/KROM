#pragma once

#include "platform/Win32Platform.hpp"
#include "platform/PlatformInput.hpp"

#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#   define WIN32_LEAN_AND_MEAN
#   define NOMINMAX
#   include <windows.h>
#   include <windowsx.h>
#   ifdef CreateWindow
#       undef CreateWindow
#   endif
#   ifdef CreateWindowEx
#       undef CreateWindowEx
#   endif
#   ifdef CreateMutex
#       undef CreateMutex
#   endif
#endif

namespace engine::platform::win32 {

#ifdef _WIN32

class Win32Input final : public StandardInput
{
public:
    void SetCursorMode(bool captured, bool hidden) override;
    MousePosition GetMousePosition() const override;
    bool PollEvent(InputEvent& out) override;

    void AttachWindow(HWND hwnd) { m_hwnd = hwnd; }
    void PostKeyEvent(Key key, bool pressed, bool repeat);

    void PostMouseMove(int32_t x, int32_t y);
    void PostMouseButton(MouseButton btn, bool pressed, int32_t x, int32_t y);
    void PostMouseScroll(float delta);

private:
    mutable std::mutex m_mutex;
    std::queue<InputEvent> m_queue;
    HWND m_hwnd = nullptr;
    int32_t m_lastX = 0;
    int32_t m_lastY = 0;
};

class Win32Mutex final : public IMutex
{
public:
    Win32Mutex();
    ~Win32Mutex() override;
    void Lock() override;
    void Unlock() override;
    bool TryLock() override;
private:
    CRITICAL_SECTION m_cs;
};

class Win32Thread final : public IThread
{
public:
    ~Win32Thread() override;
    void Start(const std::function<void()>& fn) override;
    void Join() override;
    void SetPriority(ThreadPriority p) override;
    void SetAffinity(const ThreadAffinity& a) override;
    void SetName(const char* name) override;
    bool IsRunning() const override;
private:
    std::thread m_thread;
    std::atomic<bool> m_running{false};
};

class Win32ThreadFactory final : public IThreadFactory
{
public:
    IThread* CreateThread() override;
    void DestroyThread(IThread* t) override;
    int GetHardwareConcurrency() const override;
    void SleepMs(int ms) const override;
    IMutex* CreateMutex() override;
    IJobSystem* CreateJobSystem(uint32_t) override;
};

class Win32Window final : public IWindow
{
public:
    bool Create(const WindowDesc& desc) override;
    void AttachInput(Win32Input* input) { m_input = input; if (m_input && m_hwnd) m_input->AttachWindow(m_hwnd); }
    void Destroy() override;
    WindowEventState PumpEvents(IInput& input) override;
    bool IsOpen() const override;
    void RequestClose() override;
    void Resize(uint32_t w, uint32_t h) override;
    void SetTitle(const char* title) override;
    void* GetNativeHandle() const override;
    uint32_t GetWidth() const override;
    uint32_t GetHeight() const override;
    const char* GetBackendName() const override;

    static LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
    static void RegisterClassOnce();

private:
    HWND m_hwnd = nullptr;
    uint32_t m_width = 0u;
    uint32_t m_height = 0u;
    bool m_open = false;
    bool m_closeReq = false;
    bool m_resizePending = false;
    std::string m_title;
    Win32Input* m_input = nullptr;
};

#endif

} // namespace engine::platform::win32
