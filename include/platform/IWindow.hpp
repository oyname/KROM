#pragma once
#include <cstdint>
#include <memory>
#include <string>

namespace engine::platform {

enum class WindowMode
{
    Windowed,           // normales Fenster, optional resizable
    BorderlessWindowed, // rahmenloses Fenster in Desktop-Auflösung
    Fullscreen,         // exklusives Fullscreen
};

struct WindowDesc
{
    uint32_t   width      = 1280u;
    uint32_t   height     = 720u;
    bool       visible    = true;
    bool       resizable  = true;
    WindowMode windowMode = WindowMode::Windowed;
    uint32_t framebufferWidth = 1280u;
    uint32_t framebufferHeight = 720u;
    std::string title = "KROM Engine";

    // OpenGL context hints - used by the platform and swapchain to create the
    // correct context version.  On Win32 GetNativeHandle() returns an HWND and
    // the swapchain creates a WGL Core Profile context from it.  On GLFW-based
    // platforms GetNativeHandle() returns a GLFWwindow* and the swapchain calls
    // glfwMakeContextCurrent().  Non-OpenGL backends ignore these fields.
    bool openglContext = false;
    int openglMajor = 4;
    int openglMinor = 1;
    bool openglDebugContext = false;
};

struct WindowEventState
{
    bool quitRequested = false;
    bool resized = false;
    uint32_t width = 0u;
    uint32_t height = 0u;
    uint32_t framebufferWidth = 0u;
    uint32_t framebufferHeight = 0u;
};

class IInput;

class IWindow
{
public:
    virtual ~IWindow() = default;

    virtual bool Create(const WindowDesc& desc) = 0;
    virtual void Destroy() = 0;
    virtual WindowEventState PumpEvents(IInput& input) = 0;

    [[nodiscard]] virtual bool IsOpen() const = 0;
    [[nodiscard]] virtual bool ShouldClose() const { return !IsOpen(); }
    virtual void RequestClose() = 0;
    virtual void Resize(uint32_t width, uint32_t height) = 0;
    virtual void SetTitle(const char* title) { (void)title; }

    [[nodiscard]] virtual void* GetNativeHandle() const = 0;
    [[nodiscard]] virtual void* GetGLFWHandle() const { return nullptr; }
    [[nodiscard]] virtual uint32_t GetWidth() const = 0;
    [[nodiscard]] virtual uint32_t GetHeight() const = 0;
    [[nodiscard]] virtual uint32_t GetFramebufferWidth() const { return GetWidth(); }
    [[nodiscard]] virtual uint32_t GetFramebufferHeight() const { return GetHeight(); }
    [[nodiscard]] virtual float GetDPIScale() const { return 1.0f; }
    [[nodiscard]] virtual const char* GetBackendName() const = 0;
};

class HeadlessWindow final : public IWindow
{
public:
    bool Create(const WindowDesc& desc) override;
    void Destroy() override;
    WindowEventState PumpEvents(IInput& input) override;

    [[nodiscard]] bool IsOpen() const override { return m_open; }
    [[nodiscard]] bool ShouldClose() const override { return !m_open || m_quitRequested; }
    void RequestClose() override { m_quitRequested = true; }
    void Resize(uint32_t width, uint32_t height) override;
    void SetTitle(const char* title) override;

    [[nodiscard]] void* GetNativeHandle() const override { return nullptr; }
    [[nodiscard]] void* GetGLFWHandle() const override { return nullptr; }
    [[nodiscard]] uint32_t GetWidth() const override { return m_width; }
    [[nodiscard]] uint32_t GetHeight() const override { return m_height; }
    [[nodiscard]] uint32_t GetFramebufferWidth() const override { return m_fbWidth; }
    [[nodiscard]] uint32_t GetFramebufferHeight() const override { return m_fbHeight; }
    [[nodiscard]] float GetDPIScale() const override { return 1.0f; }
    [[nodiscard]] const char* GetBackendName() const override { return "HeadlessWindow"; }

private:
    bool m_open = false;
    bool m_quitRequested = false;
    bool m_resizePending = false;
    uint32_t m_width = 0u;
    uint32_t m_height = 0u;
    uint32_t m_fbWidth = 0u;
    uint32_t m_fbHeight = 0u;
    std::string m_title;
};

std::unique_ptr<IWindow> CreateHeadlessWindow();

} // namespace engine::platform
