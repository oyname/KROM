#include "PbrShadowScene.hpp"

#include "PbrMasterMaterial.hpp"
#include "PbrInstanceBuilder.hpp"
#include "addons/camera/CameraComponents.hpp"
#include "addons/lighting/LightingComponents.hpp"
#include "addons/mesh_renderer/MeshRendererComponents.hpp"
#include "assets/MeshTangents.hpp"
#include "core/Debug.hpp"
#include "renderer/Environment.hpp"
#include "renderer/RenderWorld.hpp"
#include "platform/IInput.hpp"
#include <cmath>

namespace engine::examples {

namespace {

void FlipNormalMapGreenChannel(TextureHandle handle, assets::AssetRegistry& registry)
{
    auto* texture = registry.textures.Get(handle);
    if (!texture || texture->format != assets::TextureFormat::RGBA8_UNORM)
        return;

    for (size_t i = 1u; i < texture->pixelData.size(); i += 4u)
        texture->pixelData[i] = static_cast<uint8_t>(255u - texture->pixelData[i]);

    texture->gpuStatus.dirty = true;
}

} // namespace

bool PbrShadowScene::Build(ExampleSceneContext& context)
{
    const renderer::VertexLayout vertexLayout = CreatePbrVertexLayout();
    if (!CreateMaterials(context, vertexLayout))
        return false;

    if (!CreateEnvironment(context))
        return false;

    const MeshHandle cubeMesh    = CreateCubeMesh(context.assetRegistry);
    const MeshHandle sphereMesh  = CreateSphereMesh(context.assetRegistry);
    const MeshHandle diamondMesh = CreateDiamondMesh(context.assetRegistry);
    const MeshHandle floorMesh   = CreateFloorMesh(context.assetRegistry);
    if (!cubeMesh.IsValid() || !sphereMesh.IsValid() || !diamondMesh.IsValid() || !floorMesh.IsValid())
    {
        Debug::LogError("PbrShadowScene: failed to create procedural meshes");
        return false;
    }

    CreateSceneEntities(context, cubeMesh, sphereMesh, diamondMesh, floorMesh);
    m_cubeMeshHandle = cubeMesh;
    return true;
}

bool PbrShadowScene::Update(ExampleSceneContext& context, float deltaSeconds)
{
    if (auto* input = context.renderLoop.GetInput())
    {
        // --- Debug-Views (F1-F9) ---
        // F1 = normal, F2-F9 = isolierte Lichtterme
        struct { platform::Key key; uint32_t flags; const char* label; } kViewKeys[] = {
            { platform::Key::F1, 0u,                                       "Normal"       },
            { platform::Key::F2, renderer::DBG_VIEW_NORMALS,               "Normals"      },
            { platform::Key::F3, renderer::DBG_VIEW_NOL,                   "NdotL"        },
            { platform::Key::F4, renderer::DBG_VIEW_ROUGHNESS,             "Roughness"    },
            { platform::Key::F5, renderer::DBG_VIEW_METALLIC,              "Metallic"     },
            { platform::Key::F6, renderer::DBG_VIEW_AO,                    "AO"           },
            { platform::Key::F7, renderer::DBG_VIEW_SHADOW,                "Shadow"       },
            { platform::Key::F8, renderer::DBG_VIEW_DIRECT_DIFF,           "DirectDiff"   },
            { platform::Key::F9, renderer::DBG_VIEW_DIRECT_SPEC,           "DirectSpec"   },
        };
        static constexpr uint32_t kViewMask =
            renderer::DBG_VIEW_NORMALS | renderer::DBG_VIEW_NOL     |
            renderer::DBG_VIEW_ROUGHNESS | renderer::DBG_VIEW_METALLIC |
            renderer::DBG_VIEW_AO      | renderer::DBG_VIEW_SHADOW   |
            renderer::DBG_VIEW_DIRECT_DIFF | renderer::DBG_VIEW_DIRECT_SPEC |
            renderer::DBG_VIEW_IBL_DIFF | renderer::DBG_VIEW_IBL_SPEC |
            renderer::DBG_VIEW_FRESNEL_F0;

        for (const auto& entry : kViewKeys)
        {
            if (input->KeyHit(entry.key))
            {
                m_debugFlags = (m_debugFlags & ~kViewMask) | entry.flags;
                Debug::Log("PbrShadowScene: debug view -> %s", entry.label);
            }
        }

        // --- Disable-Toggles (Num1-4) ---
        // Num1 = IBL, Num2 = Shadows, Num3 = AO, Num4 = NormalMap
        struct { platform::Key key; uint32_t flag; const char* label; } kToggleKeys[] = {
            { platform::Key::Num1, renderer::DBG_DISABLE_IBL,       "IBL"       },
            { platform::Key::Num2, renderer::DBG_DISABLE_SHADOWS,   "Shadows"   },
            { platform::Key::Num3, renderer::DBG_DISABLE_AO,        "AO"        },
            { platform::Key::Num4, renderer::DBG_DISABLE_NORMALMAP, "NormalMap" },
        };
        for (const auto& entry : kToggleKeys)
        {
            if (input->KeyHit(entry.key))
            {
                m_debugFlags ^= entry.flag;
                Debug::Log("PbrShadowScene: disable %s -> %s", entry.label,
                           (m_debugFlags & entry.flag) ? "ON" : "OFF");
            }
        }

        context.debugFlags = m_debugFlags;

        if (input->KeyHit(platform::Key::F11))
        {
            m_stressModeActive = !m_stressModeActive;
            if (m_stressModeActive && !m_stressEntitiesCreated && m_cubeMeshHandle.IsValid())
                CreateStressEntities(context, m_cubeMeshHandle);
            Debug::Log("PbrShadowScene: stress mode %s (F11)", m_stressModeActive ? "ON" : "OFF");
        }
        else if (input->KeyHit(platform::Key::F12))
        {
            m_stressStatsEnabled = !m_stressStatsEnabled;
            m_stressStatsAccumulator = 0.0f;
            Debug::Log("PbrShadowScene: stress stats %s (F12)", m_stressStatsEnabled ? "ON" : "OFF");
        }

        constexpr float kCamSpeed = 90.0f;
        if (input->KeyDown(platform::Key::Left))
            m_cameraYawDeg += kCamSpeed * deltaSeconds;
        if (input->KeyDown(platform::Key::Right))
            m_cameraYawDeg -= kCamSpeed * deltaSeconds;
        if (input->KeyDown(platform::Key::Up))
            m_cameraPitchDeg += kCamSpeed * deltaSeconds;
        if (input->KeyDown(platform::Key::Down))
            m_cameraPitchDeg -= kCamSpeed * deltaSeconds;

        constexpr float kMoveSpeed = 5.0f;

        // Vorwärtsvektor aus Yaw + Pitch berechnen
        m_cameraPitchDeg = std::max(-89.0f, std::min(89.0f, m_cameraPitchDeg));

        const Quat camRot = Quat::FromEulerDeg(m_cameraPitchDeg, m_cameraYawDeg, 0.f);
        const Vec3 forward = camRot.Rotate(Vec3::Forward());

        if (input->KeyDown(platform::Key::W))
            m_cameraPos = m_cameraPos + forward * (kMoveSpeed * deltaSeconds);
        if (input->KeyDown(platform::Key::S))
            m_cameraPos = m_cameraPos - forward * (kMoveSpeed * deltaSeconds);

        if (auto* t = context.world.Get<TransformComponent>(m_cameraEntity))
        {
            t->SetEulerDeg(m_cameraPitchDeg, m_cameraYawDeg, 0.f);
            t->localPosition = m_cameraPos;   // direkt setzen
            t->dirty = true;                  // Transform-System informieren
        }
    }

    m_rotationYawDeg += 50.0f * deltaSeconds;
    m_rotationPitchDeg = 10.0f + 8.0f * std::sin(m_rotationYawDeg * 0.0125f);

    if (auto* t = context.world.Get<TransformComponent>(m_cubeEntity))
        t->SetEulerDeg(m_rotationPitchDeg, m_rotationYawDeg, 0.0f);

    if (auto* t = context.world.Get<TransformComponent>(m_diamondEntity))
        t->SetEulerDeg(0.0f, m_rotationYawDeg * 0.4f, 0.0f);

    if (auto* t = context.world.Get<TransformComponent>(m_sphereEntity))
        t->SetEulerDeg(0.0f, m_rotationYawDeg * 0.4f, 0.0f);

    if (m_stressModeActive)
        UpdateStressEntities(context, deltaSeconds);

    if (m_stressStatsEnabled)
        LogStressStats(context, deltaSeconds);

    return true;
}

MeshHandle PbrShadowScene::CreateCubeMesh(assets::AssetRegistry& registry) const
{
    auto meshAsset = std::make_unique<assets::MeshAsset>();
    assets::SubMeshData cube;

    const auto addFace = [&](math::Vec3 p0, math::Vec3 p1, math::Vec3 p2, math::Vec3 p3, math::Vec3 normal)
    {
        const uint32_t base = static_cast<uint32_t>(cube.positions.size() / 3u);
        for (const auto& p : { p0, p1, p2, p3 })
        {
            cube.positions.insert(cube.positions.end(), { p.x, p.y, p.z });
            cube.normals.insert(cube.normals.end(), { normal.x, normal.y, normal.z });
        }

        cube.uvs.insert(cube.uvs.end(), {
            0.0f, 0.0f,
            0.5f, 0.0f,
            0.5f, 0.5f,
            0.0f, 0.5f,
        });

        cube.indices.insert(cube.indices.end(), {
            base + 0u, base + 1u, base + 2u,
            base + 0u, base + 2u, base + 3u,
        });
    };

    constexpr float halfExtent = 0.5f;
    addFace({ -halfExtent, -halfExtent,  halfExtent }, {  halfExtent, -halfExtent,  halfExtent },
            {  halfExtent,  halfExtent,  halfExtent }, { -halfExtent,  halfExtent,  halfExtent }, { 0.f, 0.f, 1.f });
    addFace({  halfExtent, -halfExtent, -halfExtent }, { -halfExtent, -halfExtent, -halfExtent },
            { -halfExtent,  halfExtent, -halfExtent }, {  halfExtent,  halfExtent, -halfExtent }, { 0.f, 0.f, -1.f });
    addFace({ -halfExtent, -halfExtent, -halfExtent }, { -halfExtent, -halfExtent,  halfExtent },
            { -halfExtent,  halfExtent,  halfExtent }, { -halfExtent,  halfExtent, -halfExtent }, { -1.f, 0.f, 0.f });
    addFace({  halfExtent, -halfExtent,  halfExtent }, {  halfExtent, -halfExtent, -halfExtent },
            {  halfExtent,  halfExtent, -halfExtent }, {  halfExtent,  halfExtent,  halfExtent }, { 1.f, 0.f, 0.f });
    addFace({ -halfExtent,  halfExtent,  halfExtent }, {  halfExtent,  halfExtent,  halfExtent },
            {  halfExtent,  halfExtent, -halfExtent }, { -halfExtent,  halfExtent, -halfExtent }, { 0.f, 1.f, 0.f });
    addFace({ -halfExtent, -halfExtent, -halfExtent }, {  halfExtent, -halfExtent, -halfExtent },
            {  halfExtent, -halfExtent,  halfExtent }, { -halfExtent, -halfExtent,  halfExtent }, { 0.f, -1.f, 0.f });

    assets::EnsureTangents(cube);
    meshAsset->submeshes.push_back(std::move(cube));
    return registry.meshes.Add(std::move(meshAsset));
}

MeshHandle PbrShadowScene::CreateSphereMesh(assets::AssetRegistry& registry) const
{
    auto meshAsset = std::make_unique<assets::MeshAsset>();
    assets::SubMeshData sphere;

    constexpr int kRings = 36;
    constexpr int kSegs  = 48;
    constexpr float kR   = 0.5f;
    constexpr float kPi  = 3.14159265358979f;

    for (int i = 0; i <= kRings; ++i)
    {
        const float theta = kPi * static_cast<float>(i) / static_cast<float>(kRings);
        const float sinT  = std::sin(theta);
        const float cosT  = std::cos(theta);
        const float v     = static_cast<float>(i) / static_cast<float>(kRings);

        for (int j = 0; j <= kSegs; ++j)
        {
            const float phi  = 2.0f * kPi * static_cast<float>(j) / static_cast<float>(kSegs);
            const float nx   = sinT * std::cos(phi);
            const float ny   = cosT;
            const float nz   = sinT * std::sin(phi);
            sphere.positions.insert(sphere.positions.end(), { kR * nx, kR * ny, kR * nz });
            sphere.normals.insert(sphere.normals.end(), { nx, ny, nz });
            sphere.uvs.insert(sphere.uvs.end(), {
                static_cast<float>(j) / static_cast<float>(kSegs), v });
        }
    }

    for (int i = 0; i < kRings; ++i)
    {
        for (int j = 0; j < kSegs; ++j)
        {
            const uint32_t a = static_cast<uint32_t>(i * (kSegs + 1) + j);
            const uint32_t b = a + static_cast<uint32_t>(kSegs + 1);
            sphere.indices.insert(sphere.indices.end(), { a, b + 1u, b, a, a + 1u, b + 1u });
        }
    }

    assets::EnsureTangents(sphere);
    meshAsset->submeshes.push_back(std::move(sphere));
    return registry.meshes.Add(std::move(meshAsset));
}

MeshHandle PbrShadowScene::CreateDiamondMesh(assets::AssetRegistry& registry) const
{
    auto meshAsset = std::make_unique<assets::MeshAsset>();
    assets::SubMeshData diamond;

    constexpr int   kSides   = 8;
    constexpr float kPi      = 3.14159265358979f;
    constexpr float kGirdleY = 0.08f;
    constexpr float kGirdleR = 0.5f;
    constexpr float kApexY   = 0.65f;
    constexpr float kNadirY  = -0.5f;

    const auto addTri = [&](math::Vec3 a, math::Vec3 b, math::Vec3 c)
    {
        const float abx = b.x - a.x, aby = b.y - a.y, abz = b.z - a.z;
        const float acx = c.x - a.x, acy = c.y - a.y, acz = c.z - a.z;
        float nx = aby * acz - abz * acy;
        float ny = abz * acx - abx * acz;
        float nz = abx * acy - aby * acx;
        const float len = std::sqrt(nx * nx + ny * ny + nz * nz);
        if (len > 1e-6f) { nx /= len; ny /= len; nz /= len; }

        const uint32_t base = static_cast<uint32_t>(diamond.positions.size() / 3u);
        for (const auto& p : { a, b, c })
            diamond.positions.insert(diamond.positions.end(), { p.x, p.y, p.z });
        for (int k = 0; k < 3; ++k)
            diamond.normals.insert(diamond.normals.end(), { nx, ny, nz });
        diamond.uvs.insert(diamond.uvs.end(), { 0.0f, 0.0f, 0.5f, 1.0f, 1.0f, 0.0f });
        diamond.indices.insert(diamond.indices.end(), { base, base + 1u, base + 2u });
    };

    for (int i = 0; i < kSides; ++i)
    {
        const float phi0 = 2.0f * kPi * static_cast<float>(i)     / static_cast<float>(kSides);
        const float phi1 = 2.0f * kPi * static_cast<float>(i + 1) / static_cast<float>(kSides);
        const math::Vec3 g0   = { kGirdleR * std::cos(phi0), kGirdleY, kGirdleR * std::sin(phi0) };
        const math::Vec3 g1   = { kGirdleR * std::cos(phi1), kGirdleY, kGirdleR * std::sin(phi1) };
        const math::Vec3 apex = { 0.f, kApexY,  0.f };
        const math::Vec3 nadir= { 0.f, kNadirY, 0.f };

        addTri(apex, g1, g0);
        addTri(g0, g1, nadir);
    }

    assets::EnsureTangents(diamond);
    meshAsset->submeshes.push_back(std::move(diamond));
    return registry.meshes.Add(std::move(meshAsset));
}

MeshHandle PbrShadowScene::CreateFloorMesh(assets::AssetRegistry& registry) const
{
    auto meshAsset = std::make_unique<assets::MeshAsset>();
    assets::SubMeshData floor;

    constexpr float halfSize = 4.0f;
    floor.positions = {
        -halfSize, 0.0f, -halfSize,
         halfSize, 0.0f, -halfSize,
         halfSize, 0.0f,  halfSize,
        -halfSize, 0.0f,  halfSize,
    };
    floor.normals = {
        0.f, 1.f, 0.f,
        0.f, 1.f, 0.f,
        0.f, 1.f, 0.f,
        0.f, 1.f, 0.f,
    };
    floor.uvs = {
        0.f, 0.f,
        1.f, 0.f,
        1.f, 1.f,
        0.f, 1.f,
    };
    floor.indices = { 0u, 2u, 1u, 0u, 3u, 2u };

    assets::EnsureTangents(floor);
    meshAsset->submeshes.push_back(std::move(floor));
    return registry.meshes.Add(std::move(meshAsset));
}

renderer::VertexLayout PbrShadowScene::CreatePbrVertexLayout() const
{
    renderer::VertexLayout layout;
    layout.attributes.push_back({ renderer::VertexSemantic::Position,  renderer::Format::RGB32_FLOAT,  0u,  0u });
    layout.attributes.push_back({ renderer::VertexSemantic::Normal,    renderer::Format::RGB32_FLOAT,  0u, 12u });
    layout.attributes.push_back({ renderer::VertexSemantic::Tangent,   renderer::Format::RGBA32_FLOAT, 0u, 24u });
    layout.attributes.push_back({ renderer::VertexSemantic::TexCoord0, renderer::Format::RG32_FLOAT,   0u, 40u });
    layout.bindings.push_back({ 0u, 48u });
    return layout;
}

bool PbrShadowScene::CreateEnvironment(ExampleSceneContext& context) const
{
    renderer::EnvironmentDesc environment{};
    environment.mode          = renderer::EnvironmentMode::Texture;
    environment.sourceTexture = context.assetPipeline.LoadTexture("autumn_field_puresky_2k.hdr");
    environment.intensity     = 0.5f;
    environment.enableIBL     = true;
    if (!environment.sourceTexture.IsValid())
    {
        Debug::LogError("PbrShadowScene: failed to load HDR environment");
        return false;
    }
    const auto handle = context.renderLoop.GetRenderSystem().CreateEnvironment(environment);
    if (!handle.IsValid())
    {
        Debug::LogError("PbrShadowScene: failed to create HDR environment");
        return false;
    }
    const_cast<PbrShadowScene*>(this)->m_environmentHandle = handle;
    context.renderLoop.GetRenderSystem().SetActiveEnvironment(handle);
    return true;
}

bool PbrShadowScene::CreateMaterials(ExampleSceneContext& context,
                                     const renderer::VertexLayout& vertexLayout)
{
    const char* vsPath     = "pbr_lit.vs.hlsl";
    const char* fsPath     = "pbr_lit.ps.hlsl";
    const char* shadowPath = "shadow.vs.hlsl";
    const char* tonemapVsPath = "fullscreen.vs.hlsl";
    const char* tonemapPsPath = "passthrough.ps.hlsl";
#if defined(KROM_EXAMPLE_BACKEND_OPENGL)
    vsPath        = "pbr_lit.opengl.vs.glsl";
    fsPath        = "pbr_lit.opengl.fs.glsl";
    shadowPath    = "shadow.opengl.vs.glsl";
    tonemapVsPath = "fullscreen.opengl.vs.glsl";
    tonemapPsPath = "passthrough.opengl.fs.glsl";
#endif

    const ShaderHandle vs     = context.assetPipeline.LoadShader(vsPath,     assets::ShaderStage::Vertex);
    const ShaderHandle fs     = context.assetPipeline.LoadShader(fsPath,     assets::ShaderStage::Fragment);
    const ShaderHandle shadow = context.assetPipeline.LoadShader(shadowPath, assets::ShaderStage::Vertex);
    if (!vs.IsValid() || !fs.IsValid() || !shadow.IsValid())
    {
        Debug::LogError("PbrShadowScene: failed to load PBR shaders");
        return false;
    }

    const ShaderHandle tonemapVs = context.assetPipeline.LoadShader(tonemapVsPath, assets::ShaderStage::Vertex);
    const ShaderHandle tonemapPs = context.assetPipeline.LoadShader(tonemapPsPath, assets::ShaderStage::Fragment);
    if (!tonemapVs.IsValid() || !tonemapPs.IsValid())
    {
        Debug::LogError("PbrShadowScene: failed to load tonemap shaders");
        return false;
    }

    // ── Textures ──────────────────────────────────────────────────────────────

    const TextureHandle cubeAlbedo  = context.assetPipeline.LoadTexture("checkered_pavement_tiles_diff_2k.png");
    const TextureHandle cubeNormal  = context.assetPipeline.LoadTexture("checkered_pavement_tiles_nor_dx_2k.png");
    const TextureHandle floorAlbedo = context.assetPipeline.LoadTexture("cobblestone_floor_09_diff_2k.png");
    const TextureHandle floorNormal = context.assetPipeline.LoadTexture("cobblestone_floor_09_nor_dx_2k.png");

    const auto configureAlbedo = [&](TextureHandle h) {
        if (auto* t = context.assetRegistry.textures.Get(h)) {
            t->metadata.semantic       = assets::TextureSemantic::Color;
            t->metadata.normalEncoding = assets::NormalEncoding::None;
            t->metadata.colorSpace     = assets::ColorSpace::SRGB;
            t->gpuStatus.dirty         = true;
        }
    };
    const auto configureNormal = [&](TextureHandle h) {
        if (auto* t = context.assetRegistry.textures.Get(h)) {
            t->metadata.semantic       = assets::TextureSemantic::Normal;
            t->metadata.normalEncoding = assets::NormalEncoding::RGB;
            t->metadata.colorSpace     = assets::ColorSpace::Linear;
            t->gpuStatus.dirty         = true;
        }
    };

    configureAlbedo(cubeAlbedo);
    configureNormal(cubeNormal);
    configureAlbedo(floorAlbedo);
    configureNormal(floorNormal);

#if !defined(KROM_EXAMPLE_BACKEND_DX11)
    FlipNormalMapGreenChannel(cubeNormal,  context.assetRegistry);
    FlipNormalMapGreenChannel(floorNormal, context.assetRegistry);
#endif

    context.assetPipeline.UploadPendingGpuAssets();

    const TextureHandle gpuCubeAlbedo  = context.assetPipeline.GetGpuTexture(cubeAlbedo);
    const TextureHandle gpuCubeNormal  = context.assetPipeline.GetGpuTexture(cubeNormal);
    const TextureHandle gpuFloorAlbedo = context.assetPipeline.GetGpuTexture(floorAlbedo);
    const TextureHandle gpuFloorNormal = context.assetPipeline.GetGpuTexture(floorNormal);
    if (!gpuCubeAlbedo.IsValid() || !gpuCubeNormal.IsValid() ||
        !gpuFloorAlbedo.IsValid() || !gpuFloorNormal.IsValid())
    {
        Debug::LogError("PbrShadowScene: texture upload failed");
        return false;
    }

    // ── PBR master + instances ────────────────────────────────────────────────

    renderer::pbr::PbrMasterMaterial::Config pbrConfig{};
    pbrConfig.vs           = vs;
    pbrConfig.fs           = fs;
    pbrConfig.shadow       = shadow;
    pbrConfig.vertexLayout = vertexLayout;
    pbrConfig.cullMode     = renderer::MaterialCullMode::Back;
#if defined(KROM_EXAMPLE_BACKEND_OPENGL)
    pbrConfig.frontFace    = renderer::WindingOrder::CW;
#endif
    pbrConfig.castShadows  = true;
    pbrConfig.receiveShadows = true;

    renderer::pbr::PbrMasterMaterial master =
        renderer::pbr::PbrMasterMaterial::Create(context.materialSystem, pbrConfig);
    if (!master.IsValid())
    {
        Debug::LogError("PbrShadowScene: failed to create PBR master material");
        return false;
    }

    m_cubeMaterial = master.CreateInstance("PbrCube")
        .BaseColor(gpuCubeAlbedo)
        .Normal(gpuCubeNormal)
        .Roughness(0.5f)
        .Metallic(0.5f)
        .IBL(true)
        .Build();

    m_floorMaterial = master.CreateInstance("PbrFloor")
        .BaseColor(gpuFloorAlbedo)
        .Normal(gpuFloorNormal, 2.0f)
        .Roughness(0.9f)
        .Metallic(0.0f)
        .IBL(true)
        .Build();

    if (!m_cubeMaterial.IsValid() || !m_floorMaterial.IsValid())
    {
        Debug::LogError("PbrShadowScene: failed to create PBR material instances");
        return false;
    }

    // ── Tonemap ───────────────────────────────────────────────────────────────

    renderer::MaterialParam tonemapSampler{};
    tonemapSampler.name       = "linearclamp";
    tonemapSampler.type       = renderer::MaterialParam::Type::Sampler;
    tonemapSampler.samplerIdx = 0u;

    const renderer::ISwapchain* swapchain = context.renderLoop.GetRenderSystem().GetSwapchain();
    const renderer::Format backbufferFormat = swapchain
        ? swapchain->GetBackbufferFormat()
        : renderer::Format::BGRA8_UNORM_SRGB;

    renderer::DepthStencilState noDepth{};
    noDepth.depthEnable = false;
    noDepth.depthWrite  = false;

    renderer::MaterialDesc tonemapDesc{};
    tonemapDesc.name           = "PbrShadowTonemap";
    tonemapDesc.renderPass     = renderer::StandardRenderPasses::Opaque();
    tonemapDesc.vertexShader   = tonemapVs;
    tonemapDesc.fragmentShader = tonemapPs;
    tonemapDesc.rasterizer.cullMode = renderer::CullMode::None;
    tonemapDesc.depthStencil   = noDepth;
    tonemapDesc.colorFormat    = backbufferFormat;
    tonemapDesc.depthFormat    = renderer::Format::Unknown;
    tonemapDesc.params.push_back(tonemapSampler);
    m_tonemapMaterial = context.materialSystem.RegisterMaterial(std::move(tonemapDesc));
    if (!m_tonemapMaterial.IsValid())
    {
        Debug::LogError("PbrShadowScene: failed to create tonemap material");
        return false;
    }

    context.renderLoop.GetRenderSystem().SetDefaultTonemapMaterial(
        m_tonemapMaterial, context.materialSystem);

    return true;
}

void PbrShadowScene::CreateSceneEntities(ExampleSceneContext& context,
                                         MeshHandle cubeMesh,
                                         MeshHandle sphereMesh,
                                         MeshHandle diamondMesh,
                                         MeshHandle floorMesh)
{
    // ---- Cube (center) ----
    m_cubeEntity = context.world.CreateEntity();
    context.world.Add<TransformComponent>(m_cubeEntity);
    context.world.Add<WorldTransformComponent>(m_cubeEntity);
    context.world.Add<MeshComponent>(m_cubeEntity, cubeMesh);
    context.world.Add<MaterialComponent>(m_cubeEntity, m_cubeMaterial);
    context.world.Add<BoundsComponent>(m_cubeEntity, BoundsComponent{
        .centerLocal  = { 0.f, 0.f, 0.f },
        .extentsLocal = { 0.5f, 0.5f, 0.5f },
        .centerWorld  = { 0.f, 0.05f, 0.f },
        .extentsWorld = { 0.5f, 0.5f, 0.5f },
        .boundingSphere = 0.8660254f,
        .localDirty   = true,
    });
    if (auto* t = context.world.Get<TransformComponent>(m_cubeEntity))
    {
        t->localPosition = { 0.f, 0.05f, 0.f };
        t->localScale    = { 1.f, 1.f, 1.f };
        t->SetEulerDeg(m_rotationPitchDeg, m_rotationYawDeg, 0.f);
    }

    // ---- Sphere (left) ----
    m_sphereEntity = context.world.CreateEntity();
    context.world.Add<TransformComponent>(m_sphereEntity);
    context.world.Add<WorldTransformComponent>(m_sphereEntity);
    context.world.Add<MeshComponent>(m_sphereEntity, sphereMesh);
    context.world.Add<MaterialComponent>(m_sphereEntity, m_cubeMaterial);
    context.world.Add<BoundsComponent>(m_sphereEntity, BoundsComponent{
        .centerLocal  = { 0.f, 0.f, 0.f },
        .extentsLocal = { 0.5f, 0.5f, 0.5f },
        .centerWorld  = { -2.2f, 0.05f, 0.f },
        .extentsWorld = { 0.5f, 0.5f, 0.5f },
        .boundingSphere = 0.5f,
        .localDirty   = true,
    });
    if (auto* t = context.world.Get<TransformComponent>(m_sphereEntity))
    {
        t->localPosition = { -2.2f, 0.05f, 0.f };
        t->localScale    = { 1.f, 1.f, 1.f };
        t->SetEulerDeg(0.f, 0.f, 0.f);
    }

    // ---- Diamond (right) ----
    m_diamondEntity = context.world.CreateEntity();
    context.world.Add<TransformComponent>(m_diamondEntity);
    context.world.Add<WorldTransformComponent>(m_diamondEntity);
    context.world.Add<MeshComponent>(m_diamondEntity, diamondMesh);
    context.world.Add<MaterialComponent>(m_diamondEntity, m_cubeMaterial);
    context.world.Add<BoundsComponent>(m_diamondEntity, BoundsComponent{
        .centerLocal  = { 0.f, 0.075f, 0.f },
        .extentsLocal = { 0.5f, 0.575f, 0.5f },
        .centerWorld  = { 2.2f, 0.075f, 0.f },
        .extentsWorld = { 0.5f, 0.575f, 0.5f },
        .boundingSphere = 0.79f,
        .localDirty   = true,
    });
    if (auto* t = context.world.Get<TransformComponent>(m_diamondEntity))
    {
        t->localPosition = { 2.2f, 0.05f, 0.f };
        t->localScale    = { 1.f, 1.f, 1.f };
        t->SetEulerDeg(0.f, 0.f, 0.f);
    }

    // ---- Floor ----
    m_floorEntity = context.world.CreateEntity();
    context.world.Add<TransformComponent>(m_floorEntity);
    context.world.Add<WorldTransformComponent>(m_floorEntity);
    context.world.Add<MeshComponent>(m_floorEntity, floorMesh);
    if (auto* mc = context.world.Get<MeshComponent>(m_floorEntity))
        mc->castShadows = false;
    context.world.Add<MaterialComponent>(m_floorEntity, m_floorMaterial);
    context.world.Add<BoundsComponent>(m_floorEntity, BoundsComponent{
        .centerLocal  = { 0.f, 0.f, 0.f },
        .extentsLocal = { 4.f, 0.05f, 4.f },
        .centerWorld  = { 0.f, -0.75f, 0.f },
        .extentsWorld = { 4.f, 0.05f, 4.f },
        .boundingSphere = 5.657f,
        .localDirty   = true,
    });
    if (auto* t = context.world.Get<TransformComponent>(m_floorEntity))
    {
        t->localPosition = { 0.f, -0.75f, 0.f };
        t->localScale    = { 1.f, 1.f, 1.f };
        t->SetEulerDeg(0.f, 0.f, 0.f);
    }

    // ---- Directional light ----
    const EntityID lightEntity = context.world.CreateEntity();
    auto& lightTransform = context.world.Add<TransformComponent>(lightEntity);
    lightTransform.localPosition = { 0.f, 0.0f, 0.0f };
    lightTransform.localScale    = { 1.f, 1.f, 1.f };
    lightTransform.SetEulerDeg(-48.f, 135.f, 0.f);
    context.world.Add<WorldTransformComponent>(lightEntity);

    LightComponent light{};
    light.type      = LightType::Directional;
    light.color     = { 1.0f, 1.0f, 1.0f };
    light.intensity = 1.0f;
    light.castShadows = true;
    light.shadowSettings.enabled    = true;
    light.shadowSettings.type       = ShadowType::PCF;
    light.shadowSettings.filter     = ShadowFilter::PCF3x3;
    light.shadowSettings.resolution = 4096u;
    light.shadowSettings.bias       = 0.00015f;
    light.shadowSettings.normalBias = 0.0008f;
    light.shadowSettings.maxDistance = 18.0f;
    light.shadowSettings.strength   = 1.0f;
    context.world.Add<LightComponent>(lightEntity, light);

    // ---- Camera ----
    m_cameraEntity = context.world.CreateEntity();
    auto& cameraTransform = context.world.Add<TransformComponent>(m_cameraEntity);
    cameraTransform.localScale    = { 1.f, 1.f, 1.f };
    cameraTransform.SetEulerDeg(m_cameraPitchDeg, m_cameraYawDeg, 0.f);
    context.world.Add<WorldTransformComponent>(m_cameraEntity);
    context.world.Add<CameraComponent>(m_cameraEntity, CameraComponent{
        .projection  = ProjectionType::Perspective,
        .fovYDeg     = 55.f,
        .nearPlane   = 0.1f,
        .farPlane    = 100.f,
        .isMainCamera = true,
    });
}

void PbrShadowScene::CreateStressEntities(ExampleSceneContext& context, MeshHandle cubeMesh)
{
    if (m_stressEntitiesCreated)
        return;

    constexpr int kGridX   = 24;
    constexpr int kGridZ   = 24;
    constexpr float kSpacing = 1.35f;
    constexpr float kBaseY   = 0.05f;

    m_stressEntities.reserve(static_cast<size_t>(kGridX * kGridZ));
    for (int z = 0; z < kGridZ; ++z)
    {
        for (int x = 0; x < kGridX; ++x)
        {
            if (x == kGridX / 2 && z == kGridZ / 2)
                continue;

            const float px = (static_cast<float>(x) - static_cast<float>(kGridX - 1) * 0.5f) * kSpacing;
            const float pz = (static_cast<float>(z) - static_cast<float>(kGridZ - 1) * 0.5f) * kSpacing;

            const EntityID entity = context.world.CreateEntity();
            context.world.Add<TransformComponent>(entity);
            context.world.Add<WorldTransformComponent>(entity);
            context.world.Add<MeshComponent>(entity, cubeMesh);
            context.world.Add<MaterialComponent>(entity, m_cubeMaterial);
            context.world.Add<BoundsComponent>(entity, BoundsComponent{
                .centerLocal  = { 0.f, 0.f, 0.f },
                .extentsLocal = { 0.5f, 0.5f, 0.5f },
                .centerWorld  = { px, kBaseY, pz },
                .extentsWorld = { 0.5f, 0.5f, 0.5f },
                .boundingSphere = 0.8660254f,
                .localDirty   = true,
            });

            if (auto* t = context.world.Get<TransformComponent>(entity))
            {
                t->localPosition = { px, kBaseY, pz };
                t->localScale    = { 0.65f, 0.65f, 0.65f };
                t->SetEulerDeg(0.f, static_cast<float>((x * 17 + z * 11) % 360), 0.f);
            }

            m_stressEntities.push_back(entity);
        }
    }

    m_stressEntitiesCreated = true;
    Debug::Log("PbrShadowScene: created %zu stress entities (F11)", m_stressEntities.size());
}

void PbrShadowScene::UpdateStressEntities(ExampleSceneContext& context, float)
{
    const float t = m_rotationYawDeg * 0.02f;
    for (size_t i = 0; i < m_stressEntities.size(); ++i)
    {
        auto* transform = context.world.Get<TransformComponent>(m_stressEntities[i]);
        if (!transform)
            continue;

        const float phase = t + static_cast<float>(i % 31u) * 0.17f;
        const float bob   = 0.18f * std::sin(phase);
        transform->localPosition.y = 0.05f + bob;
        transform->SetEulerDeg(10.0f * std::sin(phase * 0.7f),
                               m_rotationYawDeg + static_cast<float>(i % 97u) * 3.0f,
                               6.0f * std::cos(phase * 0.5f));
    }
}

void PbrShadowScene::LogStressStats(ExampleSceneContext& context, float deltaSeconds)
{
    m_stressStatsAccumulator += deltaSeconds;
    if (m_stressStatsAccumulator < 1.0f)
        return;

    m_stressStatsAccumulator = 0.0f;
    const renderer::RenderStats& stats = context.renderLoop.GetRenderSystem().GetStats();
    const uint32_t workers = context.renderLoop.GetRenderSystem().GetJobWorkerCount();
    const float frameMs = deltaSeconds * 1000.0f;
    Debug::Log("PbrShadowScene: workers=%u peak=%u frame=%.2fms parallel=%.2fms prepare=%.2fms "
               "shaders=%.2fms materials=%.2fms collectUploads=%.2fms commitUploads=%.2fms "
               "buildGraph=%.2fms execute=%.2fms record=%.2fms submit=%.2fms present=%.2fms "
               "backendBegin=%.2fms backendAcquire=%.2fms backendSubmit=%.2fms backendPresent=%.2fms "
               "remat=%u alloc=%u update=%u bind=%u proxies=%u visible=%u opaque=%u shadow=%u "
               "graphPasses=%u uploaded=%lluB",
               workers, stats.peakActiveWorkers, frameMs,
               stats.parallelSectionMs, stats.prepareFrameMs,
               stats.collectShadersMs, stats.collectMaterialsMs,
               stats.collectUploadsMs, stats.commitUploadsMs,
               stats.buildGraphMs, stats.executeMs,
               stats.executeRecordMs, stats.executeSubmitMs, stats.executePresentMs,
               stats.backendBeginFrameMs, stats.backendAcquireMs,
               stats.backendQueueSubmitMs, stats.backendPresentMs,
               stats.backendDescriptorRematerializations,
               stats.backendDescriptorSetAllocations,
               stats.backendDescriptorSetUpdates,
               stats.backendDescriptorSetBinds,
               stats.totalProxyCount, stats.visibleProxyCount,
               stats.opaqueDraws, stats.shadowDraws,
               stats.graphPassCount,
               static_cast<unsigned long long>(stats.uploadedBytes));
}

} // namespace engine::examples
