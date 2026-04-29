#include "ForwardFeature.hpp"
// =============================================================================
// KROM Engine - examples/dx11_render_loop.cpp
// =============================================================================

#include "addons/lighting/LightingFeature.hpp"
#include "addons/lighting/LightingComponents.hpp"
#include "addons/camera/CameraComponents.hpp"
#include "addons/camera/CameraViewBuilder.hpp"
#include "addons/mesh_renderer/MeshRendererFeature.hpp"
#include "addons/mesh_renderer/MeshRendererComponents.hpp"
#include "addons/lit/LitMaterial.hpp"
#include "addons/shadow/ShadowFeature.hpp"
#include "DX11Device.hpp"
#include "assets/AssetPipeline.hpp"
#include "addons/mesh_renderer/MeshAssetSceneBindings.hpp"
#include "assets/AssetRegistry.hpp"
#include "core/Debug.hpp"
#include "core/Math.hpp"
#include "ecs/Components.hpp"
#include "ecs/World.hpp"
#include "events/EventBus.hpp"
#include "platform/NullPlatform.hpp"
#include "platform/StdTiming.hpp"
#include "platform/Win32Platform.hpp"
#include "renderer/IDevice.hpp"
#include "renderer/MaterialSystem.hpp"
#include "renderer/PlatformRenderLoop.hpp"
#include "renderer/ShaderBindingModel.hpp"
#include "renderer/RendererTypes.hpp"
#include "scene/BoundsSystem.hpp"
#include "addons/mesh_renderer/MeshBounds.hpp"
#include "scene/TransformSystem.hpp"
#include <filesystem>
#include <memory>
#include <utility>

using namespace engine;

int main()
{
#ifdef _WIN32
    Debug::ResetMinLevelForBuild();
    ecs::ComponentMetaRegistry componentRegistry;
    RegisterCoreComponents(componentRegistry);
    RegisterMeshRendererComponents(componentRegistry);
    RegisterCameraComponents(componentRegistry);
    RegisterLightingComponents(componentRegistry);
    renderer::DeviceFactory::Registry deviceFactoryRegistry;

    if (!deviceFactoryRegistry.IsRegistered(renderer::DeviceFactory::BackendType::DirectX11))
    {
        Debug::LogError("dx11_render_loop: DX11 backend registration failed");
        return -10;
    }

    const auto adapters = deviceFactoryRegistry.EnumerateAdapters(
        renderer::DeviceFactory::BackendType::DirectX11);
    if (adapters.empty())
    {
        Debug::LogError("dx11_render_loop: no DX11 adapters found");
        return -11;
    }

    const uint32_t adapterIndex = renderer::DeviceFactory::FindBestAdapter(adapters);
    for (const auto& a : adapters)
    {
        Debug::Log("dx11_render_loop: adapter[%u] '%s' VRAM=%zu MB discrete=%d FL=%d",
            a.index,
            a.name.c_str(),
            a.dedicatedVRAM / (1024ull * 1024ull),
            static_cast<int>(a.isDiscrete),
            a.featureLevel);
    }
    Debug::Log("dx11_render_loop: selected adapter index %u", adapterIndex);

    platform::win32::Win32Platform winPlatform;
    if (!winPlatform.Initialize())
    {
        Debug::LogError("dx11_render_loop: Win32Platform Initialize failed");
        return -1;
    }

    events::EventBus bus;
    renderer::PlatformRenderLoop loop;
    if (!loop.GetRenderSystem().RegisterFeature(engine::addons::mesh_renderer::CreateMeshRendererFeature()) ||
        !loop.GetRenderSystem().RegisterFeature(engine::addons::lighting::CreateLightingFeature()) ||
        !loop.GetRenderSystem().RegisterFeature(engine::addons::shadow::CreateShadowFeature()) ||
        !loop.GetRenderSystem().RegisterFeature(engine::renderer::addons::forward::CreateForwardFeature()))
    {
        Debug::LogError("dx11_render_loop: feature registration failed");
        return -12;
    }

    platform::WindowDesc wDesc{};
    wDesc.title    = "KROM - Three Rotating Cubes with Shadows (DX11)";
    wDesc.width    = 1280;
    wDesc.height   = 720;
    wDesc.resizable = true;

    renderer::IDevice::DeviceDesc dDesc{};
    dDesc.enableDebugLayer = true;
    dDesc.appName          = "KROM DX11";
    dDesc.adapterIndex     = adapterIndex;

    if (!loop.Initialize(renderer::DeviceFactory::BackendType::DirectX11,
        winPlatform, wDesc, &bus, dDesc))
    {
        Debug::LogError("dx11_render_loop: loop.Initialize failed");
        winPlatform.Shutdown();
        return -2;
    }

    assets::AssetRegistry registry;
    loop.GetRenderSystem().SetAssetRegistry(&registry);

    assets::AssetPipeline pipeline(registry, loop.GetRenderSystem().GetDevice());
    mesh_renderer::ConfigureAssetPipeline(pipeline);

    const std::filesystem::path assetRoot =
        std::filesystem::path(__FILE__).parent_path().parent_path() / "assets";
    Debug::Log("dx11_render_loop: asset root = %s", assetRoot.string().c_str());
    pipeline.SetAssetRoot(assetRoot.string());

    const auto litShaderAssets = renderer::lit::LitMaterial::DefaultShaderAssetSet();
    const ShaderHandle vsHandle        = pipeline.LoadShader(litShaderAssets.vertexShader,   assets::ShaderStage::Vertex);
    const ShaderHandle psHandle        = pipeline.LoadShader(litShaderAssets.fragmentShader, assets::ShaderStage::Fragment);
    const ShaderHandle shadowVsHandle  = pipeline.LoadShader("shadow.vs.hlsl",               assets::ShaderStage::Vertex);
    const ShaderHandle tonemapVsHandle = pipeline.LoadShader("fullscreen.vs.hlsl",           assets::ShaderStage::Vertex);
    const ShaderHandle tonemapPsHandle = pipeline.LoadShader("passthrough.ps.hlsl",          assets::ShaderStage::Fragment);

    if (!vsHandle.IsValid() || !psHandle.IsValid() ||
        !shadowVsHandle.IsValid() || !tonemapVsHandle.IsValid() || !tonemapPsHandle.IsValid())
    {
        Debug::LogError("dx11_render_loop: shader load failed");
        loop.Shutdown();
        winPlatform.Shutdown();
        return -3;
    }

    {
        auto* vs = registry.shaders.Get(vsHandle);
        if (!vs || vs->sourceCode.empty())
        {
            Debug::LogError("dx11_render_loop: lit.vs.hlsl source leer");
            loop.Shutdown();
            winPlatform.Shutdown();
            return -4;
        }
        Debug::Log("dx11_render_loop: shaders gefunden (%zu bytes VS)", vs->sourceCode.size());
    }

    const TextureHandle texHandle = pipeline.LoadTexture("krom.bmp");
    pipeline.UploadPendingGpuAssets();
    const TextureHandle gpuTex = pipeline.GetGpuTexture(texHandle);
    if (!gpuTex.IsValid())
    {
        Debug::LogError("dx11_render_loop: texture upload failed");
        loop.Shutdown();
        winPlatform.Shutdown();
        return -5;
    }

    auto meshAsset = std::make_unique<assets::MeshAsset>();
    assets::SubMeshData cube;
    cube.positions = {
        // Front
        -0.5f,-0.5f, 0.5f,   0.5f,-0.5f, 0.5f,   0.5f, 0.5f, 0.5f,  -0.5f, 0.5f, 0.5f,
        // Back
         0.5f,-0.5f,-0.5f,  -0.5f,-0.5f,-0.5f,  -0.5f, 0.5f,-0.5f,   0.5f, 0.5f,-0.5f,
        // Left
        -0.5f,-0.5f,-0.5f,  -0.5f,-0.5f, 0.5f,  -0.5f, 0.5f, 0.5f,  -0.5f, 0.5f,-0.5f,
        // Right
         0.5f,-0.5f, 0.5f,   0.5f,-0.5f,-0.5f,   0.5f, 0.5f,-0.5f,   0.5f, 0.5f, 0.5f,
        // Top
        -0.5f, 0.5f, 0.5f,   0.5f, 0.5f, 0.5f,   0.5f, 0.5f,-0.5f,  -0.5f, 0.5f,-0.5f,
        // Bottom
        -0.5f,-0.5f,-0.5f,   0.5f,-0.5f,-0.5f,   0.5f,-0.5f, 0.5f,  -0.5f,-0.5f, 0.5f,
    };
    cube.normals = {
        0,0,1,  0,0,1,  0,0,1,  0,0,1,
        0,0,-1, 0,0,-1, 0,0,-1, 0,0,-1,
        -1,0,0, -1,0,0, -1,0,0, -1,0,0,
        1,0,0,  1,0,0,  1,0,0,  1,0,0,
        0,1,0,  0,1,0,  0,1,0,  0,1,0,
        0,-1,0, 0,-1,0, 0,-1,0, 0,-1,0,
    };
    cube.uvs = {
        0,1, 1,1, 1,0, 0,0,
        0,1, 1,1, 1,0, 0,0,
        0,1, 1,1, 1,0, 0,0,
        0,1, 1,1, 1,0, 0,0,
        0,1, 1,1, 1,0, 0,0,
        0,1, 1,1, 1,0, 0,0,
    };
    cube.indices = {
         0, 1, 2,  2, 3, 0,
         4, 5, 6,  6, 7, 4,
         8, 9,10, 10,11, 8,
        12,13,14, 14,15,12,
        16,17,18, 18,19,16,
        20,21,22, 22,23,20,
    };
    meshAsset->submeshes.push_back(std::move(cube));
    const MeshHandle meshHandle = registry.meshes.Add(std::move(meshAsset));

    renderer::MaterialSystem materials;

    renderer::VertexLayout vLayout;
    vLayout.attributes.push_back({ renderer::VertexSemantic::Position,  renderer::Format::RGB32_FLOAT, 0u,  0u });
    vLayout.attributes.push_back({ renderer::VertexSemantic::Normal,    renderer::Format::RGB32_FLOAT, 0u, 12u });
    vLayout.attributes.push_back({ renderer::VertexSemantic::TexCoord0, renderer::Format::RG32_FLOAT,  0u, 24u });
    vLayout.bindings.push_back({ 0u, 32u });

    renderer::lit::LitMaterialCreateInfo litInfo{};
    litInfo.name             = "CubeLit";
    litInfo.vertexShader     = vsHandle;
    litInfo.fragmentShader   = psHandle;
    litInfo.shadowShader     = shadowVsHandle;
    litInfo.vertexLayout     = vLayout;
    litInfo.colorFormat      = renderer::Format::RGBA16_FLOAT;
    litInfo.depthFormat      = renderer::Format::D24_UNORM_S8_UINT;
    litInfo.roughnessFactor  = 0.7f;
    litInfo.enableBaseColorMap = true;
    litInfo.castShadows      = true;
    renderer::lit::LitMaterial litMaterial = renderer::lit::LitMaterial::Create(materials, litInfo);
    if (!litMaterial.IsValid() || !litMaterial.SetAlbedo(gpuTex))
    {
        Debug::LogError("dx11_render_loop: failed to create lit material");
        loop.Shutdown();
        winPlatform.Shutdown();
        return -6;
    }
    const MaterialHandle material = litMaterial.Handle();

    renderer::lit::LitMaterialCreateInfo groundInfo = litInfo;
    groundInfo.name               = "GroundLitDirectX11";
    groundInfo.enableBaseColorMap = false;
    groundInfo.baseColorFactor    = { 0.72f, 0.74f, 0.78f, 1.0f };
    groundInfo.roughnessFactor    = 0.95f;
    groundInfo.specularStrength   = 0.04f;
    groundInfo.castShadows        = false;
    renderer::lit::LitMaterial groundMaterialAsset =
        renderer::lit::LitMaterial::Create(materials, groundInfo);
    if (!groundMaterialAsset.IsValid())
    {
        Debug::LogError("dx11_render_loop: failed to create ground material");
        loop.Shutdown();
        winPlatform.Shutdown();
        return -7;
    }
    const MaterialHandle groundMaterial = groundMaterialAsset.Handle();

    renderer::DepthStencilState noDepth{};
    noDepth.depthEnable = false;
    noDepth.depthWrite  = false;

    renderer::MaterialParam tonemapSamplerParam{};
    tonemapSamplerParam.name       = "linearclamp";
    tonemapSamplerParam.type       = renderer::MaterialParam::Type::Sampler;
    tonemapSamplerParam.samplerIdx = 0u;

    renderer::MaterialDesc tonemapDesc{};
    tonemapDesc.name           = "PassthroughDirectX11";
    tonemapDesc.renderPass     = renderer::StandardRenderPasses::Opaque();
    tonemapDesc.vertexShader   = tonemapVsHandle;
    tonemapDesc.fragmentShader = tonemapPsHandle;
    tonemapDesc.depthStencil   = noDepth;
    tonemapDesc.colorFormat    = renderer::Format::RGBA8_UNORM_SRGB;
    tonemapDesc.depthFormat    = renderer::Format::D24_UNORM_S8_UINT;
    tonemapDesc.params.push_back(tonemapSamplerParam);
    const MaterialHandle tonemapMaterial = materials.RegisterMaterial(std::move(tonemapDesc));
    loop.GetRenderSystem().SetDefaultTonemapMaterial(tonemapMaterial, materials);

    ecs::World world(componentRegistry);

    const auto createCubeEntity = [&](const math::Vec3& pos)
        {
            const auto e = world.CreateEntity();
            world.Add<TransformComponent>(e);
            world.Add<WorldTransformComponent>(e);
            world.Add<MeshComponent>(e, meshHandle);
            world.Add<MaterialComponent>(e, material);
            world.Add<BoundsComponent>(e, BoundsComponent{
                .centerLocal    = { 0.f, 0.f, 0.f },
                .extentsLocal   = { 0.5f, 0.5f, 0.5f },
                .centerWorld    = pos,
                .extentsWorld   = { 0.5f, 0.5f, 0.5f },
                .boundingSphere = 0.8660254f,
                .localDirty     = true
                });
            auto* tr = world.Get<TransformComponent>(e);
            if (tr)
            {
                tr->localPosition = pos;
                tr->localScale    = { 1.f, 1.f, 1.f };
                tr->SetEulerDeg(0.f, 0.f, 0.f);
            }
            return e;
        };

    const auto cubeA = createCubeEntity({ -1.75f, 0.0f, 0.0f });
    const auto cubeB = createCubeEntity({  0.0f,  0.0f, 0.0f });
    const auto cubeC = createCubeEntity({  1.75f, 0.0f, 0.0f });

    const EntityID groundEntity = world.CreateEntity();
    world.Add<TransformComponent>(groundEntity);
    world.Add<WorldTransformComponent>(groundEntity);
    world.Add<MeshComponent>(groundEntity, meshHandle);
    if (auto* groundMesh = world.Get<MeshComponent>(groundEntity))
        groundMesh->castShadows = false;
    world.Add<MaterialComponent>(groundEntity, groundMaterial);
    world.Add<BoundsComponent>(groundEntity, BoundsComponent{
        .centerLocal    = { 0.f, 0.f, 0.f },
        .extentsLocal   = { 0.5f, 0.5f, 0.5f },
        .centerWorld    = { 0.f, -1.25f, 0.f },
        .extentsWorld   = { 5.f, 0.15f, 5.f },
        .boundingSphere = 7.08f,
        .localDirty     = true
        });
    if (auto* groundTransform = world.Get<TransformComponent>(groundEntity))
    {
        groundTransform->localPosition = { 0.f, -1.25f, 0.f };
        groundTransform->localScale    = { 10.f, 0.3f, 10.f };
        groundTransform->SetEulerDeg(0.f, 0.f, 0.f);
    }

    // Directional light von rechts oben — cubeC und cubeB beschatten cubeA und cubeB.
    const EntityID lightEntity = world.CreateEntity();
    {
        auto& tr = world.Add<TransformComponent>(lightEntity);
        tr.localPosition = { 5.f, 5.f, 0.f };
        tr.localScale    = { 1.f, 1.f, 1.f };
        tr.SetEulerDeg(-35.f, 90.f, 0.f);
        world.Add<WorldTransformComponent>(lightEntity);

        LightComponent lc{};
        lc.type      = LightType::Directional;
        lc.color     = { 1.0f, 0.95f, 0.85f };
        lc.intensity = 3.0f;
        lc.castShadows = true;
        lc.shadowSettings.enabled    = true;
        lc.shadowSettings.type       = ShadowType::PCF;
        lc.shadowSettings.filter     = ShadowFilter::PCF3x3;
        lc.shadowSettings.resolution = 2048u;
        lc.shadowSettings.bias       = 0.00015f;
        lc.shadowSettings.normalBias = 0.0006f;
        lc.shadowSettings.strength   = 0.85f;
        world.Add<LightComponent>(lightEntity, lc);
    }

    const EntityID cameraEntity = world.CreateEntity();
    auto& cameraTransform = world.Add<TransformComponent>(cameraEntity);
    cameraTransform.localPosition = { 0.f, 0.f, 6.f };
    world.Add<WorldTransformComponent>(cameraEntity);
    world.Add<CameraComponent>(cameraEntity, CameraComponent{
        .projection   = ProjectionType::Perspective,
        .fovYDeg      = 60.f,
        .nearPlane    = 0.1f,
        .farPlane     = 100.f,
        .isMainCamera = true
    });

    TransformSystem transformSystem;
    BoundsSystem    boundsSystem;

    transformSystem.Update(world);
    mesh_renderer::UpdateLocalBoundsFromMeshes(world, registry);
    boundsSystem.Update(world);

    platform::StdTiming timing;
    const double startTime = timing.GetRawTimestampSeconds();

    while (!loop.ShouldExit())
    {
        if (auto* input = loop.GetInput();
            input && input->IsKeyPressed(platform::Key::Escape))
        {
            loop.GetWindow()->RequestClose();
        }

        const float t = static_cast<float>(timing.GetRawTimestampSeconds() - startTime);

        if (auto* transformA = world.Get<TransformComponent>(cubeA))
            transformA->SetEulerDeg(t * 30.0f, t * 45.0f, 0.0f);

        if (auto* transformB = world.Get<TransformComponent>(cubeB))
            transformB->SetEulerDeg(t * 60.0f, t * 20.0f, t * 35.0f);

        if (auto* transformC = world.Get<TransformComponent>(cubeC))
            transformC->SetEulerDeg(t * 15.0f, t * 90.0f, t * 10.0f);

        transformSystem.Update(world);
        mesh_renderer::UpdateLocalBoundsFromMeshes(world, registry);
        boundsSystem.Update(world);

        const auto* swapchain = loop.GetRenderSystem().GetSwapchain();
        const uint32_t viewportWidth  =
            (swapchain && swapchain->GetWidth()  > 0u) ? swapchain->GetWidth()  : 1280u;
        const uint32_t viewportHeight =
            (swapchain && swapchain->GetHeight() > 0u) ? swapchain->GetHeight() : 720u;

        renderer::RenderView view{};
        if (!engine::addons::camera::BuildPrimaryRenderView(world, viewportWidth, viewportHeight, view))
        {
            Debug::LogError("dx11_render_loop: failed to build render view from ECS camera");
            break;
        }
        view.ambientColor     = { 0.05f, 0.05f, 0.07f };
        view.ambientIntensity = 0.55f;

        if (!loop.Tick(world, materials, view, timing))
            break;
    }

    loop.Shutdown();
    winPlatform.Shutdown();
    return 0;
#else
    return 0;
#endif
}
