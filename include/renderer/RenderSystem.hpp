#pragma once
#include "events/EventBus.hpp"
#include "jobs/JobSystem.hpp"
#include "platform/IWindow.hpp"
#include "platform/IPlatformTiming.hpp"
#include "renderer/FeatureRegistry.hpp"
#include "renderer/EnvironmentSystem.hpp"
#include "renderer/IDevice.hpp"
#include "renderer/GpuResourceRuntime.hpp"
#include "renderer/ShaderBindingModel.hpp"
#include "renderer/MaterialSystem.hpp"
#include "renderer/ShaderRuntime.hpp"
#include "renderer/RenderWorld.hpp"
#include "renderer/RenderFrameOrchestrator.hpp"
#include "renderer/RenderFrameTypes.hpp"
#include "renderer/StandardFramePipeline.hpp"
#include <memory>

namespace engine::renderer {

class RenderSystem
{
public:
    bool Initialize(DeviceFactory::BackendType backend,
                    platform::IWindow& window,
                    const platform::WindowDesc& windowDesc = {},
                    events::EventBus* eventBus = nullptr,
                    const IDevice::DeviceDesc& deviceDesc = {});
    void Shutdown();

    void HandleResize(uint32_t width, uint32_t height);
    bool RenderFrame(const ecs::World& world,
                     const MaterialSystem& materials,
                     const RenderView& view,
                     const platform::IPlatformTiming& timing,
                     const FramePipelineCallbacks& callbacks = {});

    // Optionales Default-Tonemap-Material.
    // Wird automatisch für den TonemapPass genutzt wenn kein onTonemap-Callback gesetzt ist.
    // Erwartet einen Fullscreen-Passthrough-Shader (kein Vertex-Buffer, kein Depth-Test).
    // Muss vor dem ersten RenderFrame gesetzt werden.
    void SetDefaultTonemapMaterial(MaterialHandle h, const MaterialSystem& ms) noexcept
    {
        m_defaultTonemapMat = h;
        m_tonemapMaterialSystem = &ms;
    }

    [[nodiscard]] bool IsInitialized() const noexcept { return m_device != nullptr; }
    [[nodiscard]] const RenderStats& GetStats() const noexcept { return m_stats; }
    [[nodiscard]] const RenderWorld& GetRenderWorld() const noexcept { return m_renderWorld; }
    [[nodiscard]] IDevice* GetDevice() const noexcept { return m_device.get(); }
    [[nodiscard]] ISwapchain* GetSwapchain() const noexcept { return m_swapchain.get(); }
    [[nodiscard]] ShaderRuntime& GetShaderRuntime() noexcept { return m_shaderRuntime; }
    [[nodiscard]] const ShaderRuntime& GetShaderRuntime() const noexcept { return m_shaderRuntime; }
    [[nodiscard]] EnvironmentHandle CreateEnvironment(const EnvironmentDesc& desc) { return m_environmentSystem.CreateEnvironment(desc); }
    void DestroyEnvironment(EnvironmentHandle handle) { m_environmentSystem.DestroyEnvironment(handle); }
    void SetActiveEnvironment(EnvironmentHandle handle) noexcept
    {
        m_environmentSystem.SetActiveEnvironment(handle);
        m_shaderRuntime.SetEnvironmentState(m_environmentSystem.ResolveRuntimeState());
    }
    [[nodiscard]] EnvironmentHandle GetActiveEnvironment() const noexcept { return m_environmentSystem.GetActiveEnvironment(); }
    [[nodiscard]] FeatureRegistry& GetFeatureRegistry() noexcept { return m_featureRegistry; }
    [[nodiscard]] const FeatureRegistry& GetFeatureRegistry() const noexcept { return m_featureRegistry; }
    bool RegisterFeature(std::unique_ptr<IEngineFeature> feature) { return m_featureRegistry.AddFeature(std::move(feature)); }
    void SetAssetRegistry(assets::AssetRegistry* registry) noexcept { m_shaderRuntime.SetAssetRegistry(registry); m_environmentSystem.SetAssetRegistry(registry); }

private:
    std::unique_ptr<IDevice> m_device;
    std::unique_ptr<ISwapchain> m_swapchain;
    std::unique_ptr<ICommandList> m_graphicsCommandList;
    std::unique_ptr<ICommandList> m_computeCommandList;
    std::unique_ptr<ICommandList> m_transferCommandList;
    std::unique_ptr<IFence> m_frameFence;
    GpuResourceRuntime m_gpuRuntime;
    ShaderRuntime m_shaderRuntime;
    EnvironmentSystem m_environmentSystem;
    uint64_t m_nextFenceValue = 1u;
    RenderWorld m_renderWorld;
    events::EventBus* m_eventBus = nullptr;
    RenderStats m_stats{};
    bool m_initialized = false;
    bool m_presentVsync = true;
    MaterialHandle        m_defaultTonemapMat;
    const MaterialSystem* m_tonemapMaterialSystem = nullptr;
    FeatureRegistry m_featureRegistry;
    jobs::JobSystem m_jobSystem;
    RenderFrameOrchestrator m_frameOrchestrator;
};

} // namespace engine::renderer
