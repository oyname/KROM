#pragma once

#include "platform/IPlatform.hpp"

#include <chrono>
#include <memory>
#include <vector>

namespace engine::platform::win32 {

class Win32Input;

class Win32Platform final : public IPlatform
{
public:
    ~Win32Platform() override;
    bool Initialize() override;
    void Shutdown() override;
    void PumpEvents() override;
    double GetTimeSeconds() const override;
    IWindow* CreateWindow(const WindowDesc& desc) override;
    IInput* GetInput() override;
    IThreadFactory* GetThreadFactory() override;

private:
    std::unique_ptr<IInput> m_input;
    std::unique_ptr<IThreadFactory> m_threadFactory;
    std::vector<std::unique_ptr<IWindow>> m_windows;
    std::chrono::steady_clock::time_point m_start = std::chrono::steady_clock::now();
};

} // namespace engine::platform::win32
