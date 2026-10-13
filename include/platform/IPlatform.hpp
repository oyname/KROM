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

        // Gibt die Auflösung des primären Monitors zurück.
        // Standardimplementierung liefert 0/0 (nicht verfügbar).
        virtual void GetPrimaryMonitorSize(uint32_t& outWidth, uint32_t& outHeight) const
        {
            outWidth  = 0u;
            outHeight = 0u;
        }
    };

} // namespace engine::platform
