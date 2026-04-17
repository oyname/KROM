#include "TestFramework.hpp"
#include "addons/mesh_renderer/MeshRendererComponents.hpp"
#include "assets/AssetPipeline.hpp"
#include "addons/mesh_renderer/MeshAssetSceneBindings.hpp"
#include "collision/SceneQueries.hpp"
#include "addons/mesh_renderer/MeshSceneQueries.hpp"
#include "NullDevice.hpp"
#include "scene/Scene.hpp"
#include "scene/BoundsSystem.hpp"
#include "addons/mesh_renderer/MeshBounds.hpp"
#include "ecs/Components.hpp"
#include <filesystem>
#include <fstream>
#include <thread>
#include <chrono>

using namespace engine;
using namespace engine::assets;
using namespace engine::collision;

static void TestAssetPipelineLoadAndReload(test::TestContext& ctx)
{
    namespace fs = std::filesystem;
    const fs::path root = fs::temp_directory_path() / "krom_asset_pipeline_test";
    fs::create_directories(root);


    {
        std::ofstream(root / "tri.mesh") << "v 0 0 0\nv 1 0 0\nv 0 1 0\ni 0 1 2\n";
        std::ofstream(root / "tex.tex") << "1 1\n255 0 0 255\n";
        std::ofstream(root / "test.vert") << "void main(){}\n";
        std::ofstream(root / "test.frag") << "void main(){}\n";
        std::ofstream(root / "mat.mat") << "vertex test.vert\nfragment test.frag\nvec4 tint 1 0 0 1\ntexture albedo tex.tex\n";
    }

    AssetRegistry registry;
    renderer::null_backend::NullDevice device;
    renderer::IDevice::DeviceDesc dd{}; dd.appName = "asset-test";
    CHECK(ctx, device.Initialize(dd));

    AssetPipeline pipeline(registry, &device);
    mesh_renderer::ConfigureAssetPipeline(pipeline);
    pipeline.SetAssetRoot(root);

    auto meshH = pipeline.LoadMesh("tri.mesh");
    auto texH = pipeline.LoadTexture("tex.tex");
    auto vsH = pipeline.LoadShader("test.vert", ShaderStage::Vertex);
    auto matH = pipeline.LoadMaterial("mat.mat");
    pipeline.UploadPendingGpuAssets();

    CHECK_VALID(ctx, meshH);
    CHECK_VALID(ctx, texH);
    CHECK_VALID(ctx, vsH);
    CHECK_VALID(ctx, matH);
    CHECK_EQ(ctx, registry.meshes.Get(meshH)->state, AssetState::Loaded);
    CHECK_EQ(ctx, registry.textures.Get(texH)->state, AssetState::Loaded);
    CHECK_EQ(ctx, registry.shaders.Get(vsH)->state, AssetState::Loaded);
    CHECK_EQ(ctx, registry.materials.Get(matH)->state, AssetState::Loaded);
    CHECK(ctx, registry.textures.Get(texH)->gpuStatus.uploaded);
    CHECK(ctx, pipeline.GetGpuTexture(texH).IsValid());
    CHECK(ctx, pipeline.GetGpuShader(vsH).IsValid());
    CHECK_GT(ctx, registry.shaders.Get(vsH)->compiledArtifacts.size(), 0u);

    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    { std::ofstream(root / "tri.mesh") << "v 0 0 0\nv 2 0 0\nv 0 2 0\ni 0 1 2\n"; }
    pipeline.PollHotReload();

    auto* mesh = registry.meshes.Get(meshH);
    CHECK_EQ(ctx, mesh->submeshes[0].positions[3], 2.f);

    device.Shutdown();
    fs::remove_all(root);
}

static void TestSceneQueries(test::TestContext& ctx)
{
    RegisterCoreComponents();
    RegisterMeshRendererComponents();
    ecs::World world;
    Scene scene(world);
    AssetRegistry registry;

    auto mesh = std::make_unique<MeshAsset>();
    mesh->state = AssetState::Loaded;
    SubMeshData sm;
    sm.positions = { 0.f,0.f,0.f, 1.f,0.f,0.f, 0.f,1.f,0.f };
    sm.indices = { 0,1,2 };
    mesh->submeshes.push_back(sm);
    MeshHandle mh = registry.GetOrAddMesh("inline.mesh", std::move(mesh));

    EntityID e = scene.CreateEntity("Tri");
    world.Add<MeshComponent>(e, mh);
    auto& b = world.Add<BoundsComponent>(e);
    b.centerLocal = { 0.5f,0.5f,0.f };
    b.extentsLocal = { 0.5f,0.5f,0.1f };
    scene.PropagateTransforms();
    BoundsSystem boundsSystem;
    mesh_renderer::UpdateLocalBoundsFromMeshes(world, registry);
    boundsSystem.Update(world);

    SceneQueries queries;
    queries.Build(world);

    RaycastHit hit{};
    CHECK(ctx, queries.Raycast({ {0.25f,0.25f,1.f},{0.f,0.f,-1.f} }, 10.f, hit));
    CHECK_EQ(ctx, hit.entity, e);
    CHECK(ctx, hit.distance >= 0.9f && hit.distance <= 1.1f);

    RaycastHit preciseHit{};
    CHECK(ctx, mesh_renderer::MeshSceneQueries::RaycastTriangles(world, registry, { {0.25f,0.25f,1.f},{0.f,0.f,-1.f} }, 10.f, preciseHit));
    CHECK_EQ(ctx, preciseHit.entity, e);
    CHECK(ctx, preciseHit.distance >= 0.9f && preciseHit.distance <= 1.1f);

    auto overlaps = queries.OverlapSphere({ {0.5f,0.5f,0.f}, 1.f });
    CHECK_EQ(ctx, overlaps.size(), 1u);
    if (!overlaps.empty()) CHECK_EQ(ctx, overlaps[0], e);

    RaycastHit sweep{};
    CHECK(ctx, queries.SweepSphere({ {-2.f,0.5f,0.f}, 0.5f }, { 3.f,0.f,0.f }, sweep));
    CHECK_EQ(ctx, sweep.entity, e);
}

int RunAssetsCollisionTests()
{
    test::TestSuite suite("AssetsCollision");
    suite.Add("AssetPipelineLoadAndReload", TestAssetPipelineLoadAndReload)
        .Add("SceneQueries", TestSceneQueries);
    return suite.Run();
}
