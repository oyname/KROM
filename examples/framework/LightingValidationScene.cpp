#include "LightingValidationScene.hpp"

#include "PbrInstanceBuilder.hpp"
#include "addons/camera/CameraComponents.hpp"
#include "addons/lighting/LightingComponents.hpp"
#include "addons/mesh_renderer/MeshRendererComponents.hpp"
#include "assets/MeshTangents.hpp"
#include "core/Debug.hpp"
#include "platform/IInput.hpp"
#include "renderer/RenderWorld.hpp"
#include <algorithm>
#include <cmath>
#include <memory>

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

math::Vec4 GridRowBaseColor(uint32_t row, uint32_t rowCount)
{
    if (rowCount <= 1u)
        return {0.18f, 0.18f, 0.18f, 1.0f};

    const float t = static_cast<float>(row) / static_cast<float>(rowCount - 1u);
    if (t < 0.34f)
        return {0.18f, 0.18f, 0.18f, 1.0f};
    if (t < 0.67f)
        return {0.90f, 0.90f, 0.90f, 1.0f};
    return {0.95f, 0.64f, 0.54f, 1.0f};
}

void ApplyPoseConfig(TransformComponent& transform,
                     const Vec3& position,
                     const Vec3& eulerDeg) noexcept
{
    transform.localPosition = position;
    transform.SetEulerDeg(eulerDeg.x, eulerDeg.y, eulerDeg.z);
}

} // namespace

bool LightingValidationScene::Build(ExampleSceneContext& context)
{
    context.renderLoop.GetRenderSystem().SetActiveEnvironment(renderer::EnvironmentHandle::Invalid());

    const renderer::VertexLayout vertexLayout = CreatePbrVertexLayout();
    if (!CreateMaterials(context, vertexLayout))
        return false;

    const MeshHandle sphereMesh = CreateSphereMesh(context.assetRegistry);
    const MeshHandle planeMesh  = CreatePlaneMesh(context.assetRegistry);
    if (!sphereMesh.IsValid() || !planeMesh.IsValid())
    {
        Debug::LogError("LightingValidationScene: failed to create procedural meshes");
        return false;
    }

    BuildBrdfGrid(context, sphereMesh, planeMesh);
    BuildLightTypeTest(context, sphereMesh, planeMesh);
    BuildNormalResponseTest(context, sphereMesh, planeMesh);
    BuildLights(context);
    BuildCamera(context);

    SetSceneMode(SceneMode::LightTypes, context);
    SetLightMode(LightMode::All, context);

    Debug::Log("LightingValidationScene: ready");
    Debug::Log("  F1 clear, F2 normals, F3 NdotL, F4 roughness, F5 metallic, F6 shadow");
    Debug::Log("  F7 direct diffuse, F8 direct specular, F9 Fresnel F0");
    Debug::Log("  Num1 IBL toggle, Num2 shadow toggle, Num3 AO toggle, Num4 normal-map toggle");
    Debug::Log("  Num5 BRDF grid, Num6 light types, Num7 normal response");
    Debug::Log("  Q directional, E point, R spot, T all lights");
    return true;
}

bool LightingValidationScene::Update(ExampleSceneContext& context, float deltaSeconds)
{
    if (auto* input = context.renderLoop.GetInput())
    {
        struct { platform::Key key; uint32_t flags; const char* label; } viewKeys[] = {
            { platform::Key::F1, 0u,                              "Full"        },
            { platform::Key::F2, renderer::DBG_VIEW_NORMALS,      "Normals"     },
            { platform::Key::F3, renderer::DBG_VIEW_NOL,          "NdotL"       },
            { platform::Key::F4, renderer::DBG_VIEW_ROUGHNESS,    "Roughness"   },
            { platform::Key::F5, renderer::DBG_VIEW_METALLIC,     "Metallic"    },
            { platform::Key::F6, renderer::DBG_VIEW_SHADOW,       "Shadow"      },
            { platform::Key::F7, renderer::DBG_VIEW_DIRECT_DIFF,  "DirectDiff"  },
            { platform::Key::F8, renderer::DBG_VIEW_DIRECT_SPEC,  "DirectSpec"  },
            { platform::Key::F9, renderer::DBG_VIEW_FRESNEL_F0,   "F0"          },
        };

        static constexpr uint32_t kViewMask =
            renderer::DBG_VIEW_NORMALS |
            renderer::DBG_VIEW_NOL |
            renderer::DBG_VIEW_ROUGHNESS |
            renderer::DBG_VIEW_METALLIC |
            renderer::DBG_VIEW_AO |
            renderer::DBG_VIEW_SHADOW |
            renderer::DBG_VIEW_DIRECT_DIFF |
            renderer::DBG_VIEW_DIRECT_SPEC |
            renderer::DBG_VIEW_IBL_DIFF |
            renderer::DBG_VIEW_IBL_SPEC |
            renderer::DBG_VIEW_FRESNEL_F0;

        for (const auto& entry : viewKeys)
        {
            if (input->KeyHit(entry.key))
            {
                m_debugFlags = (m_debugFlags & ~kViewMask) | entry.flags;
                Debug::Log("LightingValidationScene: debug view -> %s", entry.label);
            }
        }

        struct { platform::Key key; uint32_t flag; const char* label; } toggleKeys[] = {
            { platform::Key::Num1, renderer::DBG_DISABLE_IBL,       "IBL"       },
            { platform::Key::Num2, renderer::DBG_DISABLE_SHADOWS,   "Shadows"   },
            { platform::Key::Num3, renderer::DBG_DISABLE_AO,        "AO"        },
            { platform::Key::Num4, renderer::DBG_DISABLE_NORMALMAP, "NormalMap" },
        };
        for (const auto& entry : toggleKeys)
        {
            if (input->KeyHit(entry.key))
            {
                m_debugFlags ^= entry.flag;
                Debug::Log("LightingValidationScene: disable %s -> %s",
                           entry.label,
                           (m_debugFlags & entry.flag) ? "ON" : "OFF");
            }
        }

        if (input->KeyHit(platform::Key::Num5))
            SetSceneMode(SceneMode::BrdfGrid, context);
        if (input->KeyHit(platform::Key::Num6))
            SetSceneMode(SceneMode::LightTypes, context);
        if (input->KeyHit(platform::Key::Num7))
            SetSceneMode(SceneMode::NormalResponse, context);

        if (input->KeyHit(platform::Key::Q))
            SetLightMode(LightMode::Directional, context);
        if (input->KeyHit(platform::Key::E))
            SetLightMode(LightMode::Point, context);
        if (input->KeyHit(platform::Key::R))
            SetLightMode(LightMode::Spot, context);
        if (input->KeyHit(platform::Key::T))
            SetLightMode(LightMode::All, context);

        constexpr float kCamSpeedDeg = 90.0f;
        if (input->KeyDown(platform::Key::Left))
            m_cameraYawDeg += kCamSpeedDeg * deltaSeconds;
        if (input->KeyDown(platform::Key::Right))
            m_cameraYawDeg -= kCamSpeedDeg * deltaSeconds;
        if (input->KeyDown(platform::Key::Up))
            m_cameraPitchDeg += kCamSpeedDeg * deltaSeconds;
        if (input->KeyDown(platform::Key::Down))
            m_cameraPitchDeg -= kCamSpeedDeg * deltaSeconds;

        constexpr float kMoveSpeed = 6.0f;
        m_cameraPitchDeg = std::clamp(m_cameraPitchDeg, -89.0f, 89.0f);
        const Quat camRot = Quat::FromEulerDeg(m_cameraPitchDeg, m_cameraYawDeg, 0.0f);
        const Vec3 forward = camRot.Rotate(Vec3::Forward());
        const Vec3 right = camRot.Rotate(Vec3::Right());

        if (input->KeyDown(platform::Key::W))
            m_cameraPos += forward * (kMoveSpeed * deltaSeconds);
        if (input->KeyDown(platform::Key::S))
            m_cameraPos -= forward * (kMoveSpeed * deltaSeconds);
        if (input->KeyDown(platform::Key::A))
            m_cameraPos -= right * (kMoveSpeed * deltaSeconds);
        if (input->KeyDown(platform::Key::D))
            m_cameraPos += right * (kMoveSpeed * deltaSeconds);
    }

    if (auto* t = context.world.Get<TransformComponent>(m_cameraEntity))
    {
        t->localPosition = m_cameraPos;
        t->SetEulerDeg(m_cameraPitchDeg, m_cameraYawDeg, 0.0f);
        t->dirty = true;
    }

    UpdateLightRig(context, deltaSeconds);
    context.debugFlags = m_debugFlags;
    return true;
}

MeshHandle LightingValidationScene::CreateSphereMesh(assets::AssetRegistry& registry) const
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
            const float phi = 2.0f * kPi * static_cast<float>(j) / static_cast<float>(kSegs);
            const float nx  = sinT * std::cos(phi);
            const float ny  = cosT;
            const float nz  = sinT * std::sin(phi);
            sphere.positions.insert(sphere.positions.end(), { kR * nx, kR * ny, kR * nz });
            sphere.normals.insert(sphere.normals.end(), { nx, ny, nz });
            sphere.uvs.insert(sphere.uvs.end(), {
                static_cast<float>(j) / static_cast<float>(kSegs), v
            });
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

MeshHandle LightingValidationScene::CreatePlaneMesh(assets::AssetRegistry& registry) const
{
    auto meshAsset = std::make_unique<assets::MeshAsset>();
    assets::SubMeshData plane;

    plane.positions = {
        -1.0f, 0.0f, -1.0f,
         1.0f, 0.0f, -1.0f,
         1.0f, 0.0f,  1.0f,
        -1.0f, 0.0f,  1.0f,
    };
    plane.normals = {
        0.f, 1.f, 0.f,
        0.f, 1.f, 0.f,
        0.f, 1.f, 0.f,
        0.f, 1.f, 0.f,
    };
    plane.uvs = {
        0.f, 0.f,
        1.f, 0.f,
        1.f, 1.f,
        0.f, 1.f,
    };
    plane.indices = { 0u, 1u, 2u, 0u, 2u, 3u };

    assets::EnsureTangents(plane);
    meshAsset->submeshes.push_back(std::move(plane));
    return registry.meshes.Add(std::move(meshAsset));
}

renderer::VertexLayout LightingValidationScene::CreatePbrVertexLayout() const
{
    renderer::VertexLayout layout;
    layout.attributes.push_back({ renderer::VertexSemantic::Position,  renderer::Format::RGB32_FLOAT,  0u,  0u });
    layout.attributes.push_back({ renderer::VertexSemantic::Normal,    renderer::Format::RGB32_FLOAT,  0u, 12u });
    layout.attributes.push_back({ renderer::VertexSemantic::Tangent,   renderer::Format::RGBA32_FLOAT, 0u, 24u });
    layout.attributes.push_back({ renderer::VertexSemantic::TexCoord0, renderer::Format::RG32_FLOAT,   0u, 40u });
    layout.bindings.push_back({ 0u, 48u });
    return layout;
}

LightingValidationScene::LightRigConfig LightingValidationScene::BuildDefaultLightRigConfig()
{
    LightRigConfig config{};
    config.orbitSpeedDeg = 18.0f;

    config.directional.type = LightType::Directional;
    config.directional.color = {1.0f, 0.98f, 0.95f};
    config.directional.intensity = 4.0f;
    config.directional.castShadows = true;
    config.directional.activeByDefault = true;
    config.directional.spawnPose.position = {0.0f, 0.0f, 0.0f};
    config.directional.spawnPose.eulerDeg = {-48.0f, 135.0f, 0.0f};
    config.directional.runtimePose = config.directional.spawnPose;
    config.directional.shadow.enabled = true;
    config.directional.shadow.type = ShadowType::PCF;
    config.directional.shadow.filter = ShadowFilter::PCF3x3;
    config.directional.shadow.resolution = 4096u;
    config.directional.shadow.bias = 0.00015f;
    config.directional.shadow.normalBias = 0.0008f;
    config.directional.shadow.maxDistance = 12.0f;
    config.directional.shadow.strength = 1.0f;

    config.point.type = LightType::Point;
    config.point.color = {1.0f, 0.95f, 0.9f};
    config.point.intensity = 90.0f;
    config.point.range = 12.0f;
    config.point.castShadows = true;
    config.point.activeByDefault = false;
    config.point.spawnPose.position = {0.0f, 2.3f, 2.7f};
    config.point.spawnPose.eulerDeg = {0.0f, 0.0f, 0.0f};
    config.point.runtimePose = config.point.spawnPose;
    config.point.runtimeAnimated = true;
    config.point.orbitRadiusX = 2.4f;
    config.point.orbitHeight = 2.1f;
    config.point.orbitCenterZ = 1.4f;
    config.point.orbitRadiusZ = 1.9f;
    config.point.shadow.enabled = true;
    config.point.shadow.type = ShadowType::PCF;
    config.point.shadow.filter = ShadowFilter::PCF3x3;
    config.point.shadow.resolution = 1024u;
    config.point.shadow.bias = 0.0012f;
    config.point.shadow.normalBias = 0.0035f;
    config.point.shadow.maxDistance = 10.0f;
    config.point.shadow.strength = 1.0f;

    config.spot.type = LightType::Spot;
    config.spot.color = {1.0f, 0.96f, 0.92f};
    config.spot.intensity = 110.0f;
    config.spot.range = 9.0f;
    config.spot.spotInnerDeg = 12.0f;
    config.spot.spotOuterDeg = 18.0f;
    config.spot.castShadows = true;
    config.spot.activeByDefault = false;
    config.spot.spawnPose.position = {0.0f, 3.0f, 3.5f};
    config.spot.spawnPose.eulerDeg = {-38.0f, 0.0f, 0.0f};
    config.spot.runtimePose.position = {0.0f, 3.4f, 3.6f};
    config.spot.runtimePose.eulerDeg = {-52.0f, 0.0f, 0.0f};
    config.spot.shadow.enabled = true;
    config.spot.shadow.type = ShadowType::PCF;
    config.spot.shadow.filter = ShadowFilter::PCF3x3;
    config.spot.shadow.resolution = 2048u;
    config.spot.shadow.bias = 0.0008f;
    config.spot.shadow.normalBias = 0.003f;
    config.spot.shadow.maxDistance = 9.0f;
    config.spot.shadow.strength = 1.0f;

    return config;
}

void LightingValidationScene::ApplyLightConfig(ExampleSceneContext& context,
                                               EntityID entity,
                                               const LightRigEntryConfig& config) const
{
    if (auto* transform = context.world.Get<TransformComponent>(entity))
        ApplyPoseConfig(*transform, config.spawnPose.position, config.spawnPose.eulerDeg);

    LightComponent light{};
    light.type = config.type;
    light.color = config.color;
    light.intensity = config.intensity;
    light.range = config.range;
    light.spotInnerDeg = config.spotInnerDeg;
    light.spotOuterDeg = config.spotOuterDeg;
    light.castShadows = config.castShadows;
    light.shadowSettings.enabled = config.shadow.enabled;
    light.shadowSettings.type = config.shadow.type;
    light.shadowSettings.filter = config.shadow.filter;
    light.shadowSettings.resolution = config.shadow.resolution;
    light.shadowSettings.bias = config.shadow.bias;
    light.shadowSettings.normalBias = config.shadow.normalBias;
    light.shadowSettings.maxDistance = config.shadow.maxDistance;
    light.shadowSettings.strength = config.shadow.strength;
    context.world.Add<LightComponent>(entity, light);
}

bool LightingValidationScene::CreateMaterials(ExampleSceneContext& context,
                                              const renderer::VertexLayout& vertexLayout)
{
    const char* vsPath     = "pbr_lit.vs.hlsl";
    const char* fsPath     = "pbr_lit.ps.hlsl";
    const char* shadowPath = "shadow.vs.hlsl";
#if defined(KROM_EXAMPLE_BACKEND_OPENGL)
    vsPath     = "pbr_lit.opengl.vs.glsl";
    fsPath     = "pbr_lit.opengl.fs.glsl";
    shadowPath = "shadow.opengl.vs.glsl";
#endif

    const ShaderHandle vs     = context.assetPipeline.LoadShader(vsPath, assets::ShaderStage::Vertex);
    const ShaderHandle fs     = context.assetPipeline.LoadShader(fsPath, assets::ShaderStage::Fragment);
    const ShaderHandle shadow = context.assetPipeline.LoadShader(shadowPath, assets::ShaderStage::Vertex);
    if (!vs.IsValid() || !fs.IsValid() || !shadow.IsValid())
    {
        Debug::LogError("LightingValidationScene: failed to load PBR shaders");
        return false;
    }

    TextureHandle normalPlaneTexture = context.assetPipeline.LoadTexture("cobblestone_floor_09_nor_dx_2k.png");
    if (!normalPlaneTexture.IsValid())
    {
        Debug::LogError("LightingValidationScene: failed to load normal-map texture");
        return false;
    }

    if (auto* t = context.assetRegistry.textures.Get(normalPlaneTexture))
    {
        t->metadata.semantic       = assets::TextureSemantic::Normal;
        t->metadata.normalEncoding = assets::NormalEncoding::RGB;
        t->metadata.colorSpace     = assets::ColorSpace::Linear;
        t->gpuStatus.dirty         = true;
    }

#if !defined(KROM_EXAMPLE_BACKEND_DX11)
    FlipNormalMapGreenChannel(normalPlaneTexture, context.assetRegistry);
#endif

    context.assetPipeline.UploadPendingGpuAssets();
    normalPlaneTexture = context.assetPipeline.GetGpuTexture(normalPlaneTexture);
    if (!normalPlaneTexture.IsValid())
    {
        Debug::LogError("LightingValidationScene: normal-map upload failed");
        return false;
    }

    renderer::pbr::PbrMasterMaterial::Config config{};
    config.vs = vs;
    config.fs = fs;
    config.shadow = shadow;
    config.vertexLayout = vertexLayout;
    config.renderPass = renderer::StandardRenderPasses::Opaque();
    config.colorFormat = renderer::Format::RGBA16_FLOAT;
    config.depthFormat = renderer::Format::D24_UNORM_S8_UINT;
    config.cullMode = renderer::MaterialCullMode::None;
    config.frontFace = renderer::WindingOrder::CCW;
    config.castShadows = true;
    config.receiveShadows = true;
    m_masterMaterial = renderer::pbr::PbrMasterMaterial::Create(context.materialSystem, config);
    if (!m_masterMaterial.IsValid())
    {
        Debug::LogError("LightingValidationScene: failed to create PBR master material");
        return false;
    }

    static constexpr uint32_t kGridRows = 6u;
    static constexpr uint32_t kGridCols = 8u;
    m_gridMaterials.reserve(kGridRows * kGridCols);
    for (uint32_t row = 0u; row < kGridRows; ++row)
    {
        for (uint32_t col = 0u; col < kGridCols; ++col)
        {
            MaterialHandle material = BuildGridMaterial(row, col, kGridRows, kGridCols);
            if (!material.IsValid())
            {
                Debug::LogError("LightingValidationScene: failed to build BRDF-grid material");
                return false;
            }
            m_gridMaterials.push_back(material);
        }
    }

    m_groundMaterial = m_masterMaterial.CreateInstance("Phase2_Ground")
        .BaseColor(0.5f, 0.5f, 0.5f, 1.0f)
        .Roughness(0.95f)
        .Metallic(0.0f)
        .Occlusion(1.0f)
        .IBL(false)
        .Build();

    m_whiteDielectricMaterial = m_masterMaterial.CreateInstance("Phase2_WhiteDielectric")
        .BaseColor(0.92f, 0.92f, 0.92f, 1.0f)
        .Roughness(0.35f)
        .Metallic(0.0f)
        .Occlusion(1.0f)
        .IBL(false)
        .Build();

    m_copperMetalMaterial = m_masterMaterial.CreateInstance("Phase2_CopperMetal")
        .BaseColor(0.955f, 0.637f, 0.538f, 1.0f)
        .Roughness(0.22f)
        .Metallic(1.0f)
        .Occlusion(1.0f)
        .IBL(false)
        .Build();

    m_normalPlaneMaterial = m_masterMaterial.CreateInstance("Phase2_NormalPlane")
        .BaseColor(0.78f, 0.78f, 0.78f, 1.0f)
        .Normal(normalPlaneTexture, 2.0f)
        .Roughness(0.65f)
        .Metallic(0.0f)
        .Occlusion(1.0f)
        .IBL(false)
        .Build();

    m_normalSphereMaterial = m_masterMaterial.CreateInstance("Phase2_NormalSphere")
        .BaseColor(0.86f, 0.86f, 0.86f, 1.0f)
        .Roughness(0.3f)
        .Metallic(0.0f)
        .Occlusion(1.0f)
        .IBL(false)
        .Build();

    if (!m_groundMaterial.IsValid() ||
        !m_whiteDielectricMaterial.IsValid() ||
        !m_copperMetalMaterial.IsValid() ||
        !m_normalPlaneMaterial.IsValid() ||
        !m_normalSphereMaterial.IsValid())
    {
        Debug::LogError("LightingValidationScene: failed to create validation materials");
        return false;
    }

    return true;
}

void LightingValidationScene::BuildBrdfGrid(ExampleSceneContext& context, MeshHandle sphereMesh, MeshHandle planeMesh)
{
    static constexpr uint32_t kRows = 6u;
    static constexpr uint32_t kCols = 8u;
    static constexpr float kSpacing = 1.35f;
    const float xOrigin = -static_cast<float>(kCols - 1u) * 0.5f * kSpacing;
    const float zOrigin = -static_cast<float>(kRows - 1u) * 0.5f * kSpacing;

    const EntityID brdfGround = CreateRenderableEntity(
        context, planeMesh, m_groundMaterial, {0.0f, -0.75f, 0.0f}, {8.0f, 1.0f, 8.0f}, {8.0f, 0.05f, 8.0f}, 11.35f);
    m_brdfGridEntities.push_back(brdfGround);
    if (auto* mesh = context.world.Get<MeshComponent>(brdfGround))
        mesh->castShadows = false;

    size_t materialIndex = 0u;
    for (uint32_t row = 0u; row < kRows; ++row)
    {
        for (uint32_t col = 0u; col < kCols; ++col, ++materialIndex)
        {
            const Vec3 position{
                xOrigin + static_cast<float>(col) * kSpacing,
                0.0f,
                zOrigin + static_cast<float>(row) * kSpacing
            };
            m_brdfGridEntities.push_back(CreateRenderableEntity(
                context,
                sphereMesh,
                m_gridMaterials[materialIndex],
                position,
                {1.0f, 1.0f, 1.0f},
                {0.5f, 0.5f, 0.5f},
                0.5f));
        }
    }
}

void LightingValidationScene::BuildLightTypeTest(ExampleSceneContext& context, MeshHandle sphereMesh, MeshHandle planeMesh)
{
    const EntityID lightTypeGround = CreateRenderableEntity(
        context, planeMesh, m_groundMaterial, {0.0f, -0.75f, 0.0f}, {6.0f, 1.0f, 6.0f}, {6.0f, 0.05f, 6.0f}, 8.49f);
    m_lightTypeEntities.push_back(lightTypeGround);
    if (auto* mesh = context.world.Get<MeshComponent>(lightTypeGround))
        mesh->castShadows = false;

    m_lightTypeEntities.push_back(CreateRenderableEntity(
        context, sphereMesh, m_whiteDielectricMaterial, {-1.25f, 0.0f, 0.0f}, {1.0f, 1.0f, 1.0f}, {0.5f, 0.5f, 0.5f}, 0.5f));
    m_lightTypeEntities.push_back(CreateRenderableEntity(
        context, sphereMesh, m_copperMetalMaterial, {1.25f, 0.0f, 0.0f}, {1.0f, 1.0f, 1.0f}, {0.5f, 0.5f, 0.5f}, 0.5f));
    const EntityID lightTypeBackdrop = CreateRenderableEntity(
        context, planeMesh, m_normalPlaneMaterial, {0.0f, 0.55f, -1.8f}, {1.6f, 1.0f, 1.6f}, {1.6f, 1.6f, 0.05f}, 2.27f,
        Quat::FromEulerDeg(90.0f, 0.0f, 0.0f));
    m_lightTypeEntities.push_back(lightTypeBackdrop);
    if (auto* mesh = context.world.Get<MeshComponent>(lightTypeBackdrop))
        mesh->castShadows = false;
}

void LightingValidationScene::BuildNormalResponseTest(ExampleSceneContext& context, MeshHandle sphereMesh, MeshHandle planeMesh)
{
    const EntityID normalResponseGround = CreateRenderableEntity(
        context, planeMesh, m_groundMaterial, {0.0f, -0.75f, 0.0f}, {5.5f, 1.0f, 5.5f}, {5.5f, 0.05f, 5.5f}, 7.78f);
    m_normalResponseEntities.push_back(normalResponseGround);
    if (auto* mesh = context.world.Get<MeshComponent>(normalResponseGround))
        mesh->castShadows = false;

    const EntityID normalResponseBackdrop = CreateRenderableEntity(
        context, planeMesh, m_normalPlaneMaterial, {-1.8f, 0.55f, 0.0f}, {1.8f, 1.0f, 1.8f}, {1.8f, 1.8f, 0.05f}, 2.55f,
        Quat::FromEulerDeg(90.0f, 0.0f, 0.0f));
    m_normalResponseEntities.push_back(normalResponseBackdrop);
    if (auto* mesh = context.world.Get<MeshComponent>(normalResponseBackdrop))
        mesh->castShadows = false;
    m_normalResponseEntities.push_back(CreateRenderableEntity(
        context, sphereMesh, m_whiteDielectricMaterial, {1.6f, 0.0f, -0.45f}, {1.0f, 1.0f, 1.0f}, {0.5f, 0.5f, 0.5f}, 0.5f));
    m_normalResponseEntities.push_back(CreateRenderableEntity(
        context, sphereMesh, m_copperMetalMaterial, {1.8f, 0.0f, 1.15f}, {1.0f, 1.0f, 1.0f}, {0.5f, 0.5f, 0.5f}, 0.5f));
}

void LightingValidationScene::BuildLights(ExampleSceneContext& context)
{
    m_directionalLightEntity = context.world.CreateEntity();
    context.world.Add<TransformComponent>(m_directionalLightEntity);
    context.world.Add<WorldTransformComponent>(m_directionalLightEntity);
    context.world.Add<ActiveComponent>(m_directionalLightEntity, ActiveComponent{m_lightRigConfig.directional.activeByDefault});
    ApplyLightConfig(context, m_directionalLightEntity, m_lightRigConfig.directional);

    m_pointLightEntity = context.world.CreateEntity();
    context.world.Add<TransformComponent>(m_pointLightEntity);
    context.world.Add<WorldTransformComponent>(m_pointLightEntity);
    context.world.Add<ActiveComponent>(m_pointLightEntity, ActiveComponent{m_lightRigConfig.point.activeByDefault});
    ApplyLightConfig(context, m_pointLightEntity, m_lightRigConfig.point);

    m_spotLightEntity = context.world.CreateEntity();
    context.world.Add<TransformComponent>(m_spotLightEntity);
    context.world.Add<WorldTransformComponent>(m_spotLightEntity);
    context.world.Add<ActiveComponent>(m_spotLightEntity, ActiveComponent{m_lightRigConfig.spot.activeByDefault});
    ApplyLightConfig(context, m_spotLightEntity, m_lightRigConfig.spot);
}

void LightingValidationScene::BuildCamera(ExampleSceneContext& context)
{
    m_cameraEntity = context.world.CreateEntity();
    auto& cameraTransform = context.world.Add<TransformComponent>(m_cameraEntity);
    cameraTransform.localPosition = m_cameraPos;
    cameraTransform.localScale = {1.0f, 1.0f, 1.0f};
    cameraTransform.SetEulerDeg(m_cameraPitchDeg, m_cameraYawDeg, 0.0f);
    context.world.Add<WorldTransformComponent>(m_cameraEntity);
    context.world.Add<CameraComponent>(m_cameraEntity, CameraComponent{
        .projection = ProjectionType::Perspective,
        .fovYDeg = 55.0f,
        .nearPlane = 0.1f,
        .farPlane = 100.0f,
        .isMainCamera = true,
    });
}

void LightingValidationScene::SetSceneMode(SceneMode mode, ExampleSceneContext& context)
{
    m_sceneMode = mode;
    SetEntitiesActive(context, m_brdfGridEntities, mode == SceneMode::BrdfGrid);
    SetEntitiesActive(context, m_lightTypeEntities, mode == SceneMode::LightTypes);
    SetEntitiesActive(context, m_normalResponseEntities, mode == SceneMode::NormalResponse);
    ApplyCameraPreset();
    Debug::Log("LightingValidationScene: scene mode -> %s",
               mode == SceneMode::BrdfGrid ? "BRDF Grid"
               : mode == SceneMode::LightTypes ? "Light Types"
               : "Normal Response");
}

void LightingValidationScene::SetLightMode(LightMode mode, ExampleSceneContext& context)
{
    m_lightMode = mode;
    EnsureActiveComponent(context, m_directionalLightEntity, mode == LightMode::Directional || mode == LightMode::All);
    EnsureActiveComponent(context, m_pointLightEntity, mode == LightMode::Point || mode == LightMode::All);
    EnsureActiveComponent(context, m_spotLightEntity, mode == LightMode::Spot || mode == LightMode::All);
    Debug::Log("LightingValidationScene: light mode -> %s",
               mode == LightMode::Directional ? "Directional"
               : mode == LightMode::Point ? "Point"
               : mode == LightMode::Spot ? "Spot"
               : "All");
}

void LightingValidationScene::ApplyCameraPreset()
{
    switch (m_sceneMode)
    {
    case SceneMode::BrdfGrid:
        m_cameraPos = {0.0f, 2.6f, 10.5f};
        m_cameraYawDeg = 0.0f;
        m_cameraPitchDeg = -10.0f;
        break;
    case SceneMode::LightTypes:
        m_cameraPos = {0.0f, 1.7f, 6.4f};
        m_cameraYawDeg = 0.0f;
        m_cameraPitchDeg = -8.0f;
        break;
    case SceneMode::NormalResponse:
        m_cameraPos = {0.0f, 1.65f, 5.8f};
        m_cameraYawDeg = 0.0f;
        m_cameraPitchDeg = -7.0f;
        break;
    }
}

void LightingValidationScene::UpdateLightRig(ExampleSceneContext& context, float deltaSeconds)
{
    m_lightOrbitDeg += deltaSeconds * m_lightRigConfig.orbitSpeedDeg;
    const float orbitRad = m_lightOrbitDeg * math::DEG_TO_RAD;

    if (m_lightRigConfig.point.runtimeAnimated &&
        (m_lightMode == LightMode::Point || m_lightMode == LightMode::All))
    {
        if (auto* t = context.world.Get<TransformComponent>(m_pointLightEntity))
        {
            t->localPosition = {
                std::cos(orbitRad) * m_lightRigConfig.point.orbitRadiusX,
                m_lightRigConfig.point.orbitHeight,
                m_lightRigConfig.point.orbitCenterZ + std::sin(orbitRad) * m_lightRigConfig.point.orbitRadiusZ
            };
            t->dirty = true;
        }
    }
    if (m_lightMode == LightMode::Spot || m_lightMode == LightMode::All)
    {
        if (auto* t = context.world.Get<TransformComponent>(m_spotLightEntity))
        {
            ApplyPoseConfig(*t,
                            m_lightRigConfig.spot.runtimePose.position,
                            m_lightRigConfig.spot.runtimePose.eulerDeg);
            t->dirty = true;
        }
    }
}

void LightingValidationScene::SetEntitiesActive(ExampleSceneContext& context,
                                                const std::vector<EntityID>& entities,
                                                bool active) const
{
    for (const EntityID entity : entities)
        EnsureActiveComponent(context, entity, active);
}

void LightingValidationScene::EnsureActiveComponent(ExampleSceneContext& context, EntityID entity, bool active) const
{
    if (auto* c = context.world.Get<ActiveComponent>(entity))
    {
        c->active = active;
    }
    else
    {
        context.world.Add<ActiveComponent>(entity, ActiveComponent{active});
    }
}

EntityID LightingValidationScene::CreateRenderableEntity(ExampleSceneContext& context,
                                                         MeshHandle mesh,
                                                         MaterialHandle material,
                                                         const Vec3& position,
                                                         const Vec3& scale,
                                                         const Vec3& boundsExtents,
                                                         float boundingSphere,
                                                         const Quat& rotation) const
{
    const EntityID entity = context.world.CreateEntity();
    auto& transform = context.world.Add<TransformComponent>(entity);
    transform.localPosition = position;
    transform.localRotation = rotation;
    transform.localScale = scale;
    transform.dirty = true;
    context.world.Add<WorldTransformComponent>(entity);
    context.world.Add<ActiveComponent>(entity, ActiveComponent{true});
    context.world.Add<MeshComponent>(entity, mesh);
    context.world.Add<MaterialComponent>(entity, material);
    context.world.Add<BoundsComponent>(entity, BoundsComponent{
        .centerLocal = {0.0f, 0.0f, 0.0f},
        .extentsLocal = boundsExtents,
        .centerWorld = position,
        .extentsWorld = boundsExtents,
        .boundingSphere = boundingSphere,
        .localDirty = true,
    });
    return entity;
}

MaterialHandle LightingValidationScene::BuildGridMaterial(uint32_t row,
                                                          uint32_t col,
                                                          uint32_t rowCount,
                                                          uint32_t colCount)
{
    const math::Vec4 baseColor = GridRowBaseColor(row, rowCount);
    const float metallic = rowCount > 1u
        ? static_cast<float>(row) / static_cast<float>(rowCount - 1u)
        : 0.0f;
    const float roughness = colCount > 1u
        ? 0.04f + (static_cast<float>(col) / static_cast<float>(colCount - 1u)) * (1.0f - 0.04f)
        : 0.5f;

    return m_masterMaterial.CreateInstance(
            "Phase2_Grid_" + std::to_string(row) + "_" + std::to_string(col))
        .BaseColor(baseColor)
        .Roughness(roughness)
        .Metallic(metallic)
        .Occlusion(1.0f)
        .IBL(false)
        .Build();
}

} // namespace engine::examples
