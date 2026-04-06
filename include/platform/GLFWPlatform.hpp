#pragma once

#include "platform/IPlatform.hpp"
#include <memory>
#include <vector>

namespace engine::platform {

class GLFWInput;
class GLFWThreadFactory;
class GLFWWindow;

class GLFWPlatform final : public IPlatform
{
public:
    GLFWPlatform();
    ~GLFWPlatform() override;

    bool Initialize() override;
    void Shutdown() override;
    void PumpEvents() override;
    [[nodiscard]] double GetTimeSeconds() const override;
    [[nodiscard]] IWindow* CreateWindow(const WindowDesc& desc) override;
    [[nodiscard]] IInput* GetInput() override;
    [[nodiscard]] IThreadFactory* GetThreadFactory() override;

private:
    std::unique_ptr<GLFWInput> m_input;
    std::unique_ptr<GLFWThreadFactory> m_threadFactory;
    std::vector<std::unique_ptr<IWindow>> m_windows;
    double m_timeOffset = 0.0;
    bool m_initialized = false;
};

[[nodiscard]] bool IsGLFWPlatformAvailable() noexcept;

} // namespace engine::platform
