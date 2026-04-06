#include "ForwardFeature.hpp"
// =============================================================================
// KROM Engine - examples/dx11_render_loop.cpp
// Beispiel: Texturiertes, unbelichtetes Quad mit DX11-Backend.
//
// Was dieses Beispiel zeigt:
//   - AssetPipeline: Shader (.hlslvs/.hlslps) und Textur (.bmp) laden
//   - AssetRegistry: Quad-Mesh programmatisch registrieren
//   - MaterialSystem: Material mit Textur-Binding aufbauen
//   - VertexLayout: passend zum interleaved VB (Pos+Normal+UV, 32 Byte Stride)
//   - PlatformRenderLoop: vollständiger Frame-Loop
// =============================================================================

#include "DX11Device.hpp"
#include "assets/AssetPipeline.hpp"
#include "assets/AssetRegistry.hpp"
#include "core/Debug.hpp"
#include "core/Math.hpp"
#include "ecs/Components.hpp"
#include "ecs/World.hpp"
#include "events/EventBus.hpp"
#include "platform/NullPlatform.hpp"   // StdTiming
#include "platform/Win32Platform.hpp"
#include "renderer/IDevice.hpp"
#include "renderer/MaterialSystem.hpp"
#include "renderer/PlatformRenderLoop.hpp"
#include "renderer/ShaderBindingModel.hpp"
#include "renderer/RendererTypes.hpp"
#include <filesystem>

using namespace engine;

int main()
{
#ifdef _WIN32
    Debug::MinLevel = LogLevel::Info;
    RegisterAllComponents();

    // -------------------------------------------------------------------------
    // Backend registrieren + Adapter wählen
    // -------------------------------------------------------------------------
    if (!renderer::DeviceFactory::IsRegistered(renderer::DeviceFactory::BackendType::DirectX11))
    {
        Debug::LogError("dx11_render_loop: DX11 backend registration failed");
        return -10;
    }

    const auto adapters = renderer::DeviceFactory::EnumerateAdapters(
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
            a.index, a.name.c_str(),
            a.dedicatedVRAM / (1024ull * 1024ull),
            static_cast<int>(a.isDiscrete), a.featureLevel);
    }
    Debug::Log("dx11_render_loop: selected adapter index %u", adapterIndex);

    // -------------------------------------------------------------------------
    // Platform
    // -------------------------------------------------------------------------
    platform::win32::Win32Platform winPlatform;
    if (!winPlatform.Initialize())
    {
        Debug::LogError("dx11_render_loop: Win32Platform Initialize failed");
        return -1;
    }

    // -------------------------------------------------------------------------
    // Render Loop
    // -------------------------------------------------------------------------
    events::EventBus bus;
    renderer::PlatformRenderLoop loop;
    if (!loop.GetRenderSystem().RegisterFeature(engine::renderer::addons::forward::CreateForwardFeature()))
    {
        Debug::LogError("forward feature registration failed");
        return 1;
    }

    platform::WindowDesc wDesc{};
    wDesc.title     = "KROM - Textured Quad (DX11)";
    wDesc.width     = 1280;
    wDesc.height    = 720;
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

    // -------------------------------------------------------------------------
    // Asset Pipeline
    // -------------------------------------------------------------------------
    assets::AssetRegistry registry;
    loop.GetRenderSystem().SetAssetRegistry(&registry);

    assets::AssetPipeline pipeline(registry,
                                   loop.GetRenderSystem().GetDevice());

    const std::filesystem::path assetRoot =
        std::filesystem::path(__FILE__).parent_path().parent_path() / "assets";
    Debug::Log("dx11_render_loop: asset root = %s", assetRoot.string().c_str());
    pipeline.SetAssetRoot(assetRoot.string());

    // Shader laden - .hlslvs/.hlslps → HLSL, Stage wird aus Extension erkannt
    const ShaderHandle vsHandle = pipeline.LoadShader("quad_unlit.hlslvs",
                                                       assets::ShaderStage::Vertex);
    const ShaderHandle psHandle = pipeline.LoadShader("quad_unlit.hlslps",
                                                       assets::ShaderStage::Fragment);

    // Passthrough-Shader für TonemapPass (Fullscreen-Dreieck → HDR in tonemapped RT)
    const ShaderHandle tonemapVsHandle = pipeline.LoadShader("fullscreen.hlslvs",
                                                              assets::ShaderStage::Vertex);
    const ShaderHandle tonemapPsHandle = pipeline.LoadShader("passthrough.hlslps",
                                                              assets::ShaderStage::Fragment);

    if (!vsHandle.IsValid() || !psHandle.IsValid() ||
        !tonemapVsHandle.IsValid() || !tonemapPsHandle.IsValid())
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
            Debug::LogError("dx11_render_loop: quad_unlit.hlslvs source leer - Datei nicht gefunden in: %s",
                            assetRoot.string().c_str());
            loop.Shutdown();
            winPlatform.Shutdown();
            return -3;
        }
        Debug::Log("dx11_render_loop: shaders gefunden (%zu bytes VS)", vs->sourceCode.size());
    }

    // Textur laden (stb_image: PNG/BMP/JPEG/TGA werden unterstützt)
    const TextureHandle texHandle = pipeline.LoadTexture("krom.bmp");
    pipeline.UploadPendingGpuAssets();
    const TextureHandle gpuTex = pipeline.GetGpuTexture(texHandle);

    if (!gpuTex.IsValid())
    {
        Debug::LogError("dx11_render_loop: texture upload failed");
        loop.Shutdown();
        winPlatform.Shutdown();
        return -4;
    }

    // -------------------------------------------------------------------------
    // Quad-Mesh
    // GpuResourceRuntime interleaved layout: Position(xyz) + Normal(xyz) + UV(uv)
    // kFloatsPerVertex = 8, kStride = 32 bytes
    // -------------------------------------------------------------------------
    auto meshAsset = std::make_unique<assets::MeshAsset>();
    assets::SubMeshData quad;

    quad.positions = {
        -0.5f, -0.5f, 0.0f,   // 0 unten-links
         0.5f, -0.5f, 0.0f,   // 1 unten-rechts
         0.5f,  0.5f, 0.0f,   // 2 oben-rechts
        -0.5f,  0.5f, 0.0f,   // 3 oben-links
    };

    // Normals: GpuResourceRuntime schreibt immer alle 8 floats pro Vertex
    quad.normals = {
        0.0f, 0.0f, 1.0f,
        0.0f, 0.0f, 1.0f,
        0.0f, 0.0f, 1.0f,
        0.0f, 0.0f, 1.0f,
    };

    // UV: Y=0 oben (DX-Konvention - kein stb_image Flip)
    quad.uvs = {
        0.0f, 1.0f,   // unten-links
        1.0f, 1.0f,   // unten-rechts
        1.0f, 0.0f,   // oben-rechts
        0.0f, 0.0f,   // oben-links
    };

    quad.indices = { 0, 1, 2,  2, 3, 0 };
    meshAsset->submeshes.push_back(std::move(quad));

    const MeshHandle meshHandle = registry.meshes.Add(std::move(meshAsset));

    // -------------------------------------------------------------------------
    // Material
    // VertexLayout muss exakt zum interleaved VB passen (stride=32, binding=0)
    // -------------------------------------------------------------------------
    renderer::MaterialSystem materials;

    renderer::VertexLayout vLayout;
    vLayout.attributes.push_back({
        renderer::VertexSemantic::Position,
        renderer::Format::RGB32_FLOAT,
        /*binding=*/0u, /*offset=*/0u
    });
    vLayout.attributes.push_back({
        renderer::VertexSemantic::Normal,
        renderer::Format::RGB32_FLOAT,
        /*binding=*/0u, /*offset=*/12u
    });
    vLayout.attributes.push_back({
        renderer::VertexSemantic::TexCoord0,
        renderer::Format::RG32_FLOAT,
        /*binding=*/0u, /*offset=*/24u
    });
    vLayout.bindings.push_back({ /*binding=*/0u, /*stride=*/32u });

    // Name "albedo" → ResolveTextureSlotByName → TexSlots::Albedo (slot 0 / t0)
    renderer::MaterialParam albedoParam{};
    albedoParam.name    = "albedo";
    albedoParam.type    = renderer::MaterialParam::Type::Texture;
    albedoParam.texture = gpuTex;

    // Sampler für Albedo - LinearWrap (s0)
    renderer::MaterialParam samplerParam{};
    samplerParam.name       = "sampler_albedo";
    samplerParam.type       = renderer::MaterialParam::Type::Sampler;
    samplerParam.samplerIdx = 0u;

    renderer::MaterialDesc matDesc{};
    matDesc.name           = "QuadUnlit";
    matDesc.passTag        = renderer::RenderPassTag::Opaque;
    matDesc.vertexShader   = vsHandle;
    matDesc.fragmentShader = psHandle;
    matDesc.vertexLayout   = vLayout;
    matDesc.colorFormat    = renderer::Format::RGBA16_FLOAT;  // passt zu HDRSceneColor RT
    matDesc.depthFormat    = renderer::Format::D24_UNORM_S8_UINT;
    matDesc.params.push_back(albedoParam);
    matDesc.params.push_back(samplerParam);

    const MaterialHandle material = materials.RegisterMaterial(std::move(matDesc));

    // -------------------------------------------------------------------------
    // Tonemap-Material (Passthrough: Fullscreen-Dreieck, kein Vertex-Buffer,
    // kein Depth-Test, liest hdrSceneColor von Slot 0)
    // -------------------------------------------------------------------------
    renderer::DepthStencilState noDepth{};
    noDepth.depthEnable = false;
    noDepth.depthWrite  = false;

    // Sampler für HDR-Input (LinearClamp für Fullscreen-Blit)
    renderer::MaterialParam tonemapSamplerParam{};
    tonemapSamplerParam.name       = "linearclamp";
    tonemapSamplerParam.type       = renderer::MaterialParam::Type::Sampler;
    tonemapSamplerParam.samplerIdx = 0u;

    renderer::MaterialDesc tonemapDesc{};
    tonemapDesc.name           = "Passthrough";
    tonemapDesc.passTag        = renderer::RenderPassTag::Opaque;
    tonemapDesc.vertexShader   = tonemapVsHandle;
    tonemapDesc.fragmentShader = tonemapPsHandle;
    tonemapDesc.depthStencil   = noDepth;
    tonemapDesc.colorFormat    = renderer::Format::RGBA8_UNORM_SRGB;  // Backbuffer-Format
    tonemapDesc.depthFormat    = renderer::Format::D24_UNORM_S8_UINT;
    tonemapDesc.params.push_back(tonemapSamplerParam);

    const MaterialHandle tonemapMaterial = materials.RegisterMaterial(std::move(tonemapDesc));
    loop.GetRenderSystem().SetDefaultTonemapMaterial(tonemapMaterial, materials);

    // -------------------------------------------------------------------------
    // ECS: Entity mit Quad-Mesh + Material
    // -------------------------------------------------------------------------
    ecs::World world;

    const auto entity = world.CreateEntity();
    world.Add<TransformComponent>(entity);
    world.Add<WorldTransformComponent>(entity);
    world.Add<MeshComponent>(entity, meshHandle);
    world.Add<MaterialComponent>(entity, material);
    world.Add<BoundsComponent>(entity, BoundsComponent{
        .centerWorld    = {0.f, 0.f, 0.f},
        .extentsWorld   = {0.5f, 0.5f, 0.01f},
        .boundingSphere = 0.71f
    });

    // -------------------------------------------------------------------------
    // Kamera
    // -------------------------------------------------------------------------
    renderer::RenderView view{};
    view.view           = math::Mat4::LookAtRH(
                              {0.f, 0.f, 3.f},
                              {0.f, 0.f, 0.f},
                              math::Vec3::Up());
    const auto updateProjection = [&view, &loop]()
    {
        const auto* swapchain = loop.GetRenderSystem().GetSwapchain();
        const uint32_t width = (swapchain && swapchain->GetWidth() > 0u) ? swapchain->GetWidth() : 1280u;
        const uint32_t height = (swapchain && swapchain->GetHeight() > 0u) ? swapchain->GetHeight() : 720u;
        const float aspect = (height > 0u)
            ? (static_cast<float>(width) / static_cast<float>(height))
            : (16.0f / 9.0f);
        view.projection = math::Mat4::PerspectiveFovRH(
            60.f * math::DEG_TO_RAD,
            aspect,
            0.1f, 100.f);
    };
    updateProjection();
    view.cameraPosition = {0.f, 0.f, 1.f};
    view.cameraForward  = {0.f, 0.f, -1.f};

    // -------------------------------------------------------------------------
    // Game Loop
    // -------------------------------------------------------------------------
    platform::StdTiming timing;
    while (!loop.ShouldExit())
    {
        if (auto* input = loop.GetInput();
            input && input->IsKeyPressed(platform::Key::Escape))
            loop.GetWindow()->RequestClose();

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
