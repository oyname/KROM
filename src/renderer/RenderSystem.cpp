#include "renderer/RenderSystem.hpp"
#include <algorithm>

namespace engine::renderer {

bool RenderSystem::Initialize(DeviceFactory::BackendType backend,
                              platform::IWindow& window,
                              const platform::WindowDesc& windowDesc,
                              events::EventBus* eventBus,
                              const IDevice::DeviceDesc& deviceDesc)
{
    m_eventBus = eventBus;
    m_device = DeviceFactory::Create(backend);
    if (!m_device || !m_device->Initialize(deviceDesc))
        return false;

    IDevice::SwapchainDesc scDesc;
    scDesc.nativeWindowHandle = window.GetNativeHandle();
    scDesc.width = window.GetWidth();
    scDesc.height = window.GetHeight();
    scDesc.bufferCount = 2u;
    scDesc.vsync = true;
    scDesc.debugName = "MainSwapchain";
    scDesc.openglMajor = windowDesc.openglMajor;
    scDesc.openglMinor = windowDesc.openglMinor;
    scDesc.openglDebugContext = windowDesc.openglDebugContext;

    m_swapchain = m_device->CreateSwapchain(scDesc);
    m_commandList = m_device->CreateCommandList(QueueType::Graphics);
    m_frameFence = m_device->CreateFence(0u);
    m_isOpenGLBackend = (backend == DeviceFactory::BackendType::OpenGL);
    m_gpuRuntime.Initialize(*m_device, std::max(3u, scDesc.bufferCount));
    m_shaderRuntime.Initialize(*m_device);
    m_jobSystem.Initialize();
    m_initialized = (m_swapchain != nullptr && m_commandList != nullptr && m_frameFence != nullptr);
    if (!m_initialized)
    {
        Shutdown();
        return false;
    }

    FeatureInitializationContext featureInit{*m_device, m_shaderRuntime, m_eventBus};
    if (!m_featureRegistry.InitializeAll(featureInit))
    {
        Shutdown();
        return false;
    }
    return true;
}

void RenderSystem::Shutdown()
{
    if (m_device)
        m_device->WaitIdle();

    m_featureRegistry.ShutdownAll(FeatureShutdownContext{m_eventBus});
    m_shaderRuntime.Shutdown();
    m_gpuRuntime.Shutdown();
    m_commandList.reset();
    m_frameFence.reset();
    m_swapchain.reset();
    if (m_device)
    {
        m_device->Shutdown();
        m_device.reset();
    }
    m_jobSystem.Shutdown();
    m_initialized = false;
}

void RenderSystem::HandleResize(uint32_t width, uint32_t height)
{
    if (!m_swapchain)
        return;
    m_swapchain->Resize(width, height);
    if (m_eventBus)
        m_eventBus->Publish(events::WindowResizedEvent{width, height});
}


bool RenderSystem::RenderFrame(const ecs::World& world,
                               const MaterialSystem& materials,
                               const RenderView& view,
                               const platform::IPlatformTiming& timing,
                               const rendergraph::FramePipelineCallbacks& callbacks)
{
    if (!m_initialized || !m_device || !m_swapchain || !m_commandList)
        return false;

    RenderFrameExecutionState frameState{};
    const RenderFrameOrchestratorContext context{
        world,
        materials,
        view,
        timing,
        callbacks,
        m_isOpenGLBackend,
        m_swapchain->GetCurrentBackbufferIndex(),
        m_swapchain->GetWidth(),
        m_swapchain->GetHeight(),
        *m_device,
        *m_swapchain,
        *m_commandList,
        m_frameFence.get(),
        m_gpuRuntime,
        m_shaderRuntime,
        m_renderWorld,
        m_featureRegistry,
        m_jobSystem,
        m_eventBus,
        m_stats,
        m_defaultTonemapMat,
        m_tonemapMaterialSystem,
        m_nextFenceValue
    };

    return m_frameOrchestrator.Execute(context, frameState);
}

} // namespace engine::renderer
