#include "ExampleApp.hpp"

#include "addons/camera/CameraViewBuilder.hpp"
#include "addons/mesh_renderer/MeshAssetSceneBindings.hpp"
#include "addons/runtime/EngineAddonAdapters.hpp"
#include "core/AddonContext.hpp"
#include "core/Debug.hpp"
#include "core/Logger.hpp"

#if defined(KROM_EXAMPLE_USE_WIN32_PLATFORM)
#include "platform/Win32Platform.hpp"
#elif defined(KROM_EXAMPLE_USE_GLFW_PLATFORM)
#include "platform/GLFWPlatform.hpp"
#else
#error Example platform not configured. Define KROM_EXAMPLE_USE_WIN32_PLATFORM or KROM_EXAMPLE_USE_GLFW_PLATFORM.
#endif

#include <filesystem>
#include <memory>

namespace engine::examples {

namespace {

std::filesystem::path ResolveAssetRoot()
{
    return std::filesystem::path(__FILE__).parent_path().parent_path().parent_path() / "assets";
}

} // namespace

ExampleApp::~ExampleApp()
{
    Shutdown();
}

bool ExampleApp::Initialize(const ExampleAppConfig& config)
{
    if (m_initialized)
        return true;

    m_config = config;
    m_cameraOptions.ambientColor = config.ambientColor;
    m_cameraOptions.ambientIntensity = config.ambientIntensity;

    m_addonManager.Reset();
    m_addonServices.Clear();
    m_addonServices.Register<ecs::ComponentMetaRegistry>(&m_componentRegistry);
    m_addonServices.Register<renderer::RenderSystem>(&m_renderLoop.GetRenderSystem());

    if (!m_addonManager.AddAddon(CreateCameraAddon()) ||
        !m_addonManager.AddAddon(CreateMeshRendererAddon()) ||
        !m_addonManager.AddAddon(CreateLightingAddon()) ||
        !m_addonManager.AddAddon(CreateShadowAddon()) ||
        !m_addonManager.AddAddon(CreateForwardAddon(m_config.clearColor, true)))
    {
        Debug::LogError("ExampleApp: failed to queue engine add-ons");
        return false;
    }

    AddonContext addonContext{ GetDefaultLogger(), m_eventBus, m_addonServices };
    if (!m_addonManager.RegisterAll(addonContext))
    {
        Debug::LogError("ExampleApp: failed to register engine add-ons");
        return false;
    }

    // Any failure after this point must undo addon registration so that a
    // subsequent Initialize() call starts from a clean state.
    bool renderLoopInitialized = false;

    auto rollback = [&]() noexcept
    {
        m_assetPipeline.reset();
        if (renderLoopInitialized)
            m_renderLoop.Shutdown();
        if (m_platform)
        {
            m_platform->Shutdown();
            m_platform.reset();
        }
        m_addonManager.UnregisterAll(addonContext);
        m_world.reset();
        m_addonServices.Clear();
        m_addonManager.Reset();
    };

    RegisterCoreComponents(m_componentRegistry);
    m_world = std::make_unique<ecs::World>(m_componentRegistry);

    if (!InitializePlatform())
    {
        Debug::LogError("ExampleApp: platform initialization failed");
        rollback();
        return false;
    }

    if (!InitializeRenderLoop())
    {
        Debug::LogError("ExampleApp: render loop initialization failed");
        rollback();
        return false;
    }
    renderLoopInitialized = true;

    if (!InitializeAssetPipeline())
    {
        Debug::LogError("ExampleApp: asset pipeline initialization failed");
        rollback();
        return false;
    }

    if (!InitializeTonemapMaterial())
    {
        Debug::LogError("ExampleApp: tonemap material initialization failed");
        rollback();
        return false;
    }

    m_initialized = true;
    return true;
}

int ExampleApp::Run(IExampleScene& scene)
{
    if (!m_initialized)
        return -1;

    ExampleSceneContext context{ m_assetRegistry, *m_assetPipeline, m_renderLoop, m_materialSystem, *m_world, m_transformSystem };
    if (!scene.Build(context))
        return -2;

    m_transformSystem.Update(*m_world);

    while (!m_renderLoop.ShouldExit())
    {
        if (auto* input = m_renderLoop.GetInput();
            input && input->IsKeyPressed(platform::Key::Escape) && m_renderLoop.GetWindow())
        {
            m_renderLoop.GetWindow()->RequestClose();
        }

        const float deltaSeconds = m_timing.GetDeltaSecondsF();
        if (!scene.Update(context, deltaSeconds))
            return -3;

        m_transformSystem.Update(*m_world);

        const renderer::ISwapchain* swapchain = m_renderLoop.GetRenderSystem().GetSwapchain();
        const uint32_t viewportWidth = (swapchain && swapchain->GetWidth() > 0u) ? swapchain->GetWidth() : m_config.width;
        const uint32_t viewportHeight = (swapchain && swapchain->GetHeight() > 0u) ? swapchain->GetHeight() : m_config.height;

        renderer::RenderView view{};
        if (!engine::addons::camera::BuildPrimaryRenderView(
                *m_world,
                viewportWidth,
                viewportHeight,
                view,
                m_cameraOptions))
        {
            Debug::LogError("ExampleApp: failed to build primary render view");
            return -4;
        }

        view.debugFlags = context.debugFlags;

        if (!m_renderLoop.Tick(*m_world, m_materialSystem, view, m_timing))
        {
            if (m_renderLoop.ShouldExit())
                break;
            return -5;
        }
    }

    return 0;
}

void ExampleApp::Shutdown()
{
    AddonContext addonContext{ GetDefaultLogger(), m_eventBus, m_addonServices };
    m_addonManager.UnregisterAll(addonContext);

    m_assetPipeline.reset();

    if (m_initialized)
        m_renderLoop.Shutdown();

    if (m_platform)
        m_platform->Shutdown();

    m_addonServices.Clear();
    m_addonManager.Reset();
    m_world.reset();
    m_platform.reset();
    m_initialized = false;
}

bool ExampleApp::InitializePlatform()
{
#if defined(KROM_EXAMPLE_USE_WIN32_PLATFORM)
    m_platform = std::make_unique<platform::win32::Win32Platform>();
#elif defined(KROM_EXAMPLE_USE_GLFW_PLATFORM)
    m_platform = std::make_unique<platform::GLFWPlatform>();
#endif

    return m_platform && m_platform->Initialize();
}

bool ExampleApp::InitializeRenderLoop()
{
    if (!m_deviceFactoryRegistry.IsRegistered(m_config.backend))
    {
        Debug::LogError("ExampleApp: backend '%s' is not registered", BackendDisplayName(m_config.backend));
        return false;
    }

    const auto adapters = m_deviceFactoryRegistry.EnumerateAdapters(m_config.backend);
    if (adapters.empty())
    {
        Debug::LogError("ExampleApp: backend '%s' reported no adapters", BackendDisplayName(m_config.backend));
        return false;
    }

    platform::WindowDesc windowDesc{};
    windowDesc.title      = m_config.windowTitle;
    windowDesc.width      = m_config.width;
    windowDesc.height     = m_config.height;
    windowDesc.windowMode = m_config.windowMode;
    windowDesc.resizable  = (m_config.windowMode == platform::WindowMode::Windowed);
    if (m_config.backend == renderer::DeviceFactory::BackendType::OpenGL)
    {
        windowDesc.openglContext = true;
        windowDesc.openglMajor = 4;
        windowDesc.openglMinor = 1;
        windowDesc.openglDebugContext = m_config.enableDebugLayer;
    }

    renderer::IDevice::DeviceDesc deviceDesc{};
    deviceDesc.enableDebugLayer = m_config.enableDebugLayer;
    deviceDesc.adapterIndex = renderer::DeviceFactory::FindBestAdapter(adapters);
    deviceDesc.appName = m_config.windowTitle;

    if (!m_renderLoop.Initialize(m_config.backend, *m_platform, windowDesc, &m_eventBus, deviceDesc))
    {
        Debug::LogError("ExampleApp: render loop initialization failed for backend '%s'", BackendDisplayName(m_config.backend));
        return false;
    }

    return true;
}

bool ExampleApp::InitializeAssetPipeline()
{
    m_renderLoop.GetRenderSystem().SetAssetRegistry(&m_assetRegistry);
    m_assetPipeline = std::make_unique<assets::AssetPipeline>(m_assetRegistry, m_renderLoop.GetRenderSystem().GetDevice());
    mesh_renderer::ConfigureAssetPipeline(*m_assetPipeline);
    m_assetPipeline->SetAssetRoot(ResolveAssetRoot().string());
    return true;
}

bool ExampleApp::InitializeTonemapMaterial()
{
    const char* tonemapVsPath = "fullscreen.vs.hlsl";
    const char* tonemapPsPath = "passthrough.ps.hlsl";
    if (m_config.backend == renderer::DeviceFactory::BackendType::OpenGL)
    {
        tonemapVsPath = "fullscreen.opengl.vs.glsl";
        tonemapPsPath = "passthrough.opengl.fs.glsl";
    }

    const ShaderHandle tonemapVs = m_assetPipeline->LoadShader(tonemapVsPath, assets::ShaderStage::Vertex);
    const ShaderHandle tonemapPs = m_assetPipeline->LoadShader(tonemapPsPath, assets::ShaderStage::Fragment);
    if (!tonemapVs.IsValid() || !tonemapPs.IsValid())
    {
        Debug::LogError("ExampleApp: failed to load tonemap shaders");
        return false;
    }

    renderer::DepthStencilState noDepth{};
    noDepth.depthEnable = false;
    noDepth.depthWrite = false;

    renderer::MaterialParam tonemapSampler{};
    tonemapSampler.name = "linearclamp";
    tonemapSampler.type = renderer::MaterialParam::Type::Sampler;
    tonemapSampler.samplerIdx = 0u;

    const renderer::ISwapchain* swapchain = m_renderLoop.GetRenderSystem().GetSwapchain();
    const renderer::Format backbufferFormat = swapchain ? swapchain->GetBackbufferFormat()
                                                        : renderer::Format::BGRA8_UNORM_SRGB;

    renderer::MaterialDesc tonemapDesc{};
    tonemapDesc.name = "ExampleTonemap";
    tonemapDesc.renderPass = renderer::StandardRenderPasses::Opaque();
    tonemapDesc.vertexShader = tonemapVs;
    tonemapDesc.fragmentShader = tonemapPs;
    // Fullscreen postprocess passes should not depend on winding conventions.
    tonemapDesc.rasterizer.cullMode = renderer::CullMode::None;
    tonemapDesc.depthStencil = noDepth;
    tonemapDesc.colorFormat = backbufferFormat;
    tonemapDesc.depthFormat = renderer::Format::Unknown;
    tonemapDesc.params.push_back(tonemapSampler);

    const MaterialHandle tonemapMaterial = m_materialSystem.RegisterMaterial(std::move(tonemapDesc));
    m_renderLoop.GetRenderSystem().SetDefaultTonemapMaterial(tonemapMaterial, m_materialSystem);
    return tonemapMaterial.IsValid();
}

renderer::DeviceFactory::BackendType SelectExampleBackend()
{
#if defined(KROM_EXAMPLE_BACKEND_DX11)
    return renderer::DeviceFactory::BackendType::DirectX11;
#elif defined(KROM_EXAMPLE_BACKEND_OPENGL)
    return renderer::DeviceFactory::BackendType::OpenGL;
#elif defined(KROM_EXAMPLE_BACKEND_VULKAN)
    return renderer::DeviceFactory::BackendType::Vulkan;
#else
#error Example backend not configured.
#endif
}

const char* BackendDisplayName(renderer::DeviceFactory::BackendType backend) noexcept
{
    switch (backend)
    {
    case renderer::DeviceFactory::BackendType::DirectX11: return "DirectX11";
    case renderer::DeviceFactory::BackendType::OpenGL: return "OpenGL";
    case renderer::DeviceFactory::BackendType::Vulkan: return "Vulkan";
    default: return "Unknown";
    }
}

} // namespace engine::examples
