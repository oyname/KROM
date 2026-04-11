#include "ForwardFeature.hpp"
// =============================================================================
// KROM Engine - examples/dx11_three_cubes.cpp
// =============================================================================

#include "DX11Device.hpp"
#include "assets/AssetPipeline.hpp"
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
#include "scene/TransformSystem.hpp"
#include <filesystem>
#include <memory>
#include <utility>

using namespace engine;

int main()
{
#ifdef _WIN32
    Debug::MinLevel = LogLevel::Info;
    RegisterAllComponents();

    if (!renderer::DeviceFactory::IsRegistered(renderer::DeviceFactory::BackendType::DirectX11))
    {
        Debug::LogError("dx11_three_cubes: DX11 backend registration failed");
        return -10;
    }

    const auto adapters = renderer::DeviceFactory::EnumerateAdapters(
        renderer::DeviceFactory::BackendType::DirectX11);
    if (adapters.empty())
    {
        Debug::LogError("dx11_three_cubes: no DX11 adapters found");
        return -11;
    }

    const uint32_t adapterIndex = renderer::DeviceFactory::FindBestAdapter(adapters);
    for (const auto& a : adapters)
    {
        Debug::Log("dx11_three_cubes: adapter[%u] '%s' VRAM=%zu MB discrete=%d FL=%d",
            a.index,
            a.name.c_str(),
            a.dedicatedVRAM / (1024ull * 1024ull),
            static_cast<int>(a.isDiscrete),
            a.featureLevel);
    }
    Debug::Log("dx11_three_cubes: selected adapter index %u", adapterIndex);

    platform::win32::Win32Platform winPlatform;
    if (!winPlatform.Initialize())
    {
        Debug::LogError("dx11_three_cubes: Win32Platform Initialize failed");
        return -1;
    }

    events::EventBus bus;
    renderer::PlatformRenderLoop loop;
    if (!loop.GetRenderSystem().RegisterFeature(engine::renderer::addons::forward::CreateForwardFeature()))
    {
        Debug::LogError("dx11_three_cubes: forward feature registration failed");
        return -12;
    }

    platform::WindowDesc wDesc{};
    wDesc.title = "KROM - Three Rotating Cubes (DX11)";
    wDesc.width = 1280;
    wDesc.height = 720;
    wDesc.resizable = true;

    renderer::IDevice::DeviceDesc dDesc{};
    dDesc.enableDebugLayer = true;
    dDesc.appName = "KROM DX11";
    dDesc.adapterIndex = adapterIndex;

    if (!loop.Initialize(renderer::DeviceFactory::BackendType::DirectX11,
        winPlatform,
        wDesc,
        &bus,
        dDesc))
    {
        Debug::LogError("dx11_three_cubes: loop.Initialize failed");
        winPlatform.Shutdown();
        return -2;
    }

    assets::AssetRegistry registry;
    loop.GetRenderSystem().SetAssetRegistry(&registry);

    assets::AssetPipeline pipeline(registry, loop.GetRenderSystem().GetDevice());

    const std::filesystem::path assetRoot =
        std::filesystem::path(__FILE__).parent_path().parent_path() / "assets";
    Debug::Log("dx11_three_cubes: asset root = %s", assetRoot.string().c_str());
    pipeline.SetAssetRoot(assetRoot.string());

    const ShaderHandle vsHandle = pipeline.LoadShader("quad_unlit.hlslvs",
        assets::ShaderStage::Vertex);
    const ShaderHandle psHandle = pipeline.LoadShader("quad_unlit.hlslps",
        assets::ShaderStage::Fragment);
    const ShaderHandle tonemapVsHandle = pipeline.LoadShader("fullscreen.hlslvs",
        assets::ShaderStage::Vertex);
    const ShaderHandle tonemapPsHandle = pipeline.LoadShader("passthrough.hlslps",
        assets::ShaderStage::Fragment);

    if (!vsHandle.IsValid() || !psHandle.IsValid() ||
        !tonemapVsHandle.IsValid() || !tonemapPsHandle.IsValid())
    {
        Debug::LogError("dx11_three_cubes: shader load failed");
        loop.Shutdown();
        winPlatform.Shutdown();
        return -3;
    }

    {
        auto* vs = registry.shaders.Get(vsHandle);
        if (!vs || vs->sourceCode.empty())
        {
            Debug::LogError("dx11_three_cubes: quad_unlit.hlslvs source leer");
            loop.Shutdown();
            winPlatform.Shutdown();
            return -4;
        }

        Debug::Log("dx11_three_cubes: shaders gefunden (%zu bytes VS)", vs->sourceCode.size());
    }

    const TextureHandle texHandle = pipeline.LoadTexture("krom.bmp");
    pipeline.UploadPendingGpuAssets();
    const TextureHandle gpuTex = pipeline.GetGpuTexture(texHandle);

    if (!gpuTex.IsValid())
    {
        Debug::LogError("dx11_three_cubes: texture upload failed");
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
    vLayout.attributes.push_back({
        renderer::VertexSemantic::Position,
        renderer::Format::RGB32_FLOAT,
        0u,
        0u
        });
    vLayout.attributes.push_back({
        renderer::VertexSemantic::Normal,
        renderer::Format::RGB32_FLOAT,
        0u,
        12u
        });
    vLayout.attributes.push_back({
        renderer::VertexSemantic::TexCoord0,
        renderer::Format::RG32_FLOAT,
        0u,
        24u
        });
    vLayout.bindings.push_back({ 0u, 32u });

    renderer::MaterialParam albedoParam{};
    albedoParam.name = "albedo";
    albedoParam.type = renderer::MaterialParam::Type::Texture;
    albedoParam.texture = gpuTex;

    renderer::MaterialParam samplerParam{};
    samplerParam.name = "sampler_albedo";
    samplerParam.type = renderer::MaterialParam::Type::Sampler;
    samplerParam.samplerIdx = 0u;

    renderer::MaterialDesc matDesc{};
    matDesc.name = "CubeUnlit";
    matDesc.passTag = renderer::RenderPassTag::Opaque;
    matDesc.vertexShader = vsHandle;
    matDesc.fragmentShader = psHandle;
    matDesc.vertexLayout = vLayout;
    matDesc.colorFormat = renderer::Format::RGBA16_FLOAT;
    matDesc.depthFormat = renderer::Format::D24_UNORM_S8_UINT;
    matDesc.params.push_back(albedoParam);
    matDesc.params.push_back(samplerParam);
    const MaterialHandle material = materials.RegisterMaterial(std::move(matDesc));

    renderer::DepthStencilState noDepth{};
    noDepth.depthEnable = false;
    noDepth.depthWrite = false;

    renderer::MaterialParam tonemapSamplerParam{};
    tonemapSamplerParam.name = "linearclamp";
    tonemapSamplerParam.type = renderer::MaterialParam::Type::Sampler;
    tonemapSamplerParam.samplerIdx = 0u;

    renderer::MaterialDesc tonemapDesc{};
    tonemapDesc.name = "PassthroughDirectX11";
    tonemapDesc.passTag = renderer::RenderPassTag::Opaque;
    tonemapDesc.vertexShader = tonemapVsHandle;
    tonemapDesc.fragmentShader = tonemapPsHandle;
    tonemapDesc.depthStencil = noDepth;
    tonemapDesc.colorFormat = renderer::Format::RGBA8_UNORM_SRGB;
    tonemapDesc.depthFormat = renderer::Format::D24_UNORM_S8_UINT;
    tonemapDesc.params.push_back(tonemapSamplerParam);
    const MaterialHandle tonemapMaterial = materials.RegisterMaterial(std::move(tonemapDesc));
    loop.GetRenderSystem().SetDefaultTonemapMaterial(tonemapMaterial, materials);

    ecs::World world;

    const auto createCubeEntity = [&](const math::Vec3& pos)
        {
            const auto e = world.CreateEntity();

            world.Add<TransformComponent>(e);
            world.Add<WorldTransformComponent>(e);
            world.Add<MeshComponent>(e, meshHandle);
            world.Add<MaterialComponent>(e, material);
            world.Add<BoundsComponent>(e, BoundsComponent{
                .centerLocal = { 0.f, 0.f, 0.f },
                .extentsLocal = { 0.5f, 0.5f, 0.5f },
                .centerWorld = pos,
                .extentsWorld = { 0.5f, 0.5f, 0.5f },
                .boundingSphere = 0.8660254f,
                .localDirty = true
                });

            auto* tr = world.Get<TransformComponent>(e);
            if (tr)
            {
                tr->localPosition = pos;
                tr->localScale = { 1.f, 1.f, 1.f };
                tr->SetEulerDeg(0.f, 0.f, 0.f);
            }

            return e;
        };

    const auto cubeA = createCubeEntity({ -1.75f, 0.0f, 0.0f });
    const auto cubeB = createCubeEntity({ 0.0f, 0.0f, 0.0f });
    const auto cubeC = createCubeEntity({ 1.75f, 0.0f, 0.0f });

    TransformSystem transformSystem;
    BoundsSystem boundsSystem;

    transformSystem.Update(world);
    boundsSystem.Update(world, registry);

    renderer::RenderView view{};
    view.view = math::Mat4::LookAtRH(
        { 0.f, 0.f, 6.f },
        { 0.f, 0.f, 0.f },
        math::Vec3::Up());

    const auto updateProjection = [&view, &loop]()
        {
            const auto* swapchain = loop.GetRenderSystem().GetSwapchain();
            const uint32_t width =
                (swapchain && swapchain->GetWidth() > 0u) ? swapchain->GetWidth() : 1280u;
            const uint32_t height =
                (swapchain && swapchain->GetHeight() > 0u) ? swapchain->GetHeight() : 720u;

            const float aspect = (height > 0u)
                ? static_cast<float>(width) / static_cast<float>(height)
                : (16.0f / 9.0f);

            view.projection = math::Mat4::PerspectiveFovRH(
                60.f * math::DEG_TO_RAD,
                aspect,
                0.1f,
                100.f);
        };

    updateProjection();
    view.cameraPosition = { 0.f, 0.f, 6.f };
    view.cameraForward = { 0.f, 0.f, -1.f };

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
        boundsSystem.Update(world, registry);
        updateProjection();

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
