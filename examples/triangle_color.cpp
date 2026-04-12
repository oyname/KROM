// triangle_color.cpp
//
// KROM Engine — Triangle with Vertex Colors (DX11)
//
// Renders a single triangle with red, green, and blue vertex colors.
// The GPU interpolates the colors across the triangle surface automatically.
//
// Demonstrates:
//   - VertexSemantic::Color0 in the vertex layout
//   - Shader with COLOR input (triangle_color.hlslvs / .hlslps)
//   - Minimal ECS scene: one entity, no texture, no sampler
//   - PlatformRenderLoop with the forward feature
//
// Build (Visual Studio, x64, Windows):
//   Link: engine_core, engine_forward, engine_platform_win32
//   Self-registering addon: krom_link_self_registering_addon(target engine_dx11)
//   Assets: copy the assets/ folder next to the executable.
//           Required files: triangle_color.hlslvs, triangle_color.hlslps,
//                           fullscreen.hlslvs, passthrough.hlslps

#include "ForwardFeature.hpp"
#include "DX11Device.hpp"
#include "assets/AssetPipeline.hpp"
#include "assets/AssetRegistry.hpp"
#include "core/Debug.hpp"
#include "core/Math.hpp"
#include "ecs/Components.hpp"
#include "ecs/World.hpp"
#include "events/EventBus.hpp"
#include "platform/StdTiming.hpp"
#include "platform/Win32Platform.hpp"
#include "renderer/IDevice.hpp"
#include "renderer/MaterialSystem.hpp"
#include "renderer/PlatformRenderLoop.hpp"
#include "renderer/RendererTypes.hpp"
#include <filesystem>
#include <memory>

using namespace engine;

int main()
{
#ifdef _WIN32
    Debug::ResetMinLevelForBuild();
    RegisterAllComponents();

    // -------------------------------------------------------------------------
    // Adapter
    // -------------------------------------------------------------------------
    const auto adapters = renderer::DeviceFactory::EnumerateAdapters(
        renderer::DeviceFactory::BackendType::DirectX11);
    if (adapters.empty()) { Debug::LogError("no DX11 adapters"); return -1; }

    const uint32_t adapterIndex = renderer::DeviceFactory::FindBestAdapter(adapters);
    Debug::Log("selected adapter: %s", adapters[adapterIndex].name.c_str());

    // -------------------------------------------------------------------------
    // Platform + render loop
    // -------------------------------------------------------------------------
    platform::win32::Win32Platform winPlatform;
    if (!winPlatform.Initialize()) return -1;

    events::EventBus bus;
    renderer::PlatformRenderLoop loop;
    loop.GetRenderSystem().RegisterFeature(
        renderer::addons::forward::CreateForwardFeature());

    platform::WindowDesc wDesc{};
    wDesc.title     = "KROM - Triangle with Vertex Colors (DX11)";
    wDesc.width     = 1280u;
    wDesc.height    = 720u;
    wDesc.resizable = true;

    renderer::IDevice::DeviceDesc dDesc{};
    dDesc.enableDebugLayer = true;
    dDesc.adapterIndex     = adapterIndex;

    if (!loop.Initialize(renderer::DeviceFactory::BackendType::DirectX11,
                         winPlatform, wDesc, &bus, dDesc))
    {
        winPlatform.Shutdown();
        return -2;
    }

    // -------------------------------------------------------------------------
    // Assets
    // -------------------------------------------------------------------------
    assets::AssetRegistry registry;
    loop.GetRenderSystem().SetAssetRegistry(&registry);

    assets::AssetPipeline pipeline(registry, loop.GetRenderSystem().GetDevice());

    pipeline.SetAssetRoot(std::filesystem::path(__FILE__).parent_path().parent_path() / "assets");

    // Vertex-color shaders (no texture, no sampler)
    const ShaderHandle vs = pipeline.LoadShader("triangle_color.hlslvs",
        assets::ShaderStage::Vertex);
    const ShaderHandle ps = pipeline.LoadShader("triangle_color.hlslps",
        assets::ShaderStage::Fragment);

    // Tonemap passthrough shaders (required by the forward feature)
    const ShaderHandle tvs = pipeline.LoadShader("fullscreen.hlslvs",
        assets::ShaderStage::Vertex);
    const ShaderHandle tps = pipeline.LoadShader("passthrough.hlslps",
        assets::ShaderStage::Fragment);

    if (!vs.IsValid() || !ps.IsValid() || !tvs.IsValid() || !tps.IsValid())
    {
        Debug::LogError("triangle_color: shader load failed");
        loop.Shutdown();
        winPlatform.Shutdown();
        return -3;
    }

    // -------------------------------------------------------------------------
    // Triangle mesh
    //
    // Vertex layout: Position (xyz) + Color (rgba) — 28 bytes per vertex.
    // Colors are stored as normalized floats: 0.0 = black, 1.0 = full channel.
    // The GPU interpolates them smoothly between vertices.
    // -------------------------------------------------------------------------
    auto meshAsset = std::make_unique<assets::MeshAsset>();
    assets::SubMeshData tri;

    // Positions: centered on screen, slightly in front of the camera
    tri.positions = {
         0.0f,  0.5f, 0.0f,   // top    — red
        -0.5f, -0.5f, 0.0f,   // left   — green
         0.5f, -0.5f, 0.0f,   // right  — blue
    };

    // Colors: RGBA packed as floats (r, g, b, a per vertex)
    tri.colors = {
        1.0f, 0.0f, 0.0f, 1.0f,   // red
        0.0f, 1.0f, 0.0f, 1.0f,   // green
        0.0f, 0.0f, 1.0f, 1.0f,   // blue
    };

    tri.indices = { 0, 1, 2 };
    meshAsset->submeshes.push_back(std::move(tri));
    const MeshHandle meshHandle = registry.meshes.Add(std::move(meshAsset));

    // -------------------------------------------------------------------------
    // Material
    //
    // Vertex layout: Position (12 bytes) + Color0 (16 bytes) = 28 bytes stride.
    // No normals, no UVs, no texture.
    // -------------------------------------------------------------------------
    renderer::MaterialSystem materials;

    renderer::VertexLayout layout;
    layout.attributes = {
        { renderer::VertexSemantic::Position, renderer::Format::RGB32_FLOAT,  0,  0 },
        { renderer::VertexSemantic::Color0,   renderer::Format::RGBA32_FLOAT, 0, 12 },
    };
    layout.bindings = { { 0, 28u } };

    renderer::MaterialDesc mat{};
    mat.name           = "TriangleColor";
    mat.passTag        = renderer::RenderPassTag::Opaque;
    mat.vertexShader   = vs;
    mat.fragmentShader = ps;
    mat.vertexLayout   = layout;
    mat.colorFormat    = renderer::Format::RGBA16_FLOAT;
    mat.depthFormat    = renderer::Format::D24_UNORM_S8_UINT;
    // No params — this material has no texture or sampler
    const MaterialHandle matHandle = materials.RegisterMaterial(std::move(mat));

    // Tonemap passthrough (required by ForwardFeature to resolve HDR → backbuffer)
    renderer::DepthStencilState noDepth{};
    noDepth.depthEnable = false;
    noDepth.depthWrite  = false;

    renderer::MaterialParam tmSampler{};
    tmSampler.name = "linearclamp";
    tmSampler.type = renderer::MaterialParam::Type::Sampler;

    renderer::MaterialDesc tmDesc{};
    tmDesc.name           = "Passthrough";
    tmDesc.passTag        = renderer::RenderPassTag::Opaque;
    tmDesc.vertexShader   = tvs;
    tmDesc.fragmentShader = tps;
    tmDesc.depthStencil   = noDepth;
    tmDesc.colorFormat    = renderer::Format::RGBA8_UNORM_SRGB;
    tmDesc.depthFormat    = renderer::Format::D24_UNORM_S8_UINT;
    tmDesc.params         = { tmSampler };
    const MaterialHandle tmHandle = materials.RegisterMaterial(std::move(tmDesc));
    loop.GetRenderSystem().SetDefaultTonemapMaterial(tmHandle, materials);

    // -------------------------------------------------------------------------
    // ECS scene — one entity, no rotation
    // -------------------------------------------------------------------------
    ecs::World world;

    const auto entity = world.CreateEntity();
    world.Add<TransformComponent>(entity);
    world.Add<WorldTransformComponent>(entity);
    world.Add<MeshComponent>(entity, meshHandle);
    world.Add<MaterialComponent>(entity, matHandle);
    world.Add<BoundsComponent>(entity, BoundsComponent{
        .extentsWorld   = { 0.5f, 0.5f, 0.01f },
        .boundingSphere = 0.71f,
    });

    // -------------------------------------------------------------------------
    // Camera — looking straight at the origin from z=2
    // -------------------------------------------------------------------------
    renderer::RenderView view{};
    view.view           = math::Mat4::LookAtRH({ 0.f, 0.f, 2.f }, {}, math::Vec3::Up());
    view.cameraPosition = { 0.f, 0.f, 2.f };
    view.cameraForward  = { 0.f, 0.f,-1.f };

    const auto updateProj = [&]() {
        const auto* sc = loop.GetRenderSystem().GetSwapchain();
        const float w  = sc ? static_cast<float>(sc->GetWidth())  : 1280.f;
        const float h  = sc ? static_cast<float>(sc->GetHeight()) : 720.f;
        view.projection = math::Mat4::PerspectiveFovRH(
            60.f * math::DEG_TO_RAD, w / h, 0.1f, 100.f);
    };
    updateProj();

    // -------------------------------------------------------------------------
    // Game loop
    // -------------------------------------------------------------------------
    platform::StdTiming timing;
    while (!loop.ShouldExit())
    {
        if (auto* input = loop.GetInput();
            input && input->IsKeyPressed(platform::Key::Escape))
            loop.GetWindow()->RequestClose();

        updateProj();
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
