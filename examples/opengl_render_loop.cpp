#include "ForwardFeature.hpp"
// =============================================================================
// KROM Engine - examples/opengl_render_loop.cpp
// Beispiel: Texturiertes, unbelichtetes Quad mit OpenGL-Backend.
//
// Was dieses Beispiel zeigt:
//   - AssetPipeline: Shader (.vert/.frag) und Textur (.bmp) laden
//   - AssetRegistry: Quad-Mesh programmatisch registrieren
//   - MaterialSystem: Material mit Textur-Binding aufbauen
//   - VertexLayout: passend zum interleaved VB (Pos+Normal+UV, 32 Byte Stride)
//   - PlatformRenderLoop: vollständiger Frame-Loop
// =============================================================================

#include "OpenGLDevice.hpp"
#include "assets/AssetPipeline.hpp"
#include "assets/AssetRegistry.hpp"
#include "core/Debug.hpp"
#include "core/Math.hpp"
#include "ecs/Components.hpp"
#include "ecs/World.hpp"
#include "events/EventBus.hpp"
#include "platform/NullPlatform.hpp"   // StdTiming
#include "renderer/IDevice.hpp"
#include "renderer/MaterialSystem.hpp"
#include "renderer/PlatformRenderLoop.hpp"
#include "renderer/ShaderBindingModel.hpp"
#include "renderer/RendererTypes.hpp"
#include <filesystem>

#if defined(_WIN32)
#   include "platform/Win32Platform.hpp"
#else
#   include "platform/GLFWPlatform.hpp"
#endif

using namespace engine;

int main()
{
    Debug::MinLevel = LogLevel::Info;
    RegisterAllComponents();

    // -------------------------------------------------------------------------
    // Backend registrieren
    // -------------------------------------------------------------------------
    if (!renderer::DeviceFactory::IsRegistered(renderer::DeviceFactory::BackendType::OpenGL))
    {
        Debug::LogError("opengl_render_loop: OpenGL backend registration failed");
        return -10;
    }

    // -------------------------------------------------------------------------
    // Platform
    // -------------------------------------------------------------------------
#if defined(_WIN32)
    platform::win32::Win32Platform runtimePlatform;
#else
#   if !KROM_HAS_GLFW
    Debug::LogError("opengl_render_loop: no OpenGL platform available (no GLFW)");
    return -11;
#   endif
    platform::GLFWPlatform runtimePlatform;
#endif

    if (!runtimePlatform.Initialize())
    {
        Debug::LogError("opengl_render_loop: platform Initialize failed");
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
    wDesc.title              = "KROM - Textured Quad (OpenGL)";
    wDesc.width              = 1280;
    wDesc.height             = 720;
    wDesc.resizable          = true;
    wDesc.openglContext      = true;
    wDesc.openglMajor        = 4;
    wDesc.openglMinor        = 1;
    wDesc.openglDebugContext = true;

    renderer::IDevice::DeviceDesc dDesc{};
    dDesc.enableDebugLayer = true;
    dDesc.appName          = "KROM OpenGL";

    if (!loop.Initialize(renderer::DeviceFactory::BackendType::OpenGL,
                         runtimePlatform, wDesc, &bus, dDesc))
    {
        Debug::LogError("opengl_render_loop: loop.Initialize failed");
        runtimePlatform.Shutdown();
        return -2;
    }

    // -------------------------------------------------------------------------
    // Asset Pipeline
    // -------------------------------------------------------------------------
    assets::AssetRegistry registry;
    loop.GetRenderSystem().SetAssetRegistry(&registry);

    assets::AssetPipeline pipeline(registry,
                                   loop.GetRenderSystem().GetDevice());

    // __FILE__ liefert zur Compile-Zeit den absoluten Pfad dieser .cpp-Datei.
    // Damit funktioniert das Beispiel unabhängig vom Arbeitsverzeichnis des Debuggers.
    const std::filesystem::path assetRoot =
        std::filesystem::path(__FILE__).parent_path().parent_path() / "assets";
    Debug::Log("opengl_render_loop: asset root = %s", assetRoot.string().c_str());
    pipeline.SetAssetRoot(assetRoot.string());

    // Shader laden - Extension .vert/.frag → GLSL, Stage wird automatisch erkannt
    const ShaderHandle vsHandle = pipeline.LoadShader("quad_unlit.vert",
                                                       assets::ShaderStage::Vertex);
    const ShaderHandle fsHandle = pipeline.LoadShader("quad_unlit.frag",
                                                       assets::ShaderStage::Fragment);

    // Passthrough-Shader für TonemapPass (Fullscreen-Dreieck → HDR in tonemapped RT)
    const ShaderHandle tonemapVsHandle = pipeline.LoadShader("fullscreen.vert",
                                                              assets::ShaderStage::Vertex);
    const ShaderHandle tonemapFsHandle = pipeline.LoadShader("passthrough.frag",
                                                              assets::ShaderStage::Fragment);

    if (!vsHandle.IsValid() || !fsHandle.IsValid() ||
        !tonemapVsHandle.IsValid() || !tonemapFsHandle.IsValid())
    {
        Debug::LogError("opengl_render_loop: shader load failed");
        loop.Shutdown();
        runtimePlatform.Shutdown();
        return -3;
    }

    // Prüfen ob der Shader-Source tatsächlich geladen wurde (nicht nur Handle valide)
    {
        auto* vs = registry.shaders.Get(vsHandle);
        if (!vs || vs->sourceCode.empty())
        {
            Debug::LogError("opengl_render_loop: quad_unlit.vert source leer - Datei nicht gefunden in: %s",
                            assetRoot.string().c_str());
            loop.Shutdown();
            runtimePlatform.Shutdown();
            return -3;
        }
        Debug::Log("opengl_render_loop: shaders gefunden (%zu bytes VS)", vs->sourceCode.size());
    }

    // Textur laden (stb_image: PNG/BMP/JPEG/TGA werden unterstützt)
    const TextureHandle texHandle = pipeline.LoadTexture("krom.bmp");
    pipeline.UploadPendingGpuAssets();
    const TextureHandle gpuTex = pipeline.GetGpuTexture(texHandle);

    if (!gpuTex.IsValid())
    {
        Debug::LogError("opengl_render_loop: texture upload failed");
        loop.Shutdown();
        runtimePlatform.Shutdown();
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

    // Name "albedo" → ResolveTextureSlotByName → TexSlots::Albedo (slot 0)
    renderer::MaterialParam albedoParam{};
    albedoParam.name    = "albedo";
    albedoParam.type    = renderer::MaterialParam::Type::Texture;
    albedoParam.texture = gpuTex;

    // Sampler für Albedo-Textur - Default: LinearWrap (Slot 0)
    renderer::MaterialParam samplerParam{};
    samplerParam.name       = "sampler_albedo";  // → ResolveSamplerSlotByName → LinearWrap
    samplerParam.type       = renderer::MaterialParam::Type::Sampler;
    samplerParam.samplerIdx = 0u;  // 0 → ShaderRuntime nutzt passenden Default-Sampler

    renderer::MaterialDesc matDesc{};
    matDesc.name           = "QuadUnlit";
    matDesc.passTag        = renderer::RenderPassTag::Opaque;
    matDesc.vertexShader   = vsHandle;
    matDesc.fragmentShader = fsHandle;
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

    // Sampler für HDR-Input (LinearClamp passt für Fullscreen-Blit)
    renderer::MaterialParam tonemapSamplerParam{};
    tonemapSamplerParam.name       = "linearclamp";
    tonemapSamplerParam.type       = renderer::MaterialParam::Type::Sampler;
    tonemapSamplerParam.samplerIdx = 0u;

    renderer::MaterialDesc tonemapDesc{};
    tonemapDesc.name           = "Passthrough";
    tonemapDesc.passTag        = renderer::RenderPassTag::Opaque;
    tonemapDesc.vertexShader   = tonemapVsHandle;
    tonemapDesc.fragmentShader = tonemapFsHandle;
    tonemapDesc.depthStencil   = noDepth;
    tonemapDesc.rasterizer.cullMode = renderer::CullMode::None;
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
    view.cameraPosition = {0.f, 0.f, 3.f};
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
    runtimePlatform.Shutdown();
    return 0;
}
