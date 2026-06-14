#include "krom.h"

#include "addons/script/ScriptList.hpp"
#include "addons/script/ScriptSerialization.hpp"
#include "addons/animation/AnimationComponents.hpp"
#include "addons/animation/AnimationSystem.hpp"
#include "addons/animation/SkinningSystem.hpp"
#include "addons/camera/CameraComponents.hpp"
#include "addons/camera/CameraSerialization.hpp"
#include "addons/camera/CameraViewBuilder.hpp"
#include "addons/forward/ForwardFeature.hpp"
#include "addons/gtao/GtaoFeature.hpp"
#include "addons/lighting/LightingComponents.hpp"
#include "addons/lighting/LightingFeature.hpp"
#include "addons/lighting/LightingSerialization.hpp"
#include "addons/mesh_renderer/MeshAssetSceneBindings.hpp"
#include "addons/mesh_renderer/MeshBounds.hpp"
#include "addons/mesh_renderer/MeshRendererComponents.hpp"
#include "addons/mesh_renderer/MeshRendererFeature.hpp"
#include "addons/mesh_renderer/MeshRendererSerialization.hpp"
#include "addons/lit/LitMaterial.hpp"
#include "addons/pbr/PbrInstanceBuilder.hpp"
#include "addons/pbr/PbrMasterMaterial.hpp"
#include "addons/prefab/Prefab.hpp"
#include "addons/shadow/ShadowFeature.hpp"
#include "addons/unlit/UnlitMaterial.hpp"
#include "assets/AssetPipeline.hpp"
#include "assets/AssetRegistry.hpp"
#include "assets/MeshTangents.hpp"
#include "assets/VertexLayoutBridge.hpp"
#include "core/Debug.hpp"
#include "ecs/Components.hpp"
#include "ecs/World.hpp"
#include "events/EventBus.hpp"
#include "platform/StdTiming.hpp"
#include "renderer/MaterialDomain.hpp"
#include "renderer/MaterialSystem.hpp"
#include "renderer/ShaderCompiler.hpp"
#include "renderer/PlatformRenderLoop.hpp"
#include "renderer/runtime/MaterialRuntimeBridge.hpp"
#include "scene/BoundsSystem.hpp"
#include "scene/TransformSystem.hpp"

#ifdef KROM_HAS_GAME_SCRIPTS
extern "C" void KromRegisterGameScripts(engine::script::ScriptRegistry& registry);
#endif

#if defined(KROM_APP_BACKEND_DX11)
#include "addons/dx11/DX11ShaderReflector.hpp"
#elif defined(KROM_APP_BACKEND_VULKAN)
#include "addons/vulkan/VKShaderReflector.hpp"
#endif

#if defined(KROM_APP_USE_WIN32_PLATFORM)
#include "platform/Win32Platform.hpp"
#elif defined(KROM_APP_USE_GLFW_PLATFORM)
#include "platform/GLFWPlatform.hpp"
#else
#error Krom wrapper requires KROM_APP_USE_WIN32_PLATFORM or KROM_APP_USE_GLFW_PLATFORM.
#endif


#include "serialization/SceneSerializer.hpp"

// KROM_WRAPPER_HAS_GLTF steuert nur noch die Importer-Registrierung,
// nicht mehr die Lade-Logik. CreateEntityFromAsset ist format-agnostisch.
#ifndef KROM_WRAPPER_HAS_GLTF
#define KROM_WRAPPER_HAS_GLTF 0
#endif

#if KROM_WRAPPER_HAS_GLTF
#include "addons/gltf/GltfImporter.hpp"
#endif

#include <cfloat>
#include <algorithm>
#include <cmath>
#include <chrono>
#include <fstream>
#include <future>
#include <string_view>
#include <thread>
#include <memory>
#include <unordered_map>
#include <unordered_set>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace Krom {

namespace {

// =============================================================================
// Scene-Datenstrukturen
// =============================================================================

struct KromSceneScriptDesc
{
    std::string className;
    std::unordered_map<std::string, engine::serialization::JsonValue> fields;
};

struct KromSceneEntityDesc
{
    std::string name;
    std::string guid;
    bool persistent = false;

    engine::math::Vec3 position{0.0f, 0.0f, 0.0f};
    engine::math::Vec3 rotationEulerDeg{0.0f, 0.0f, 0.0f};
    engine::math::Vec3 scale{1.0f, 1.0f, 1.0f};

    std::string modelPath;
    std::string materialPath;

    // Licht
    bool hasLight = false;
    int  lightType = 0;  // 0=directional 1=point 2=spot
    engine::math::Vec3 lightColor{1.0f, 1.0f, 1.0f};
    float lightIntensity = 1.0f;
    float lightRange = 100.0f;
    float lightSpotInnerDeg = 15.0f;
    float lightSpotOuterDeg = 30.0f;
    bool  lightCastShadows = true;
    uint32_t lightShadowRes = 2048u;
    float lightShadowBias = 0.0015f;
    float lightShadowNormalBias = 0.001f;
    float lightShadowMaxDist = 100.0f;
    float lightShadowStrength = 1.0f;
    uint32_t lightCascadeCount = 1u;
    float lightCascadeLambda = 0.75f;
    bool  lightRangeSpecified = false;
    bool  lightShadowMaxDistSpecified = false;

    // Kamera
    bool  hasCamera = false;
    float cameraFovYDeg = 60.0f;
    float cameraNear = 0.05f;
    float cameraFar = 1000.0f;
    bool  cameraIsMain = true;
    engine::BackgroundMode cameraBackgroundMode = engine::BackgroundMode::ClearColor;
    std::array<float, 4>   cameraClearColor     = {0.f, 0.f, 0.f, 1.f};

    // Scripts
    std::vector<KromSceneScriptDesc> scripts;
};

struct KromSceneData
{
    std::string name;
    bool persistent = false;
    int  version = 1;
    std::vector<KromSceneEntityDesc> entities;
    bool componentScene = false;
    std::string sourceJson;
    std::string environmentTexturePath;
    bool hasEnvironment = false;
    bool environmentEnableIBL = false;
    float environmentIntensity = 1.0f;
    std::array<float, 4> backgroundColor{0.f, 0.f, 0.f, 1.f};
    bool hasBackgroundColor = false;
    engine::math::Vec3 ambientColor{0.06f, 0.06f, 0.08f};
    bool hasAmbientColor = false;
    float ambientIntensity = 1.0f;
    bool hasAmbientIntensity = false;
    bool valid = false;
};

struct SceneRecord
{
    std::string name;
    std::string path;
    bool persistent = false;
    bool active = false;
    std::vector<LPENTITY> rootEntities;

    // Async-Support
    std::shared_future<KromSceneData> asyncFuture;
    KromSceneData stagedData;
    bool staging = false;  // Hintergrund-Load läuft
    bool staged  = false;  // Parsing fertig, Entity-Erstellung aussteht
};

struct SurfaceRecord
{
    LPENTITY entity = NULL_LPENTITY;
    LPMESH mesh = NULL_MESH;
    uint32_t submeshIndex = 0u;
};

// =============================================================================

struct State
{
    GraphicsConfig config{};
    std::filesystem::path assetRoot;
    std::filesystem::path engineAssetRoot; // builtin Engine-Shader/Assets neben der EXE

    engine::renderer::DeviceFactory::Registry deviceFactoryRegistry;
    engine::events::EventBus eventBus;
    std::unique_ptr<engine::platform::IPlatform> platform;
    engine::renderer::PlatformRenderLoop renderLoop;
    engine::assets::AssetRegistry assetRegistry;
    std::unique_ptr<engine::assets::AssetPipeline> assetPipeline;
    engine::renderer::MaterialSystem materialSystem;
    engine::ecs::ComponentMetaRegistry componentRegistry;
    std::unique_ptr<engine::ecs::World> world;
    engine::TransformSystem transformSystem;
    engine::BoundsSystem boundsSystem;
    engine::platform::StdTiming timing;
    engine::addons::camera::CameraBuildOptions cameraOptions{};
    engine::renderer::EnvironmentHandle activeEnvironment = engine::renderer::EnvironmentHandle::Invalid();
    std::unique_ptr<engine::addons::animation::AnimationSystem> animationSystem;
    std::unique_ptr<engine::addons::animation::SkinningSystem> skinningSystem;

    InitFn initCallback;
    TickFn tickCallback;

    bool componentsRegistered = false;
    bool featuresRegistered = false;
    bool initialized = false;
    bool running = false;
    bool forwardPlusActive = false;

    // Scene-Management
    std::unordered_map<uint32_t, SceneRecord> scenes;
    std::unordered_set<uint32_t>              persistentEntityIds;
    std::unordered_map<uint32_t, std::string> texturePaths;
    std::unordered_map<uint64_t, LPMATERIAL>  runtimeMaterialCache;
    std::unordered_map<uint32_t, SurfaceRecord> surfaces;
    uint32_t                                  nextSurfaceId = 1u;
    uint32_t                                  nextSceneId = 1u;
    std::filesystem::path                     projectRoot;
    std::vector<std::string>                  projectScenePaths;
    std::filesystem::path                     runtimeStatePath;
    float                                     runtimeStateWriteTimer = 0.0f;
    bool                                      editorLiveStateEnabled = false;

    // Ebene 3 — Systeme und Scripts
    std::vector<std::function<void(engine::ecs::World&, float)>> userSystems;
    engine::script::ScriptRegistry                               scriptAddonRegistry;
};

State& Get()
{
    static State state;
    return state;
}

void GenerateMissingNormals(engine::assets::SubMeshData& submesh);

// Ermittelt den Engine-Asset-Root: Verzeichnis "assets" neben der laufenden EXE,
// oder – falls nicht vorhanden – "assets" relativ zum aktuellen Arbeitsverzeichnis.
static std::filesystem::path ResolveEngineAssetRootOnDisk()
{
#if defined(_WIN32)
    wchar_t exeBuf[260]{};
    if (::GetModuleFileNameW(nullptr, exeBuf, 260) > 0)
    {
        const auto exeDir = std::filesystem::path(exeBuf).parent_path();
        if (auto p = exeDir / "assets"; std::filesystem::is_directory(p))
            return p;
    }
#endif
    if (auto p = std::filesystem::current_path() / "assets"; std::filesystem::is_directory(p))
        return p;
    return {};
}

std::string ResolveEngineAssetPath(State& state, const char* path)
{
    if (!path || path[0] == '\0')
        return {};

    const std::filesystem::path requested(path);
    if (requested.is_absolute() && std::filesystem::exists(requested))
        return requested.string();

    // 1. Projekt-Assets (Custom-Shader des Users)
    if (!state.assetRoot.empty() && std::filesystem::exists(state.assetRoot / requested))
        return std::filesystem::absolute(state.assetRoot / requested).string();

    // 2. Engine-Assets (builtin Shader neben der EXE / im CWD)
    if (!state.engineAssetRoot.empty() && std::filesystem::exists(state.engineAssetRoot / requested))
        return std::filesystem::absolute(state.engineAssetRoot / requested).string();

    return path;
}

engine::ShaderHandle LoadEngineShader(State& state,
                                      const char* path,
                                      engine::assets::ShaderStage stage)
{
    if (!state.assetPipeline)
        return engine::ShaderHandle::Invalid();

    return state.assetPipeline->LoadShader(ResolveEngineAssetPath(state, path), stage);
}

engine::renderer::DeviceFactory::BackendType ToBackend(int backend) noexcept
{
    using BackendType = engine::renderer::DeviceFactory::BackendType;
    switch (backend)
    {
    case Renderer::DX11: return BackendType::DirectX11;
    case Renderer::OpenGL: return BackendType::OpenGL;
    case Renderer::DX12: return BackendType::DirectX12;
    case Renderer::Vulkan: return BackendType::Vulkan;
    default: return BackendType::Vulkan;
    }
}

const char* BackendName(engine::renderer::DeviceFactory::BackendType backend) noexcept
{
    using BackendType = engine::renderer::DeviceFactory::BackendType;
    switch (backend)
    {
    case BackendType::DirectX11: return "DirectX11";
    case BackendType::DirectX12: return "DirectX12";
    case BackendType::OpenGL: return "OpenGL";
    case BackendType::Vulkan: return "Vulkan";
    default: return "Unknown";
    }
}

engine::math::Quat QuatFromRotationMatrix(const engine::math::Mat4& m) noexcept
{
    const float trace = m.m[0][0] + m.m[1][1] + m.m[2][2];
    if (trace > 0.f)
    {
        const float s = std::sqrt(trace + 1.f) * 2.f;
        return {(m.m[1][2] - m.m[2][1]) / s,
                (m.m[2][0] - m.m[0][2]) / s,
                (m.m[0][1] - m.m[1][0]) / s,
                0.25f * s};
    }
    if (m.m[0][0] > m.m[1][1] && m.m[0][0] > m.m[2][2])
    {
        const float s = std::sqrt(1.f + m.m[0][0] - m.m[1][1] - m.m[2][2]) * 2.f;
        return {0.25f * s,
                (m.m[1][0] + m.m[0][1]) / s,
                (m.m[2][0] + m.m[0][2]) / s,
                (m.m[1][2] - m.m[2][1]) / s};
    }
    if (m.m[1][1] > m.m[2][2])
    {
        const float s = std::sqrt(1.f + m.m[1][1] - m.m[0][0] - m.m[2][2]) * 2.f;
        return {(m.m[1][0] + m.m[0][1]) / s,
                0.25f * s,
                (m.m[2][1] + m.m[1][2]) / s,
                (m.m[2][0] - m.m[0][2]) / s};
    }

    const float s = std::sqrt(1.f + m.m[2][2] - m.m[0][0] - m.m[1][1]) * 2.f;
    return {(m.m[2][0] + m.m[0][2]) / s,
            (m.m[2][1] + m.m[1][2]) / s,
            0.25f * s,
            (m.m[0][1] - m.m[1][0]) / s};
}

engine::math::Vec3 ResolveEntityPosition(const State& state, LPENTITY entity) noexcept
{
    if (!state.world || !state.world->IsAlive(entity))
        return {};
    if (const auto* wt = state.world->Get<engine::WorldTransformComponent>(entity))
        return wt->position;
    if (const auto* tr = state.world->Get<engine::TransformComponent>(entity))
        return tr->localPosition;
    return {};
}

bool CollectWorldBoundsRecursive(const engine::ecs::World& world,
                                 LPENTITY entity,
                                 engine::math::Vec3& ioMin,
                                 engine::math::Vec3& ioMax,
                                 bool& ioHasBounds)
{
    if (!world.IsAlive(entity))
        return false;

    if (const auto* bounds = world.Get<engine::BoundsComponent>(entity))
    {
        const engine::math::Vec3 localMin = bounds->centerWorld - bounds->extentsWorld;
        const engine::math::Vec3 localMax = bounds->centerWorld + bounds->extentsWorld;
        if (!ioHasBounds)
        {
            ioMin = localMin;
            ioMax = localMax;
            ioHasBounds = true;
        }
        else
        {
            ioMin.x = std::min(ioMin.x, localMin.x);
            ioMin.y = std::min(ioMin.y, localMin.y);
            ioMin.z = std::min(ioMin.z, localMin.z);
            ioMax.x = std::max(ioMax.x, localMax.x);
            ioMax.y = std::max(ioMax.y, localMax.y);
            ioMax.z = std::max(ioMax.z, localMax.z);
        }
    }

    if (const auto* children = world.Get<engine::ChildrenComponent>(entity))
    {
        for (const LPENTITY child : children->children)
            CollectWorldBoundsRecursive(world, child, ioMin, ioMax, ioHasBounds);
    }

    return ioHasBounds;
}

engine::math::Vec3 ComputeImportedModelCenter(const engine::assets::ImportedAssetBundle& bundle) noexcept
{
    using namespace engine;
    math::Vec3 boundsMin{FLT_MAX, FLT_MAX, FLT_MAX};
    math::Vec3 boundsMax{-FLT_MAX, -FLT_MAX, -FLT_MAX};
    bool hasPosition = false;

    std::vector<math::Mat4> nodeWorld(bundle.nodes.size(), math::Mat4::Identity());
    std::vector<uint8_t> nodeWorldReady(bundle.nodes.size(), 0u);
    auto resolveNodeWorld = [&](auto&& self, size_t i) -> const math::Mat4& {
        if (nodeWorldReady[i] != 0u)
            return nodeWorld[i];
        const assets::ImportedSceneNode& node = bundle.nodes[i];
        const math::Mat4 local = math::Mat4::TRS(node.translation, node.rotation, node.scale);
        if (node.parentIndex >= 0 && static_cast<size_t>(node.parentIndex) < nodeWorld.size())
            nodeWorld[i] = self(self, static_cast<size_t>(node.parentIndex)) * local;
        else
            nodeWorld[i] = local;
        nodeWorldReady[i] = 1u;
        return nodeWorld[i];
    };
    for (size_t i = 0; i < bundle.nodes.size(); ++i)
        resolveNodeWorld(resolveNodeWorld, i);

    auto include = [&](const math::Vec3& p) {
        boundsMin.x = std::min(boundsMin.x, p.x); boundsMax.x = std::max(boundsMax.x, p.x);
        boundsMin.y = std::min(boundsMin.y, p.y); boundsMax.y = std::max(boundsMax.y, p.y);
        boundsMin.z = std::min(boundsMin.z, p.z); boundsMax.z = std::max(boundsMax.z, p.z);
        hasPosition = true;
    };

    if (!bundle.nodes.empty())
    {
        for (size_t ni = 0; ni < bundle.nodes.size(); ++ni)
        {
            const assets::ImportedSceneNode& node = bundle.nodes[ni];
            if (node.meshIndex < 0 || static_cast<size_t>(node.meshIndex) >= bundle.meshes.size())
                continue;
            const assets::MeshAsset& mesh = bundle.meshes[static_cast<size_t>(node.meshIndex)];
            for (const assets::SubMeshData& sm : mesh.submeshes)
                for (size_t pi = 0; pi + 2 < sm.positions.size(); pi += 3)
                    include(nodeWorld[ni].TransformPoint({sm.positions[pi], sm.positions[pi + 1], sm.positions[pi + 2]}));
        }
    }
    else
    {
        for (const assets::MeshAsset& mesh : bundle.meshes)
            for (const assets::SubMeshData& sm : mesh.submeshes)
                for (size_t pi = 0; pi + 2 < sm.positions.size(); pi += 3)
                    include({sm.positions[pi], sm.positions[pi + 1], sm.positions[pi + 2]});
    }

    return hasPosition ? (boundsMin + boundsMax) * 0.5f : math::Vec3::Zero();
}

float ComputeImportedModelRadius(const engine::assets::ImportedAssetBundle& bundle,
                                 const engine::math::Vec3& center) noexcept
{
    using namespace engine;
    float radiusSq = 0.0f;
    bool hasPosition = false;

    std::vector<math::Mat4> nodeWorld(bundle.nodes.size(), math::Mat4::Identity());
    std::vector<uint8_t> nodeWorldReady(bundle.nodes.size(), 0u);
    auto resolveNodeWorld = [&](auto&& self, size_t i) -> const math::Mat4& {
        if (nodeWorldReady[i] != 0u)
            return nodeWorld[i];
        const assets::ImportedSceneNode& node = bundle.nodes[i];
        const math::Mat4 local = math::Mat4::TRS(node.translation, node.rotation, node.scale);
        if (node.parentIndex >= 0 && static_cast<size_t>(node.parentIndex) < nodeWorld.size())
            nodeWorld[i] = self(self, static_cast<size_t>(node.parentIndex)) * local;
        else
            nodeWorld[i] = local;
        nodeWorldReady[i] = 1u;
        return nodeWorld[i];
    };
    for (size_t i = 0; i < bundle.nodes.size(); ++i)
        resolveNodeWorld(resolveNodeWorld, i);

    const auto include = [&](const math::Vec3& p) {
        hasPosition = true;
        radiusSq = std::max(radiusSq, (p - center).LengthSq());
    };

    if (!bundle.nodes.empty())
    {
        for (size_t ni = 0; ni < bundle.nodes.size(); ++ni)
        {
            const assets::ImportedSceneNode& node = bundle.nodes[ni];
            if (node.meshIndex < 0 || static_cast<size_t>(node.meshIndex) >= bundle.meshes.size())
                continue;
            const assets::MeshAsset& mesh = bundle.meshes[static_cast<size_t>(node.meshIndex)];
            for (const assets::SubMeshData& sm : mesh.submeshes)
                for (size_t pi = 0; pi + 2 < sm.positions.size(); pi += 3)
                    include(nodeWorld[ni].TransformPoint({sm.positions[pi], sm.positions[pi + 1], sm.positions[pi + 2]}));
        }
    }
    else
    {
        for (const assets::MeshAsset& mesh : bundle.meshes)
            for (const assets::SubMeshData& sm : mesh.submeshes)
                for (size_t pi = 0; pi + 2 < sm.positions.size(); pi += 3)
                    include({sm.positions[pi], sm.positions[pi + 1], sm.positions[pi + 2]});
    }

    return hasPosition ? std::max(std::sqrt(radiusSq), 0.1f) : 1.0f;
}

bool MeshHasSkinning(const engine::assets::MeshAsset& mesh) noexcept
{
    for (const auto& submesh : mesh.submeshes)
    {
        if (!submesh.boneWeights.empty() && !submesh.boneIndices.empty())
            return true;
    }
    return false;
}

engine::MaterialHandle CreateRuntimeLitMaterialFromAssets(State& state,
                                                          const engine::assets::MeshAsset& mesh,
                                                          const engine::assets::MaterialAsset* asset,
                                                          const char* name)
{
    if (mesh.submeshes.empty())
    {
        engine::Debug::LogError("krom.h: runtime material '%s' cannot be created because mesh has no submeshes",
            name ? name : "KromRuntimeLit");
        return engine::MaterialHandle::Invalid();
    }

    const bool skinned = MeshHasSkinning(mesh);
    const char* vsPath = skinned ? "skinned_lit.vs.hlsl" : "lit.vs.hlsl";
    const char* fsPath = "lit.ps.hlsl";
    const char* shadowPath = "shadow.vs.hlsl";
    if (ToBackend(state.config.backend) == engine::renderer::DeviceFactory::BackendType::OpenGL)
    {
        vsPath = "lit.opengl.vs.glsl";
        fsPath = "lit.opengl.fs.glsl";
        shadowPath = "shadow.opengl.vs.glsl";
    }

    const engine::ShaderHandle vs = LoadEngineShader(state, vsPath, engine::assets::ShaderStage::Vertex);
    const engine::ShaderHandle fs = LoadEngineShader(state, fsPath, engine::assets::ShaderStage::Fragment);
    const engine::ShaderHandle shadow = LoadEngineShader(state, shadowPath, engine::assets::ShaderStage::Vertex);
    if (!vs.IsValid() || !fs.IsValid() || !shadow.IsValid())
    {
        engine::Debug::LogError("krom.h: failed to load runtime lit shaders vs='%s' fs='%s' shadow='%s'",
            vsPath, fsPath, shadowPath);
        return engine::MaterialHandle::Invalid();
    }

    std::string layoutError;
    const auto contract = skinned
        ? engine::renderer::VertexContracts::SkinnedLit()
        : engine::renderer::VertexContracts::StaticLit();
    const engine::renderer::VertexLayout layout =
        engine::assets::ResolveVertexLayout(contract, mesh.submeshes[0], &layoutError);
    if (layout.attributes.empty())
    {
        engine::Debug::LogError("krom.h: vertex layout resolution failed for '%s': %s",
            name ? name : "<material>", layoutError.c_str());
        return engine::MaterialHandle::Invalid();
    }

    engine::TextureHandle albedoTexture = engine::TextureHandle::Invalid();
    engine::TextureHandle emissiveTexture = engine::TextureHandle::Invalid();
    if (asset && !asset->baseColorTexture.path.empty())
        albedoTexture = state.assetPipeline->LoadTexture(asset->baseColorTexture.path);
    if (asset && !asset->emissiveTexture.path.empty())
        emissiveTexture = state.assetPipeline->LoadTexture(asset->emissiveTexture.path);
    if (albedoTexture.IsValid() || emissiveTexture.IsValid())
    {
        state.assetPipeline->UploadPendingGpuAssets();
        if (albedoTexture.IsValid())
            albedoTexture = state.assetPipeline->GetGpuTexture(albedoTexture);
        if (emissiveTexture.IsValid())
            emissiveTexture = state.assetPipeline->GetGpuTexture(emissiveTexture);
    }

    engine::renderer::lit::LitMaterialCreateInfo info{};
    info.name = name ? name : "KromRuntimeLit";
    info.vertexShader = vs;
    info.fragmentShader = fs;
    info.shadowShader = shadow;
    info.vertexLayout = layout;
    info.baseColorFactor = asset ? asset->baseColorFactor : engine::math::Vec4::One();
    info.emissiveFactor = asset
        ? engine::math::Vec4{asset->emissiveFactor.x, asset->emissiveFactor.y, asset->emissiveFactor.z, 1.0f}
        : engine::math::Vec4::Zero();
    info.specularStrength = asset ? asset->metallicFactor : 0.0f;
    info.roughnessFactor = asset ? asset->roughnessFactor : 1.0f;
    info.opacityFactor = asset ? asset->baseColorFactor.w : 1.0f;
    info.alphaCutoff = asset ? asset->alphaCutoff : 0.5f;
    info.enableBaseColorMap = albedoTexture.IsValid();
    info.enableEmissiveMap = emissiveTexture.IsValid();
    info.alphaTest = asset && asset->alphaMode == engine::assets::MaterialAlphaMode::Mask;
    info.doubleSided = asset && asset->doubleSided;
    info.castShadows = !asset || asset->castShadows;
    info.cullMode = (asset && asset->doubleSided)
        ? engine::renderer::MaterialCullMode::None
        : engine::renderer::MaterialCullMode::Back;

    const engine::MaterialHandle handle =
        engine::renderer::lit::LitMaterial::Register(state.materialSystem, info);
    if (!handle.IsValid())
    {
        engine::Debug::LogError("krom.h: failed to register runtime lit material '%s'",
            name ? name : "KromRuntimeLit");
        return handle;
    }

    engine::renderer::lit::LitMaterial litMaterial(state.materialSystem, handle);
    if (albedoTexture.IsValid())
        (void)litMaterial.SetAlbedo(albedoTexture);
    if (emissiveTexture.IsValid())
        (void)litMaterial.SetEmissive(emissiveTexture);
    return handle;
}

// Gibt einen MaterialSystem-Handle zurück: Wenn 'material' ein AssetRegistry-Handle
// ist (MaterialAsset vorhanden) und die Entity ein Mesh hat, wird ein LitMaterial
// erstellt und im MaterialSystem registriert. Sonst wird 'material' unverändert zurückgegeben.
bool EnsureTangentsForPbr(engine::assets::MeshAsset& mesh)
{
    bool changed = false;
    for (engine::assets::SubMeshData& submesh : mesh.submeshes)
    {
        GenerateMissingNormals(submesh);
        if (engine::assets::HasValidTangents(submesh))
            continue;
        if (!engine::assets::EnsureTangents(submesh))
            return false;
        submesh.rawInterleavedBytes.clear();
        submesh.rawVertexStride = 0u;
        changed = true;
    }
    if (changed)
    {
        mesh.gpuStatus.dirty = true;
        mesh.gpuStatus.uploaded = false;
    }
    return true;
}

engine::TextureHandle LoadRuntimeTexture(State& state, const std::string& path)
{
    if (!state.assetPipeline || path.empty())
        return engine::TextureHandle::Invalid();
    const engine::TextureHandle assetTexture = state.assetPipeline->LoadTexture(path);
    if (!assetTexture.IsValid())
        return engine::TextureHandle::Invalid();
    state.texturePaths[assetTexture.value] = path;
    state.assetPipeline->UploadPendingGpuAssets();
    return state.assetPipeline->GetGpuTexture(assetTexture);
}

bool IsBuiltInMaterialParamName(const std::string& name) noexcept
{
    static constexpr const char* kNames[] = {
        "baseColorFactor", "emissiveFactor", "metallicFactor", "roughnessFactor",
        "normalStrength", "normalScale", "occlusionStrength", "opacityFactor",
        "alphaCutoff", "uvScale", "uvOffset", "materialFeatureMask", "materialModel",
        "albedo", "baseColor", "baseColorMap", "normal", "normalMap", "orm",
        "ormMap", "emissive", "emissiveMap", "tAlbedo", "tNormal", "tORM",
        "tEmissive", "uAlbedo", "uNormal", "uORM", "uEmissive", "sLinear",
        "sLinearWrap"
    };
    if (!name.empty() && name[0] == '_')
        return true;
    for (const char* builtIn : kNames)
        if (name == builtIn)
            return true;
    return false;
}

engine::renderer::MaterialParam ToRuntimeParam(const engine::assets::MaterialParam& src)
{
    engine::renderer::MaterialParam dst{};
    dst.name = src.name;
    switch (src.type)
    {
    case engine::assets::MaterialParam::Type::Float:   dst.type = engine::renderer::MaterialParam::Type::Float; break;
    case engine::assets::MaterialParam::Type::Vec2:    dst.type = engine::renderer::MaterialParam::Type::Vec2; break;
    case engine::assets::MaterialParam::Type::Vec3:    dst.type = engine::renderer::MaterialParam::Type::Vec3; break;
    case engine::assets::MaterialParam::Type::Vec4:    dst.type = engine::renderer::MaterialParam::Type::Vec4; break;
    case engine::assets::MaterialParam::Type::Int:     dst.type = engine::renderer::MaterialParam::Type::Int; break;
    case engine::assets::MaterialParam::Type::Bool:    dst.type = engine::renderer::MaterialParam::Type::Bool; break;
    case engine::assets::MaterialParam::Type::Texture: dst.type = engine::renderer::MaterialParam::Type::Texture; break;
    }
    switch (src.type)
    {
    case engine::assets::MaterialParam::Type::Float:
    case engine::assets::MaterialParam::Type::Vec2:
    case engine::assets::MaterialParam::Type::Vec3:
    case engine::assets::MaterialParam::Type::Vec4:
        dst.value.f[0] = src.value.f[0];
        dst.value.f[1] = src.value.f[1];
        dst.value.f[2] = src.value.f[2];
        dst.value.f[3] = src.value.f[3];
        break;
    case engine::assets::MaterialParam::Type::Int:
        dst.value.i = src.value.i;
        break;
    case engine::assets::MaterialParam::Type::Bool:
        dst.value.b = src.value.b;
        break;
    case engine::assets::MaterialParam::Type::Texture:
        dst.texture = src.texture;
        break;
    }
    return dst;
}

std::string ResolveRuntimeShaderPath(State& state,
                                     const std::string& shaderPath,
                                     engine::assets::ShaderStage stage)
{
    if (ToBackend(state.config.backend) != engine::renderer::DeviceFactory::BackendType::OpenGL)
        return shaderPath;

    std::filesystem::path candidate(shaderPath);
    const std::string filename = candidate.filename().string();
    std::string variantName;
    if (stage == engine::assets::ShaderStage::Vertex)
    {
        constexpr std::string_view suffix = ".vs.hlsl";
        if (filename.size() > suffix.size() &&
            filename.compare(filename.size() - suffix.size(), suffix.size(), suffix.data(), suffix.size()) == 0)
            variantName = filename.substr(0, filename.size() - suffix.size()) + ".opengl.vs.glsl";
    }
    else if (stage == engine::assets::ShaderStage::Fragment)
    {
        constexpr std::string_view suffix = ".ps.hlsl";
        if (filename.size() > suffix.size() &&
            filename.compare(filename.size() - suffix.size(), suffix.size(), suffix.data(), suffix.size()) == 0)
            variantName = filename.substr(0, filename.size() - suffix.size()) + ".opengl.fs.glsl";
    }
    if (variantName.empty())
        return shaderPath;

    candidate.replace_filename(variantName);
    const std::string variantPath = candidate.generic_string();
    const std::string resolved = ResolveEngineAssetPath(state, variantPath.c_str());
    return std::filesystem::exists(resolved) ? variantPath : shaderPath;
}

engine::ShaderHandle LoadRuntimeShader(State& state,
                                       const std::string& shaderPath,
                                       engine::assets::ShaderStage stage)
{
    if (!state.assetPipeline)
        return engine::ShaderHandle::Invalid();
    const std::string resolvedShaderPath = ResolveRuntimeShaderPath(state, shaderPath, stage);
    return state.assetPipeline->LoadShader(ResolveEngineAssetPath(state, resolvedShaderPath.c_str()), stage);
}

void ReflectCustomTextureBindings(State& state,
                                  engine::ShaderHandle vs,
                                  engine::ShaderHandle fs,
                                  std::unordered_map<std::string, uint32_t>& outOverrides)
{
    const engine::assets::ShaderAsset* vsAsset = state.assetRegistry.shaders.Get(vs);
    const engine::assets::ShaderAsset* fsAsset = state.assetRegistry.shaders.Get(fs);
    if (!vsAsset || !fsAsset)
        return;

    engine::renderer::ShaderParameterLayout reflectedLayout{};
#if defined(KROM_APP_BACKEND_DX11)
    engine::dx11::DX11ShaderReflector reflector;
    reflector.ReflectProgram(*vsAsset, *fsAsset, reflectedLayout, nullptr);
#elif defined(KROM_APP_BACKEND_VULKAN)
    engine::vulkan::VKShaderReflector reflector;
    reflector.ReflectProgram(*vsAsset, *fsAsset, reflectedLayout, nullptr);
#endif
    for (uint32_t i = 0u; i < reflectedLayout.slotCount; ++i)
    {
        const auto& slot = reflectedLayout.slots[i];
        if (slot.type == engine::renderer::MaterialParameterType::Texture2D ||
            slot.type == engine::renderer::MaterialParameterType::TextureCube)
            outOverrides[std::string(slot.Name())] = slot.binding;
    }
}

engine::MaterialHandle CreateRuntimeUnlitMaterialFromAssets(State& state,
                                                            engine::assets::MeshAsset& mesh,
                                                            const engine::assets::MaterialAsset& asset,
                                                            const char* name,
                                                            engine::ShaderHandle customVS = engine::ShaderHandle::Invalid(),
                                                            engine::ShaderHandle customFS = engine::ShaderHandle::Invalid())
{
    using namespace engine;

    for (assets::SubMeshData& submesh : mesh.submeshes)
        GenerateMissingNormals(submesh);

    std::string layoutError;
    const renderer::VertexLayout layout =
        assets::ResolveVertexLayout(renderer::VertexContracts::StaticLit(), mesh.submeshes[0], &layoutError);
    if (layout.attributes.empty())
    {
        Debug::LogError("krom.h: unlit/custom vertex layout failed for '%s': %s",
                        name ? name : "<material>", layoutError.c_str());
        return MaterialHandle::Invalid();
    }

    ShaderHandle vs = customVS;
    ShaderHandle fs = customFS;
    if (!vs.IsValid() || !fs.IsValid())
    {
        const bool opengl = ToBackend(state.config.backend) == renderer::DeviceFactory::BackendType::OpenGL;
        vs = LoadEngineShader(state, opengl ? "quad_unlit.opengl.vs.glsl" : "quad_unlit.vs.hlsl", assets::ShaderStage::Vertex);
        fs = LoadEngineShader(state, opengl ? "quad_unlit.opengl.fs.glsl" : "quad_unlit.ps.hlsl", assets::ShaderStage::Fragment);
    }
    if (!vs.IsValid() || !fs.IsValid())
    {
        Debug::LogError("krom.h: failed to load unlit/custom shaders for '%s'",
                        name ? name : "<material>");
        return MaterialHandle::Invalid();
    }

    renderer::unlit::UnlitMaterialCreateInfo info{};
    info.name = name ? name : "KromRuntimeUnlit";
    info.vertexShader = vs;
    info.fragmentShader = fs;
    info.vertexLayout = layout;
    info.baseColorFactor = asset.baseColorFactor;
    info.emissiveFactor = {asset.emissiveFactor.x, asset.emissiveFactor.y, asset.emissiveFactor.z, 1.f};
    info.opacityFactor = asset.baseColorFactor.w;
    info.alphaCutoff = asset.alphaCutoff;
    info.enableBaseColorMap = !asset.baseColorTexture.path.empty();
    info.enableEmissiveMap = !asset.emissiveTexture.path.empty();
    info.alphaTest = asset.alphaMode == assets::MaterialAlphaMode::Mask;
    info.doubleSided = asset.doubleSided;
    info.castShadows = asset.castShadows;
    info.cullMode = asset.doubleSided ? renderer::MaterialCullMode::None : renderer::MaterialCullMode::Back;

    info.extraParameters.reserve(asset.params.size());
    for (const assets::MaterialParam& param : asset.params)
    {
        if (IsBuiltInMaterialParamName(param.name))
            continue;
        renderer::MaterialParam runtimeParam = ToRuntimeParam(param);
        if (param.type == assets::MaterialParam::Type::Texture && !param.texturePath.empty())
            runtimeParam.texture = LoadRuntimeTexture(state, param.texturePath);
        info.extraParameters.push_back(runtimeParam);
    }
    ReflectCustomTextureBindings(state, vs, fs, info.textureBindingOverrides);

    const MaterialHandle handle = renderer::unlit::UnlitMaterial::Register(state.materialSystem, info);
    if (!handle.IsValid())
        return handle;

    if (const TextureHandle albedo = LoadRuntimeTexture(state, asset.baseColorTexture.path); albedo.IsValid())
        state.materialSystem.SetTexture(handle, "albedo", albedo);
    if (const TextureHandle emissive = LoadRuntimeTexture(state, asset.emissiveTexture.path); emissive.IsValid())
        state.materialSystem.SetTexture(handle, "emissive", emissive);
    for (const assets::MaterialParam& param : asset.params)
    {
        if (IsBuiltInMaterialParamName(param.name) || param.type != assets::MaterialParam::Type::Texture || param.texturePath.empty())
            continue;
        if (const TextureHandle tex = LoadRuntimeTexture(state, param.texturePath); tex.IsValid())
            state.materialSystem.SetTexture(handle, param.name, tex);
    }
    state.materialSystem.MarkDirty(handle);
    return handle;
}

engine::MaterialHandle CreateRuntimeCustomMaterialFromAssets(State& state,
                                                             engine::assets::MeshAsset& mesh,
                                                             const engine::assets::MaterialAsset& asset,
                                                             const char* name)
{
    engine::ShaderHandle vs = asset.vertexShader;
    engine::ShaderHandle fs = asset.fragmentShader;
    if (!vs.IsValid() && !asset.vertexShaderPath.empty())
        vs = LoadRuntimeShader(state, asset.vertexShaderPath, engine::assets::ShaderStage::Vertex);
    if (!fs.IsValid() && !asset.fragmentShaderPath.empty())
        fs = LoadRuntimeShader(state, asset.fragmentShaderPath, engine::assets::ShaderStage::Fragment);
    if (!vs.IsValid() || !fs.IsValid())
    {
        engine::Debug::LogError("krom.h: custom material '%s' missing shaders (vs='%s', fs='%s')",
                                name ? name : "<material>",
                                asset.vertexShaderPath.c_str(),
                                asset.fragmentShaderPath.c_str());
        return engine::MaterialHandle::Invalid();
    }
    return CreateRuntimeUnlitMaterialFromAssets(state, mesh, asset, name, vs, fs);
}

uint64_t RuntimeMaterialCacheKey(LPMATERIAL material, LPMESH mesh) noexcept
{
    return (static_cast<uint64_t>(material.value) << 32u) | static_cast<uint64_t>(mesh.value);
}

engine::MaterialHandle CreateRuntimePbrMaterialFromAssets(State& state,
                                                          engine::assets::MeshAsset& mesh,
                                                          const engine::assets::MaterialAsset& asset,
                                                          const char* name)
{
    using namespace engine;
    if (!EnsureTangentsForPbr(mesh))
    {
        Debug::LogError("krom.h: PBR material requires tangents");
        return MaterialHandle::Invalid();
    }

    std::string layoutError;
    renderer::VertexLayout layout =
        assets::ResolveVertexLayout(renderer::VertexContracts::PbrLit(), mesh.submeshes[0], &layoutError);
    if (layout.attributes.empty())
    {
        Debug::LogError("krom.h: PBR vertex layout failed: %s", layoutError.c_str());
        return MaterialHandle::Invalid();
    }

    const bool opengl = ToBackend(state.config.backend) == renderer::DeviceFactory::BackendType::OpenGL;
    const char* vsPath = opengl ? "pbr_lit.opengl.vs.glsl" : "pbr_lit.vs.hlsl";
    const char* fsPath = opengl ? "pbr_lit.opengl.fs.glsl" : "pbr_lit.ps.hlsl";
    const char* shadowPath = opengl ? "shadow_pbr.opengl.vs.glsl" : "shadow_pbr.vs.hlsl";
    const char* shadowFsPath = opengl ? "shadow_pbr.opengl.fs.glsl" : "shadow_pbr.ps.hlsl";

    const ShaderHandle vs = LoadEngineShader(state, vsPath, assets::ShaderStage::Vertex);
    const ShaderHandle fs = LoadEngineShader(state, fsPath, assets::ShaderStage::Fragment);
    const ShaderHandle shadow = LoadEngineShader(state, shadowPath, assets::ShaderStage::Vertex);
    const ShaderHandle shadowFs = LoadEngineShader(state, shadowFsPath, assets::ShaderStage::Fragment);
    if (!vs.IsValid() || !fs.IsValid() || !shadow.IsValid())
    {
        Debug::LogError("krom.h: failed to load PBR shaders vs='%s' fs='%s' shadow='%s'",
            vsPath, fsPath, shadowPath);
        return MaterialHandle::Invalid();
    }

    renderer::pbr::PbrMasterMaterial::Config config{};
    config.vs = vs;
    config.fs = fs;
    config.shadow = shadow;
    config.shadowFs = shadowFs;
    config.vertexLayout = layout;
    config.cullMode = asset.doubleSided ? renderer::MaterialCullMode::None : renderer::MaterialCullMode::Back;
    config.castShadows = asset.castShadows;
    config.receiveShadows = true;

    renderer::pbr::PbrMasterMaterial master =
        renderer::pbr::PbrMasterMaterial::Create(state.materialSystem, config);
    if (!master.IsValid())
        return MaterialHandle::Invalid();

    TextureHandle baseColorTex = LoadRuntimeTexture(state, asset.baseColorTexture.path);
    TextureHandle ormTex = LoadRuntimeTexture(state, asset.metallicRoughnessTexture.path);
    TextureHandle normalTex = LoadRuntimeTexture(state, asset.normalTexture.path);
    TextureHandle emissiveTex = LoadRuntimeTexture(state, asset.emissiveTexture.path);

    auto builder = master.CreateInstance(name ? name : "KromPBR");
    if (baseColorTex.IsValid()) builder.BaseColor(baseColorTex);
    else builder.BaseColor(asset.baseColorFactor);

    if (ormTex.IsValid())
    {
        builder.Occlusion(ormTex, renderer::pbr::MaterialChannel::R, asset.occlusionStrength);
        builder.Roughness(ormTex, renderer::pbr::MaterialChannel::G, asset.roughnessFactor);
        builder.Metallic(ormTex, renderer::pbr::MaterialChannel::B, asset.metallicFactor);
    }
    else
    {
        builder.Occlusion(asset.occlusionStrength);
        builder.Roughness(asset.roughnessFactor);
        builder.Metallic(asset.metallicFactor);
    }

    if (normalTex.IsValid())
        builder.Normal(normalTex, asset.normalScale);
    if (emissiveTex.IsValid())
        builder.Emissive(emissiveTex);
    else
        builder.Emissive(asset.emissiveFactor.x, asset.emissiveFactor.y, asset.emissiveFactor.z);

    builder.Opacity(asset.baseColorFactor.w)
           .DoubleSided(asset.doubleSided)
           .IBL(false);
    if (asset.alphaMode == assets::MaterialAlphaMode::Mask)
        builder.AlphaTest(asset.alphaCutoff);

    const MaterialHandle material = builder.Build();
    if (material.IsValid())
    {
        // baseColorFactor wird im Builder nur gesetzt wenn KEINE Textur verwendet wird.
        // Hat das Material eine BaseColor-Textur, muss der Factor explizit nachgesetzt
        // werden, damit er die Textur im Shader korrekt multipliziert (Standard-PBR).
        if (baseColorTex.IsValid())
            state.materialSystem.SetVec4(material, "baseColorFactor", asset.baseColorFactor);
        state.materialSystem.SetVec2(material, "uvScale", asset.uvScale);
        state.materialSystem.SetVec2(material, "uvOffset", asset.uvOffset);
        state.materialSystem.MarkDirty(material);
    }
    return material;
}

LPMATERIAL RealizeMaterialForEntity(State& state, LPENTITY entity, LPMATERIAL material)
{
    if (!state.world || !material.IsValid())
        return material;

    const engine::assets::MaterialAsset* asset = state.assetRegistry.materials.Get(material);
    if (!asset)
        return material;  // Kein AssetRegistry-Handle → bereits ein MaterialSystem-Handle

    const engine::MeshComponent* meshComp = state.world->Get<engine::MeshComponent>(entity);
    if (!meshComp || !meshComp->mesh.IsValid())
        return material;

    engine::assets::MeshAsset* mesh = state.assetRegistry.meshes.Get(meshComp->mesh);
    if (!mesh || mesh->submeshes.empty())
        return material;

    for (engine::assets::SubMeshData& submesh : mesh->submeshes)
        GenerateMissingNormals(submesh);

    const uint64_t cacheKey = RuntimeMaterialCacheKey(material, meshComp->mesh);
    if (const auto cached = state.runtimeMaterialCache.find(cacheKey); cached != state.runtimeMaterialCache.end())
        return cached->second.IsValid() ? cached->second : material;

    if (asset->templateName == "custom")
    {
        const LPMATERIAL realized = CreateRuntimeCustomMaterialFromAssets(
            state, *mesh, *asset,
            asset->debugName.empty() ? nullptr : asset->debugName.c_str());
        if (realized.IsValid())
            state.runtimeMaterialCache[cacheKey] = realized;
        return realized.IsValid() ? realized : material;
    }

    if (asset->templateName == "unlit")
    {
        const LPMATERIAL realized = CreateRuntimeUnlitMaterialFromAssets(
            state, *mesh, *asset,
            asset->debugName.empty() ? nullptr : asset->debugName.c_str());
        if (realized.IsValid())
            state.runtimeMaterialCache[cacheKey] = realized;
        return realized.IsValid() ? realized : material;
    }

    if (asset->templateName == "pbr-lit")
    {
        const LPMATERIAL realized = CreateRuntimePbrMaterialFromAssets(
            state, *mesh, *asset,
            asset->debugName.empty() ? nullptr : asset->debugName.c_str());
        if (realized.IsValid())
            state.runtimeMaterialCache[cacheKey] = realized;
        return realized.IsValid() ? realized : material;
    }

    const LPMATERIAL realized = CreateRuntimeLitMaterialFromAssets(
        state, *mesh, asset,
        asset->debugName.empty() ? nullptr : asset->debugName.c_str());
    if (realized.IsValid())
        state.runtimeMaterialCache[cacheKey] = realized;
    return realized.IsValid() ? realized : material;
}

// Erstellt ein MaterialAsset im Registry und gibt einen AssetRegistry-Handle zurück.
// RealizeMaterialForEntity wandelt ihn beim SetMaterial(Recursive)-Aufruf pro Entity
// in ein richtiges LitMaterial (mit korrektem Vertex-Layout) um.
LPMATERIAL BuildCodeMaterial(State& state,
                              const char* albedoPath,
                              const engine::math::Vec4& baseColorFactor,
                              float roughness,
                              float metallic)
{
    auto asset = std::make_unique<engine::assets::MaterialAsset>();
    if (albedoPath && albedoPath[0] != '\0')
    {
        asset->baseColorTexture.path = albedoPath;
        asset->debugName = albedoPath;
    }
    else
    {
        asset->debugName = "KromCodeMaterial";
    }
    asset->baseColorFactor   = baseColorFactor;
    asset->metallicFactor    = metallic;
    asset->roughnessFactor   = roughness;
    asset->castShadows       = true;
    return state.assetRegistry.materials.Add(std::move(asset));
}

LPMATERIAL BuildPbrCodeMaterial(State& state, const PBR& pbr)
{
    auto asset = std::make_unique<engine::assets::MaterialAsset>();
    asset->templateName = "pbr-lit";
    asset->debugName = pbr.color.texture && pbr.color.texture[0] != '\0'
        ? pbr.color.texture
        : "KromPBRMaterial";
    asset->baseColorFactor = pbr.color.color;
    asset->roughnessFactor = pbr.roughness;
    asset->metallicFactor = pbr.metallic;
    asset->occlusionStrength = pbr.occlusion;
    asset->normalScale = pbr.normalStrength;
    asset->uvScale = pbr.uvScale;
    asset->uvOffset = pbr.uvOffset;
    asset->castShadows = true;

    if (pbr.color.texture && pbr.color.texture[0] != '\0')
        asset->baseColorTexture.path = pbr.color.texture;
    if (pbr.normal && pbr.normal[0] != '\0')
        asset->normalTexture.path = pbr.normal;
    if (pbr.orm && pbr.orm[0] != '\0')
        asset->metallicRoughnessTexture.path = pbr.orm;

    return state.assetRegistry.materials.Add(std::move(asset));
}

void AddVertex(engine::assets::SubMeshData& submesh, const MeshVertex& vertex)
{
    submesh.positions.insert(submesh.positions.end(), {vertex.position.x, vertex.position.y, vertex.position.z});
    submesh.normals.insert(submesh.normals.end(), {vertex.normal.x, vertex.normal.y, vertex.normal.z});
    submesh.uvs.insert(submesh.uvs.end(), {vertex.uv.x, vertex.uv.y});
    submesh.colors.insert(submesh.colors.end(), {vertex.color.x, vertex.color.y, vertex.color.z, vertex.color.w});
}

void GenerateMissingNormals(engine::assets::SubMeshData& submesh)
{
    const size_t vertexCount = submesh.positions.size() / 3u;
    if (vertexCount == 0u)
        return;

    if (submesh.normals.size() < vertexCount * 3u)
        submesh.normals.resize(vertexCount * 3u, 0.0f);

    std::vector<engine::math::Vec3> generated(vertexCount, engine::math::Vec3::Zero());
    const auto indexAt = [&](size_t i) -> uint32_t {
        return submesh.indices.empty() ? static_cast<uint32_t>(i) : submesh.indices[i];
    };
    const size_t indexCount = submesh.indices.empty() ? vertexCount : submesh.indices.size();
    for (size_t i = 0; i + 2u < indexCount; i += 3u)
    {
        const uint32_t ia = indexAt(i);
        const uint32_t ib = indexAt(i + 1u);
        const uint32_t ic = indexAt(i + 2u);
        if (ia >= vertexCount || ib >= vertexCount || ic >= vertexCount)
            continue;

        const engine::math::Vec3 a{submesh.positions[ia * 3u], submesh.positions[ia * 3u + 1u], submesh.positions[ia * 3u + 2u]};
        const engine::math::Vec3 b{submesh.positions[ib * 3u], submesh.positions[ib * 3u + 1u], submesh.positions[ib * 3u + 2u]};
        const engine::math::Vec3 c{submesh.positions[ic * 3u], submesh.positions[ic * 3u + 1u], submesh.positions[ic * 3u + 2u]};
        const engine::math::Vec3 normal = engine::math::Vec3::Cross(b - a, c - a).Normalized();
        if (normal.LengthSq() <= engine::math::EPSILON)
            continue;

        for (const uint32_t idx : {ia, ib, ic})
            generated[idx] += normal;
    }

    for (size_t vertex = 0u; vertex < vertexCount; ++vertex)
    {
        const size_t i = vertex * 3u;
        engine::math::Vec3 normal{submesh.normals[i], submesh.normals[i + 1u], submesh.normals[i + 2u]};
        if (normal.LengthSq() <= engine::math::EPSILON)
            normal = generated[vertex].LengthSq() > engine::math::EPSILON
                ? generated[vertex].Normalized()
                : engine::math::Vec3::Up();
        else
            normal = normal.Normalized();

        submesh.normals[i] = normal.x;
        submesh.normals[i + 1u] = normal.y;
        submesh.normals[i + 2u] = normal.z;
    }
}

engine::assets::SubMeshData* GetSurfaceSubmesh(State& state, LPSURFACE surface)
{
    const auto recordIt = state.surfaces.find(surface.id);
    if (recordIt == state.surfaces.end())
        return nullptr;

    engine::assets::MeshAsset* mesh = state.assetRegistry.meshes.Get(recordIt->second.mesh);
    if (!mesh || recordIt->second.submeshIndex >= mesh->submeshes.size())
        return nullptr;
    return &mesh->submeshes[recordIt->second.submeshIndex];
}

const SurfaceRecord* GetSurfaceRecord(const State& state, LPSURFACE surface)
{
    const auto it = state.surfaces.find(surface.id);
    return it != state.surfaces.end() ? &it->second : nullptr;
}

void TouchSurface(State& state, LPSURFACE surface)
{
    const SurfaceRecord* record = GetSurfaceRecord(state, surface);
    if (!record)
        return;

    engine::assets::MeshAsset* mesh = state.assetRegistry.meshes.Get(record->mesh);
    if (!mesh || record->submeshIndex >= mesh->submeshes.size())
        return;

    engine::assets::SubMeshData& submesh = mesh->submeshes[record->submeshIndex];
    submesh.rawInterleavedBytes.clear();
    submesh.rawVertexStride = 0u;
    mesh->gpuStatus.dirty = true;
    mesh->gpuStatus.uploaded = false;
    std::erase_if(state.runtimeMaterialCache, [&](const auto& item) {
        return static_cast<uint32_t>(item.first & 0xFFFFFFFFull) == record->mesh.value;
    });

    if (state.world && state.world->IsAlive(record->entity))
    {
        if (!state.world->Has<engine::BoundsComponent>(record->entity))
            state.world->Add<engine::BoundsComponent>(record->entity);
        engine::mesh_renderer::UpdateLocalBoundsForEntity(*state.world, record->entity, state.assetRegistry);
    }
}

LPMESH RegisterProceduralMesh(State& state, engine::assets::SubMeshData submesh, const char* name, bool generateTangents)
{
    if (submesh.positions.empty())
        return NULL_MESH;

    GenerateMissingNormals(submesh);
    if (submesh.uvs.empty())
        submesh.uvs.assign((submesh.positions.size() / 3u) * 2u, 0.0f);
    if (submesh.colors.empty())
        submesh.colors.assign((submesh.positions.size() / 3u) * 4u, 1.0f);
    if (generateTangents)
        (void)engine::assets::EnsureTangents(submesh);

    auto mesh = std::make_unique<engine::assets::MeshAsset>();
    mesh->debugName = (name && name[0] != '\0') ? name : "KromProceduralMesh";
    mesh->path = mesh->debugName;
    mesh->state = engine::assets::AssetState::Loaded;
    mesh->submeshes.push_back(std::move(submesh));
    mesh->gpuStatus.dirty = true;
    return state.assetRegistry.meshes.Add(std::move(mesh));
}

LPENTITY CreateProceduralEntity(State& state, LPMESH mesh, const char* name)
{
    if (!mesh.IsValid())
        return NULL_LPENTITY;

    LPENTITY entity = CreateEntity((name && name[0] != '\0') ? name : "ProceduralEntity");
    if (!entity.IsValid() || !SetMesh(entity, mesh))
        return NULL_LPENTITY;

    const LPMATERIAL material = BuildCodeMaterial(
        state,
        nullptr,
        engine::math::Vec4{0.82f, 0.82f, 0.78f, 1.0f},
        0.8f,
        0.0f);
    if (material.IsValid())
        (void)SetMaterial(entity, material);
    return entity;
}

// =============================================================================
// .kscene Parser
// =============================================================================

static engine::math::Vec3 ReadVec3Array(const engine::serialization::JsonValue& v) noexcept
{
    if (v.IsArray() && v.arrayVal.size() >= 3)
        return {v.arrayVal[0].AsFloat(), v.arrayVal[1].AsFloat(), v.arrayVal[2].AsFloat()};
    return {0.0f, 0.0f, 0.0f};
}

static bool ReadScriptFieldValueFromJson(const engine::serialization::JsonValue& node,
                                         engine::script::ScriptFieldType type,
                                         engine::script::ScriptFieldValue& outValue) noexcept
{
    outValue.type = type;
    switch (type)
    {
    case engine::script::ScriptFieldType::Float:
        if (!node.IsNumber()) return false;
        outValue.floatValue = node.AsFloat();
        return true;
    case engine::script::ScriptFieldType::Int:
        if (!node.IsNumber()) return false;
        outValue.intValue = node.AsInt();
        return true;
    case engine::script::ScriptFieldType::Bool:
        if (!node.IsBool()) return false;
        outValue.boolValue = node.AsBool();
        return true;
    case engine::script::ScriptFieldType::Vec3:
        if (!node.IsArray() || node.arrayVal.size() < 3u) return false;
        outValue.vec3Value = ReadVec3Array(node);
        return true;
    case engine::script::ScriptFieldType::String:
    case engine::script::ScriptFieldType::Prefab:
    case engine::script::ScriptFieldType::Entity:
        if (!node.IsString()) return false;
        outValue.stringValue = node.AsString();
        return true;
    }
    return false;
}

static bool IsComponentSerializedScene(const engine::serialization::JsonValue& root) noexcept
{
    const auto* arr = root.Get("entities");
    if (!arr || !arr->IsArray())
        return false;

    for (const auto& e : arr->arrayVal)
    {
        const auto* components = e.Get("components");
        if (components && components->IsArray())
            return true;
    }
    return false;
}

static KromSceneData ParseKscene(const std::string& json)
{
    KromSceneData data;
    std::string err;
    const engine::serialization::JsonValue root =
        engine::serialization::JsonParser::Parse(json, err);
    if (!err.empty())
    {
        engine::Debug::LogError("krom.h: kscene JSON-Fehler: %s", err.c_str());
        return data;
    }

    if (const auto* s = root.Get("scene"))     data.name       = s->AsString();
    if (const auto* p = root.Get("persistent")) data.persistent = p->AsBool();
    if (const auto* v = root.Get("version"))    data.version    = v->AsInt();
    if (const auto* env = root.Get("environment"))
    {
        if (env->IsObject())
        {
            if (const auto* path = env->Get("texturePath"); path && path->IsString())
            {
                data.environmentTexturePath = path->AsString();
                data.hasEnvironment = !data.environmentTexturePath.empty();
            }
            if (const auto* enable = env->Get("enableIBL"); enable && enable->IsBool())
                data.environmentEnableIBL = enable->AsBool();
            if (const auto* intensity = env->Get("iblIntensity"); intensity && intensity->IsNumber())
                data.environmentIntensity = std::max(0.0f, intensity->AsFloat());
            if (const auto* bg = env->Get("backgroundColor"); bg && bg->IsArray() && bg->arrayVal.size() >= 3u)
            {
                data.backgroundColor[0] = bg->arrayVal[0].AsFloat();
                data.backgroundColor[1] = bg->arrayVal[1].AsFloat();
                data.backgroundColor[2] = bg->arrayVal[2].AsFloat();
                data.backgroundColor[3] = bg->arrayVal.size() >= 4u ? bg->arrayVal[3].AsFloat() : 1.f;
                data.hasBackgroundColor = true;
            }
            if (const auto* amb = env->Get("ambientColor"); amb && amb->IsArray() && amb->arrayVal.size() >= 3u)
            {
                data.ambientColor = {amb->arrayVal[0].AsFloat(),
                                     amb->arrayVal[1].AsFloat(),
                                     amb->arrayVal[2].AsFloat()};
                data.hasAmbientColor = true;
            }
            if (const auto* ambI = env->Get("ambientIntensity"); ambI && ambI->IsNumber())
            {
                data.ambientIntensity = std::max(0.0f, ambI->AsFloat());
                data.hasAmbientIntensity = true;
            }
        }
    }

    if (IsComponentSerializedScene(root))
    {
        data.componentScene = true;
        data.sourceJson = json;
        data.valid = true;
        return data;
    }

    const auto* arr = root.Get("entities");
    if (!arr || !arr->IsArray()) { data.valid = true; return data; }

    for (const auto& e : arr->arrayVal)
    {
        KromSceneEntityDesc desc;

        if (const auto* n   = e.Get("name"))           desc.name      = n->AsString();
        if (const auto* g   = e.Get("guid"))           desc.guid      = g->AsString();
        if (const auto* p   = e.Get("persistent"))     desc.persistent= p->AsBool();
        if (const auto* pos = e.Get("position"))       desc.position  = ReadVec3Array(*pos);
        if (const auto* rot = e.Get("rotationEulerDeg")) desc.rotationEulerDeg = ReadVec3Array(*rot);
        if (const auto* sc  = e.Get("scale"))          desc.scale     = ReadVec3Array(*sc);
        if (const auto* m   = e.Get("model"))          desc.modelPath = m->AsString();
        if (const auto* mat = e.Get("material"))       desc.materialPath = mat->AsString();

        if (const auto* light = e.Get("light"))
        {
            desc.hasLight = true;
            if (const auto* t  = light->Get("type"))
            {
                const std::string& ts = t->AsString();
                if      (ts == "point") desc.lightType = 1;
                else if (ts == "spot")  desc.lightType = 2;
                else                    desc.lightType = 0;
            }
            if (const auto* c   = light->Get("color"))             desc.lightColor            = ReadVec3Array(*c);
            if (const auto* i   = light->Get("intensity"))         desc.lightIntensity         = i->AsFloat();
            if (const auto* r   = light->Get("range"))             { desc.lightRange           = r->AsFloat(); desc.lightRangeSpecified = true; }
            if (const auto* si  = light->Get("spotInnerDeg"))      desc.lightSpotInnerDeg      = si->AsFloat();
            if (const auto* so  = light->Get("spotOuterDeg"))      desc.lightSpotOuterDeg      = so->AsFloat();
            if (const auto* cs  = light->Get("castShadows"))       desc.lightCastShadows       = cs->AsBool();
            if (const auto* sr  = light->Get("shadowResolution"))  desc.lightShadowRes         = sr->AsUint();
            if (const auto* sb  = light->Get("shadowBias"))        desc.lightShadowBias        = sb->AsFloat();
            if (const auto* snb = light->Get("shadowNormalBias"))  desc.lightShadowNormalBias  = snb->AsFloat();
            if (const auto* smd = light->Get("shadowMaxDistance")) { desc.lightShadowMaxDist    = smd->AsFloat(); desc.lightShadowMaxDistSpecified = true; }
            if (const auto* ss  = light->Get("shadowStrength"))    desc.lightShadowStrength    = ss->AsFloat();
            if (const auto* scc = light->Get("shadowCascadeCount"))  desc.lightCascadeCount    = scc->AsUint();
            if (const auto* scl = light->Get("shadowCascadeLambda")) desc.lightCascadeLambda   = scl->AsFloat();
            if (!desc.lightRangeSpecified && desc.lightType != 0)
                desc.lightRange = 12.0f;
            if (!desc.lightShadowMaxDistSpecified && desc.lightType != 0)
                desc.lightShadowMaxDist = desc.lightRange;
        }

        if (const auto* cam = e.Get("camera"))
        {
            desc.hasCamera = true;
            if (const auto* f  = cam->Get("fovYDeg"))        desc.cameraFovYDeg        = f->AsFloat();
            if (const auto* n  = cam->Get("nearPlane"))      desc.cameraNear           = n->AsFloat();
            if (const auto* f2 = cam->Get("farPlane"))       desc.cameraFar            = f2->AsFloat();
            if (const auto* im = cam->Get("isMain"))         desc.cameraIsMain         = im->AsBool();
            if (const auto* bm = cam->Get("backgroundMode")) desc.cameraBackgroundMode = static_cast<engine::BackgroundMode>(bm->AsUint());
            {
                const auto* cc = cam->Get("clearColor");
                if (!cc) cc = cam->Get("solidColor");
                if (cc && cc->IsArray() && cc->arrayVal.size() >= 4)
                    for (size_t i = 0; i < 4; ++i) desc.cameraClearColor[i] = cc->At(i).AsFloat();
            }
        }

        if (const auto* scripts = e.Get("scripts"))
        {
            if (scripts->IsArray())
            {
                for (const auto& s : scripts->arrayVal)
                {
                    KromSceneScriptDesc scriptDesc;
                    if (s.IsString())
                    {
                        scriptDesc.className = s.AsString();
                    }
                    else if (s.IsObject())
                    {
                        if (const auto* cls = s.Get("class"))
                            scriptDesc.className = cls->AsString();
                        if (scriptDesc.className.empty())
                            if (const auto* cls = s.Get("className"))
                                scriptDesc.className = cls->AsString();

                        if (const auto* fields = s.Get("fields"))
                        {
                            if (fields->IsObject())
                            {
                                for (const auto& [fieldName, fieldValue] : fields->objectVal)
                                    scriptDesc.fields[fieldName] = fieldValue;
                            }
                        }
                    }

                    if (!scriptDesc.className.empty())
                        desc.scripts.push_back(std::move(scriptDesc));
                }
            }
        }

        data.entities.push_back(std::move(desc));
    }
    data.valid = true;
    return data;
}

// =============================================================================
// Scene Entity-Instantiierung
// =============================================================================

static void DetachEntityFromParent(engine::ecs::World& world, LPENTITY entity);

static void DestroyEntityRecursive(engine::ecs::World& world, LPENTITY entity)
{
    if (!world.IsAlive(entity))
        return;
    if (const auto* children = world.Get<engine::ChildrenComponent>(entity))
    {
        const auto copy = children->children;
        for (const LPENTITY child : copy)
            DestroyEntityRecursive(world, child);
    }
    DetachEntityFromParent(world, entity);
    world.DestroyEntity(entity);
}

static void DetachEntityFromParent(engine::ecs::World& world, LPENTITY entity)
{
    if (!world.IsAlive(entity) || !world.Has<engine::ParentComponent>(entity))
        return;

    const LPENTITY parent = world.Get<engine::ParentComponent>(entity)->parent;
    if (world.IsAlive(parent) && world.Has<engine::ChildrenComponent>(parent))
        world.Get<engine::ChildrenComponent>(parent)->Remove(entity);
    world.Remove<engine::ParentComponent>(entity);
}

struct CapturedTransform
{
    engine::math::Vec3 position{0.0f, 0.0f, 0.0f};
    engine::math::Quat rotation = engine::math::Quat::Identity();
    engine::math::Vec3 scale{1.0f, 1.0f, 1.0f};
};

static CapturedTransform CaptureWorldTransform(State& state, LPENTITY entity)
{
    CapturedTransform result{};
    if (!state.world || !state.world->IsAlive(entity))
        return result;

    state.transformSystem.Update(*state.world);
    if (const auto* worldTransform = state.world->Get<engine::WorldTransformComponent>(entity))
    {
        result.position = worldTransform->position;
        result.rotation = worldTransform->rotation;
        result.scale = worldTransform->scale;
        return result;
    }
    if (const auto* transform = state.world->Get<engine::TransformComponent>(entity))
    {
        result.position = transform->localPosition;
        result.rotation = transform->localRotation;
        result.scale = transform->localScale;
    }
    return result;
}

static bool IsDescendantOf(const engine::ecs::World& world, LPENTITY entity, LPENTITY possibleAncestor)
{
    LPENTITY cursor = entity;
    uint32_t depth = 0u;
    while (world.IsAlive(cursor) && depth < 1024u)
    {
        if (cursor == possibleAncestor)
            return true;
        const auto* parent = world.Get<engine::ParentComponent>(cursor);
        if (!parent || !parent->parent.IsValid())
            return false;
        cursor = parent->parent;
        ++depth;
    }
    return false;
}

static void ApplyLocalFromWorld(State& state, LPENTITY entity, const CapturedTransform& worldTransform)
{
    if (!state.world || !state.world->IsAlive(entity))
        return;

    auto* transform = state.world->Get<engine::TransformComponent>(entity);
    if (!transform)
        return;

    const auto* parent = state.world->Get<engine::ParentComponent>(entity);
    if (parent && parent->parent.IsValid() && state.world->IsAlive(parent->parent))
    {
        state.transformSystem.Update(*state.world);
        if (const auto* parentWorld = state.world->Get<engine::WorldTransformComponent>(parent->parent))
        {
            const engine::math::Quat inverseParentRotation = parentWorld->rotation.Conjugate().Normalized();
            engine::math::Vec3 localPosition = inverseParentRotation.Rotate(worldTransform.position - parentWorld->position);
            if (transform->inheritParentScale)
            {
                localPosition.x = std::abs(parentWorld->scale.x) > engine::math::EPSILON
                    ? localPosition.x / parentWorld->scale.x : localPosition.x;
                localPosition.y = std::abs(parentWorld->scale.y) > engine::math::EPSILON
                    ? localPosition.y / parentWorld->scale.y : localPosition.y;
                localPosition.z = std::abs(parentWorld->scale.z) > engine::math::EPSILON
                    ? localPosition.z / parentWorld->scale.z : localPosition.z;
            }

            transform->localPosition = localPosition;
            transform->localRotation = (inverseParentRotation * worldTransform.rotation).Normalized();
            transform->localScale = transform->inheritParentScale
                ? engine::math::Vec3{
                    std::abs(parentWorld->scale.x) > engine::math::EPSILON ? worldTransform.scale.x / parentWorld->scale.x : worldTransform.scale.x,
                    std::abs(parentWorld->scale.y) > engine::math::EPSILON ? worldTransform.scale.y / parentWorld->scale.y : worldTransform.scale.y,
                    std::abs(parentWorld->scale.z) > engine::math::EPSILON ? worldTransform.scale.z / parentWorld->scale.z : worldTransform.scale.z}
                : worldTransform.scale;
            transform->dirty = true;
            return;
        }
    }

    transform->localPosition = worldTransform.position;
    transform->localRotation = worldTransform.rotation;
    transform->localScale = worldTransform.scale;
    transform->dirty = true;
}

static void MarkPersistentRecursive(State& state, LPENTITY entity)
{
    if (!state.world || !state.world->IsAlive(entity))
        return;

    state.persistentEntityIds.insert(entity.value);
    if (const auto* children = state.world->Get<engine::ChildrenComponent>(entity))
    {
        for (const LPENTITY child : children->children)
            MarkPersistentRecursive(state, child);
    }
}

static void PreservePersistentSubtrees(engine::ecs::World& world,
                                       LPENTITY entity,
                                       const std::unordered_set<uint32_t>& persistentEntityIds)
{
    if (!world.IsAlive(entity))
        return;

    if (const auto* children = world.Get<engine::ChildrenComponent>(entity))
    {
        const auto copy = children->children;
        for (const LPENTITY child : copy)
        {
            if (!world.IsAlive(child))
                continue;

            if (persistentEntityIds.count(child.value) != 0u)
            {
                DetachEntityFromParent(world, child);
                continue;
            }

            PreservePersistentSubtrees(world, child, persistentEntityIds);
        }
    }
}

static LPENTITY InstantiateSceneEntity(State& state, const KromSceneEntityDesc& desc)
{
    LPENTITY entity = NULL_LPENTITY;

    if (!desc.modelPath.empty())
    {
        entity = CreateEntityFromAsset(desc.modelPath.c_str(),
                                       desc.name.empty() ? nullptr : desc.name.c_str());
        if (entity.IsValid())
        {
            SetPosition(entity, desc.position);
            SetRotationEulerDeg(entity,
                desc.rotationEulerDeg.x, desc.rotationEulerDeg.y, desc.rotationEulerDeg.z);
            SetScale(entity, desc.scale);

            if (!desc.materialPath.empty())
            {
                const LPMATERIAL mat = LoadMaterial(desc.materialPath.c_str());
                if (mat != NULL_MATERIAL)
                    SetMaterialRecursive(entity, mat);
            }
        }
    }
    else if (desc.hasLight)
    {
        entity = CreateEntity(desc.name.empty() ? "Light" : desc.name.c_str());
        if (entity.IsValid())
        {
            SetPosition(entity, desc.position);
            SetRotationEulerDeg(entity,
                desc.rotationEulerDeg.x, desc.rotationEulerDeg.y, desc.rotationEulerDeg.z);

            engine::LightComponent light{};
            switch (desc.lightType)
            {
            case 1:  light.type = engine::LightType::Point; break;
            case 2:  light.type = engine::LightType::Spot;  break;
            default: light.type = engine::LightType::Directional; break;
            }
            light.color     = desc.lightColor;
            light.intensity = desc.lightIntensity;
            light.range     = desc.lightRange;
            light.spotInnerDeg = desc.lightSpotInnerDeg;
            light.spotOuterDeg = desc.lightSpotOuterDeg;
            light.castShadows = desc.lightCastShadows;
            light.shadowSettings.enabled     = desc.lightCastShadows;
            light.shadowSettings.resolution  = desc.lightShadowRes;
            light.shadowSettings.bias        = desc.lightShadowBias;
            light.shadowSettings.normalBias  = desc.lightShadowNormalBias;
            light.shadowSettings.maxDistance = desc.lightShadowMaxDist;
            light.shadowSettings.strength    = desc.lightShadowStrength;
            light.shadowSettings.cascadeCount  = std::max(1u, desc.lightCascadeCount);
            light.shadowSettings.cascadeLambda = desc.lightCascadeLambda;
            state.world->Add<engine::LightComponent>(entity, light);
        }
    }
    else if (desc.hasCamera)
    {
        entity = CreateCamera(
            desc.name.empty() ? "Camera" : desc.name.c_str(),
            desc.position,
            desc.cameraFovYDeg,
            desc.cameraIsMain);
        if (entity.IsValid())
        {
            SetRotationEulerDeg(entity,
                desc.rotationEulerDeg.x, desc.rotationEulerDeg.y, desc.rotationEulerDeg.z);
            if (auto* cam = state.world->Get<engine::CameraComponent>(entity))
            {
                cam->nearPlane        = desc.cameraNear;
                cam->farPlane         = desc.cameraFar;
                cam->backgroundMode   = desc.cameraBackgroundMode;
                cam->clearColor       = desc.cameraClearColor;
            }
        }
    }
    else if (!desc.name.empty())
    {
        entity = CreateEntity(desc.name.c_str());
        if (entity.IsValid())
        {
            SetPosition(entity, desc.position);
            SetRotationEulerDeg(entity,
                desc.rotationEulerDeg.x, desc.rotationEulerDeg.y, desc.rotationEulerDeg.z);
            SetScale(entity, desc.scale);
        }
    }

    if (entity.IsValid() && !desc.guid.empty())
    {
        if (auto* guid = state.world->Get<engine::GuidComponent>(entity))
            guid->guid = desc.guid;
        else
            state.world->Add<engine::GuidComponent>(entity, engine::GuidComponent{desc.guid});
    }

    if (entity.IsValid() && !desc.scripts.empty())
    {
        engine::script::ScriptList sl;
        sl.SetOwnerEntity(entity);
        for (const auto& scriptDesc : desc.scripts)
        {
            if (!sl.Add(scriptDesc.className, entity, state.scriptAddonRegistry))
                continue;

            auto& instances = sl.Instances_Mutable();
            if (instances.empty())
                continue;

            engine::script::ScriptInstance& inst = instances.back();
            if (const auto* fields = state.scriptAddonRegistry.GetFields(inst.className))
            {
                for (const engine::script::ScriptFieldMeta& field : *fields)
                {
                    const auto valueIt = scriptDesc.fields.find(field.name);
                    if (valueIt == scriptDesc.fields.end())
                        continue;

                    engine::script::ScriptFieldValue value{};
                    if (ReadScriptFieldValueFromJson(valueIt->second, field.type, value))
                        inst.fieldValues[field.name] = value;
                }
            }
            sl.ApplyStoredFieldValues(instances.size() - 1u, state.scriptAddonRegistry);
        }
        state.world->Add<engine::script::ScriptList>(entity, std::move(sl));
    }

    return entity;
}

static void UnloadSceneById(State& state, uint32_t sceneId)
{
    auto it = state.scenes.find(sceneId);
    if (it == state.scenes.end())
        return;
    SceneRecord& record = it->second;
    if (state.world)
    {
        for (const LPENTITY entity : record.rootEntities)
        {
            if (!state.world->IsAlive(entity))
                continue;
            if (state.persistentEntityIds.count(entity.value))
                continue;
            PreservePersistentSubtrees(*state.world, entity, state.persistentEntityIds);
            DestroyEntityRecursive(*state.world, entity);
        }
    }
    if (record.active && !record.name.empty())
        state.eventBus.Emit<engine::events::SceneUnloadedEvent>(record.name);
    state.scenes.erase(it);
}

static void RebuildChildrenComponentsForLoadedEntities(State& state,
                                                       const std::vector<LPENTITY>& loadedEntities)
{
    if (!state.world)
        return;

    for (const LPENTITY entity : loadedEntities)
    {
        if (!state.world->IsAlive(entity))
            continue;

        const auto* parent = state.world->Get<engine::ParentComponent>(entity);
        if (!parent || !parent->parent.IsValid() || !state.world->IsAlive(parent->parent))
            continue;

        if (!state.world->Has<engine::ChildrenComponent>(parent->parent))
            state.world->Add<engine::ChildrenComponent>(parent->parent);
        state.world->Get<engine::ChildrenComponent>(parent->parent)->Add(entity);
    }
}

static void EnsureRuntimeTransformComponents(State& state,
                                             const std::vector<LPENTITY>& loadedEntities)
{
    if (!state.world)
        return;

    for (const LPENTITY entity : loadedEntities)
    {
        if (!state.world->IsAlive(entity))
            continue;

        auto* transform = state.world->Get<engine::TransformComponent>(entity);
        if (!transform)
            continue;

        if (!state.world->Has<engine::WorldTransformComponent>(entity))
            state.world->Add<engine::WorldTransformComponent>(entity);
        transform->dirty = true;
    }
}

static void ResolveSerializedSceneAssetBindings(State& state,
                                                const std::vector<LPENTITY>& loadedEntities)
{
    if (!state.world || !state.assetPipeline)
        return;

    engine::addons::mesh_renderer::ResolveMeshAssetBindings(
        *state.assetPipeline,
        state.assetRegistry,
        *state.world);

    for (const LPENTITY entity : loadedEntities)
    {
        if (!state.world->IsAlive(entity))
            continue;

        if (const auto* mesh = state.world->Get<engine::MeshComponent>(entity))
        {
            if (!mesh->mesh.IsValid() && mesh->meshAssetPath.empty())
            {
                const auto* name = state.world->Get<engine::NameComponent>(entity);
                engine::Debug::LogWarning(
                    "krom.h: Entity '%s' hat MeshComponent ohne meshAssetPath; "
                    "alte meshHandle-Werte koennen in der Runtime nicht neu geladen werden",
                    name ? name->name.c_str() : "<unnamed>");
            }
        }

        auto* material = state.world->Get<engine::MaterialComponent>(entity);
        if (!material)
            continue;

        if (!material->materialAssetPath.empty())
        {
            const LPMATERIAL assetMaterial = LoadMaterial(material->materialAssetPath.c_str());
            if (assetMaterial.IsValid())
                material->material = RealizeMaterialForEntity(state, entity, assetMaterial);
        }

        for (engine::MaterialComponent::SlotOverride& slot : material->slotOverrides)
        {
            if (slot.materialAssetPath.empty())
                continue;

            const LPMATERIAL assetMaterial = LoadMaterial(slot.materialAssetPath.c_str());
            if (assetMaterial.IsValid())
                slot.material = RealizeMaterialForEntity(state, entity, assetMaterial);
        }
    }
}

static LPENTITY InstantiatePrefabForScript(State& state,
                                           std::string_view prefabAssetPath,
                                           const engine::math::Vec3& position,
                                           const engine::math::Quat& rotation,
                                           const engine::math::Vec3& scale)
{
    if (!state.world || prefabAssetPath.empty())
        return NULL_LPENTITY;

    std::filesystem::path path{std::string(prefabAssetPath)};
    if (path.is_relative() && !state.assetRoot.empty())
        path = state.assetRoot / path;

    engine::addons::prefab::PrefabAsset prefab;
    std::string error;
    if (!engine::addons::prefab::LoadPrefabFromFile(path, state.assetRegistry, prefab, &error))
    {
        engine::Debug::LogError("krom.h: Prefab konnte nicht geladen werden '%s': %s",
            path.string().c_str(),
            error.c_str());
        return NULL_LPENTITY;
    }

    engine::addons::prefab::PrefabInstantiateOptions options{};
    options.position = position;
    options.rotation = rotation;
    options.scale = scale;
    options.scriptRegistry = &state.scriptAddonRegistry;

    const engine::addons::prefab::PrefabInstance instance =
        engine::addons::prefab::InstantiatePrefab(*state.world, prefab, options);
    if (!instance.IsValid())
    {
        engine::Debug::LogError("krom.h: Prefab konnte nicht instanziiert werden '%s'",
            path.string().c_str());
        return NULL_LPENTITY;
    }

    if (state.assetPipeline)
    {
        engine::addons::mesh_renderer::ResolveMeshAssetBindings(
            *state.assetPipeline,
            state.assetRegistry,
            *state.world);
    }

    for (const LPENTITY entity : instance.entities)
    {
        if (!entity.IsValid() || !state.world->IsAlive(entity))
            continue;
        if (!state.world->Has<engine::WorldTransformComponent>(entity))
            state.world->Add<engine::WorldTransformComponent>(entity);
        if (auto* transform = state.world->Get<engine::TransformComponent>(entity))
        {
            transform->dirty = true;
            ++transform->localVersion;
        }
        if (state.world->Has<engine::MeshComponent>(entity))
            engine::mesh_renderer::UpdateLocalBoundsForEntity(*state.world, entity, state.assetRegistry);
    }

    state.transformSystem.Update(*state.world);
    state.boundsSystem.Update(*state.world);
    return instance.root;
}

static const char* ScriptFieldTypeName(engine::script::ScriptFieldType type) noexcept
{
    switch (type)
    {
    case engine::script::ScriptFieldType::Float:  return "Float";
    case engine::script::ScriptFieldType::Int:    return "Int";
    case engine::script::ScriptFieldType::Bool:   return "Bool";
    case engine::script::ScriptFieldType::Vec3:   return "Vec3";
    case engine::script::ScriptFieldType::String: return "String";
    case engine::script::ScriptFieldType::Prefab: return "Prefab";
    case engine::script::ScriptFieldType::Entity: return "Entity";
    }
    return "Unknown";
}

static void WriteRuntimeScriptFieldValue(engine::serialization::JsonWriter& w,
                                         const char* key,
                                         const engine::script::ScriptFieldValue& value)
{
    switch (value.type)
    {
    case engine::script::ScriptFieldType::Float:  w.WriteFloat(key, value.floatValue); break;
    case engine::script::ScriptFieldType::Int:    w.WriteInt(key, value.intValue); break;
    case engine::script::ScriptFieldType::Bool:   w.WriteBool(key, value.boolValue); break;
    case engine::script::ScriptFieldType::Vec3:   w.WriteVec3(key, value.vec3Value); break;
    case engine::script::ScriptFieldType::Entity:
    case engine::script::ScriptFieldType::String:
    case engine::script::ScriptFieldType::Prefab:
        w.WriteString(key, value.stringValue);
        break;
    }
}

static void WriteRuntimeStateSnapshot(State& state)
{
    if (!state.editorLiveStateEnabled || !state.world || state.runtimeStatePath.empty())
        return;

    engine::serialization::JsonWriter w;
    w.BeginObject();
    w.WriteUint("version", 1u);
    w.BeginArray("fields");

    state.world->ForEachAlive([&](LPENTITY entity)
    {
        const auto* guid = state.world->Get<engine::GuidComponent>(entity);
        if (!guid || guid->guid.empty())
            return;

        auto* scripts = state.world->Get<engine::script::ScriptList>(entity);
        if (!scripts)
            return;

        const auto& instances = scripts->Instances();
        for (size_t scriptIndex = 0u; scriptIndex < instances.size(); ++scriptIndex)
        {
            const engine::script::ScriptInstance& inst = instances[scriptIndex];
            if (!inst.script)
                continue;

            const auto* fields = state.scriptAddonRegistry.GetFields(inst.className);
            if (!fields)
                continue;

            for (const engine::script::ScriptFieldMeta& field : *fields)
            {
                engine::script::ScriptFieldValue value{};
                if (!state.scriptAddonRegistry.ReadField(*inst.script, inst.className, field.name, value))
                    continue;
                if (value.type == engine::script::ScriptFieldType::Entity &&
                    value.entityValue.IsValid())
                {
                    if (const auto* targetGuid = state.world->Get<engine::GuidComponent>(value.entityValue))
                        value.stringValue = targetGuid->guid;
                }

                w.BeginObject();
                w.WriteString("entityGuid", guid->guid);
                w.WriteUint("scriptIndex", static_cast<uint32_t>(scriptIndex));
                w.WriteString("script", inst.className);
                w.WriteString("field", field.name);
                w.WriteString("type", ScriptFieldTypeName(value.type));
                WriteRuntimeScriptFieldValue(w, "value", value);
                w.EndObject();
            }
        }
    });

    w.EndArray();
    w.EndObject();

    std::error_code ec;
    std::filesystem::create_directories(state.runtimeStatePath.parent_path(), ec);
    if (ec)
        return;

    const std::filesystem::path tmpPath = state.runtimeStatePath.string() + ".tmp";
    {
        std::ofstream out(tmpPath, std::ios::binary | std::ios::trunc);
        if (!out)
            return;
        out << w.GetString();
        if (!out.good())
            return;
    }
    std::filesystem::rename(tmpPath, state.runtimeStatePath, ec);
    if (ec)
    {
        std::filesystem::remove(state.runtimeStatePath, ec);
        ec.clear();
        std::filesystem::rename(tmpPath, state.runtimeStatePath, ec);
    }
}

static bool LoadComponentSerializedScene(State& state,
                                         const KromSceneData& data,
                                         SceneRecord& record)
{
    engine::serialization::SceneDeserializer deserializer(*state.world);
    deserializer.RegisterDefaultHandlers();
    engine::addons::camera::RegisterCameraDeserializationHandlers(deserializer);
    engine::addons::lighting::RegisterLightingDeserializationHandlers(deserializer);
    engine::addons::mesh_renderer::RegisterMeshRendererDeserializationHandlers(deserializer);
    engine::script::RegisterScriptDeserializationHandlers(deserializer, &state.scriptAddonRegistry);

    engine::serialization::DeserializeResult result =
        deserializer.DeserializeFromJson(data.sourceJson);
    if (!result.success)
    {
        engine::Debug::LogError("krom.h: serialized scene '%s' konnte nicht geladen werden: %s",
            data.name.c_str(), result.error.c_str());
        return false;
    }

    std::vector<LPENTITY> loadedEntities;
    loadedEntities.reserve(result.entityRemap.size());
    for (const auto& [oldId, newId] : result.entityRemap)
    {
        (void)oldId;
        if (newId.IsValid() && state.world->IsAlive(newId))
            loadedEntities.push_back(newId);
    }

    RebuildChildrenComponentsForLoadedEntities(state, loadedEntities);
    EnsureRuntimeTransformComponents(state, loadedEntities);
    ResolveSerializedSceneAssetBindings(state, loadedEntities);
    engine::script::ResolveScriptEntityReferences(*state.world, state.scriptAddonRegistry);

    for (const LPENTITY entity : loadedEntities)
    {
        if (!state.world->IsAlive(entity))
            continue;
        if (!state.world->Has<engine::ParentComponent>(entity))
            record.rootEntities.push_back(entity);
        if (data.persistent)
            MarkPersistentRecursive(state, entity);
    }

    engine::Debug::Log("krom.h: serialized scene '%s' geladen (%u entities, %u components)",
        data.name.c_str(), result.entitiesRead, result.componentsRead);
    return true;
}

static void ApplySceneEnvironment(State& state, const KromSceneData& data)
{
    // ── Ambient ───────────────────────────────────────────────────────────────
    if (data.hasAmbientColor)    state.cameraOptions.ambientColor    = data.ambientColor;
    if (data.hasAmbientIntensity) state.cameraOptions.ambientIntensity = data.ambientIntensity;

    // ── IBL-Modus von der Game-Main-Kamera ableiten ───────────────────────────
    // Der Editor-IBL-Toggle ist unabhängig von der Game-Kamera.
    // Im Wrapper bestimmt ausschließlich backgroundMode der Main-Kamera ob IBL aktiv ist.
    bool gameCameraWantsSkybox = false;
    if (state.world)
    {
        state.world->ForEachAlive([&](engine::EntityID id)
        {
            const auto* cam = state.world->Get<engine::CameraComponent>(id);
            if (cam && cam->isMainCamera)
                gameCameraWantsSkybox = (cam->backgroundMode == engine::BackgroundMode::Skybox);
        });
    }

    // ── Environment-Textur laden ──────────────────────────────────────────────
    if (data.hasEnvironment && !data.environmentTexturePath.empty() && state.assetPipeline)
    {
        const LPTEXTURE sourceTexture = LoadTexture(data.environmentTexturePath.c_str());
        if (sourceTexture.IsValid())
        {
            state.assetPipeline->UploadPendingGpuAssets();

            engine::renderer::EnvironmentDesc envDesc{};
            envDesc.mode          = engine::renderer::EnvironmentMode::Texture;
            envDesc.sourceTexture = sourceTexture;
            envDesc.intensity     = data.environmentIntensity;
            envDesc.enableIBL     = data.environmentEnableIBL;

            engine::renderer::RenderSystem& rs = state.renderLoop.GetRenderSystem();
            const engine::renderer::EnvironmentHandle next = rs.CreateEnvironment(envDesc);
            if (next.IsValid())
            {
                const engine::renderer::EnvironmentHandle prev = state.activeEnvironment;
                state.activeEnvironment = next;
                rs.SetActiveEnvironment(next);
                if (prev.IsValid()) rs.DestroyEnvironment(prev);

                engine::Debug::Log("krom.h: environment '%s' intensity=%.2f ibl=%s (von Game-Kamera)",
                    data.environmentTexturePath.c_str(), data.environmentIntensity,
                    gameCameraWantsSkybox ? "true" : "false");
            }
            else
            {
                engine::Debug::LogWarning("krom.h: environment konnte nicht erstellt werden: %s",
                    data.environmentTexturePath.c_str());
            }
        }
        else
        {
            engine::Debug::LogWarning("krom.h: environment-Textur nicht gefunden: %s",
                data.environmentTexturePath.c_str());
        }
    }

    // ── Kamera-Hintergrund ────────────────────────────────────────────────────
    // backgroundMode und clearColor kommen direkt aus der Entity-Deserialisierung —
    // kein Override nötig.
}

static LPSCENE LoadSceneFromData(State& state, KromSceneData data, bool additive,
                                 const char* filePath = nullptr)
{
    if (!data.valid || !state.world)
        return NULL_SCENE;

    if (!additive)
    {
        // Alle nicht-persistenten Scenes entladen
        std::vector<uint32_t> toUnload;
        for (const auto& [id, scene] : state.scenes)
            if (!scene.persistent)
                toUnload.push_back(id);
        for (const uint32_t id : toUnload)
            UnloadSceneById(state, id);
    }

    const uint32_t sceneId = state.nextSceneId++;
    SceneRecord& record = state.scenes[sceneId];
    record.name       = data.name;
    record.path       = (filePath && filePath[0] != '\0') ? filePath : data.name;
    record.persistent = data.persistent;
    record.active     = true;

    if (data.componentScene)
    {
        if (!LoadComponentSerializedScene(state, data, record))
        {
            state.scenes.erase(sceneId);
            return NULL_SCENE;
        }
    }
    else
    {
        for (const auto& desc : data.entities)
        {
            const LPENTITY entity = InstantiateSceneEntity(state, desc);
            if (entity.IsValid())
            {
                record.rootEntities.push_back(entity);
                if (desc.persistent || data.persistent)
                    MarkPersistentRecursive(state, entity);
            }
        }
    }

    state.transformSystem.Update(*state.world);
    state.boundsSystem.Update(*state.world);
    engine::script::ResolveScriptEntityReferences(*state.world, state.scriptAddonRegistry);
    ApplySceneEnvironment(state, data);
    if (!record.name.empty())
        state.eventBus.Emit<engine::events::SceneLoadedEvent>(record.name);

    engine::Debug::Log("krom.h: scene '%s' geladen (%zu entities, persistent=%s)",
        data.name.c_str(), record.rootEntities.size(),
        record.persistent ? "true" : "false");

    return LPSCENE{sceneId};
}

static KromSceneData LoadKsceneFile(const std::filesystem::path& assetRoot,
                                     const char* path)
{
    const std::filesystem::path resolved =
        std::filesystem::weakly_canonical(assetRoot / path);
    std::ifstream file(resolved);
    if (!file)
    {
        engine::Debug::LogError("krom.h: kscene nicht gefunden: %s",
            resolved.string().c_str());
        return KromSceneData{};
    }
    const std::string json(std::istreambuf_iterator<char>(file),
                           std::istreambuf_iterator<char>{});
    return ParseKscene(json);
}

static std::string NormalizeProjectScenePath(std::string scenePath,
                                             const std::string& assetRootName)
{
    std::replace(scenePath.begin(), scenePath.end(), '\\', '/');
    if (scenePath.empty())
        return {};

    std::string assetPrefix = assetRootName;
    std::replace(assetPrefix.begin(), assetPrefix.end(), '\\', '/');
    if (!assetPrefix.empty() && assetPrefix.back() != '/')
        assetPrefix.push_back('/');
    if (!assetPrefix.empty() &&
        scenePath.size() > assetPrefix.size() &&
        scenePath.compare(0u, assetPrefix.size(), assetPrefix) == 0)
    {
        scenePath.erase(0u, assetPrefix.size());
    }

    const std::filesystem::path path(scenePath);
    if (scenePath.find('/') == std::string::npos &&
        scenePath.find('\\') == std::string::npos)
    {
        return (std::filesystem::path("Scenes") / path)
            .replace_extension(".json")
            .generic_string();
    }

    if (!path.has_extension())
        return std::filesystem::path(scenePath).replace_extension(".json").generic_string();
    return std::filesystem::path(scenePath).generic_string();
}

static bool ReadProjectFile(State& state, const char* projectFilePath)
{
    if (!projectFilePath || projectFilePath[0] == '\0')
        return false;

    std::filesystem::path projectPath(projectFilePath);
    if (std::filesystem::is_directory(projectPath))
        projectPath /= "krom-project.json";

    std::ifstream file(projectPath);
    if (!file)
    {
        engine::Debug::LogError("krom.h: project file not found: %s",
            projectPath.string().c_str());
        return false;
    }

    const std::string json(std::istreambuf_iterator<char>(file),
                           std::istreambuf_iterator<char>{});
    std::string err;
    const engine::serialization::JsonValue root =
        engine::serialization::JsonParser::Parse(json, err);
    if (!err.empty() || !root.IsObject())
    {
        engine::Debug::LogError("krom.h: project JSON error in '%s': %s",
            projectPath.string().c_str(),
            err.empty() ? "root is not an object" : err.c_str());
        return false;
    }

    state.projectRoot = std::filesystem::weakly_canonical(projectPath.parent_path());
    state.runtimeStatePath = state.projectRoot / "editor" / "runtime-state.json";
    state.runtimeStateWriteTimer = 0.0f;
    if (state.editorLiveStateEnabled)
    {
        std::error_code runtimeStateEc;
        std::filesystem::create_directories(state.runtimeStatePath.parent_path(), runtimeStateEc);
        std::filesystem::remove(state.runtimeStatePath, runtimeStateEc);
    }

    std::string assetRootName = "Assets";
    if (const auto* assetRoot = root.Get("assetRoot"); assetRoot && assetRoot->IsString())
        assetRootName = assetRoot->AsString();
    SetAssetRoot(state.projectRoot / assetRootName);

    if (const auto* shaderCache = root.Get("shaderCache"); shaderCache && shaderCache->IsString())
        SetShaderCacheDir(state.projectRoot / shaderCache->AsString());

    state.projectScenePaths.clear();
    if (const auto* order = root.Get("sceneOrder"); order && order->IsArray())
    {
        for (const engine::serialization::JsonValue& item : order->arrayVal)
        {
            if (!item.IsString())
                continue;
            std::string normalized = NormalizeProjectScenePath(item.AsString(), assetRootName);
            if (!normalized.empty())
                state.projectScenePaths.push_back(std::move(normalized));
        }
    }

    if (state.projectScenePaths.empty())
    {
        if (const auto* editorScene = root.Get("editorScene"); editorScene && editorScene->IsString())
        {
            std::string normalized = NormalizeProjectScenePath(editorScene->AsString(), assetRootName);
            if (!normalized.empty())
                state.projectScenePaths.push_back(std::move(normalized));
        }
    }

    engine::Debug::Log("krom.h: project loaded '%s' assetRoot='%s' scenes=%zu",
        projectPath.string().c_str(),
        state.assetRoot.string().c_str(),
        state.projectScenePaths.size());
    return true;
}

// =============================================================================

void ApplyMaterialRecursive(State& state, engine::ecs::World& world, LPENTITY entity, LPMATERIAL material)
{
    if (!world.IsAlive(entity))
        return;

    if (world.Has<engine::MeshComponent>(entity))
    {
        const LPMATERIAL runtimeMat = RealizeMaterialForEntity(state, entity, material);
        if (!world.Has<engine::MaterialComponent>(entity))
            world.Add<engine::MaterialComponent>(entity, engine::MaterialComponent{runtimeMat});
        else
            world.Get<engine::MaterialComponent>(entity)->material = runtimeMat;
    }

    if (const auto* children = world.Get<engine::ChildrenComponent>(entity))
    {
        for (const LPENTITY child : children->children)
            ApplyMaterialRecursive(state, world, child, material);
    }
}

bool BundleHasSkeletonsOrAnimations(const engine::assets::ImportedAssetBundle& bundle) noexcept
{
    if (!bundle.skeletons.empty() || !bundle.animations.empty())
        return true;
    for (const auto& mesh : bundle.meshes)
    {
        if (MeshHasSkinning(mesh))
            return true;
    }
    return false;
}

void EnsureAnimationSystems(State& state)
{
    if (!state.animationSystem)
        state.animationSystem = std::make_unique<engine::addons::animation::AnimationSystem>(state.assetRegistry);
    if (!state.skinningSystem)
    {
        if (engine::renderer::IDevice* device = state.renderLoop.GetRenderSystem().GetDevice())
            state.skinningSystem = std::make_unique<engine::addons::animation::SkinningSystem>(*device);
    }
}

void RegisterEngineComponents(State& state)
{
    if (state.componentsRegistered)
        return;

    engine::RegisterCoreComponents(state.componentRegistry);
    engine::RegisterAnimationComponents(state.componentRegistry);
    engine::RegisterCameraComponents(state.componentRegistry);
    engine::RegisterMeshRendererComponents(state.componentRegistry);
    engine::RegisterLightingComponents(state.componentRegistry);
    engine::ecs::RegisterComponent<engine::script::ScriptList>(state.componentRegistry, "ScriptList");
    state.componentsRegistered = true;
}

bool RegisterEngineFeatures(State& state)
{
    if (state.featuresRegistered)
        return true;

    using namespace engine;
    using BackendType = renderer::DeviceFactory::BackendType;

    state.forwardPlusActive = state.config.enableForwardPlus &&
        ToBackend(state.config.backend) != BackendType::OpenGL;

    renderer::addons::forward::ForwardFeatureConfig forwardConfig{};
    forwardConfig.clearColorValue = state.config.clearColor;
    forwardConfig.enableEnvironmentBackground = true;
    forwardConfig.enableBloom = true;
    forwardConfig.bloomThreshold = 1.0f;
    forwardConfig.bloomIntensity = 0.12f;
    forwardConfig.bloomBlurRadius = 1.0f;
    forwardConfig.mode = state.forwardPlusActive
        ? renderer::addons::forward::ForwardRendererMode::ForwardPlus
        : renderer::addons::forward::ForwardRendererMode::Forward;

    renderer::RenderSystem& renderSystem = state.renderLoop.GetRenderSystem();
    if (!renderSystem.RegisterFeature(addons::mesh_renderer::CreateMeshRendererFeature()) ||
        !renderSystem.RegisterFeature(addons::lighting::CreateLightingFeature()) ||
        !renderSystem.RegisterFeature(addons::shadow::CreateShadowFeature()) ||
        !renderSystem.RegisterFeature(renderer::addons::forward::CreateForwardFeature(forwardConfig)))
    {
        Debug::LogError("krom.h: failed to register core render features");
        return false;
    }

    if (state.config.enableGtao && ToBackend(state.config.backend) != BackendType::OpenGL)
    {
        if (!renderSystem.RegisterFeature(renderer::addons::gtao::CreateGtaoFeature()))
        {
            Debug::LogError("krom.h: failed to register GTAO feature");
            return false;
        }
    }

    state.featuresRegistered = true;
    return true;
}

bool InitializePlatform(State& state)
{
#if defined(KROM_APP_USE_WIN32_PLATFORM)
    state.platform = std::make_unique<engine::platform::win32::Win32Platform>();
#elif defined(KROM_APP_USE_GLFW_PLATFORM)
    state.platform = std::make_unique<engine::platform::GLFWPlatform>();
#endif
    return state.platform && state.platform->Initialize();
}

bool InitializeRenderLoop(State& state)
{
    using BackendType = engine::renderer::DeviceFactory::BackendType;
    const BackendType backend = ToBackend(state.config.backend);

    if (!state.deviceFactoryRegistry.IsRegistered(backend))
    {
        engine::Debug::LogError("krom.h: backend '%s' is not registered", BackendName(backend));
        return false;
    }

    const auto adapters = state.deviceFactoryRegistry.EnumerateAdapters(backend);
    if (adapters.empty())
    {
        engine::Debug::LogError("krom.h: backend '%s' reported no adapters", BackendName(backend));
        return false;
    }

    engine::platform::WindowDesc windowDesc{};
    windowDesc.title = state.config.title;
    windowDesc.width = state.config.width;
    windowDesc.height = state.config.height;
    windowDesc.windowMode = state.config.windowMode;
    windowDesc.resizable = state.config.resizable;
    windowDesc.vsync = state.config.vsync;
    if (backend == BackendType::OpenGL)
    {
        windowDesc.openglContext = true;
        windowDesc.openglMajor = 4;
        windowDesc.openglMinor = 1;
        windowDesc.openglDebugContext = state.config.enableDebugLayer;
    }

    engine::renderer::IDevice::DeviceDesc deviceDesc{};
    deviceDesc.enableDebugLayer = state.config.enableDebugLayer;
    deviceDesc.adapterIndex = engine::renderer::DeviceFactory::FindBestAdapter(adapters);
    deviceDesc.appName = state.config.title;

    return state.renderLoop.Initialize(backend, *state.platform, windowDesc, &state.eventBus, deviceDesc);
}

bool InitializeAssetPipeline(State& state)
{
    // Asset-Root ist optional — wird erst benoetigt wenn ein Asset geladen wird.
    // Fehlt sie beim ersten Ladeversuch, gibt LoadMesh/LoadTexture usw. einen
    // Fehler aus. Hier kein hartes Abbrechen mehr.

    if (state.engineAssetRoot.empty())
        state.engineAssetRoot = ResolveEngineAssetRootOnDisk();

    state.renderLoop.GetRenderSystem().SetAssetRegistry(&state.assetRegistry);
    state.assetPipeline = std::make_unique<engine::assets::AssetPipeline>(
        state.assetRegistry,
        state.renderLoop.GetRenderSystem().GetDevice());
    engine::mesh_renderer::ConfigureAssetPipeline(*state.assetPipeline);
#if KROM_WRAPPER_HAS_GLTF
    state.assetPipeline->RegisterMeshImporter(
        std::make_unique<engine::addons::gltf::GltfImporter>());
#endif
    state.assetPipeline->SetAssetRoot(state.assetRoot);
    return true;
}

bool InitializeTonemapMaterial(State& state)
{
    // Kein Asset-Root gesetzt — Tonemap-Pass ueberspringen.
    // Das Fenster oeffnet sich trotzdem; Rendering ohne Post-Processing ist moeglich.
    // SetAssetRoot() muss vor Graphics() gesetzt werden wenn Rendering gewuenscht ist.
    if (state.assetRoot.empty())
        return true;

    const char* tonemapVsPath = "fullscreen.vs.hlsl";
    const char* tonemapPsPath = "passthrough.ps.hlsl";
    if (ToBackend(state.config.backend) == engine::renderer::DeviceFactory::BackendType::OpenGL)
    {
        tonemapVsPath = "fullscreen.opengl.vs.glsl";
        tonemapPsPath = "passthrough.opengl.fs.glsl";
    }

    const engine::ShaderHandle tonemapVs =
        LoadEngineShader(state, tonemapVsPath, engine::assets::ShaderStage::Vertex);
    const engine::ShaderHandle tonemapPs =
        LoadEngineShader(state, tonemapPsPath, engine::assets::ShaderStage::Fragment);
    if (!tonemapVs.IsValid() || !tonemapPs.IsValid())
    {
        engine::Debug::LogError("krom.h: failed to load tonemap shaders from asset root '%s'",
            state.assetRoot.string().c_str());
        return false;
    }

    engine::renderer::MaterialParam tonemapSampler{};
    tonemapSampler.name = "linearclamp";
    tonemapSampler.type = engine::renderer::MaterialParam::Type::Sampler;
    tonemapSampler.samplerIdx = 0u;

    const engine::renderer::ISwapchain* swapchain = state.renderLoop.GetRenderSystem().GetSwapchain();
    const engine::renderer::Format backbufferFormat = swapchain
        ? swapchain->GetBackbufferFormat()
        : engine::renderer::Format::BGRA8_UNORM_SRGB;

    engine::renderer::MaterialDesc tonemapDesc{};
    engine::renderer::MaterialRuntimeDesc tonemapRuntime{};
    tonemapDesc.name = "KromTonemap";
    tonemapDesc.domain = engine::renderer::MaterialDomain::Postprocess;
    tonemapDesc.renderPolicy.depth.test = false;
    tonemapDesc.renderPolicy.depth.write = false;
    tonemapDesc.renderPolicy.cull.mode = engine::renderer::MaterialCullMode::None;
    tonemapDesc.renderPolicy.castShadows = false;
    tonemapDesc.renderPolicy.receiveShadows = false;
    tonemapDesc.parameters.push_back(tonemapSampler);

    tonemapRuntime.renderPass = engine::renderer::StandardRenderPasses::Postprocess();
    tonemapRuntime.vertexShader = tonemapVs;
    tonemapRuntime.fragmentShader = tonemapPs;
    tonemapRuntime.colorFormat = backbufferFormat;
    tonemapRuntime.depthFormat = engine::renderer::Format::Unknown;

    const engine::MaterialHandle material =
        engine::renderer::MaterialRuntimeBridge::RegisterMaterial(
            state.materialSystem,
            std::move(tonemapDesc),
            tonemapRuntime);
    state.renderLoop.GetRenderSystem().SetDefaultTonemapMaterial(material, state.materialSystem);
    return material.IsValid();
}

bool EnsureInitialized()
{
    State& state = Get();
    if (state.initialized)
        return true;

    RegisterEngineComponents(state);
    state.world = std::make_unique<engine::ecs::World>(state.componentRegistry);
    state.cameraOptions.ambientColor = state.config.ambientColor;
    state.cameraOptions.ambientIntensity = state.config.ambientIntensity;
#ifdef KROM_HAS_GAME_SCRIPTS
    KromRegisterGameScripts(state.scriptAddonRegistry);
    engine::Debug::Log("krom.h: Projekt-Scripts registriert.");
#endif

    if (!InitializePlatform(state))
    {
        engine::Debug::LogError("krom.h: platform initialization failed");
        return false;
    }
    if (!RegisterEngineFeatures(state))
        return false;
    if (!InitializeRenderLoop(state))
    {
        engine::Debug::LogError("krom.h: render loop initialization failed");
        return false;
    }
    if (!InitializeAssetPipeline(state))
        return false;
    if (!InitializeTonemapMaterial(state))
        return false;

    state.initialized = true;
    state.running = true;
    return true;
}

} // namespace

// =========================================================================
// Ebene 2 + 3 — Component-Zugriff, Systeme, Scripts
// =========================================================================

engine::ecs::ComponentMetaRegistry& GetComponentRegistry()
{
    return Get().componentRegistry;
}

engine::ecs::World& GetWorld()
{
    State& state = Get();
    assert(state.world && "Krom::GetWorld() erfordert vorherigen Graphics3D()-Aufruf");
    return *state.world;
}

void RegisterSystem(SystemFn fn)
{
    Get().userSystems.push_back(std::move(fn));
}

engine::script::ScriptRegistry& GetScriptRegistry()
{
    return Get().scriptAddonRegistry;
}

// =========================================================================

void SetShaderCacheDir(const std::filesystem::path& path)
{
    engine::renderer::ShaderCompiler::SetCacheDirectory(path);
}

void SetAssetRoot(const std::filesystem::path& path)
{
    State& state = Get();
    state.assetRoot = path;
    if (state.assetPipeline)
        state.assetPipeline->SetAssetRoot(path);

    // Cache-Verzeichnis neben das Asset-Root legen, damit Editor und
    // Wrapper mit demselben Asset-Verzeichnis denselben Cache teilen.
    const auto abs = std::filesystem::weakly_canonical(std::filesystem::absolute(path));
    engine::renderer::ShaderCompiler::SetCacheDirectory(abs.parent_path() / "shader_artifacts");
}

const std::filesystem::path& GetAssetRoot()
{
    return Get().assetRoot;
}

bool LoadProject(const char* projectFilePath)
{
    return ReadProjectFile(Get(), projectFilePath);
}

void SetEditorLiveStateEnabled(bool enabled)
{
    State& state = Get();
    state.editorLiveStateEnabled = enabled;
    state.runtimeStateWriteTimer = 0.0f;

    if (!enabled || state.projectRoot.empty())
        return;

    state.runtimeStatePath = state.projectRoot / "editor" / "runtime-state.json";
    std::error_code ec;
    std::filesystem::create_directories(state.runtimeStatePath.parent_path(), ec);
    std::filesystem::remove(state.runtimeStatePath, ec);
}

bool EditorLiveStateEnabled()
{
    return Get().editorLiveStateEnabled;
}

int DefaultRenderer() noexcept
{
#if defined(KROM_APP_BACKEND_DX11)
    return Renderer::DX11;
#elif defined(KROM_APP_BACKEND_OPENGL)
    return Renderer::OpenGL;
#elif defined(KROM_APP_BACKEND_VULKAN)
    return Renderer::Vulkan;
#else
    return Renderer::Vulkan;
#endif
}

const char* RendererName(int backend) noexcept
{
    switch (backend)
    {
    case Renderer::DX11: return "DX11";
    case Renderer::OpenGL: return "OpenGL";
    case Renderer::DX12: return "DX12";
    case Renderer::Vulkan: return "Vulkan";
    default: return "Unknown";
    }
}

bool IsRendererAvailable(int backend)
{
    return Get().deviceFactoryRegistry.IsAvailable(ToBackend(backend));
}

std::vector<AdapterInfo> EnumerateAdapters(int backend)
{
    const auto raw = Get().deviceFactoryRegistry.EnumerateAdapters(ToBackend(backend));
    std::vector<AdapterInfo> result;
    result.reserve(raw.size());
    for (const auto& a : raw)
        result.push_back({a.index, a.name, a.dedicatedVRAM, a.isDiscrete, a.featureLevel});
    return result;
}

void SetConfig(const GraphicsConfig& config)
{
    Get().config = config;
}

bool Graphics(const GraphicsConfig& config)
{
    SetConfig(config);
    return EnsureInitialized();
}

bool Graphics(int backend,
              int width,
              int height,
              const char* title,
              float clearR,
              float clearG,
              float clearB,
              bool resizable)
{
    GraphicsConfig config{};
    config.backend = backend;
    config.width = static_cast<uint32_t>(width);
    config.height = static_cast<uint32_t>(height);
    config.title = title ? title : "KROM";
    config.clearColor = {clearR, clearG, clearB, 1.0f};
    config.resizable = resizable;
    return Graphics(config);
}

bool Graphics(int width,
              int height,
              const char* title,
              float clearR,
              float clearG,
              float clearB,
              bool resizable)
{
    return Graphics(DefaultRenderer(), width, height, title, clearR, clearG, clearB, resizable);
}

void OnInit(InitFn fn)
{
    Get().initCallback = std::move(fn);
}

void OnUpdate(TickFn fn)
{
    Get().tickCallback = std::move(fn);
}

bool KeyDown(Key key)
{
    if (engine::platform::IInput* input = Get().renderLoop.GetInput())
        return input->KeyDown(key);
    return false;
}

bool KeyHit(Key key)
{
    if (engine::platform::IInput* input = Get().renderLoop.GetInput())
        return input->KeyHit(key);
    return false;
}

bool MouseButtonDown(MouseButton button)
{
    if (engine::platform::IInput* input = Get().renderLoop.GetInput())
        return input->MouseButtonDown(button);
    return false;
}

int MouseDeltaX()
{
    if (engine::platform::IInput* input = Get().renderLoop.GetInput())
        return input->MouseDeltaX();
    return 0;
}

int MouseDeltaY()
{
    if (engine::platform::IInput* input = Get().renderLoop.GetInput())
        return input->MouseDeltaY();
    return 0;
}

float MouseScrollDelta()
{
    if (engine::platform::IInput* input = Get().renderLoop.GetInput())
        return input->MouseScrollDelta();
    return 0.0f;
}

void Quit()
{
    State& state = Get();
    state.running = false;
    if (engine::platform::IWindow* window = state.renderLoop.GetWindow())
        window->RequestClose();
}

bool AppRunning()
{
    State& state = Get();
    return state.running && !state.renderLoop.ShouldExit();
}

LPENTITY CreateEntity(const char* name)
{
    State& state = Get();
    if (!state.world)
        return NULL_LPENTITY;

    const LPENTITY entity = state.world->CreateEntity();
    state.world->Add<engine::TransformComponent>(entity);
    state.world->Add<engine::WorldTransformComponent>(entity);
    state.world->Add<engine::ActiveComponent>(entity);
    if (name && name[0] != '\0')
        state.world->Add<engine::NameComponent>(entity, engine::NameComponent{name});
    return entity;
}

bool IsAlive(LPENTITY entity)
{
    State& state = Get();
    return state.world && state.world->IsAlive(entity);
}

bool DestroyEntity(LPENTITY entity)
{
    State& state = Get();
    if (!state.world || !state.world->IsAlive(entity))
        return false;

    state.persistentEntityIds.erase(entity.value);
    std::erase_if(state.surfaces, [&](const auto& item) {
        return item.second.entity == entity;
    });
    DestroyEntityRecursive(*state.world, entity);
    state.transformSystem.Update(*state.world);
    state.boundsSystem.Update(*state.world);
    return true;
}

bool SetName(LPENTITY entity, const char* name)
{
    State& state = Get();
    if (!state.world || !state.world->IsAlive(entity))
        return false;

    const std::string value = name ? name : "";
    if (auto* nameComponent = state.world->Get<engine::NameComponent>(entity))
        nameComponent->name = value;
    else
        state.world->Add<engine::NameComponent>(entity, engine::NameComponent{value});
    return true;
}

const char* GetName(LPENTITY entity)
{
    State& state = Get();
    if (!state.world || !state.world->IsAlive(entity))
        return "";
    if (const auto* nameComponent = state.world->Get<engine::NameComponent>(entity))
        return nameComponent->name.c_str();
    return "";
}

bool SetLayer(LPENTITY entity, uint32_t layerMask)
{
    State& state = Get();
    if (!state.world || !state.world->IsAlive(entity)) return false;
    if (auto* mesh = state.world->Get<engine::MeshComponent>(entity))
    { mesh->layerMask = layerMask; return true; }
    return false;
}

uint32_t GetLayer(LPENTITY entity)
{
    State& state = Get();
    if (!state.world || !state.world->IsAlive(entity)) return engine::renderer::LAYER_DEFAULT;
    if (const auto* mesh = state.world->Get<engine::MeshComponent>(entity))
        return mesh->layerMask;
    return engine::renderer::LAYER_DEFAULT;
}

bool SetCameraLayers(LPENTITY camera, uint32_t cullingMask)
{
    State& state = Get();
    if (!state.world || !state.world->IsAlive(camera)) return false;
    if (auto* cam = state.world->Get<engine::CameraComponent>(camera))
    { cam->cullingMask = cullingMask; return true; }
    return false;
}

uint32_t GetCameraLayers(LPENTITY camera)
{
    State& state = Get();
    if (!state.world || !state.world->IsAlive(camera)) return engine::renderer::LAYER_ALL;
    if (const auto* cam = state.world->Get<engine::CameraComponent>(camera))
        return cam->cullingMask;
    return engine::renderer::LAYER_ALL;
}

bool SetTag(LPENTITY entity, const char* tag)
{
    State& state = Get();
    if (!state.world || !state.world->IsAlive(entity)) return false;
    const std::string value = tag ? tag : "";
    if (!state.world->Has<engine::TagComponent>(entity))
        state.world->Add<engine::TagComponent>(entity, engine::TagComponent{value});
    else if (auto* tc = state.world->Get<engine::TagComponent>(entity))
        tc->tag = value;
    return true;
}

const char* GetTag(LPENTITY entity)
{
    State& state = Get();
    if (!state.world || !state.world->IsAlive(entity)) return "";
    if (const auto* tc = state.world->Get<engine::TagComponent>(entity))
        return tc->tag.c_str();
    return "";
}

LPENTITY FindEntityByTag(const char* tag)
{
    State& state = Get();
    if (!state.world || !tag) return NULL_LPENTITY;
    LPENTITY found = NULL_LPENTITY;
    state.world->View<engine::TagComponent>([&](LPENTITY id, const engine::TagComponent& tc)
    {
        if (!found.IsValid() && tc.tag == tag)
            found = id;
    });
    return found;
}

LPENTITY FindChild(LPENTITY rootEntity, const char* name)
{
    State& state = Get();
    if (!state.world || !state.world->IsAlive(rootEntity) || !name)
        return NULL_LPENTITY;

    LPENTITY found = NULL_LPENTITY;
    std::function<void(LPENTITY)> search = [&](LPENTITY entity)
    {
        if (found.IsValid() || !state.world->IsAlive(entity))
            return;

        if (const auto* nameComponent = state.world->Get<engine::NameComponent>(entity))
        {
            if (nameComponent->name == name)
            {
                found = entity;
                return;
            }
        }

        if (const auto* children = state.world->Get<engine::ChildrenComponent>(entity))
        {
            for (const LPENTITY child : children->children)
                search(child);
        }
    };
    search(rootEntity);
    return found;
}

int CountChildren(LPENTITY entity)
{
    State& state = Get();
    if (!state.world || !state.world->IsAlive(entity))
        return 0;
    if (const auto* children = state.world->Get<engine::ChildrenComponent>(entity))
        return static_cast<int>(children->children.size());
    return 0;
}

LPENTITY GetChild(LPENTITY entity, int index)
{
    State& state = Get();
    if (!state.world || !state.world->IsAlive(entity) || index < 0)
        return NULL_LPENTITY;
    const auto* children = state.world->Get<engine::ChildrenComponent>(entity);
    if (!children)
        return NULL_LPENTITY;
    const size_t childIndex = static_cast<size_t>(index);
    if (childIndex >= children->children.size())
        return NULL_LPENTITY;
    return children->children[childIndex];
}

bool SetVisible(LPENTITY entity, bool visible, bool recursive)
{
    // Nur Rendering an/aus — Entity laeuft weiter durch Systeme und Transform.
    // Fuer vollstaendige Deaktivierung: SetActive() verwenden.
    State& state = Get();
    if (!state.world || !state.world->IsAlive(entity))
        return false;

    std::function<void(LPENTITY)> apply = [&](LPENTITY current)
    {
        if (!state.world->IsAlive(current))
            return;

        if (auto* mesh = state.world->Get<engine::MeshComponent>(current))
            mesh->visible = visible;

        if (!recursive)
            return;
        if (const auto* children = state.world->Get<engine::ChildrenComponent>(current))
        {
            for (const LPENTITY child : children->children)
                apply(child);
        }
    };
    apply(entity);
    return true;
}

bool SetActive(LPENTITY entity, bool active, bool recursive)
{
    // Vollstaendige Deaktivierung — Entity wird aus Rendering UND Systemen entfernt.
    // Fuer reines Verstecken: SetVisible() verwenden.
    State& state = Get();
    if (!state.world || !state.world->IsAlive(entity))
        return false;

    std::function<void(LPENTITY)> apply = [&](LPENTITY current)
    {
        if (!state.world->IsAlive(current))
            return;

        if (auto* a = state.world->Get<engine::ActiveComponent>(current))
            a->active = active;
        else
            state.world->Add<engine::ActiveComponent>(current, engine::ActiveComponent{active});

        if (!recursive)
            return;
        if (const auto* children = state.world->Get<engine::ChildrenComponent>(current))
        {
            for (const LPENTITY child : children->children)
                apply(child);
        }
    };
    apply(entity);
    return true;
}

bool SetParent(LPENTITY child, LPENTITY parent, bool keepWorldTransform)
{
    State& state = Get();
    if (!state.world ||
        !state.world->IsAlive(child) ||
        !state.world->IsAlive(parent) ||
        child == parent ||
        IsDescendantOf(*state.world, parent, child))
    {
        return false;
    }

    const CapturedTransform worldTransform = keepWorldTransform
        ? CaptureWorldTransform(state, child)
        : CapturedTransform{};

    DetachEntityFromParent(*state.world, child);
    state.world->Add<engine::ParentComponent>(child, parent);
    if (!state.world->Has<engine::ChildrenComponent>(parent))
        state.world->Add<engine::ChildrenComponent>(parent);
    state.world->Get<engine::ChildrenComponent>(parent)->Add(child);

    if (keepWorldTransform)
        ApplyLocalFromWorld(state, child, worldTransform);
    else if (auto* transform = state.world->Get<engine::TransformComponent>(child))
        transform->dirty = true;

    state.transformSystem.Update(*state.world);
    state.boundsSystem.Update(*state.world);
    return true;
}

bool Detach(LPENTITY child, bool keepWorldTransform)
{
    State& state = Get();
    if (!state.world || !state.world->IsAlive(child))
        return false;

    const CapturedTransform worldTransform = keepWorldTransform
        ? CaptureWorldTransform(state, child)
        : CapturedTransform{};

    DetachEntityFromParent(*state.world, child);
    if (keepWorldTransform)
        ApplyLocalFromWorld(state, child, worldTransform);
    else if (auto* transform = state.world->Get<engine::TransformComponent>(child))
        transform->dirty = true;

    state.transformSystem.Update(*state.world);
    state.boundsSystem.Update(*state.world);
    return true;
}

bool SetPosition(LPENTITY entity, const engine::math::Vec3& position)
{
    State& state = Get();
    if (!state.world || !state.world->IsAlive(entity))
        return false;

    if (auto* transform = state.world->Get<engine::TransformComponent>(entity))
    {
        transform->localPosition = position;
        transform->dirty = true;
        return true;
    }
    return false;
}

bool SetPosition(LPENTITY entity, float x, float y, float z)
{
    return SetPosition(entity, engine::math::Vec3{x, y, z});
}

bool SetRotationEulerDeg(LPENTITY entity, float pitch, float yaw, float roll)
{
    State& state = Get();
    if (!state.world || !state.world->IsAlive(entity))
        return false;

    if (auto* transform = state.world->Get<engine::TransformComponent>(entity))
    {
        transform->SetEulerDeg(pitch, yaw, roll);
        return true;
    }
    return false;
}

engine::math::Vec3 GetRotationEulerDeg(LPENTITY entity)
{
    State& state = Get();
    if (!state.world || !state.world->IsAlive(entity))
        return {};

    // WorldTransform bevorzugen (nach TransformSystem.Update() immer aktuell).
    // Quaternion → Euler (Reihenfolge YXZ = Yaw→Pitch→Roll = typisch für Kameras).
    const engine::math::Quat* q = nullptr;
    if (const auto* wt = state.world->Get<engine::WorldTransformComponent>(entity))
        q = &wt->rotation;
    else if (const auto* tr = state.world->Get<engine::TransformComponent>(entity))
        q = &tr->localRotation;
    if (!q)
        return {};

    const float sinP = 2.0f * (q->w * q->x - q->z * q->y);
    const float pitch = std::abs(sinP) >= 0.9999f
        ? std::copysign(90.0f, sinP)
        : engine::math::RAD_TO_DEG * std::asin(std::clamp(sinP, -1.0f, 1.0f));
    const float yaw = engine::math::RAD_TO_DEG *
        std::atan2(2.0f * (q->w * q->y + q->x * q->z),
                   1.0f - 2.0f * (q->y * q->y + q->x * q->x));
    const float roll = engine::math::RAD_TO_DEG *
        std::atan2(2.0f * (q->w * q->z + q->x * q->y),
                   1.0f - 2.0f * (q->x * q->x + q->z * q->z));
    return {pitch, yaw, roll};
}

bool SetScale(LPENTITY entity, const engine::math::Vec3& scale)
{
    State& state = Get();
    if (!state.world || !state.world->IsAlive(entity))
        return false;

    if (auto* transform = state.world->Get<engine::TransformComponent>(entity))
    {
        transform->localScale = scale;
        transform->dirty = true;
        return true;
    }
    return false;
}

bool SetScale(LPENTITY entity, float x, float y, float z)
{
    return SetScale(entity, engine::math::Vec3{x, y, z});
}

bool SetScale(LPENTITY entity, float uniformScale)
{
    return SetScale(entity, engine::math::Vec3{uniformScale, uniformScale, uniformScale});
}

bool MoveEntity(LPENTITY entity, float x, float y, float z)
{
    State& state = Get();
    if (!state.world || !state.world->IsAlive(entity))
        return false;

    auto* transform = state.world->Get<engine::TransformComponent>(entity);
    if (!transform)
        return false;

    const engine::math::Vec3 localOffset = transform->localRotation.Rotate({x, y, z});
    transform->localPosition += localOffset;
    transform->dirty = true;
    return true;
}

bool TurnEntity(LPENTITY entity, float pitch, float yaw, float roll)
{
    State& state = Get();
    if (!state.world || !state.world->IsAlive(entity))
        return false;

    auto* transform = state.world->Get<engine::TransformComponent>(entity);
    if (!transform)
        return false;

    transform->RotateLocalEulerDeg(pitch, yaw, roll);
    return true;
}

bool GetWorldBounds(LPENTITY entity,
                    engine::math::Vec3& outCenter,
                    engine::math::Vec3& outExtents,
                    float& outBoundingSphere)
{
    State& state = Get();
    if (!state.world || !state.world->IsAlive(entity))
        return false;

    engine::math::Vec3 boundsMin{};
    engine::math::Vec3 boundsMax{};
    bool hasBounds = false;
    if (!CollectWorldBoundsRecursive(*state.world, entity, boundsMin, boundsMax, hasBounds))
        return false;

    outCenter = (boundsMin + boundsMax) * 0.5f;
    outExtents = (boundsMax - boundsMin) * 0.5f;
    outBoundingSphere = outExtents.Length();
    return true;
}

bool LookAt(LPENTITY entity, const engine::math::Vec3& target, const engine::math::Vec3& up)
{
    State& state = Get();
    if (!state.world || !state.world->IsAlive(entity))
        return false;

    auto* transform = state.world->Get<engine::TransformComponent>(entity);
    if (!transform)
        return false;

    const engine::math::Vec3 position = ResolveEntityPosition(state, entity);
    const engine::math::Vec3 forward = (target - position).Normalized();
    if (forward.LengthSq() <= engine::math::EPSILON)
        return false;

    engine::math::Vec3 right = engine::math::Vec3::Cross(forward, up).Normalized();
    if (right.LengthSq() <= engine::math::EPSILON)
        right = engine::math::Vec3::Cross(forward, engine::math::Vec3::Right()).Normalized();
    if (right.LengthSq() <= engine::math::EPSILON)
        return false;

    const engine::math::Vec3 correctedUp = engine::math::Vec3::Cross(right, forward).Normalized();
    const engine::math::Vec3 back = -forward;

    engine::math::Mat4 rotation = engine::math::Mat4::Identity();
    rotation.m[0][0] = right.x;
    rotation.m[0][1] = right.y;
    rotation.m[0][2] = right.z;
    rotation.m[1][0] = correctedUp.x;
    rotation.m[1][1] = correctedUp.y;
    rotation.m[1][2] = correctedUp.z;
    rotation.m[2][0] = back.x;
    rotation.m[2][1] = back.y;
    rotation.m[2][2] = back.z;

    transform->localRotation = QuatFromRotationMatrix(rotation).Normalized();
    transform->dirty = true;
    return true;
}

bool LookAt(LPENTITY entity, LPENTITY targetEntity, const engine::math::Vec3& up)
{
    return LookAt(entity, ResolveEntityPosition(Get(), targetEntity), up);
}

LPENTITY CreateCamera(const char* name,
                      const engine::math::Vec3& position,
                      float fovYDeg,
                      bool isMainCamera)
{
    State& state = Get();
    const LPENTITY entity = CreateEntity(name);
    if (!entity.IsValid())
        return NULL_LPENTITY;

    SetPosition(entity, position);
    SetRotationEulerDeg(entity, -10.0f, 0.0f, 0.0f);

    engine::CameraComponent camera{};
    camera.projection = engine::ProjectionType::Perspective;
    camera.fovYDeg = fovYDeg;
    camera.nearPlane = 0.05f;
    camera.farPlane = 1000.0f;
    camera.isMainCamera = isMainCamera;
    state.world->Add<engine::CameraComponent>(entity, camera);
    return entity;
}

LPENTITY CreateDirectionalLight(const char* name,
                                const engine::math::Vec3& position,
                                const engine::math::Vec3& eulerDeg,
                                const engine::math::Vec3& color,
                                float intensity)
{
    State& state = Get();
    const LPENTITY entity = CreateEntity(name);
    if (!entity.IsValid())
        return NULL_LPENTITY;

    SetPosition(entity, position);
    SetRotationEulerDeg(entity, eulerDeg.x, eulerDeg.y, eulerDeg.z);

    engine::LightComponent light{};
    light.type = engine::LightType::Directional;
    light.color = color;
    light.intensity = intensity;
    light.castShadows = true;
    light.shadowSettings.enabled = true;
    light.shadowSettings.resolution = 2048u;
    light.shadowSettings.bias = 0.0015f;
    light.shadowSettings.normalBias = 0.001f;
    light.shadowSettings.maxDistance = 100.0f;
    state.world->Add<engine::LightComponent>(entity, light);
    return entity;
}

LPENTITY CreatePointLight(const char* name,
                          const engine::math::Vec3& position,
                          const engine::math::Vec3& color,
                          float intensity,
                          float range)
{
    State& state = Get();
    const LPENTITY entity = CreateEntity(name);
    if (!entity.IsValid())
        return NULL_LPENTITY;

    SetPosition(entity, position);

    engine::LightComponent light{};
    light.type = engine::LightType::Point;
    light.color = color;
    light.intensity = intensity;
    light.range = range;
    light.castShadows = true;
    light.shadowSettings.enabled = true;
    light.shadowSettings.resolution = 1024u;
    light.shadowSettings.bias = 0.0015f;
    light.shadowSettings.normalBias = 0.001f;
    light.shadowSettings.maxDistance = range;
    state.world->Add<engine::LightComponent>(entity, light);
    return entity;
}

LPENTITY CreateSpotLight(const char* name,
                         const engine::math::Vec3& position,
                         const engine::math::Vec3& eulerDeg,
                         const engine::math::Vec3& color,
                         float intensity,
                         float range,
                         float innerDeg,
                         float outerDeg)
{
    State& state = Get();
    const LPENTITY entity = CreateEntity(name);
    if (!entity.IsValid())
        return NULL_LPENTITY;

    SetPosition(entity, position);
    SetRotationEulerDeg(entity, eulerDeg.x, eulerDeg.y, eulerDeg.z);

    engine::LightComponent light{};
    light.type = engine::LightType::Spot;
    light.color = color;
    light.intensity = intensity;
    light.range = range;
    light.spotInnerDeg = innerDeg;
    light.spotOuterDeg = outerDeg;
    light.castShadows = true;
    light.shadowSettings.enabled = true;
    light.shadowSettings.resolution = 1024u;
    light.shadowSettings.bias = 0.0015f;
    light.shadowSettings.normalBias = 0.001f;
    light.shadowSettings.maxDistance = range;
    state.world->Add<engine::LightComponent>(entity, light);
    return entity;
}

LPENTITY CreateEntityFromAsset(const char* path, const char* name)
{
    State& state = Get();
    if (!state.world || !state.assetPipeline || !path || path[0] == '\0')
        return NULL_LPENTITY;

    // Format-agnostisch: AssetPipeline wählt den passenden registrierten Importer.
    // Ob GLB, FBX, OBJ oder ein anderes Format — der Wrapper muss es nicht wissen.
    engine::assets::ImportedAssetBundle bundle = state.assetPipeline->ImportBundle(path);

    if (bundle.Ok())
    {
        const std::filesystem::path logicalPath(path);

        const engine::math::Vec3 modelCenter = ComputeImportedModelCenter(bundle);
        const bool needsAnimationSystems = BundleHasSkeletonsOrAnimations(bundle);
        if (needsAnimationSystems)
            EnsureAnimationSystems(state);

        const engine::assets::MeshAsset* sharedFallbackMesh =
            !bundle.meshes.empty() ? &bundle.meshes[0] : nullptr;
        const engine::assets::MaterialAsset* sharedFallbackAsset =
            !bundle.materials.empty() ? &bundle.materials[0] : nullptr;
        const std::string sharedFallbackName =
            ((name && name[0] != '\0') ? std::string(name) : logicalPath.stem().string()) + "_SharedRuntimeMaterial";
        const engine::MaterialHandle sharedFallbackMaterial =
            sharedFallbackMesh
                ? CreateRuntimeLitMaterialFromAssets(
                    state, *sharedFallbackMesh, sharedFallbackAsset, sharedFallbackName.c_str())
                : engine::MaterialHandle::Invalid();

        engine::addons::prefab::PrefabBuildOptions buildOptions;
        buildOptions.name = (name && name[0] != '\0') ? name : logicalPath.stem().string();
        buildOptions.createSyntheticRoot = true;
        buildOptions.playFirstAnimation = true;
        buildOptions.loopAnimations = true;

        engine::addons::prefab::PrefabAsset prefab =
            engine::addons::prefab::BuildPrefabFromImportedBundle(
                std::move(bundle), state.assetRegistry, buildOptions);

        for (engine::addons::prefab::PrefabEntityRecord& record : prefab.records)
        {
            if (record.parentIndex == static_cast<int32_t>(prefab.rootIndex))
                record.localPosition -= modelCenter;

            if (record.mesh.IsValid())
            {
                auto* meshAsset = state.assetRegistry.meshes.Get(record.mesh);
                const auto* materialAsset = record.material.IsValid()
                    ? state.assetRegistry.materials.Get(record.material)
                    : nullptr;
                if (meshAsset)
                {
                    const std::string runtimeName = !record.name.empty()
                        ? record.name + "_RuntimeMaterial"
                        : buildOptions.name + "_RuntimeMaterial";
                    const engine::MaterialHandle runtimeMaterial =
                        CreateRuntimeLitMaterialFromAssets(state, *meshAsset, materialAsset, runtimeName.c_str());
                    record.material = runtimeMaterial.IsValid() ? runtimeMaterial : sharedFallbackMaterial;
                    if (!record.material.IsValid())
                        engine::Debug::LogError("krom.h: kein Material für Node '%s' in '%s'",
                            record.name.c_str(), path);
                }
            }
        }

        engine::addons::prefab::PrefabInstantiateOptions options{};
        options.scriptRegistry = &state.scriptAddonRegistry;
        const engine::addons::prefab::PrefabInstance instance =
            engine::addons::prefab::InstantiatePrefab(*state.world, prefab, options);
        if (!instance.IsValid())
        {
            engine::Debug::LogError("krom.h: Instantiierung fehlgeschlagen: '%s'", path);
            return NULL_LPENTITY;
        }
        for (const LPENTITY instantiatedEntity : instance.entities)
        {
            if (instantiatedEntity.IsValid() && state.world->Has<engine::MeshComponent>(instantiatedEntity))
                engine::mesh_renderer::UpdateLocalBoundsForEntity(*state.world, instantiatedEntity, state.assetRegistry);
        }
        state.transformSystem.Update(*state.world);
        state.boundsSystem.Update(*state.world);
        return instance.root;
    }

    // Fallback: kein Importer registriert → einfaches Single-Mesh-Entity
    engine::Debug::Log("krom.h: kein Bundle-Importer für '%s', lade als einfaches Mesh", path);
    const LPENTITY entity = CreateEntity(name ? name : path);
    if (!entity.IsValid())
        return NULL_LPENTITY;
    return SetMeshFromAsset(entity, path) ? entity : NULL_LPENTITY;
}

bool PrintAssetTree(const char* path)
{
    State& state = Get();
    if (!state.assetPipeline || !path || path[0] == '\0')
    {
        engine::Debug::LogError("krom.h: PrintAssetTree requires Graphics3D/Graphics and a valid path");
        return false;
    }

    const engine::assets::ImportedAssetBundle bundle = state.assetPipeline->ImportBundle(path);
    if (!bundle.Ok())
    {
        engine::Debug::LogError("krom.h: PrintAssetTree import failed for '%s': %s",
            path, bundle.error.c_str());
        return false;
    }

    engine::Debug::Log("Asset tree: %s", path);
    engine::Debug::Log("  nodes=%zu meshes=%zu materials=%zu skeletons=%zu animations=%zu",
        bundle.nodes.size(),
        bundle.meshes.size(),
        bundle.materials.size(),
        bundle.skeletons.size(),
        bundle.animations.size());

    std::vector<std::vector<size_t>> children(bundle.nodes.size());
    std::vector<size_t> roots;
    for (size_t i = 0; i < bundle.nodes.size(); ++i)
    {
        const int32_t parent = bundle.nodes[i].parentIndex;
        if (parent >= 0 && static_cast<size_t>(parent) < bundle.nodes.size())
            children[static_cast<size_t>(parent)].push_back(i);
        else
            roots.push_back(i);
    }

    const auto printMesh = [&](int32_t meshIndex, const std::string& indent)
    {
        if (meshIndex < 0 || static_cast<size_t>(meshIndex) >= bundle.meshes.size())
            return;

        const engine::assets::MeshAsset& mesh = bundle.meshes[static_cast<size_t>(meshIndex)];
        engine::Debug::Log("%s  mesh[%d] submeshes=%zu materials=%zu",
            indent.c_str(), meshIndex, mesh.submeshes.size(), mesh.materialHandles.size());
        for (size_t si = 0; si < mesh.submeshes.size(); ++si)
        {
            const engine::assets::SubMeshData& submesh = mesh.submeshes[si];
            engine::Debug::Log("%s    submesh[%zu] vertices=%zu indices=%zu materialIndex=%u",
                indent.c_str(),
                si,
                submesh.positions.size() / 3u,
                submesh.indices.size(),
                submesh.materialIndex);
        }
    };

    std::function<void(size_t, std::string)> printNode =
        [&](size_t nodeIndex, std::string indent)
    {
        const engine::assets::ImportedSceneNode& node = bundle.nodes[nodeIndex];
        const char* nodeName = node.name.empty() ? "<unnamed>" : node.name.c_str();
        engine::Debug::Log("%snode[%zu] '%s' parent=%d mesh=%d skin=%d",
            indent.c_str(),
            nodeIndex,
            nodeName,
            node.parentIndex,
            node.meshIndex,
            node.skinIndex);
        printMesh(node.meshIndex, indent);
        for (const size_t child : children[nodeIndex])
            printNode(child, indent + "  ");
    };

    if (bundle.nodes.empty())
    {
        for (size_t meshIndex = 0; meshIndex < bundle.meshes.size(); ++meshIndex)
            printMesh(static_cast<int32_t>(meshIndex), "  ");
    }
    else
    {
        for (const size_t root : roots)
            printNode(root, "  ");
    }

    for (size_t i = 0; i < bundle.materials.size(); ++i)
    {
        const engine::assets::MaterialAsset& material = bundle.materials[i];
        engine::Debug::Log("  material[%zu] name='%s' template='%s' baseColor='%s' normal='%s' orm='%s'",
            i,
            material.debugName.empty() ? "<unnamed>" : material.debugName.c_str(),
            material.templateName.c_str(),
            material.baseColorTexture.path.c_str(),
            material.normalTexture.path.c_str(),
            material.metallicRoughnessTexture.path.c_str());
    }

    return true;
}

LPMESH LoadMesh(const char* path)
{
    return (Get().assetPipeline && path) ? Get().assetPipeline->LoadMesh(path) : NULL_MESH;
}

LPTEXTURE LoadTexture(const char* path)
{
    State& state = Get();
    const LPTEXTURE texture = (state.assetPipeline && path) ? state.assetPipeline->LoadTexture(path) : NULL_TEXTURE;
    if (texture.IsValid() && path && path[0] != '\0')
        state.texturePaths[texture.value] = path;
    return texture;
}

LPMATERIAL CreateTextureMaterial(const char* albedoTexturePath,
                                 int materialModel,
                                 engine::math::Vec4 baseColorFactor,
                                 float roughness,
                                 float metallic)
{
    State& state = Get();
    if (!state.initialized)
        return NULL_MATERIAL;

    if (materialModel != MaterialModel::Lit)
    {
        engine::Debug::LogWarning(
            "krom.h: CreateTextureMaterial currently supports Lit for EntityTexture; use .mat/PBR workflow for advanced materials");
    }

    return BuildCodeMaterial(state, albedoTexturePath, baseColorFactor, roughness, metallic);
}

LPMATERIAL CreateTextureMaterial(LPTEXTURE texture,
                                 int materialModel,
                                 engine::math::Vec4 baseColorFactor,
                                 float roughness,
                                 float metallic)
{
    State& state = Get();
    const auto it = state.texturePaths.find(texture.value);
    if (it == state.texturePaths.end())
    {
        engine::Debug::LogError("krom.h: texture handle has no known path; load it through Krom::LoadTexture/LoadTexture first");
        return NULL_MATERIAL;
    }
    return CreateTextureMaterial(it->second.c_str(), materialModel, baseColorFactor, roughness, metallic);
}

LPMATERIAL LoadMaterial(const char* path)
{
    return (Get().assetPipeline && path) ? Get().assetPipeline->LoadMaterial(path) : NULL_MATERIAL;
}

bool SetMesh(LPENTITY entity, LPMESH mesh)
{
    State& state = Get();
    if (!state.world || !state.world->IsAlive(entity) || !mesh.IsValid())
        return false;

    if (!state.world->Has<engine::MeshComponent>(entity))
        state.world->Add<engine::MeshComponent>(entity, engine::MeshComponent{mesh});
    else
        state.world->Get<engine::MeshComponent>(entity)->mesh = mesh;

    if (!state.world->Has<engine::BoundsComponent>(entity))
        state.world->Add<engine::BoundsComponent>(entity);
    engine::mesh_renderer::UpdateLocalBoundsForEntity(*state.world, entity, state.assetRegistry);
    return true;
}

bool SetMaterial(LPENTITY entity, LPMATERIAL material)
{
    State& state = Get();
    if (!state.world || !state.world->IsAlive(entity) || !material.IsValid())
        return false;

    const LPMATERIAL runtimeMat = RealizeMaterialForEntity(state, entity, material);
    if (!state.world->Has<engine::MaterialComponent>(entity))
        state.world->Add<engine::MaterialComponent>(entity, engine::MaterialComponent{runtimeMat});
    else
        state.world->Get<engine::MaterialComponent>(entity)->material = runtimeMat;
    return true;
}

bool SetMaterialRecursive(LPENTITY rootEntity, LPMATERIAL material)
{
    State& state = Get();
    if (!state.world || !state.world->IsAlive(rootEntity) || !material.IsValid())
        return false;

    ApplyMaterialRecursive(state, *state.world, rootEntity, material);
    return true;
}

bool SetTexture(LPENTITY entity, const char* albedoTexturePath, int materialModel, bool recursive)
{
    if (!albedoTexturePath || albedoTexturePath[0] == '\0')
        return false;

    const LPMATERIAL material = CreateTextureMaterial(albedoTexturePath, materialModel);
    if (!material.IsValid())
        return false;

    return recursive ? SetMaterialRecursive(entity, material) : SetMaterial(entity, material);
}

bool SetTexture(LPENTITY entity, LPTEXTURE texture, int materialModel, bool recursive)
{
    const LPMATERIAL material = CreateTextureMaterial(texture, materialModel);
    if (!material.IsValid())
        return false;

    return recursive ? SetMaterialRecursive(entity, material) : SetMaterial(entity, material);
}

bool SetPBR(LPENTITY entity, const PBR& materialDesc, bool recursive)
{
    State& state = Get();
    if (!state.initialized || !state.world || !state.world->IsAlive(entity))
        return false;

    const LPMATERIAL material = BuildPbrCodeMaterial(state, materialDesc);
    if (!material.IsValid())
        return false;

    return recursive ? SetMaterialRecursive(entity, material) : SetMaterial(entity, material);
}

bool SetMeshFromAsset(LPENTITY entity, const char* path)
{
    const LPMESH mesh = LoadMesh(path);
    return mesh.IsValid() && SetMesh(entity, mesh);
}

bool SetMaterialFromAsset(LPENTITY entity, const char* path)
{
    State& state = Get();
    const LPMATERIAL material = LoadMaterial(path);
    if (!material.IsValid())
        return false;

    const bool ok = SetMaterial(entity, material);
    if (ok && state.world->Has<engine::MaterialComponent>(entity))
        state.world->Get<engine::MaterialComponent>(entity)->materialAssetPath = path ? path : "";
    return ok;
}

LPMESH CreateMeshAsset(const MeshData& meshData)
{
    State& state = Get();
    if (!state.initialized || !meshData.vertices || meshData.vertexCount == 0u)
        return NULL_MESH;

    engine::assets::SubMeshData submesh;
    submesh.positions.reserve(static_cast<size_t>(meshData.vertexCount) * 3u);
    submesh.normals.reserve(static_cast<size_t>(meshData.vertexCount) * 3u);
    submesh.uvs.reserve(static_cast<size_t>(meshData.vertexCount) * 2u);
    submesh.colors.reserve(static_cast<size_t>(meshData.vertexCount) * 4u);

    for (uint32_t i = 0u; i < meshData.vertexCount; ++i)
        AddVertex(submesh, meshData.vertices[i]);

    if (meshData.indices && meshData.indexCount > 0u)
        submesh.indices.assign(meshData.indices, meshData.indices + meshData.indexCount);
    else
    {
        submesh.indices.reserve(meshData.vertexCount);
        for (uint32_t i = 0u; i < meshData.vertexCount; ++i)
            submesh.indices.push_back(i);
    }

    if (meshData.generateNormals)
        GenerateMissingNormals(submesh);
    return RegisterProceduralMesh(state, std::move(submesh), meshData.name, meshData.generateTangents);
}

LPENTITY CreateMeshEntity(const MeshData& meshData)
{
    State& state = Get();
    const LPMESH mesh = CreateMeshAsset(meshData);
    return CreateProceduralEntity(state, mesh, meshData.name);
}

LPENTITY CreateMesh(const char* name)
{
    State& state = Get();
    if (!state.initialized || !state.world)
        return NULL_LPENTITY;

    auto meshAsset = std::make_unique<engine::assets::MeshAsset>();
    meshAsset->debugName = (name && name[0] != '\0') ? name : "Mesh";
    meshAsset->path = meshAsset->debugName;
    meshAsset->state = engine::assets::AssetState::Loaded;
    meshAsset->gpuStatus.dirty = true;

    const LPMESH mesh = state.assetRegistry.meshes.Add(std::move(meshAsset));
    LPENTITY entity = CreateEntity((name && name[0] != '\0') ? name : "Mesh");
    if (!entity.IsValid() || !SetMesh(entity, mesh))
        return NULL_LPENTITY;
    return entity;
}

LPSURFACE CreateSurface(LPENTITY meshEntity)
{
    State& state = Get();
    if (!state.initialized || !state.world || !state.world->IsAlive(meshEntity))
        return NULL_SURFACE;

    engine::MeshComponent* meshComponent = state.world->Get<engine::MeshComponent>(meshEntity);
    if (!meshComponent || !meshComponent->mesh.IsValid())
        return NULL_SURFACE;

    engine::assets::MeshAsset* mesh = state.assetRegistry.meshes.Get(meshComponent->mesh);
    if (!mesh)
        return NULL_SURFACE;

    engine::assets::SubMeshData submesh;
    submesh.materialIndex = static_cast<uint32_t>(mesh->submeshes.size());
    mesh->submeshes.push_back(std::move(submesh));
    mesh->gpuStatus.dirty = true;
    mesh->gpuStatus.uploaded = false;

    const uint32_t id = state.nextSurfaceId++;
    state.surfaces[id] = SurfaceRecord{meshEntity, meshComponent->mesh, static_cast<uint32_t>(mesh->submeshes.size() - 1u)};
    return LPSURFACE{id};
}

int AddVertex(LPSURFACE surface, float x, float y, float z, float u, float v)
{
    State& state = Get();
    engine::assets::SubMeshData* submesh = GetSurfaceSubmesh(state, surface);
    if (!submesh)
        return -1;

    const int index = static_cast<int>(submesh->positions.size() / 3u);
    submesh->positions.insert(submesh->positions.end(), {x, y, z});
    submesh->normals.insert(submesh->normals.end(), {0.0f, 0.0f, 0.0f});
    submesh->uvs.insert(submesh->uvs.end(), {u, v});
    submesh->colors.insert(submesh->colors.end(), {1.0f, 1.0f, 1.0f, 1.0f});
    TouchSurface(state, surface);
    return index;
}

int AddVertex(LPSURFACE surface, float x, float y, float z, float u, float v, float nx, float ny, float nz)
{
    const int index = Krom::AddVertex(surface, x, y, z, u, v);
    if (index >= 0)
        (void)Krom::VertexNormal(surface, index, nx, ny, nz);
    return index;
}

bool AddTriangle(LPSURFACE surface, int v0, int v1, int v2)
{
    State& state = Get();
    engine::assets::SubMeshData* submesh = GetSurfaceSubmesh(state, surface);
    if (!submesh || v0 < 0 || v1 < 0 || v2 < 0)
        return false;

    const uint32_t vertexCount = static_cast<uint32_t>(submesh->positions.size() / 3u);
    const uint32_t i0 = static_cast<uint32_t>(v0);
    const uint32_t i1 = static_cast<uint32_t>(v1);
    const uint32_t i2 = static_cast<uint32_t>(v2);
    if (i0 >= vertexCount || i1 >= vertexCount || i2 >= vertexCount)
        return false;

    submesh->indices.insert(submesh->indices.end(), {i0, i1, i2});
    TouchSurface(state, surface);
    return true;
}

bool VertexNormal(LPSURFACE surface, int index, float nx, float ny, float nz)
{
    State& state = Get();
    engine::assets::SubMeshData* submesh = GetSurfaceSubmesh(state, surface);
    if (!submesh || index < 0)
        return false;

    const uint32_t vertex = static_cast<uint32_t>(index);
    if (vertex >= submesh->positions.size() / 3u)
        return false;

    if (submesh->normals.size() < submesh->positions.size())
        submesh->normals.resize(submesh->positions.size(), 0.0f);
    const size_t offset = static_cast<size_t>(vertex) * 3u;
    submesh->normals[offset] = nx;
    submesh->normals[offset + 1u] = ny;
    submesh->normals[offset + 2u] = nz;
    TouchSurface(state, surface);
    return true;
}

bool VertexTexCoords(LPSURFACE surface, int index, float u, float v)
{
    State& state = Get();
    engine::assets::SubMeshData* submesh = GetSurfaceSubmesh(state, surface);
    if (!submesh || index < 0)
        return false;

    const uint32_t vertex = static_cast<uint32_t>(index);
    if (vertex >= submesh->positions.size() / 3u)
        return false;

    const size_t vertexCount = submesh->positions.size() / 3u;
    if (submesh->uvs.size() < vertexCount * 2u)
        submesh->uvs.resize(vertexCount * 2u, 0.0f);
    const size_t offset = static_cast<size_t>(vertex) * 2u;
    submesh->uvs[offset] = u;
    submesh->uvs[offset + 1u] = v;
    TouchSurface(state, surface);
    return true;
}

int CountVertices(LPSURFACE surface)
{
    State& state = Get();
    const engine::assets::SubMeshData* submesh = GetSurfaceSubmesh(state, surface);
    return submesh ? static_cast<int>(submesh->positions.size() / 3u) : 0;
}

int CountTriangles(LPSURFACE surface)
{
    State& state = Get();
    const engine::assets::SubMeshData* submesh = GetSurfaceSubmesh(state, surface);
    return submesh ? static_cast<int>(submesh->indices.size() / 3u) : 0;
}

LPMATERIAL CreateLitMaterial(const char* albedoTexturePath,
                              engine::math::Vec4 baseColorFactor,
                              float roughness,
                              float metallic)
{
    State& state = Get();
    if (!state.initialized)
        return NULL_MATERIAL;
    return BuildCodeMaterial(state, albedoTexturePath, baseColorFactor, roughness, metallic);
}

engine::renderer::EnvironmentHandle CreateEnvironmentFromHDR(const char* path,
                                                             float intensity,
                                                             bool enableIBL)
{
    if (!path || !Get().assetPipeline)
        return engine::renderer::EnvironmentHandle::Invalid();

    const LPTEXTURE hdrTexture = LoadTexture(path);
    if (!hdrTexture.IsValid())
        return engine::renderer::EnvironmentHandle::Invalid();

    Get().assetPipeline->UploadPendingGpuAssets();

    engine::renderer::EnvironmentDesc desc{};
    desc.mode = engine::renderer::EnvironmentMode::Texture;
    desc.sourceTexture = hdrTexture;
    desc.intensity = intensity;
    desc.enableIBL = enableIBL;
    return Get().renderLoop.GetRenderSystem().CreateEnvironment(desc);
}

void SetActiveEnvironment(engine::renderer::EnvironmentHandle environment)
{
    if (environment.IsValid())
    {
        Get().activeEnvironment = environment;
        Get().renderLoop.GetRenderSystem().SetActiveEnvironment(environment);
    }
}

void WarmUpShaders()
{
    State& state = Get();
    if (!state.initialized || !state.assetPipeline)
        return;

    // Parallel-kompiliert Shader-Caches (std::async) und lädt Texturen hoch.
    // BuildPendingShaderCaches() füllt compiledArtifacts in den ShaderAssets —
    // danach braucht PrepareShaderAsset() nur noch CreateShaderFromBytecode().
    state.assetPipeline->UploadPendingGpuAssets();

    engine::renderer::ShaderRuntime& shaderRuntime =
        state.renderLoop.GetRenderSystem().GetShaderRuntime();
    const engine::renderer::MaterialSystemShaderMaterialSource materialSource(state.materialSystem);

    // Nur Shader hochladen, die die aktuelle Szene tatsächlich benötigt.
    std::vector<engine::ShaderHandle> shaderRequests;
    (void)shaderRuntime.CollectShaderRequests(materialSource, shaderRequests);
    (void)shaderRuntime.CommitShaderRequests(shaderRequests);

    (void)shaderRuntime.PrepareAllMaterials(materialSource);
}

int Run()
{
    State& state = Get();
    if (!EnsureInitialized())
        return -1;

    if (state.initCallback && !state.initCallback())
        return -2;

    state.transformSystem.Update(*state.world);
    state.boundsSystem.Update(*state.world);

    // Keine Kamera in der Welt — Standard-Kamera automatisch anlegen damit
    // der Render-Pass nicht fehlschlaegt (z.B. bei reinen Fenster-Beispielen).
    {
        bool hasMainCamera = false;
        state.world->View<engine::CameraComponent>([&](LPENTITY, engine::CameraComponent& c)
        {
            if (c.isMainCamera) hasMainCamera = true;
        });
        if (!hasMainCamera)
            CreateCamera("__DefaultCamera");
    }

    while (!state.renderLoop.ShouldExit())
    {
        engine::platform::IWindow* window = state.renderLoop.GetWindow();
        engine::platform::IInput* input = state.renderLoop.GetInput();
        if (!state.platform || !window || !input)
        {
            engine::Debug::LogError("krom.h: runtime frame failed - platform/window/input missing");
            return -4;
        }

        state.timing.BeginFrame();
        state.platform->PumpEvents();
        const engine::platform::WindowEventState windowState = window->PumpEvents(*input);

        if (windowState.resized)
        {
            const uint32_t resizeWidth = windowState.framebufferWidth > 0u ? windowState.framebufferWidth : windowState.width;
            const uint32_t resizeHeight = windowState.framebufferHeight > 0u ? windowState.framebufferHeight : windowState.height;
            state.renderLoop.GetRenderSystem().HandleResize(resizeWidth, resizeHeight);
        }

        if (windowState.quitRequested || window->ShouldClose())
        {
            state.timing.EndFrame();
            break;
        }

        const float deltaSeconds = state.timing.GetDeltaSecondsF();
        if (state.tickCallback)
            state.tickCallback(deltaSeconds);

        for (auto& sys : state.userSystems)
            sys(*state.world, deltaSeconds);

        engine::script::PrefabInstantiateFn instantiatePrefab =
            [&state](std::string_view prefabAssetPath,
                     const engine::math::Vec3& position,
                     const engine::math::Quat& rotation,
                     const engine::math::Vec3& scale) -> LPENTITY
            {
                return InstantiatePrefabForScript(state, prefabAssetPath, position, rotation, scale);
            };

        std::vector<LPENTITY> scriptEntities;
        state.world->ForEachAlive([&](LPENTITY id)
        {
            if (state.world->Has<engine::script::ScriptList>(id))
                scriptEntities.push_back(id);
        });
        for (const LPENTITY id : scriptEntities)
        {
            if (!state.world->IsAlive(id))
                continue;
            if (auto* sl = state.world->Get<engine::script::ScriptList>(id))
                sl->Update(deltaSeconds, id, &instantiatePrefab);
        }
        if (state.editorLiveStateEnabled)
        {
            state.runtimeStateWriteTimer += deltaSeconds;
            if (state.runtimeStateWriteTimer >= 0.1f)
            {
                state.runtimeStateWriteTimer = 0.0f;
                WriteRuntimeStateSnapshot(state);
            }
        }

        if (state.animationSystem)
            state.animationSystem->Update(*state.world, deltaSeconds);
        if (state.skinningSystem)
            state.skinningSystem->Upload(*state.world);

        engine::mesh_renderer::UpdateLocalBoundsFromMeshes(*state.world, state.assetRegistry);
        state.transformSystem.Update(*state.world);
        state.boundsSystem.Update(*state.world);

        const engine::renderer::ISwapchain* swapchain = state.renderLoop.GetRenderSystem().GetSwapchain();
        const uint32_t viewportWidth = (swapchain && swapchain->GetWidth() > 0u)
            ? swapchain->GetWidth()
            : state.config.width;
        const uint32_t viewportHeight = (swapchain && swapchain->GetHeight() > 0u)
            ? swapchain->GetHeight()
            : state.config.height;

        engine::renderer::RenderView view{};
        if (!engine::addons::camera::BuildPrimaryRenderView(
                *state.world,
                viewportWidth,
                viewportHeight,
                view,
                state.cameraOptions))
        {
            engine::Debug::LogError("krom.h: failed to build primary render view - create a main camera first");
            state.timing.EndFrame();
            return -3;
        }

        if (!state.renderLoop.GetRenderSystem().RenderFrame(*state.world, state.materialSystem, view, state.timing))
        {
            state.timing.EndFrame();
            if (state.renderLoop.ShouldExit())
                break;
            return -4;
        }

        state.timing.EndFrame();

        static uint32_t s_debugFrameCounter = 0u;
        if (s_debugFrameCounter < 5u)
        {
            const auto& stats = state.renderLoop.GetRenderSystem().GetStats();
            engine::Debug::Log(
                "krom.h: frame=%u proxies=%u visible=%u opaque=%u transparent=%u shadow=%u uploadedBytes=%llu",
                s_debugFrameCounter,
                stats.totalProxyCount,
                stats.visibleProxyCount,
                stats.opaqueDraws,
                stats.transparentDraws,
                stats.shadowDraws,
                static_cast<unsigned long long>(stats.uploadedBytes));
            ++s_debugFrameCounter;
        }
    }

    state.running = false;
    return 0;
}

int Run(TickFn fn)
{
    OnUpdate(std::move(fn));
    return Run();
}

void Shutdown()
{
    State& state = Get();
    state.running = false;
    state.initialized = false;
    state.featuresRegistered = false;
    state.tickCallback = {};
    state.initCallback = {};
    state.scenes.clear();
    state.persistentEntityIds.clear();
    state.texturePaths.clear();
    state.runtimeMaterialCache.clear();
    state.surfaces.clear();
    state.nextSurfaceId = 1u;
    state.nextSceneId = 1u;
    state.projectRoot.clear();
    state.projectScenePaths.clear();
    if (state.activeEnvironment.IsValid())
    {
        state.renderLoop.GetRenderSystem().DestroyEnvironment(state.activeEnvironment);
        state.activeEnvironment = engine::renderer::EnvironmentHandle::Invalid();
    }
    state.assetPipeline.reset();
    state.skinningSystem.reset();
    state.animationSystem.reset();
    state.world.reset();
    state.renderLoop.Shutdown();
    state.platform.reset();
}

// =============================================================================
// Scene-Management
// =============================================================================

LPSCENE LoadScene(const char* path)
{
    State& state = Get();
    if (!EnsureInitialized() || !path)
        return NULL_SCENE;
    return LoadSceneFromData(state, LoadKsceneFile(state.assetRoot, path), false, path);
}

LPSCENE LoadScene(int sceneIndex)
{
    State& state = Get();
    if (sceneIndex <= 0)
        return NULL_SCENE;

    const size_t index = static_cast<size_t>(sceneIndex - 1);
    if (index >= state.projectScenePaths.size())
    {
        engine::Debug::LogError("krom.h: LoadScene(%d) out of range; project has %zu scenes",
            sceneIndex, state.projectScenePaths.size());
        return NULL_SCENE;
    }
    return LoadScene(state.projectScenePaths[index].c_str());
}

LPSCENE LoadSceneAdditive(const char* path)
{
    State& state = Get();
    if (!EnsureInitialized() || !path)
        return NULL_SCENE;
    return LoadSceneFromData(state, LoadKsceneFile(state.assetRoot, path), true, path);
}

LPSCENE LoadSceneAdditive(int sceneIndex)
{
    State& state = Get();
    if (sceneIndex <= 0)
        return NULL_SCENE;

    const size_t index = static_cast<size_t>(sceneIndex - 1);
    if (index >= state.projectScenePaths.size())
    {
        engine::Debug::LogError("krom.h: LoadSceneAdditive(%d) out of range; project has %zu scenes",
            sceneIndex, state.projectScenePaths.size());
        return NULL_SCENE;
    }
    return LoadSceneAdditive(state.projectScenePaths[index].c_str());
}

LPSCENE LoadSceneAsync(const char* path)
{
    State& state = Get();
    if (!EnsureInitialized() || !path)
        return NULL_SCENE;

    const uint32_t sceneId = state.nextSceneId++;
    SceneRecord& record = state.scenes[sceneId];
    record.name    = "";
    record.path    = path;
    record.staging = true;
    record.staged  = false;
    record.active  = false;

    // Absoluten Pfad vor dem Thread-Start berechnen (thread-safe)
    const std::filesystem::path resolved =
        std::filesystem::weakly_canonical(state.assetRoot / path);
    const std::string resolvedStr = resolved.string();

    // Hintergrund-Thread: nur Datei-I/O + JSON-Parsing — kein ECS-Zugriff
    record.asyncFuture = std::async(std::launch::async, [resolvedStr]() -> KromSceneData
    {
        std::ifstream file(resolvedStr);
        if (!file)
        {
            engine::Debug::LogError("krom.h: LoadSceneAsync: Datei nicht gefunden: %s",
                resolvedStr.c_str());
            return KromSceneData{};
        }
        const std::string json(std::istreambuf_iterator<char>(file),
                               std::istreambuf_iterator<char>{});
        return ParseKscene(json);
    }).share();

    engine::Debug::Log("krom.h: LoadSceneAsync gestartet: %s", path);
    return LPSCENE{sceneId};
}

bool IsSceneReady(LPSCENE scene)
{
    State& state = Get();
    auto it = state.scenes.find(scene.id);
    if (it == state.scenes.end())
        return false;
    SceneRecord& record = it->second;
    if (record.staged || record.active)
        return true;
    if (!record.staging)
        return false;

    if (record.asyncFuture.valid() &&
        record.asyncFuture.wait_for(std::chrono::seconds(0)) == std::future_status::ready)
    {
        record.stagedData = record.asyncFuture.get();
        record.staging    = false;
        record.staged     = true;
        return true;
    }
    return false;
}

bool ActivateScene(LPSCENE scene)
{
    State& state = Get();
    auto it = state.scenes.find(scene.id);
    if (it == state.scenes.end())
        return false;
    SceneRecord& record = it->second;

    if (record.active)
        return true;  // bereits aktiv

    // Falls async noch läuft: blockierend warten
    if (record.staging && record.asyncFuture.valid())
    {
        record.stagedData = record.asyncFuture.get();
        record.staging    = false;
        record.staged     = true;
    }

    if (!record.staged)
        return false;

    // Entities jetzt auf dem Main-Thread erstellen
    const KromSceneData& data = record.stagedData;
    if (!data.valid)
    {
        state.scenes.erase(it);
        return false;
    }

    record.name       = data.name;
    record.persistent = data.persistent;
    record.active     = true;
    record.staged     = false;

    for (const auto& desc : data.entities)
    {
        const LPENTITY entity = InstantiateSceneEntity(state, desc);
        if (entity.IsValid())
        {
            record.rootEntities.push_back(entity);
            if (desc.persistent || data.persistent)
                MarkPersistentRecursive(state, entity);
        }
    }

    state.transformSystem.Update(*state.world);
    state.boundsSystem.Update(*state.world);
    if (!record.name.empty())
        state.eventBus.Emit<engine::events::SceneLoadedEvent>(record.name);

    engine::Debug::Log("krom.h: scene '%s' aktiviert (%zu entities)",
        data.name.c_str(), record.rootEntities.size());
    return true;
}

void UnloadScene(LPSCENE scene)
{
    State& state = Get();
    UnloadSceneById(state, scene.id);
}

void UnloadAll()
{
    State& state = Get();
    std::vector<uint32_t> toUnload;
    for (const auto& [id, scene] : state.scenes)
    {
        if (!scene.persistent)
            toUnload.push_back(id);
    }
    for (const uint32_t id : toUnload)
        UnloadSceneById(state, id);
}

void SetPersistent(LPENTITY entity)
{
    if (entity.IsValid())
        Get().persistentEntityIds.insert(entity.value);
}

void SetScenePersistent(LPSCENE scene)
{
    State& state = Get();
    auto it = state.scenes.find(scene.id);
    if (it == state.scenes.end())
        return;
    SceneRecord& record = it->second;
    record.persistent = true;
    for (const LPENTITY entity : record.rootEntities)
        MarkPersistentRecursive(state, entity);
}

LPENTITY FindEntity(const char* name)
{
    State& state = Get();
    if (!state.world || !name)
        return NULL_LPENTITY;

    LPENTITY found = NULL_LPENTITY;
    state.world->ForEachAlive([&](engine::EntityID id)
    {
        if (found.IsValid()) return;
        if (const auto* nc = state.world->Get<engine::NameComponent>(id))
            if (nc->name == name)
                found = id;
    });
    return found;
}

LPENTITY GetMainCamera()
{
    State& state = Get();
    if (!state.world)
        return NULL_LPENTITY;

    LPENTITY mainCamera = NULL_LPENTITY;
    LPENTITY fallbackCamera = NULL_LPENTITY;
    state.world->View<engine::CameraComponent>(
        [&](LPENTITY entity, const engine::CameraComponent& camera)
        {
            if (!fallbackCamera.IsValid())
                fallbackCamera = entity;
            if (camera.isMainCamera && !mainCamera.IsValid())
                mainCamera = entity;
        });

    return mainCamera.IsValid() ? mainCamera : fallbackCamera;
}

LPENTITY FindEntityInScene(LPSCENE scene, const char* name)
{
    State& state = Get();
    if (!state.world || !name)
        return NULL_LPENTITY;

    auto it = state.scenes.find(scene.id);
    if (it == state.scenes.end())
        return NULL_LPENTITY;

    LPENTITY found = NULL_LPENTITY;
    std::function<void(LPENTITY)> search = [&](LPENTITY e)
    {
        if (found.IsValid() || !state.world->IsAlive(e)) return;
        if (const auto* nc = state.world->Get<engine::NameComponent>(e))
            if (nc->name == name) { found = e; return; }
        if (const auto* ch = state.world->Get<engine::ChildrenComponent>(e))
            for (const LPENTITY child : ch->children)
                search(child);
    };
    for (const LPENTITY root : it->second.rootEntities)
        search(root);
    return found;
}

} // namespace Krom
