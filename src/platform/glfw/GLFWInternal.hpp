#pragma once

#include "platform/GLFWPlatform.hpp"
#include "platform/PlatformInput.hpp"

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <queue>
#include <thread>

struct GLFWwindow;

namespace engine::platform {

class GLFWInput final : public StandardInput
{
public:
    bool PollEvent(InputEvent& outEvent) override;
    void SetCursorMode(bool captured, bool hidden) override;
    MousePosition GetMousePosition() const override;

    void OnKeyEvent(const InputKeyEvent& e) override;
    void OnMouseButtonEvent(const InputMouseButtonEvent& e) override;
    void OnMouseMoveEvent(const InputMouseMoveEvent& e) override;
    void OnMouseScrollEvent(const InputMouseScrollEvent& e) override;

    void AttachWindow(GLFWwindow* window);

private:
    template <typename Fn>
    void PushEvent(Fn&& fn)
    {
        std::scoped_lock lock(m_mutex);
        InputEvent ev{};
        fn(ev);
        m_events.push(ev);
    }

    mutable std::mutex m_mutex;
    std::queue<InputEvent> m_events;
    GLFWwindow* m_window = nullptr;
};

class GLFWThread final : public IThread
{
public:
    ~GLFWThread() override;

    void Start(const std::function<void()>& entryPoint) override;
    void Join() override;
    void SetPriority(ThreadPriority priority) override;
    void SetAffinity(const ThreadAffinity& affinity) override;
    void SetName(const char* name) override;
    [[nodiscard]] bool IsRunning() const override { return m_running.load(); }

private:
    void ApplyCurrentThreadSettings();

    std::thread m_thread;
    std::atomic<bool> m_running{false};
    std::function<void()> m_entryPoint;
    ThreadPriority m_priority = ThreadPriority::Normal;
    ThreadAffinity m_affinity{};
    std::string m_name;
};

class StdMutex final : public IMutex
{
public:
    void Lock() override { m_mutex.lock(); }
    void Unlock() override { m_mutex.unlock(); }
    [[nodiscard]] bool TryLock() override { return m_mutex.try_lock(); }
private:
    std::mutex m_mutex;
};

class NullJobSystem final : public IJobSystem
{
public:
    explicit NullJobSystem(uint32_t workers) : m_workers(workers) {}
    void Initialize(uint32_t numWorkerThreads) override { m_workers = numWorkerThreads; }
    void Shutdown() override {}
private:
    uint32_t m_workers = 0;
};

class GLFWThreadFactory final : public IThreadFactory
{
public:
    IThread* CreateThread() override;
    void DestroyThread(IThread* thread) override;
    [[nodiscard]] int GetHardwareConcurrency() const override;
    void SleepMs(int milliseconds) const override;
    IMutex* CreateMutex() override;
    IJobSystem* CreateJobSystem(uint32_t numWorkerThreads) override;
};

class GLFWWindow final : public IWindow
{
public:
    explicit GLFWWindow(GLFWInput& input);
    ~GLFWWindow() override;

    bool Create(const WindowDesc& desc) override;
    void Destroy() override;
    WindowEventState PumpEvents(IInput& input) override;
    [[nodiscard]] bool IsOpen() const override;
    [[nodiscard]] bool ShouldClose() const override;
    void RequestClose() override;
    void Resize(uint32_t width, uint32_t height) override;
    void SetTitle(const char* title) override;
    [[nodiscard]] void* GetNativeHandle() const override;
    [[nodiscard]] void* GetGLFWHandle() const override { return m_handle; }
    [[nodiscard]] uint32_t GetWidth() const override { return m_width; }
    [[nodiscard]] uint32_t GetHeight() const override { return m_height; }
    [[nodiscard]] uint32_t GetFramebufferWidth() const override { return m_fbWidth; }
    [[nodiscard]] uint32_t GetFramebufferHeight() const override { return m_fbHeight; }
    [[nodiscard]] float GetDPIScale() const override { return m_dpiScale; }
    [[nodiscard]] const char* GetBackendName() const override { return "GLFWWindow"; }

    void HandleFramebufferResize(int w, int h);
    void HandleWindowResize(int w, int h);

private:
    GLFWwindow* m_handle = nullptr;
    bool m_hasGLContext = false;
    GLFWInput& m_input;
    uint32_t m_width = 0u;
    uint32_t m_height = 0u;
    uint32_t m_fbWidth = 0u;
    uint32_t m_fbHeight = 0u;
    float m_dpiScale = 1.0f;
    bool m_resizePending = false;
    std::string m_title;
};

} // namespace engine::platform
