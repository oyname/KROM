#pragma once

#include "platform/IInput.hpp"
#include "platform/IThread.hpp"
#include "platform/IWindow.hpp"

namespace engine::platform {

class IPlatform
{
public:
    virtual ~IPlatform() = default;

    virtual bool Initialize() = 0;
    virtual void Shutdown() = 0;
    virtual void PumpEvents() = 0;
    [[nodiscard]] virtual double GetTimeSeconds() const = 0;
    [[nodiscard]] virtual IWindow* CreateWindow(const WindowDesc& desc) = 0;
    [[nodiscard]] virtual IInput* GetInput() = 0;
    [[nodiscard]] virtual IThreadFactory* GetThreadFactory() = 0;
};

} // namespace engine::platform
