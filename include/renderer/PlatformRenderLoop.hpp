#pragma once

#include "events/EventBus.hpp"
#include "platform/IPlatform.hpp"
#include "platform/IPlatformTiming.hpp"
#include "renderer/MaterialSystem.hpp"
#include "renderer/RenderSystem.hpp"
#include "renderer/StandardFramePipeline.hpp"

namespace engine::ecs { class World; }

namespace engine::renderer {

class PlatformRenderLoop
{
public:
    bool Initialize(DeviceFactory::BackendType backend,
                    platform::IPlatform& platform,
                    const platform::WindowDesc& windowDesc,
                    events::EventBus* eventBus = nullptr,
                    const IDevice::DeviceDesc& deviceDesc = {});
    void Shutdown();

    bool Tick(const ecs::World& world,
              const MaterialSystem& materials,
              const RenderView& view,
              platform::IPlatformTiming& timing,
              const FramePipelineCallbacks& callbacks = {});

    [[nodiscard]] bool ShouldExit() const noexcept { return m_shouldExit || m_window == nullptr; }
    [[nodiscard]] platform::IWindow* GetWindow() const noexcept { return m_window; }
    [[nodiscard]] platform::IInput* GetInput() const noexcept { return m_input; }
    [[nodiscard]] RenderSystem& GetRenderSystem() noexcept { return m_renderer; }
    [[nodiscard]] const RenderSystem& GetRenderSystem() const noexcept { return m_renderer; }

private:
    platform::IPlatform*  m_platform   = nullptr;
    platform::IWindow*    m_window     = nullptr;
    platform::IInput*     m_input      = nullptr;
    events::EventBus*     m_eventBus   = nullptr;
    platform::WindowDesc  m_windowDesc;
    RenderSystem          m_renderer;
    bool                  m_shouldExit = false;
};

} // namespace engine::renderer
