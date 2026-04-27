#include "PbrShadowScene.hpp"

#include "PbrMaterial.hpp"
#include "addons/camera/CameraComponents.hpp"
#include "addons/lighting/LightingComponents.hpp"
#include "addons/mesh_renderer/MeshRendererComponents.hpp"
#include "assets/MeshTangents.hpp"
#include "core/Debug.hpp"
#include "renderer/Environment.hpp"
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
    ApplyDebugViewMode(context, DebugViewMode::Full);
    return true;
}

bool PbrShadowScene::Update(ExampleSceneContext& context, float deltaSeconds)
{

    if (auto* input = context.renderLoop.GetInput())
    {
        if (input->KeyHit(platform::Key::F1))
            ApplyDebugViewMode(context, DebugViewMode::Full);
        else if (input->KeyHit(platform::Key::F2))
            ApplyDebugViewMode(context, DebugViewMode::NoNormal);
        else if (input->KeyHit(platform::Key::F3))
            ApplyDebugViewMode(context, DebugViewMode::NoIBL);
        else if (input->KeyHit(platform::Key::F4))
            ApplyDebugViewMode(context, DebugViewMode::NoNormalNoIBL);
        else if (input->KeyHit(platform::Key::F5))
            ApplyDebugViewMode(context, DebugViewMode::Prefilter);
        else if (input->KeyHit(platform::Key::F6))
            ApplyDebugViewMode(context, DebugViewMode::SpecularIBL);
        else if (input->KeyHit(platform::Key::F7))
            ApplyDebugViewMode(context, DebugViewMode::DirectSpecular);
        else if (input->KeyHit(platform::Key::F8))
            ApplyDebugViewMode(context, DebugViewMode::FullRawResolve);
        else if (input->KeyHit(platform::Key::F9))
            ApplyDebugViewMode(context, DebugViewMode::ShadowVisibility);
        else if (input->KeyHit(platform::Key::F10))
            ApplyDebugViewMode(context, DebugViewMode::DiffuseIBL);
        else if (input->KeyHit(platform::Key::F11))
        {
            m_stressModeActive = !m_stressModeActive;
            if (m_stressModeActive && !m_stressEntitiesCreated && m_cubeMeshHandle.IsValid())
                CreateStressEntities(context, m_cubeMeshHandle);
            Debug::Log("PbrShadowScene: parallel stress mode %s (F11)", m_stressModeActive ? "ON" : "OFF");
        }
        else if (input->KeyHit(platform::Key::F12))
        {
            m_stressStatsEnabled = !m_stressStatsEnabled;
            m_stressStatsAccumulator = 0.0f;
            Debug::Log("PbrShadowScene: stress stats logging %s (F12)", m_stressStatsEnabled ? "ON" : "OFF");
        }
    }

    m_rotationYawDeg += 50.0f * deltaSeconds;
    m_rotationPitchDeg = 10.0f + 8.0f * std::sin(m_rotationYawDeg * 0.0125f);

    if (auto* transform = context.world.Get<TransformComponent>(m_cubeEntity))
        transform->SetEulerDeg(m_rotationPitchDeg, m_rotationYawDeg, 0.0f);

    if (auto* transform = context.world.Get<TransformComponent>(m_diamondEntity))
        transform->SetEulerDeg(0.0f, m_rotationYawDeg * 0.4f, 0.0f);

    if (auto* transform = context.world.Get<TransformComponent>(m_sphereEntity))
        transform->SetEulerDeg(0.0f, m_rotationYawDeg * 0.4f, 0.0f);

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

    constexpr int kRings = 12;
    constexpr int kSegs  = 16;
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
            sphere.indices.insert(sphere.indices.end(), { a, b, b + 1u, a, b + 1u, a + 1u });
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

    constexpr int   kSides    = 8;
    constexpr float kPi       = 3.14159265358979f;
    constexpr float kGirdleY  = 0.08f;
    constexpr float kGirdleR  = 0.5f;
    constexpr float kApexY    = 0.65f;
    constexpr float kNadirY   = -0.5f;

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

        addTri(apex, g1, g0);   // crown face — outward normal
        addTri(g0, g1, nadir);  // pavilion face — outward normal
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
    floor.indices = {
        0u, 1u, 2u,
        0u, 2u, 3u,
    };

    assets::EnsureTangents(floor);
    meshAsset->submeshes.push_back(std::move(floor));
    return registry.meshes.Add(std::move(meshAsset));
}

renderer::VertexLayout PbrShadowScene::CreatePbrVertexLayout() const
{
    renderer::VertexLayout layout;
    layout.attributes.push_back({ renderer::VertexSemantic::Position,  renderer::Format::RGB32_FLOAT, 0u,  0u });
    layout.attributes.push_back({ renderer::VertexSemantic::Normal,    renderer::Format::RGB32_FLOAT, 0u, 12u });
    layout.attributes.push_back({ renderer::VertexSemantic::Tangent,   renderer::Format::RGBA32_FLOAT, 0u, 24u });
    layout.attributes.push_back({ renderer::VertexSemantic::TexCoord0, renderer::Format::RG32_FLOAT, 0u, 40u });
    layout.bindings.push_back({ 0u, 48u });
    return layout;
}

bool PbrShadowScene::CreateEnvironment(ExampleSceneContext& context) const
{
    renderer::EnvironmentDesc environment{};
    environment.mode = renderer::EnvironmentMode::Texture;
    environment.sourceTexture = context.assetPipeline.LoadTexture("autumn_field_puresky_2k.hdr");
    environment.intensity = 0.5f;
    environment.enableIBL = true;
    if (!environment.sourceTexture.IsValid())
    {
        Debug::LogError("PbrShadowScene: failed to load HDR environment");
        return false;
    }
    const auto environmentHandle = context.renderLoop.GetRenderSystem().CreateEnvironment(environment);
    if (!environmentHandle.IsValid())
    {
        Debug::LogError("PbrShadowScene: failed to create HDR environment");
        return false;
    }
    const_cast<PbrShadowScene*>(this)->m_environmentHandle = environmentHandle;
    context.renderLoop.GetRenderSystem().SetActiveEnvironment(environmentHandle);
    return true;
}

bool PbrShadowScene::CreateMaterials(ExampleSceneContext& context,
                                     const renderer::VertexLayout& vertexLayout)
{
    const char* vertexShaderPath = "pbr_lit.vs.hlsl";
    const char* fragmentShaderPath = "pbr_lit.ps.hlsl";
    const char* prefilterDebugShaderPath = "ibl_prefilter_debug.ps.hlsl";
    const char* specularDebugShaderPath = "ibl_specular_debug.ps.hlsl";
    const char* directSpecularDebugShaderPath = "direct_specular_debug.ps.hlsl";
    const char* shadowVisibilityDebugShaderPath = "shadow_visibility_debug.ps.hlsl";
    const char* diffuseIblDebugShaderPath = "ibl_diffuse_debug.ps.hlsl";
    const char* shadowShaderPath = "shadow.vs.hlsl";
#if defined(KROM_EXAMPLE_BACKEND_OPENGL)
    vertexShaderPath = "pbr_lit.opengl.vs.glsl";
    fragmentShaderPath = "pbr_lit.opengl.fs.glsl";
    prefilterDebugShaderPath = "ibl_prefilter_debug.opengl.fs.glsl";
    specularDebugShaderPath = "ibl_specular_debug.opengl.fs.glsl";
    directSpecularDebugShaderPath = "direct_specular_debug.opengl.fs.glsl";
    shadowVisibilityDebugShaderPath = "shadow_visibility_debug.opengl.fs.glsl";
    diffuseIblDebugShaderPath = "ibl_diffuse_debug.opengl.fs.glsl";
    shadowShaderPath = "shadow.opengl.vs.glsl";
#endif

    const ShaderHandle vertexShader = context.assetPipeline.LoadShader(vertexShaderPath, assets::ShaderStage::Vertex);
    const ShaderHandle fragmentShader = context.assetPipeline.LoadShader(fragmentShaderPath, assets::ShaderStage::Fragment);
    const ShaderHandle prefilterDebugShader = context.assetPipeline.LoadShader(prefilterDebugShaderPath, assets::ShaderStage::Fragment);
    const ShaderHandle specularDebugShader = context.assetPipeline.LoadShader(specularDebugShaderPath, assets::ShaderStage::Fragment);
    const ShaderHandle directSpecularDebugShader = context.assetPipeline.LoadShader(directSpecularDebugShaderPath, assets::ShaderStage::Fragment);
    const ShaderHandle shadowVisibilityDebugShader = context.assetPipeline.LoadShader(shadowVisibilityDebugShaderPath, assets::ShaderStage::Fragment);
    const ShaderHandle diffuseIblDebugShader = context.assetPipeline.LoadShader(diffuseIblDebugShaderPath, assets::ShaderStage::Fragment);
    const ShaderHandle shadowShader = context.assetPipeline.LoadShader(shadowShaderPath, assets::ShaderStage::Vertex);
    if (!vertexShader.IsValid() || !fragmentShader.IsValid() || !prefilterDebugShader.IsValid() || !specularDebugShader.IsValid() || !directSpecularDebugShader.IsValid() || !shadowVisibilityDebugShader.IsValid() || !diffuseIblDebugShader.IsValid() || !shadowShader.IsValid())
    {
        Debug::LogError("PbrShadowScene: failed to load PBR shaders");
        return false;
    }

    const char* tonemapVsPath = "fullscreen.vs.hlsl";
    const char* tonemapPsPath = "passthrough.ps.hlsl";
    const char* tonemapRawPsPath = "passthrough_raw.ps.hlsl";
#if defined(KROM_EXAMPLE_BACKEND_OPENGL)
    tonemapVsPath = "fullscreen.opengl.vs.glsl";
    tonemapPsPath = "passthrough.opengl.fs.glsl";
    tonemapRawPsPath = "passthrough_raw.opengl.fs.glsl";
#endif
    const ShaderHandle tonemapVs = context.assetPipeline.LoadShader(tonemapVsPath, assets::ShaderStage::Vertex);
    const ShaderHandle tonemapPs = context.assetPipeline.LoadShader(tonemapPsPath, assets::ShaderStage::Fragment);
    const ShaderHandle tonemapRawPs = context.assetPipeline.LoadShader(tonemapRawPsPath, assets::ShaderStage::Fragment);
    if (!tonemapVs.IsValid() || !tonemapPs.IsValid() || !tonemapRawPs.IsValid())
    {
        Debug::LogError("PbrShadowScene: failed to load raw tonemap shaders");
        return false;
    }

    const TextureHandle cubeAlbedo = context.assetPipeline.LoadTexture("checkered_pavement_tiles_diff_2k.png"); 
    const TextureHandle cubeNormal = context.assetPipeline.LoadTexture("checkered_pavement_tiles_nor_dx_2k.png");
    const TextureHandle floorAlbedo = context.assetPipeline.LoadTexture("cobblestone_floor_09_diff_2k.png");
    const TextureHandle floorNormal = context.assetPipeline.LoadTexture("cobblestone_floor_09_nor_dx_2k.png");

    auto configureAlbedo = [&](TextureHandle handle)
    {
        if (auto* texture = context.assetRegistry.textures.Get(handle))
        {
            texture->metadata.semantic = assets::TextureSemantic::Color;
            texture->metadata.normalEncoding = assets::NormalEncoding::None;
            texture->metadata.colorSpace = assets::ColorSpace::SRGB;
            texture->gpuStatus.dirty = true;
        }
    };
    auto configureNormal = [&](TextureHandle handle)
    {
        if (auto* texture = context.assetRegistry.textures.Get(handle))
        {
            texture->metadata.semantic = assets::TextureSemantic::Normal;
            texture->metadata.normalEncoding = assets::NormalEncoding::RGB;
            texture->metadata.colorSpace = assets::ColorSpace::Linear;
            texture->gpuStatus.dirty = true;
        }
    };

    configureAlbedo(cubeAlbedo);
    configureNormal(cubeNormal);
    configureAlbedo(floorAlbedo);
    configureNormal(floorNormal);

#if !defined(KROM_EXAMPLE_BACKEND_DX11)
    // The shipped *_nor_dx_* textures use DirectX-style tangent-space Y.
    // Non-DX backends need the green channel flipped to keep the normal orientation stable.
    FlipNormalMapGreenChannel(cubeNormal, context.assetRegistry);
    FlipNormalMapGreenChannel(floorNormal, context.assetRegistry);
#endif

    context.assetPipeline.UploadPendingGpuAssets();

    const TextureHandle gpuCubeAlbedo = context.assetPipeline.GetGpuTexture(cubeAlbedo);
    const TextureHandle gpuCubeNormal = context.assetPipeline.GetGpuTexture(cubeNormal);
    const TextureHandle gpuFloorAlbedo = context.assetPipeline.GetGpuTexture(floorAlbedo);
    const TextureHandle gpuFloorNormal = context.assetPipeline.GetGpuTexture(floorNormal);
    if (!gpuCubeAlbedo.IsValid() || !gpuCubeNormal.IsValid() || !gpuFloorAlbedo.IsValid() || !gpuFloorNormal.IsValid())
    {
        Debug::LogError("PbrShadowScene: texture upload failed");
        return false;
    }

    renderer::pbr::PbrMaterialCreateInfo cubeInfo{};
    cubeInfo.name = "PbrShadowCube";
    cubeInfo.vertexShader = vertexShader;
    cubeInfo.fragmentShader = fragmentShader;
    cubeInfo.shadowShader = shadowShader;
    cubeInfo.vertexLayout = vertexLayout;
    cubeInfo.colorFormat = renderer::Format::RGBA16_FLOAT;
    cubeInfo.depthFormat = renderer::Format::D24_UNORM_S8_UINT;
    cubeInfo.enableBaseColorMap = true;
    cubeInfo.enableNormalMap = true;
    cubeInfo.enableORMMap = false;
    cubeInfo.metallicFactor = 0.5f;
    cubeInfo.roughnessFactor = 0.5f;
    cubeInfo.normalStrength = 1.0f;
    cubeInfo.castShadows = true;
    cubeInfo.receiveShadows = true;
    renderer::pbr::PbrMaterial cubeMaterialNormal = renderer::pbr::PbrMaterial::Create(context.materialSystem, cubeInfo);
    if (!cubeMaterialNormal.IsValid() || !cubeMaterialNormal.SetAlbedo(gpuCubeAlbedo) || !cubeMaterialNormal.SetNormal(gpuCubeNormal))
    {
        Debug::LogError("PbrShadowScene: failed to create cube material with normal map");
        return false;
    }
    cubeInfo.name = "PbrShadowCubeFlat";
    cubeInfo.enableNormalMap = false;
    renderer::pbr::PbrMaterial cubeMaterialFlat = renderer::pbr::PbrMaterial::Create(context.materialSystem, cubeInfo);
    if (!cubeMaterialFlat.IsValid() || !cubeMaterialFlat.SetAlbedo(gpuCubeAlbedo))
    {
        Debug::LogError("PbrShadowScene: failed to create cube material without normal map");
        return false;
    }
    cubeInfo.name = "PbrShadowCubePrefilter";
    cubeInfo.fragmentShader = prefilterDebugShader;
    cubeInfo.enableNormalMap = true;
    cubeInfo.enableORMMap = false;
    cubeInfo.enableEmissiveMap = false;
    cubeInfo.enableIBL = true;
    renderer::pbr::PbrMaterial cubeMaterialPrefilter = renderer::pbr::PbrMaterial::Create(context.materialSystem, cubeInfo);
    if (!cubeMaterialPrefilter.IsValid() || !cubeMaterialPrefilter.SetNormal(gpuCubeNormal))
    {
        Debug::LogError("PbrShadowScene: failed to create cube prefilter debug material");
        return false;
    }
    cubeInfo.name = "PbrShadowCubeSpecularIBL";
    cubeInfo.fragmentShader = specularDebugShader;
    renderer::pbr::PbrMaterial cubeMaterialSpecularIBL = renderer::pbr::PbrMaterial::Create(context.materialSystem, cubeInfo);
    if (!cubeMaterialSpecularIBL.IsValid() || !cubeMaterialSpecularIBL.SetNormal(gpuCubeNormal))
    {
        Debug::LogError("PbrShadowScene: failed to create cube specular IBL debug material");
        return false;
    }
    cubeInfo.name = "PbrShadowCubeDirectSpecular";
    cubeInfo.fragmentShader = directSpecularDebugShader;
    renderer::pbr::PbrMaterial cubeMaterialDirectSpecular = renderer::pbr::PbrMaterial::Create(context.materialSystem, cubeInfo);
    if (!cubeMaterialDirectSpecular.IsValid() || !cubeMaterialDirectSpecular.SetNormal(gpuCubeNormal))
    {
        Debug::LogError("PbrShadowScene: failed to create cube direct specular debug material");
        return false;
    }
    cubeInfo.name = "PbrShadowCubeShadowVisibility";
    cubeInfo.fragmentShader = shadowVisibilityDebugShader;
    cubeInfo.enableIBL = false;
    renderer::pbr::PbrMaterial cubeMaterialShadowVisibility = renderer::pbr::PbrMaterial::Create(context.materialSystem, cubeInfo);
    if (!cubeMaterialShadowVisibility.IsValid() || !cubeMaterialShadowVisibility.SetNormal(gpuCubeNormal))
    {
        Debug::LogError("PbrShadowScene: failed to create cube shadow visibility material");
        return false;
    }
    cubeInfo.name = "PbrShadowCubeDiffuseIBL";
    cubeInfo.fragmentShader = diffuseIblDebugShader;
    cubeInfo.enableIBL = true;
    renderer::pbr::PbrMaterial cubeMaterialDiffuseIBL = renderer::pbr::PbrMaterial::Create(context.materialSystem, cubeInfo);
    if (!cubeMaterialDiffuseIBL.IsValid() || !cubeMaterialDiffuseIBL.SetNormal(gpuCubeNormal))
    {
        Debug::LogError("PbrShadowScene: failed to create cube diffuse IBL debug material");
        return false;
    }

    renderer::pbr::PbrMaterialCreateInfo floorInfo = cubeInfo;
    floorInfo.name = "PbrShadowFloor";
    floorInfo.fragmentShader = fragmentShader;
    floorInfo.enableNormalMap = true;
    floorInfo.metallicFactor = 0.0f;
    floorInfo.roughnessFactor = 0.9f;
    floorInfo.normalStrength = 2.0f;
    floorInfo.castShadows = false;
    floorInfo.receiveShadows = true;
    renderer::pbr::PbrMaterial floorMaterialNormal = renderer::pbr::PbrMaterial::Create(context.materialSystem, floorInfo);
    if (!floorMaterialNormal.IsValid() || !floorMaterialNormal.SetAlbedo(gpuFloorAlbedo) || !floorMaterialNormal.SetNormal(gpuFloorNormal))
    {
        Debug::LogError("PbrShadowScene: failed to create floor material with normal map");
        return false;
    }
    floorInfo.name = "PbrShadowFloorFlat";
    floorInfo.enableNormalMap = false;
    renderer::pbr::PbrMaterial floorMaterialFlat = renderer::pbr::PbrMaterial::Create(context.materialSystem, floorInfo);
    if (!floorMaterialFlat.IsValid() || !floorMaterialFlat.SetAlbedo(gpuFloorAlbedo))
    {
        Debug::LogError("PbrShadowScene: failed to create floor material without normal map");
        return false;
    }
    floorInfo.name = "PbrShadowFloorPrefilter";
    floorInfo.fragmentShader = prefilterDebugShader;
    floorInfo.enableNormalMap = true;
    floorInfo.enableORMMap = false;
    floorInfo.enableEmissiveMap = false;
    floorInfo.enableIBL = true;
    renderer::pbr::PbrMaterial floorMaterialPrefilter = renderer::pbr::PbrMaterial::Create(context.materialSystem, floorInfo);
    if (!floorMaterialPrefilter.IsValid() || !floorMaterialPrefilter.SetNormal(gpuFloorNormal))
    {
        Debug::LogError("PbrShadowScene: failed to create floor prefilter debug material");
        return false;
    }
    floorInfo.name = "PbrShadowFloorSpecularIBL";
    floorInfo.fragmentShader = specularDebugShader;
    renderer::pbr::PbrMaterial floorMaterialSpecularIBL = renderer::pbr::PbrMaterial::Create(context.materialSystem, floorInfo);
    if (!floorMaterialSpecularIBL.IsValid() || !floorMaterialSpecularIBL.SetNormal(gpuFloorNormal))
    {
        Debug::LogError("PbrShadowScene: failed to create floor specular IBL debug material");
        return false;
    }
    floorInfo.name = "PbrShadowFloorDirectSpecular";
    floorInfo.fragmentShader = directSpecularDebugShader;
    renderer::pbr::PbrMaterial floorMaterialDirectSpecular = renderer::pbr::PbrMaterial::Create(context.materialSystem, floorInfo);
    if (!floorMaterialDirectSpecular.IsValid() || !floorMaterialDirectSpecular.SetNormal(gpuFloorNormal))
    {
        Debug::LogError("PbrShadowScene: failed to create floor direct specular debug material");
        return false;
    }
    floorInfo.name = "PbrShadowFloorShadowVisibility";
    floorInfo.fragmentShader = shadowVisibilityDebugShader;
    floorInfo.enableIBL = false;
    renderer::pbr::PbrMaterial floorMaterialShadowVisibility = renderer::pbr::PbrMaterial::Create(context.materialSystem, floorInfo);
    if (!floorMaterialShadowVisibility.IsValid() || !floorMaterialShadowVisibility.SetNormal(gpuFloorNormal))
    {
        Debug::LogError("PbrShadowScene: failed to create floor shadow visibility material");
        return false;
    }
    floorInfo.name = "PbrShadowFloorDiffuseIBL";
    floorInfo.fragmentShader = diffuseIblDebugShader;
    floorInfo.enableIBL = true;
    renderer::pbr::PbrMaterial floorMaterialDiffuseIBL = renderer::pbr::PbrMaterial::Create(context.materialSystem, floorInfo);
    if (!floorMaterialDiffuseIBL.IsValid() || !floorMaterialDiffuseIBL.SetNormal(gpuFloorNormal))
    {
        Debug::LogError("PbrShadowScene: failed to create floor diffuse IBL debug material");
        return false;
    }

    m_cubeMaterialNormal = cubeMaterialNormal.Handle();
    m_cubeMaterialFlat = cubeMaterialFlat.Handle();
    m_cubeMaterialPrefilter = cubeMaterialPrefilter.Handle();
    m_cubeMaterialSpecularIBL = cubeMaterialSpecularIBL.Handle();
    m_cubeMaterialDirectSpecular = cubeMaterialDirectSpecular.Handle();
    m_cubeMaterialShadowVisibility = cubeMaterialShadowVisibility.Handle();
    m_cubeMaterialDiffuseIBL = cubeMaterialDiffuseIBL.Handle();
    m_floorMaterialNormal = floorMaterialNormal.Handle();
    m_floorMaterialFlat = floorMaterialFlat.Handle();
    m_floorMaterialPrefilter = floorMaterialPrefilter.Handle();
    m_floorMaterialSpecularIBL = floorMaterialSpecularIBL.Handle();
    m_floorMaterialDirectSpecular = floorMaterialDirectSpecular.Handle();
    m_floorMaterialShadowVisibility = floorMaterialShadowVisibility.Handle();
    m_floorMaterialDiffuseIBL = floorMaterialDiffuseIBL.Handle();

    renderer::DepthStencilState noDepth{};
    noDepth.depthEnable = false;
    noDepth.depthWrite = false;

    renderer::MaterialParam tonemapSampler{};
    tonemapSampler.name = "linearclamp";
    tonemapSampler.type = renderer::MaterialParam::Type::Sampler;
    tonemapSampler.samplerIdx = 0u;

    const renderer::ISwapchain* swapchain = context.renderLoop.GetRenderSystem().GetSwapchain();
    const renderer::Format backbufferFormat = swapchain ? swapchain->GetBackbufferFormat()
                                                        : renderer::Format::BGRA8_UNORM_SRGB;

    renderer::MaterialDesc tonemapDesc{};
    tonemapDesc.name = "PbrShadowTonemap";
    tonemapDesc.renderPass = renderer::StandardRenderPasses::Opaque();
    tonemapDesc.vertexShader = tonemapVs;
    tonemapDesc.fragmentShader = tonemapPs;
    tonemapDesc.depthStencil = noDepth;
    tonemapDesc.colorFormat = backbufferFormat;
    tonemapDesc.depthFormat = renderer::Format::Unknown;
    tonemapDesc.params.push_back(tonemapSampler);
    m_tonemapMaterialDefault = context.materialSystem.RegisterMaterial(std::move(tonemapDesc));

    renderer::MaterialDesc tonemapRawDesc{};
    tonemapRawDesc.name = "PbrShadowRawResolve";
    tonemapRawDesc.renderPass = renderer::StandardRenderPasses::Opaque();
    tonemapRawDesc.vertexShader = tonemapVs;
    tonemapRawDesc.fragmentShader = tonemapRawPs;
    tonemapRawDesc.depthStencil = noDepth;
    tonemapRawDesc.colorFormat = backbufferFormat;
    tonemapRawDesc.depthFormat = renderer::Format::Unknown;
    tonemapRawDesc.params.push_back(tonemapSampler);
    m_tonemapMaterialRaw = context.materialSystem.RegisterMaterial(std::move(tonemapRawDesc));
    if (!m_tonemapMaterialDefault.IsValid() || !m_tonemapMaterialRaw.IsValid())
    {
        Debug::LogError("PbrShadowScene: failed to create resolve materials");
        return false;
    }
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
    context.world.Add<MaterialComponent>(m_cubeEntity, m_cubeMaterialNormal);
    context.world.Add<BoundsComponent>(m_cubeEntity, BoundsComponent{
        .centerLocal = { 0.f, 0.f, 0.f },
        .extentsLocal = { 0.5f, 0.5f, 0.5f },
        .centerWorld = { 0.f, 0.05f, 0.f },
        .extentsWorld = { 0.5f, 0.5f, 0.5f },
        .boundingSphere = 0.8660254f,
        .localDirty = true,
    });
    if (auto* cubeTransform = context.world.Get<TransformComponent>(m_cubeEntity))
    {
        cubeTransform->localPosition = { 0.f, 0.05f, 0.f };
        cubeTransform->localScale = { 1.f, 1.f, 1.f };
        cubeTransform->SetEulerDeg(m_rotationPitchDeg, m_rotationYawDeg, 0.f);
    }

    // ---- Sphere (left) ----
    m_sphereEntity = context.world.CreateEntity();
    context.world.Add<TransformComponent>(m_sphereEntity);
    context.world.Add<WorldTransformComponent>(m_sphereEntity);
    context.world.Add<MeshComponent>(m_sphereEntity, sphereMesh);
    context.world.Add<MaterialComponent>(m_sphereEntity, m_cubeMaterialNormal);
    context.world.Add<BoundsComponent>(m_sphereEntity, BoundsComponent{
        .centerLocal = { 0.f, 0.f, 0.f },
        .extentsLocal = { 0.5f, 0.5f, 0.5f },
        .centerWorld = { -2.2f, 0.05f, 0.f },
        .extentsWorld = { 0.5f, 0.5f, 0.5f },
        .boundingSphere = 0.5f,
        .localDirty = true,
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
    context.world.Add<MaterialComponent>(m_diamondEntity, m_cubeMaterialNormal);
    context.world.Add<BoundsComponent>(m_diamondEntity, BoundsComponent{
        .centerLocal = { 0.f, 0.075f, 0.f },
        .extentsLocal = { 0.5f, 0.575f, 0.5f },
        .centerWorld = { 2.2f, 0.075f, 0.f },
        .extentsWorld = { 0.5f, 0.575f, 0.5f },
        .boundingSphere = 0.79f,
        .localDirty = true,
    });
    if (auto* t = context.world.Get<TransformComponent>(m_diamondEntity))
    {
        t->localPosition = { 2.2f, 0.05f, 0.f };
        t->localScale    = { 1.f, 1.f, 1.f };
        t->SetEulerDeg(0.f, 0.f, 0.f);
    }

    m_floorEntity = context.world.CreateEntity();
    context.world.Add<TransformComponent>(m_floorEntity);
    context.world.Add<WorldTransformComponent>(m_floorEntity);
    context.world.Add<MeshComponent>(m_floorEntity, floorMesh);
    if (auto* floorMeshComponent = context.world.Get<MeshComponent>(m_floorEntity))
        floorMeshComponent->castShadows = false;
    context.world.Add<MaterialComponent>(m_floorEntity, m_floorMaterialNormal);
    context.world.Add<BoundsComponent>(m_floorEntity, BoundsComponent{
        .centerLocal = { 0.f, 0.f, 0.f },
        .extentsLocal = { 4.f, 0.05f, 4.f },
        .centerWorld = { 0.f, -0.75f, 0.f },
        .extentsWorld = { 4.f, 0.05f, 4.f },
        .boundingSphere = 5.657f,
        .localDirty = true,
    });
    if (auto* floorTransform = context.world.Get<TransformComponent>(m_floorEntity))
    {
        floorTransform->localPosition = { 0.f, -0.75f, 0.f };
        floorTransform->localScale = { 1.f, 1.f, 1.f };
        floorTransform->SetEulerDeg(0.f, 0.f, 0.f);
    }

    const EntityID lightEntity = context.world.CreateEntity();
    auto& lightTransform = context.world.Add<TransformComponent>(lightEntity);
    lightTransform.localPosition = { 4.f, 5.5f, 3.5f };
    lightTransform.localScale = { 1.f, 1.f, 1.f };
    lightTransform.SetEulerDeg(-48.f, 135.f, 0.f);
    context.world.Add<WorldTransformComponent>(lightEntity);

    LightComponent light{};
    light.type = LightType::Directional;
    light.color = { 1.0f, 1.0f, 1.0f };
    light.intensity = 1.0f;
    light.castShadows = true;
    light.shadowSettings.enabled = true;
    light.shadowSettings.type = ShadowType::PCF;
    light.shadowSettings.filter = ShadowFilter::PCF3x3;
    light.shadowSettings.resolution = 4096u;
    light.shadowSettings.bias = 0.00015f;
    light.shadowSettings.normalBias = 0.0008f;
    light.shadowSettings.maxDistance = 18.0f;
    light.shadowSettings.strength = 1.0f;
    context.world.Add<LightComponent>(lightEntity, light);

    const EntityID cameraEntity = context.world.CreateEntity();
    auto& cameraTransform = context.world.Add<TransformComponent>(cameraEntity);
    cameraTransform.localPosition = { 0.0f, 1.4f, 4.6f };
    cameraTransform.localScale = { 1.f, 1.f, 1.f };
    cameraTransform.SetEulerDeg(-12.f, 0.f, 0.f);
    context.world.Add<WorldTransformComponent>(cameraEntity);
    context.world.Add<CameraComponent>(cameraEntity, CameraComponent{
        .projection = ProjectionType::Perspective,
        .fovYDeg = 55.f,
        .nearPlane = 0.1f,
        .farPlane = 100.f,
        .isMainCamera = true,
    });
}

void PbrShadowScene::CreateStressEntities(ExampleSceneContext& context, MeshHandle cubeMesh)
{
    if (m_stressEntitiesCreated)
        return;

    constexpr int kGridX = 24;
    constexpr int kGridZ = 24;
    constexpr float kSpacing = 1.35f;
    constexpr float kBaseY = 0.05f;

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
            context.world.Add<MaterialComponent>(entity, ((x + z) & 1) == 0 ? m_cubeMaterialNormal : m_cubeMaterialFlat);
            context.world.Add<BoundsComponent>(entity, BoundsComponent{
                .centerLocal = { 0.f, 0.f, 0.f },
                .extentsLocal = { 0.5f, 0.5f, 0.5f },
                .centerWorld = { px, kBaseY, pz },
                .extentsWorld = { 0.5f, 0.5f, 0.5f },
                .boundingSphere = 0.8660254f,
                .localDirty = true,
            });

            if (auto* transform = context.world.Get<TransformComponent>(entity))
            {
                transform->localPosition = { px, kBaseY, pz };
                transform->localScale = { 0.65f, 0.65f, 0.65f };
                transform->SetEulerDeg(0.f, static_cast<float>((x * 17 + z * 11) % 360), 0.f);
            }

            m_stressEntities.push_back(entity);
        }
    }

    m_stressEntitiesCreated = true;
    Debug::Log("PbrShadowScene: created %zu stress entities for parallel render load", m_stressEntities.size());
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
        const float bob = 0.18f * std::sin(phase);
        const math::Vec3 base = { transform->localPosition.x, 0.05f, transform->localPosition.z };
        transform->localPosition.y = base.y + bob;
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
    Debug::Log("PbrShadowScene: stress stats workers=%u peak=%u frame=%.2fms parallel=%.2fms prepare=%.2fms shaders=%.2fms materials=%.2fms collectUploads=%.2fms commitUploads=%.2fms buildGraph=%.2fms execute=%.2fms record=%.2fms submit=%.2fms present=%.2fms backendBegin=%.2fms backendAcquire=%.2fms backendSubmit=%.2fms backendPresent=%.2fms remat=%u alloc=%u update=%u bind=%u proxies=%u visible=%u opaque=%u shadow=%u graphPasses=%u uploaded=%lluB",
               workers,
               stats.peakActiveWorkers,
               frameMs,
               stats.parallelSectionMs,
               stats.prepareFrameMs,
               stats.collectShadersMs,
               stats.collectMaterialsMs,
               stats.collectUploadsMs,
               stats.commitUploadsMs,
               stats.buildGraphMs,
               stats.executeMs,
               stats.executeRecordMs,
               stats.executeSubmitMs,
               stats.executePresentMs,
               stats.backendBeginFrameMs,
               stats.backendAcquireMs,
               stats.backendQueueSubmitMs,
               stats.backendPresentMs,
               stats.backendDescriptorRematerializations,
               stats.backendDescriptorSetAllocations,
               stats.backendDescriptorSetUpdates,
               stats.backendDescriptorSetBinds,
               stats.totalProxyCount,
               stats.visibleProxyCount,
               stats.opaqueDraws,
               stats.shadowDraws,
               stats.graphPassCount,
               static_cast<unsigned long long>(stats.uploadedBytes));
}

void PbrShadowScene::ApplyDebugViewMode(ExampleSceneContext& context, DebugViewMode mode)
{
    m_debugViewMode = mode;

    const bool useNormalMaps = mode == DebugViewMode::Full
                            || mode == DebugViewMode::NoIBL
                            || mode == DebugViewMode::FullRawResolve;
    const bool useIBL = mode == DebugViewMode::Full
                     || mode == DebugViewMode::NoNormal
                     || mode == DebugViewMode::FullRawResolve;
    const bool showPrefilter = mode == DebugViewMode::Prefilter;
    const bool showSpecularIBL = mode == DebugViewMode::SpecularIBL;
    const bool showDirectSpecular = mode == DebugViewMode::DirectSpecular;
    const bool useRawResolve = mode == DebugViewMode::FullRawResolve;
    const bool showShadowVisibility = mode == DebugViewMode::ShadowVisibility;
    const bool showDiffuseIBL = mode == DebugViewMode::DiffuseIBL;

    const auto selectCubeMat = [&]() -> MaterialHandle {
        return showPrefilter       ? m_cubeMaterialPrefilter
             : showSpecularIBL     ? m_cubeMaterialSpecularIBL
             : showDirectSpecular  ? m_cubeMaterialDirectSpecular
             : showShadowVisibility? m_cubeMaterialShadowVisibility
             : showDiffuseIBL      ? m_cubeMaterialDiffuseIBL
             : (useNormalMaps      ? m_cubeMaterialNormal : m_cubeMaterialFlat);
    };

    if (auto* cubeMaterial = context.world.Get<MaterialComponent>(m_cubeEntity))
        cubeMaterial->material = selectCubeMat();
    if (auto* mat = context.world.Get<MaterialComponent>(m_sphereEntity))
        mat->material = selectCubeMat();
    if (auto* mat = context.world.Get<MaterialComponent>(m_diamondEntity))
        mat->material = selectCubeMat();
    if (auto* floorMaterial = context.world.Get<MaterialComponent>(m_floorEntity))
        floorMaterial->material = showPrefilter ? m_floorMaterialPrefilter
                                : showSpecularIBL ? m_floorMaterialSpecularIBL
                                : showDirectSpecular ? m_floorMaterialDirectSpecular
                                : showShadowVisibility ? m_floorMaterialShadowVisibility
                                : showDiffuseIBL ? m_floorMaterialDiffuseIBL
                                : (useNormalMaps ? m_floorMaterialNormal : m_floorMaterialFlat);

    context.renderLoop.GetRenderSystem().SetActiveEnvironment(
        (useIBL || showPrefilter || showSpecularIBL || showDiffuseIBL) ? m_environmentHandle : renderer::EnvironmentHandle::Invalid());

    context.renderLoop.GetRenderSystem().SetDefaultTonemapMaterial(
        useRawResolve ? m_tonemapMaterialRaw : m_tonemapMaterialDefault,
        context.materialSystem);

    const char* label = "Full";
    switch (mode)
    {
    case DebugViewMode::Full: label = "Full (Normal + IBL)"; break;
    case DebugViewMode::NoNormal: label = "No Normalmap"; break;
    case DebugViewMode::NoIBL: label = "No IBL"; break;
    case DebugViewMode::NoNormalNoIBL: label = "No Normalmap + No IBL"; break;
    case DebugViewMode::Prefilter: label = "Prefilter Debug"; break;
    case DebugViewMode::SpecularIBL: label = "Specular IBL Debug"; break;
    case DebugViewMode::DirectSpecular: label = "Direct Specular Debug"; break;
    case DebugViewMode::FullRawResolve: label = "Full + Raw Resolve"; break;
    case DebugViewMode::ShadowVisibility: label = "Shadow Visibility Debug"; break;
    case DebugViewMode::DiffuseIBL: label = "Diffuse IBL Debug"; break;
    }
    Debug::Log("PbrShadowScene: debug mode = %s (F1..F10)", label);
}

} // namespace engine::examples
