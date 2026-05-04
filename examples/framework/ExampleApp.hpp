#pragma once

#include "ExampleScene.hpp"
#include "addons/camera/CameraViewBuilder.hpp"
#include "core/AddonManager.hpp"
#include "core/ServiceRegistry.hpp"
#include "events/EventBus.hpp"
#include "platform/StdTiming.hpp"
#include "renderer/IDevice.hpp"
#include "platform/IWindow.hpp"
#include <array>
#include <memory>
#include <string>

namespace engine::examples {

struct ExampleAppConfig
{
    // -- Window --
    std::string                    windowTitle = "KROM Example";
    uint32_t                       width       = 1280u;
    uint32_t                       height      = 720u;
    platform::WindowMode           windowMode  = platform::WindowMode::Windowed;

    // -- Rendering --
    renderer::DeviceFactory::BackendType backend = renderer::DeviceFactory::BackendType::Vulkan;
    bool                 enableDebugLayer = true;
    std::array<float, 4> clearColor       = { 0.3f, 0.3f, 0.3f, 1.f };

    // -- World --
    math::Vec3 ambientColor     { 0.06f, 0.06f, 0.08f };
    float      ambientIntensity = 1.0f;
};

class ExampleApp
{
public:
    ExampleApp() = default;
    ~ExampleApp();

    ExampleApp(const ExampleApp&) = delete;
    ExampleApp& operator=(const ExampleApp&) = delete;

    [[nodiscard]] bool Initialize(const ExampleAppConfig& config);
    [[nodiscard]] int Run(IExampleScene& scene);
    void Shutdown();

private:
    [[nodiscard]] bool InitializePlatform();
    [[nodiscard]] bool InitializeRenderLoop();
    [[nodiscard]] bool InitializeAssetPipeline();
    [[nodiscard]] bool InitializeTonemapMaterial();
    ExampleAppConfig m_config{};
    renderer::DeviceFactory::Registry m_deviceFactoryRegistry;
    events::EventBus m_eventBus;
    std::unique_ptr<platform::IPlatform> m_platform;
    renderer::PlatformRenderLoop m_renderLoop;
    assets::AssetRegistry m_assetRegistry;
    std::unique_ptr<assets::AssetPipeline> m_assetPipeline;
    renderer::MaterialSystem m_materialSystem;
    ecs::ComponentMetaRegistry m_componentRegistry;
    std::unique_ptr<ecs::World> m_world;
    TransformSystem m_transformSystem;
    platform::StdTiming m_timing;
    engine::addons::camera::CameraBuildOptions m_cameraOptions{};
    ServiceRegistry m_addonServices;
    AddonManager m_addonManager;
    bool m_initialized = false;
};

[[nodiscard]] renderer::DeviceFactory::BackendType SelectExampleBackend();
[[nodiscard]] const char* BackendDisplayName(renderer::DeviceFactory::BackendType backend) noexcept;

} // namespace engine::examples
