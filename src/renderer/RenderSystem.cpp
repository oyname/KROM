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

    m_device = m_deviceFactoryRegistry.Create(backend);
    if (!m_device || !m_device->Initialize(deviceDesc))
        return false;

    IDevice::SwapchainDesc scDesc;
    scDesc.nativeWindowHandle = window.GetNativeHandle();
    scDesc.width      = window.GetWidth();
    scDesc.height     = window.GetHeight();
    scDesc.bufferCount = 2u;
    scDesc.vsync      = true;
    scDesc.windowMode = windowDesc.windowMode;
    scDesc.debugName  = "MainSwapchain";

    m_swapchain = m_device->CreateSwapchain(scDesc);
    m_graphicsCommandList = m_device->CreateCommandList(QueueType::Graphics);
    const QueueCapabilities computeCaps = m_device->GetQueueCapabilities(QueueType::Compute);
    const QueueCapabilities transferCaps = m_device->GetQueueCapabilities(QueueType::Transfer);
    if (computeCaps.supported)
        m_computeCommandList = m_device->CreateCommandList(QueueType::Compute);
    if (transferCaps.supported)
        m_transferCommandList = m_device->CreateCommandList(QueueType::Transfer);
    m_frameFence = m_device->CreateFence(0u);
    m_presentVsync = scDesc.vsync;
    m_gpuRuntime.Initialize(*m_device, std::max(3u, scDesc.bufferCount));
    m_shaderRuntime.SetGpuResourceRuntime(&m_gpuRuntime);
    if (!m_shaderRuntime.Initialize(*m_device))
    {
        Shutdown();
        return false;
    }
    m_jobSystem.Initialize();
    m_environmentSystem.SetGpuResourceRuntime(&m_gpuRuntime);
    m_environmentSystem.SetJobSystem(&m_jobSystem);
    if (!m_environmentSystem.Initialize(*m_device, m_shaderRuntime.GetAssetRegistry()))
    {
        Shutdown();
        return false;
    }
    m_initialized = (m_swapchain != nullptr && m_graphicsCommandList != nullptr && m_frameFence != nullptr);
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
    m_environmentSystem.Shutdown();
    m_shaderRuntime.Shutdown();
    m_gpuRuntime.Shutdown();
    m_transferCommandList.reset();
    m_computeCommandList.reset();
    m_graphicsCommandList.reset();
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
                               const FramePipelineCallbacks& callbacks)
{
    if (!m_initialized || !m_device || !m_swapchain || !m_graphicsCommandList)
        return false;

    if (m_swapchain->GetWidth() == 0u || m_swapchain->GetHeight() == 0u)
        return true;

    m_shaderRuntime.SetEnvironmentState(m_environmentSystem.ResolveRuntimeState());

    RenderFrameExecutionState frameState{};
    const RenderFrameOrchestratorContext context{
        world,
        materials,
        view,
        timing,
        callbacks,
        m_swapchain->GetCurrentBackbufferIndex(),
        m_swapchain->GetWidth(),
        m_swapchain->GetHeight(),
        *m_device,
        *m_swapchain,
        *m_graphicsCommandList,
        m_computeCommandList.get(),
        m_transferCommandList.get(),
        m_frameFence.get(),
        m_gpuRuntime,
        m_shaderRuntime,
        m_renderPassRegistry,
        m_featureRegistry,
        m_jobSystem,
        m_eventBus,
        m_stats,
        m_defaultTonemapMat,
        m_tonemapMaterialSystem,
        m_nextFenceValue,
        m_presentVsync
    };

    const bool ok = m_frameOrchestrator.Execute(context, frameState);
    if (ok)
        m_lastRenderSnapshot = std::move(frameState.extraction.snapshot);
    return ok;
}

} // namespace engine::renderer
