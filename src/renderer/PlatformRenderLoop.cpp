#include "renderer/PlatformRenderLoop.hpp"

#include "ecs/World.hpp"

namespace engine::renderer {

bool PlatformRenderLoop::Initialize(DeviceFactory::BackendType backend,
                                    platform::IPlatform& platform,
                                    const platform::WindowDesc& windowDesc,
                                    events::EventBus* eventBus,
                                    const IDevice::DeviceDesc& deviceDesc)
{
    Shutdown();

    m_platform   = &platform;
    m_eventBus   = eventBus;
    m_windowDesc = windowDesc;
    m_input      = platform.GetInput();
    m_window     = platform.CreateWindow(windowDesc);
    if (!m_window || !m_input)
    {
        Shutdown();
        return false;
    }

    if (!m_renderer.Initialize(backend, *m_window, windowDesc, eventBus, deviceDesc))
    {
        Shutdown();
        return false;
    }

    m_shouldExit = false;
    return true;
}

void PlatformRenderLoop::Shutdown()
{
    m_shouldExit = false;
    m_renderer.Shutdown();
    if (m_window)
    {
        m_window->Destroy();
        m_window = nullptr;
    }
    m_input = nullptr;
    m_platform = nullptr;
    m_eventBus = nullptr;
}

bool PlatformRenderLoop::Tick(const ecs::World& world,
                              const MaterialSystem& materials,
                              const RenderView& view,
                              platform::IPlatformTiming& timing,
                              const rendergraph::FramePipelineCallbacks& callbacks)
{
    if (!m_platform || !m_window || !m_input)
        return false;

    timing.BeginFrame();
    m_platform->PumpEvents();
    const platform::WindowEventState state = m_window->PumpEvents(*m_input);

    if (state.resized)
    {
        const uint32_t resizeWidth = state.framebufferWidth > 0u ? state.framebufferWidth : state.width;
        const uint32_t resizeHeight = state.framebufferHeight > 0u ? state.framebufferHeight : state.height;
        if (resizeWidth > 0u && resizeHeight > 0u)
            m_renderer.HandleResize(resizeWidth, resizeHeight);
    }

    if (state.quitRequested || m_window->ShouldClose())
    {
        m_shouldExit = true;
        timing.EndFrame();
        return false;
    }

    const bool rendered = m_renderer.RenderFrame(world, materials, view, timing, callbacks);
    timing.EndFrame();
    return rendered;
}

} // namespace engine::renderer
