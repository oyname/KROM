#include "renderer/PlatformRenderLoop.hpp"

#include "ecs/World.hpp"
#include "core/Debug.hpp"

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
    {
        Debug::LogError("PlatformRenderLoop::Tick: platform/window/input missing");
        return false;
    }

    timing.BeginFrame();
    m_platform->PumpEvents();
    const platform::WindowEventState state = m_window->PumpEvents(*m_input);

    Debug::Log("PlatformRenderLoop::Tick: quit=%d shouldClose=%d resized=%d size=%ux%u fb=%ux%u",
                   state.quitRequested ? 1 : 0,
                   m_window->ShouldClose() ? 1 : 0,
                   state.resized ? 1 : 0,
                   state.width,
                   state.height,
                   state.framebufferWidth,
                   state.framebufferHeight);

    if (state.resized)
    {
        const uint32_t resizeWidth = state.framebufferWidth > 0u ? state.framebufferWidth : state.width;
        const uint32_t resizeHeight = state.framebufferHeight > 0u ? state.framebufferHeight : state.height;
        m_renderer.HandleResize(resizeWidth, resizeHeight);
    }

    if (state.quitRequested || m_window->ShouldClose())
    {
        Debug::Log("PlatformRenderLoop::Tick: exiting because window requested close");
        m_shouldExit = true;
        timing.EndFrame();
        return false;
    }

    const bool rendered = m_renderer.RenderFrame(world, materials, view, timing, callbacks);
    Debug::Log("PlatformRenderLoop::Tick: RenderFrame returned %d", rendered ? 1 : 0);
    timing.EndFrame();
    return rendered;
}

} // namespace engine::renderer
