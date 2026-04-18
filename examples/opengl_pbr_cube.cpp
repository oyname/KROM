#include "ForwardFeature.hpp"
// =============================================================================
// KROM Engine - examples/vulkan_pbr_cube.cpp
// Beispiel: Rotierender PBR-Würfel mit Directional-Light (Vulkan).
// =============================================================================

#include "assets/AssetPipeline.hpp"
#include "addons/camera/CameraComponents.hpp"
#include "addons/camera/CameraViewBuilder.hpp"
#include "addons/mesh_renderer/MeshAssetSceneBindings.hpp"
#include "assets/AssetRegistry.hpp"
#include "assets/MeshTangents.hpp"
#include "core/Debug.hpp"
#include "core/Math.hpp"
#include "addons/lighting/LightingComponents.hpp"
#include "addons/mesh_renderer/MeshRendererComponents.hpp"
#include "ecs/Components.hpp"
#include "ecs/World.hpp"
#include "events/EventBus.hpp"
#include "platform/StdTiming.hpp"
#include "platform/Win32Platform.hpp"
#include "renderer/IDevice.hpp"
#include "renderer/MaterialSystem.hpp"
#include "PbrMaterial.hpp"
#include "renderer/PlatformRenderLoop.hpp"
#include "renderer/Environment.hpp"
#include "renderer/EnvironmentSystem.hpp"
#include "renderer/RendererTypes.hpp"
#include "scene/TransformSystem.hpp"
#include <filesystem>
#include <memory>
#include <functional>
#include <cmath>

#if defined(_WIN32)
#   include "platform/Win32Platform.hpp"
#else
#   include "platform/GLFWPlatform.hpp"
#endif

using namespace engine;


namespace {

TextureHandle CreateProceduralTexture(assets::AssetRegistry& registry,
                                      const char* debugName,
                                      uint32_t width,
                                      uint32_t height,
                                      bool srgb,
                                      const std::function<void(uint32_t,uint32_t,uint8_t*)>& writer)
{
    auto tex = std::make_unique<assets::TextureAsset>();
    tex->debugName = debugName;
    tex->state = assets::AssetState::Loaded;
    tex->gpuStatus.dirty = true;
    tex->gpuStatus.uploaded = false;
    tex->width = width;
    tex->height = height;
    tex->depth = 1u;
    tex->mipLevels = 1u;
    tex->arraySize = 1u;
    tex->format = assets::TextureFormat::RGBA8_UNORM;
    tex->sRGB = srgb;
    tex->pixelData.resize(static_cast<size_t>(width) * static_cast<size_t>(height) * 4u);

    for (uint32_t y = 0; y < height; ++y)
        for (uint32_t x = 0; x < width; ++x)
            writer(x, y, &tex->pixelData[(static_cast<size_t>(y) * width + x) * 4u]);

    return registry.textures.Add(std::move(tex));
}

TextureHandle CreateCheckerTexture(assets::AssetRegistry& registry)
{
    return CreateProceduralTexture(registry, "checker_basecolor", 64u, 64u, true,
        [](uint32_t x, uint32_t y, uint8_t* px)
        {
            const bool c = (((x / 8u) + (y / 8u)) & 1u) != 0u;
            if (c) { px[0] = 230u; px[1] = 230u; px[2] = 230u; }
            else   { px[0] =  70u; px[1] = 110u; px[2] = 200u; }
            px[3] = 255u;
        });
}

TextureHandle CreateNormalTexture(assets::AssetRegistry& registry)
{
    return CreateProceduralTexture(registry, "detail_normal", 64u, 64u, false,
        [](uint32_t x, uint32_t y, uint8_t* px)
        {
            const float fx = (static_cast<float>(x) + 0.5f) / 64.0f;
            const float fy = (static_cast<float>(y) + 0.5f) / 64.0f;
            const float sx = std::sin(fx * 6.2831853f * 4.0f) * 0.35f;
            const float sy = std::cos(fy * 6.2831853f * 4.0f) * 0.35f;
            const float nx = sx;
            const float ny = sy;
            float nz = 1.0f - nx * nx - ny * ny;
            nz = nz > 0.0f ? std::sqrt(nz) : 0.0f;
            px[0] = static_cast<uint8_t>((nx * 0.5f + 0.5f) * 255.0f);
            px[1] = static_cast<uint8_t>((ny * 0.5f + 0.5f) * 255.0f);
            px[2] = static_cast<uint8_t>((nz * 0.5f + 0.5f) * 255.0f);
            px[3] = 255u;
        });
}

}
TextureHandle CreateOrmTexture(assets::AssetRegistry& registry, uint8_t occlusion, uint8_t roughness, uint8_t metallic)
{
    return CreateProceduralTexture(registry, "detail_orm", 1u, 1u, false,
        [=](uint32_t, uint32_t, uint8_t* px)
        {
            px[0] = occlusion;
            px[1] = roughness;
            px[2] = metallic;
            px[3] = 255u;
        });
}



int main()
{
    Debug::ResetMinLevelForBuild();
    RegisterCoreComponents();
    RegisterMeshRendererComponents();
    RegisterCameraComponents();
    RegisterLightingComponents();

    if (!renderer::DeviceFactory::IsRegistered(renderer::DeviceFactory::BackendType::OpenGL))
        return -10;

    const auto adapters = renderer::DeviceFactory::EnumerateAdapters(
        renderer::DeviceFactory::BackendType::OpenGL);
    if (adapters.empty())
        return -11;

    const uint32_t adapterIndex = renderer::DeviceFactory::FindBestAdapter(adapters);

#if defined(_WIN32)
    platform::win32::Win32Platform runtimePlatform;
#else
#   if !KROM_HAS_GLFW
    return -12;
#   endif
    platform::GLFWPlatform runtimePlatform;
#endif

    if (!runtimePlatform.Initialize())
        return -1;

    events::EventBus bus;
    renderer::PlatformRenderLoop loop;
    if (!loop.GetRenderSystem().RegisterFeature(
        engine::renderer::addons::forward::CreateForwardFeature()))
    {
        runtimePlatform.Shutdown();
        return 1;
    }

    platform::WindowDesc wDesc{};
    wDesc.title = "KROM - PBR Rotating Cube (OpenGL)";
    wDesc.width = 1280;
    wDesc.height = 720;
    wDesc.resizable = true;
    wDesc.openglContext = true;
    wDesc.openglMajor = 4;
    wDesc.openglMinor = 1;
    wDesc.openglDebugContext = false;

    renderer::IDevice::DeviceDesc dDesc{};
    dDesc.enableDebugLayer = true;
    dDesc.adapterIndex = adapterIndex;
    dDesc.appName = "KROM OpenGL";

    if (!loop.Initialize(renderer::DeviceFactory::BackendType::OpenGL,
        runtimePlatform, wDesc, &bus, dDesc))
    {
        runtimePlatform.Shutdown();
        return -2;
    }

    assets::AssetRegistry registry;
    loop.GetRenderSystem().SetAssetRegistry(&registry);
    assets::AssetPipeline pipeline(registry, loop.GetRenderSystem().GetDevice());
    mesh_renderer::ConfigureAssetPipeline(pipeline);

    const std::filesystem::path assetRoot =
        std::filesystem::path(__FILE__).parent_path().parent_path() / "assets";
    pipeline.SetAssetRoot(assetRoot.string());

    const auto pbrShaderAssets = renderer::pbr::PbrMaterial::DefaultShaderAssetSet(
        renderer::pbr::PbrShaderBackend::OpenGL);
    const ShaderHandle vsHandle = pipeline.LoadShader(pbrShaderAssets.vertexShader, assets::ShaderStage::Vertex);
    const ShaderHandle psHandle = pipeline.LoadShader(pbrShaderAssets.fragmentShader, assets::ShaderStage::Fragment);
    const ShaderHandle shadowHandle = pipeline.LoadShader(pbrShaderAssets.shadowShader, assets::ShaderStage::Vertex);
    const ShaderHandle tonemapVsHandle = pipeline.LoadShader("fullscreen.opengl.vs.glsl", assets::ShaderStage::Vertex);
    const ShaderHandle tonemapPsHandle = pipeline.LoadShader("passthrough.opengl.fs.glsl", assets::ShaderStage::Fragment);

    if (!vsHandle.IsValid() || !psHandle.IsValid() || !shadowHandle.IsValid() ||
        !tonemapVsHandle.IsValid() || !tonemapPsHandle.IsValid())
    {
        loop.Shutdown();
        runtimePlatform.Shutdown();
        return -3;
    }

    const TextureHandle baseColorTexHandle = CreateCheckerTexture(registry);
    const TextureHandle normalTexHandle = CreateNormalTexture(registry);
    const TextureHandle ormTexHandle = CreateOrmTexture(registry, 255u, 255u, 0u);
    pipeline.UploadPendingGpuAssets();
    const TextureHandle gpuTex = pipeline.GetGpuTexture(baseColorTexHandle);
    const TextureHandle gpuNormalTex = pipeline.GetGpuTexture(normalTexHandle);
    const TextureHandle gpuOrmTex = pipeline.GetGpuTexture(ormTexHandle);

    if (!gpuTex.IsValid() || !gpuNormalTex.IsValid() || !gpuOrmTex.IsValid())
    {
        Debug::LogError("pbr_cube: procedural texture upload failed");
        loop.Shutdown();
        runtimePlatform.Shutdown();
        return -4;
    }
    // -------------------------------------------------------------------------
    // HDR-Environment laden und als aktives IBL setzen.
    // -------------------------------------------------------------------------
    const TextureHandle envHandle = pipeline.LoadTexture("irradeiance.hdr");
    pipeline.UploadPendingGpuAssets();

    if (envHandle.IsValid())
    {
        renderer::EnvironmentDesc env{};
        env.sourceTexture = envHandle;
        env.intensity = 1.0f;
        env.enableIBL = true;
        const auto activeEnvironment = loop.GetRenderSystem().CreateEnvironment(env);
        loop.GetRenderSystem().SetActiveEnvironment(activeEnvironment);
        Debug::Log("Environment-IBL aktiv");
    }
    else
    {
        engine::renderer::EnvironmentDesc desc;
        desc.mode = engine::renderer::EnvironmentMode::ProceduralSky;
        desc.skyDesc.sunDirection = { 0.5f, 0.6f, 0.4f };
        desc.skyDesc.sunIntensity = 20.0f;
        desc.intensity = 1.0f;
        const auto h = loop.GetRenderSystem().CreateEnvironment(desc);
        loop.GetRenderSystem().SetActiveEnvironment(h);
        Debug::Log("ProceduralSky-IBL aktiv");
    }

    auto meshAsset = std::make_unique<assets::MeshAsset>();
    assets::SubMeshData cube;

    const auto addFace = [&](math::Vec3 p0, math::Vec3 p1, math::Vec3 p2, math::Vec3 p3, math::Vec3 n)
    {
        const uint32_t base = static_cast<uint32_t>(cube.positions.size() / 3u);

        for (const auto& p : { p0, p1, p2, p3 })
        {
            cube.positions.push_back(p.x);
            cube.positions.push_back(p.y);
            cube.positions.push_back(p.z);
            cube.normals.push_back(n.x);
            cube.normals.push_back(n.y);
            cube.normals.push_back(n.z);
        }

        cube.uvs.insert(cube.uvs.end(), {
            0.f, 0.f,  1.f, 0.f,  1.f, 1.f,  0.f, 1.f
        });

        cube.indices.insert(cube.indices.end(), {
            base + 0, base + 1, base + 2,
            base + 0, base + 2, base + 3
        });
    };

    const float H = 0.5f;
    addFace({-H,-H, H}, { H,-H, H}, { H, H, H}, {-H, H, H}, { 0, 0, 1});
    addFace({ H,-H,-H}, {-H,-H,-H}, {-H, H,-H}, { H, H,-H}, { 0, 0,-1});
    addFace({-H,-H,-H}, {-H,-H, H}, {-H, H, H}, {-H, H,-H}, {-1, 0, 0});
    addFace({ H,-H, H}, { H,-H,-H}, { H, H,-H}, { H, H, H}, { 1, 0, 0});
    addFace({-H, H, H}, { H, H, H}, { H, H,-H}, {-H, H,-H}, { 0, 1, 0});
    addFace({-H,-H,-H}, { H,-H,-H}, { H,-H, H}, {-H,-H, H}, { 0,-1, 0});

    assets::EnsureTangents(cube);
    meshAsset->submeshes.push_back(std::move(cube));
    const MeshHandle meshHandle = registry.meshes.Add(std::move(meshAsset));

    renderer::VertexLayout vLayout;
    vLayout.attributes.push_back({ renderer::VertexSemantic::Position, renderer::Format::RGB32_FLOAT, 0u, 0u });
    vLayout.attributes.push_back({ renderer::VertexSemantic::Normal, renderer::Format::RGB32_FLOAT, 0u, 12u });
    vLayout.attributes.push_back({ renderer::VertexSemantic::Tangent, renderer::Format::RGBA32_FLOAT, 0u, 24u });
    vLayout.attributes.push_back({ renderer::VertexSemantic::TexCoord0, renderer::Format::RG32_FLOAT, 0u, 40u });
    vLayout.bindings.push_back({ 0u, 48u });

    renderer::MaterialSystem materials;

    renderer::pbr::PbrMaterialCreateInfo pbrInfo{};
    pbrInfo.name = "CubePBR";
    pbrInfo.vertexShader = vsHandle;
    pbrInfo.fragmentShader = psHandle;
    pbrInfo.shadowShader = shadowHandle;
    pbrInfo.vertexLayout = vLayout;
    pbrInfo.colorFormat = renderer::Format::RGBA16_FLOAT;
    pbrInfo.depthFormat = renderer::Format::D24_UNORM_S8_UINT;
    pbrInfo.roughnessFactor = 1.0f;
    pbrInfo.metallicFactor = 1.0f;
    renderer::pbr::PbrMaterial::ApplyDefaultShaderAssetSet(pbrInfo, renderer::pbr::PbrShaderBackend::OpenGL);

    renderer::pbr::PbrMaterial pbrMaterial = renderer::pbr::PbrMaterial::Create(materials, pbrInfo);
    if (!pbrMaterial.IsValid() ||
        !pbrMaterial.SetAlbedo(gpuTex) ||
        !pbrMaterial.SetNormal(gpuNormalTex) ||
        !pbrMaterial.SetORM(gpuOrmTex))
    {
        Debug::LogError("opengl_pbr_cube: PBR addon material setup failed");
        loop.Shutdown();
        runtimePlatform.Shutdown();
        return -5;
    }

    renderer::DepthStencilState noDepth{};
    noDepth.depthEnable = false;
    noDepth.depthWrite = false;

    renderer::MaterialParam tonemapSamplerParam{};
    tonemapSamplerParam.name = "linearclamp";
    tonemapSamplerParam.type = renderer::MaterialParam::Type::Sampler;
    tonemapSamplerParam.samplerIdx = 0u;

    renderer::MaterialDesc tonemapDesc{};
    tonemapDesc.name = "PassthroughGL";
    tonemapDesc.renderPass = renderer::StandardRenderPasses::Opaque();
    tonemapDesc.vertexShader = tonemapVsHandle;
    tonemapDesc.fragmentShader = tonemapPsHandle;
    tonemapDesc.depthStencil = noDepth;
    tonemapDesc.rasterizer.cullMode = renderer::CullMode::None;
    tonemapDesc.colorFormat = renderer::Format::RGBA8_UNORM_SRGB;
    tonemapDesc.depthFormat = renderer::Format::D24_UNORM_S8_UINT;
    tonemapDesc.params.push_back(tonemapSamplerParam);

    const MaterialHandle tonemapMaterial = materials.RegisterMaterial(std::move(tonemapDesc));
    loop.GetRenderSystem().SetDefaultTonemapMaterial(tonemapMaterial, materials);

    ecs::World world;

    const auto cubeEntity = world.CreateEntity();
    world.Add<TransformComponent>(cubeEntity);
    world.Add<WorldTransformComponent>(cubeEntity);
    world.Add<MeshComponent>(cubeEntity, meshHandle);
    world.Add<MaterialComponent>(cubeEntity, pbrMaterial.Handle());
    world.Add<BoundsComponent>(cubeEntity, BoundsComponent{
        .centerWorld = { 0.f, 0.f, 0.f },
        .extentsWorld = { 0.5f, 0.5f, 0.5f },
        .boundingSphere = 0.87f
    });

    const auto lightEntity = world.CreateEntity();
    {
        auto& tc = world.Add<TransformComponent>(lightEntity);
        tc.SetEulerDeg(-45.f, 30.f, 0.f);
    }
    world.Add<WorldTransformComponent>(lightEntity);
    world.Add<LightComponent>(lightEntity, LightComponent{
        .type = LightType::Directional,
        .color = { 1.f, 0.95f, 0.88f },
        .intensity = 3.f,
        .castShadows = false,
    });

    const EntityID cameraEntity = world.CreateEntity();
    auto& cameraTransform = world.Add<TransformComponent>(cameraEntity);
    cameraTransform.localPosition = { 0.f, 0.f, 5.f };
    world.Add<WorldTransformComponent>(cameraEntity);
    world.Add<CameraComponent>(cameraEntity, CameraComponent{
        .projection = ProjectionType::Perspective,
        .fovYDeg = 60.f,
        .nearPlane = 0.1f,
        .farPlane = 100.f,
        .isMainCamera = true
    });

    const engine::addons::camera::CameraBuildOptions cameraOptions{
        .ambientColor = { 0.05f, 0.05f, 0.08f },
        .ambientIntensity = 1.f
    };

    engine::TransformSystem transformSystem;
    platform::StdTiming timing;

    float angleY = 0.f;
    float angleX = 15.f;

    while (!loop.ShouldExit())
    {
        if (auto* input = loop.GetInput(); input && input->IsKeyPressed(platform::Key::Escape))
        {
            loop.GetWindow()->RequestClose();
        }

        const float dt = timing.GetDeltaSecondsF();
        angleY += 45.f * dt;
        angleX += 20.f * dt;

        if (auto* tc = world.Get<TransformComponent>(cubeEntity))
            tc->SetEulerDeg(angleX, angleY, 0.f);

        transformSystem.Update(world);

        const auto* swapchain = loop.GetRenderSystem().GetSwapchain();
        const uint32_t viewportWidth =
            (swapchain && swapchain->GetWidth() > 0u) ? swapchain->GetWidth() : 1280u;
        const uint32_t viewportHeight =
            (swapchain && swapchain->GetHeight() > 0u) ? swapchain->GetHeight() : 720u;

        renderer::RenderView view{};
        if (!engine::addons::camera::BuildPrimaryRenderView(
                world, viewportWidth, viewportHeight, view, cameraOptions))
        {
            Debug::LogError("opengl_pbr_cube: failed to build render view from ECS camera");
            break;
        }

        if (!loop.Tick(world, materials, view, timing))
            break;
    }

    loop.Shutdown();
    runtimePlatform.Shutdown();
    return 0;
}
