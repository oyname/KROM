#include "TestFramework.hpp"
#include "addons/mesh_renderer/MeshRendererComponents.hpp"
#include "assets/AssetPipeline.hpp"
#include "assets/KMeshSerializer.hpp"
#include "assets/VertexLayoutBridge.hpp"
#include "addons/mesh_renderer/MeshAssetSceneBindings.hpp"
#include "collision/SceneQueries.hpp"
#include "addons/mesh_renderer/MeshSceneQueries.hpp"
#include "GltfImporter.hpp"
#include "NullDevice.hpp"
#include "scene/Scene.hpp"
#include "scene/BoundsSystem.hpp"
#include "addons/mesh_renderer/MeshBounds.hpp"
#include "ecs/Components.hpp"
#include <filesystem>
#include <fstream>
#include <thread>
#include <chrono>
#include <cmath>
#include <cstring>

using namespace engine;
using namespace engine::assets;
using namespace engine::collision;
using namespace engine::addons::gltf;

namespace {

[[nodiscard]] ecs::ComponentMetaRegistry CreateAssetCollisionRegistry()
{
    ecs::ComponentMetaRegistry registry;
    RegisterCoreComponents(registry);
    RegisterMeshRendererComponents(registry);
    return registry;
}

[[nodiscard]] const renderer::VertexAttribute* FindAttribute(
    const renderer::VertexLayout& layout,
    renderer::VertexSemantic     semantic) noexcept
{
    for (const auto& attr : layout.attributes)
    {
        if (attr.semantic == semantic)
            return &attr;
    }
    return nullptr;
}

} // namespace

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

static void TestKMeshRoundTrip(test::TestContext& ctx)
{
    namespace fs = std::filesystem;
    const fs::path root = fs::temp_directory_path() / "krom_kmesh_roundtrip_test";
    fs::create_directories(root);

    SubMeshData src;
    src.positions   = { 0.f,0.f,0.f, 1.f,0.f,0.f, 0.f,1.f,0.f };
    src.normals     = { 0.f,0.f,1.f, 0.f,0.f,1.f, 0.f,0.f,1.f };
    src.tangents    = { 1.f,0.f,0.f,1.f, 1.f,0.f,0.f,1.f, 1.f,0.f,0.f,1.f };
    src.uvs         = { 0.f,0.f, 1.f,0.f, 0.f,1.f };
    src.indices     = { 0u, 1u, 2u };
    src.materialIndex = 7u;

    const fs::path cache = root / "tri.kmesh";
    CHECK(ctx, KMeshSave(cache, { src }));

    std::vector<SubMeshData> loaded;
    CHECK(ctx, KMeshTryLoad(cache, loaded));
    CHECK_EQ(ctx, loaded.size(), 1u);
    if (!loaded.empty())
    {
        CHECK_EQ(ctx, loaded[0].materialIndex, 7u);
        CHECK_EQ(ctx, loaded[0].positions.size(), src.positions.size());
        CHECK_EQ(ctx, loaded[0].normals.size(), src.normals.size());
        CHECK_EQ(ctx, loaded[0].tangents.size(), src.tangents.size());
        CHECK_EQ(ctx, loaded[0].uvs.size(), src.uvs.size());
        CHECK_EQ(ctx, loaded[0].indices.size(), src.indices.size());
        CHECK(ctx, loaded[0].positions[3] == 1.f);
        CHECK(ctx, loaded[0].tangents[3] == 1.f);
        CHECK_EQ(ctx, loaded[0].indices[2], 2u);
    }

    fs::remove_all(root);
}

static void TestKMeshRawFastPathFillsInterleavedBytes(test::TestContext& ctx)
{
    namespace fs = std::filesystem;
    const fs::path root = fs::temp_directory_path() / "krom_kmesh_rawbytes_test";
    fs::create_directories(root);

    // pos(12) + normal(12) + uv(8) → canonical stride 32, 3 vertices
    SubMeshData src;
    src.positions = { 1.f, 2.f, 3.f,   4.f, 5.f, 6.f,   7.f, 8.f, 9.f };
    src.normals   = { 0.f, 0.f, 1.f,   0.f, 0.f, 1.f,   0.f, 0.f, 1.f };
    src.uvs       = { 0.1f, 0.2f,   0.3f, 0.4f,   0.5f, 0.6f };
    src.indices   = { 0u, 1u, 2u };

    const fs::path cache = root / "rawbytes.kmesh";
    CHECK(ctx, KMeshSave(cache, { src }));

    std::vector<SubMeshData> loaded;
    CHECK(ctx, KMeshTryLoad(cache, loaded));
    CHECK_EQ(ctx, loaded.size(), 1u);
    if (loaded.empty()) { fs::remove_all(root); return; }

    const SubMeshData& sm = loaded[0];
    const uint32_t expectedStride = 32u; // pos(12) + normal(12) + uv(8)
    const size_t   expectedBytes  = 3u * expectedStride;

    CHECK_EQ(ctx, sm.rawVertexStride, expectedStride);
    CHECK_EQ(ctx, sm.rawInterleavedBytes.size(), expectedBytes);

    if (sm.rawInterleavedBytes.size() == expectedBytes)
    {
        // Vertex 0: Position at byte 0, Normal at byte 12, UV at byte 24
        float pos0[3], nrm0[3], uv0[2];
        std::memcpy(pos0, sm.rawInterleavedBytes.data(),      12);
        std::memcpy(nrm0, sm.rawInterleavedBytes.data() + 12, 12);
        std::memcpy(uv0,  sm.rawInterleavedBytes.data() + 24,  8);

        CHECK(ctx, pos0[0] == 1.f && pos0[1] == 2.f && pos0[2] == 3.f);
        CHECK(ctx, nrm0[0] == 0.f && nrm0[1] == 0.f && nrm0[2] == 1.f);
        CHECK(ctx, uv0[0]  == 0.1f && uv0[1] == 0.2f);

        // Vertex 2 starts at byte offset 64 (2 * 32)
        float pos2[3];
        std::memcpy(pos2, sm.rawInterleavedBytes.data() + 64, 12);
        CHECK(ctx, pos2[0] == 7.f && pos2[1] == 8.f && pos2[2] == 9.f);
    }

    // Flat arrays must still be correct (unpack step ran over the block-read bytes)
    CHECK_EQ(ctx, sm.positions.size(), 9u);
    CHECK(ctx, sm.positions[3] == 4.f); // vertex 1 x
    CHECK(ctx, sm.uvs[2] == 0.3f);      // vertex 1 u

    fs::remove_all(root);
}

static void TestVertexLayoutContractPreservesKMeshOffsets(test::TestContext& ctx)
{
    SubMeshData sm;
    sm.positions   = { 0.f, 0.f, 0.f };
    sm.normals     = { 0.f, 0.f, 1.f };
    sm.tangents    = { 1.f, 0.f, 0.f, 1.f };
    sm.uvs         = { 0.f, 0.f };
    sm.boneWeights = { 1.f, 0.f, 0.f, 0.f };
    sm.boneIndices = { 0u, 0u, 0u, 0u };

    std::string error;
    const renderer::VertexLayout layout =
        ResolveVertexLayout(renderer::VertexContracts::Shadow(), sm, &error);

    CHECK(ctx, error.empty());
    CHECK_EQ(ctx, layout.attributes.size(), 3u);
    CHECK_EQ(ctx, layout.bindings.size(), 1u);
    if (!layout.bindings.empty())
        CHECK_EQ(ctx, layout.bindings[0].stride, 80u);

    const renderer::VertexAttribute* position =
        FindAttribute(layout, renderer::VertexSemantic::Position);
    const renderer::VertexAttribute* boneWeight =
        FindAttribute(layout, renderer::VertexSemantic::BoneWeight);
    const renderer::VertexAttribute* boneIndex =
        FindAttribute(layout, renderer::VertexSemantic::BoneIndex);

    CHECK(ctx, position != nullptr);
    CHECK(ctx, boneWeight != nullptr);
    CHECK(ctx, boneIndex != nullptr);
    if (position)   CHECK_EQ(ctx, position->offset, 0u);
    if (boneWeight) CHECK_EQ(ctx, boneWeight->offset, 48u);
    if (boneIndex)  CHECK_EQ(ctx, boneIndex->offset, 64u);
}

static void TestVertexLayoutContractRejectsMissingRequiredSemantics(test::TestContext& ctx)
{
    SubMeshData sm;
    sm.positions = { 0.f, 0.f, 0.f };

    std::string error;
    const renderer::VertexLayout layout =
        ResolveVertexLayout(renderer::VertexContracts::StaticLit(), sm, &error);

    CHECK(ctx, layout.attributes.empty());
    CHECK(ctx, !error.empty());
}

static void TestSceneQueries(test::TestContext& ctx)
{
    ecs::ComponentMetaRegistry componentRegistry = CreateAssetCollisionRegistry();
    ecs::World world(componentRegistry);
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

static void TestRaySphereReportsOriginInsideAsImmediateHit(test::TestContext& ctx)
{
    float t = -1.f;
    CHECK(ctx, SceneQueries::IntersectRaySphere(
        { {0.f,0.f,0.f},{0.f,0.f,-1.f} },
        { {0.f,0.f,0.f}, 100.f },
        10.f,
        t));
    CHECK(ctx, t == 0.f);
}

static void TestRaycastTrianglesUsesNonIndexedTriangleList(test::TestContext& ctx)
{
    ecs::ComponentMetaRegistry componentRegistry = CreateAssetCollisionRegistry();
    ecs::World world(componentRegistry);
    Scene scene(world);
    AssetRegistry registry;

    auto mesh = std::make_unique<MeshAsset>();
    mesh->state = AssetState::Loaded;
    SubMeshData sm;
    sm.positions = { 0.f,0.f,0.f, 1.f,0.f,0.f, 0.f,1.f,0.f };
    mesh->submeshes.push_back(sm);
    MeshHandle mh = registry.GetOrAddMesh("nonindexed.mesh", std::move(mesh));

    EntityID e = scene.CreateEntity("NonIndexedTri");
    world.Add<MeshComponent>(e, mh);
    world.Add<BoundsComponent>(e);
    scene.PropagateTransforms();
    mesh_renderer::UpdateLocalBoundsFromMeshes(world, registry);
    BoundsSystem boundsSystem;
    boundsSystem.Update(world);

    RaycastHit hit{};
    CHECK(ctx, mesh_renderer::MeshSceneQueries::RaycastTriangles(
        world, registry, { {0.25f,0.25f,1.f},{0.f,0.f,-1.f} }, 10.f, hit));
    CHECK_EQ(ctx, hit.entity, e);
    CHECK(ctx, hit.distance >= 0.9f && hit.distance <= 1.1f);
}

static void TestMeshCollisionPipelinePrefersPreciseTriangleOverBoundsFalsePositive(test::TestContext& ctx)
{
    ecs::ComponentMetaRegistry componentRegistry = CreateAssetCollisionRegistry();
    ecs::World world(componentRegistry);
    Scene scene(world);
    AssetRegistry registry;

    auto falsePositiveMesh = std::make_unique<MeshAsset>();
    falsePositiveMesh->state = AssetState::Loaded;
    SubMeshData falsePositiveSubmesh;
    falsePositiveSubmesh.positions = { 10.f,0.f,0.f, 11.f,0.f,0.f, 10.f,1.f,0.f };
    falsePositiveSubmesh.indices = { 0u, 1u, 2u };
    falsePositiveMesh->submeshes.push_back(falsePositiveSubmesh);
    MeshHandle falsePositiveHandle = registry.GetOrAddMesh("false_positive.mesh", std::move(falsePositiveMesh));

    EntityID falsePositive = scene.CreateEntity("BoundsOnlyFalsePositive");
    world.Add<MeshComponent>(falsePositive, falsePositiveHandle);
    auto& falseBounds = world.Add<BoundsComponent>(falsePositive);
    falseBounds.centerLocal = { 0.f, 0.f, 0.f };
    falseBounds.extentsLocal = { 20.f, 20.f, 20.f };

    auto preciseMesh = std::make_unique<MeshAsset>();
    preciseMesh->state = AssetState::Loaded;
    SubMeshData preciseSubmesh;
    preciseSubmesh.positions = { 0.f,0.f,-1.f, 1.f,0.f,-1.f, 0.f,1.f,-1.f };
    preciseSubmesh.indices = { 0u, 1u, 2u };
    preciseMesh->submeshes.push_back(preciseSubmesh);
    MeshHandle preciseHandle = registry.GetOrAddMesh("precise.mesh", std::move(preciseMesh));

    EntityID precise = scene.CreateEntity("PreciseTri");
    world.Add<MeshComponent>(precise, preciseHandle);
    auto& preciseBounds = world.Add<BoundsComponent>(precise);
    preciseBounds.centerLocal = { 0.5f, 0.5f, -1.f };
    preciseBounds.extentsLocal = { 0.5f, 0.5f, 0.1f };

    scene.PropagateTransforms();
    BoundsSystem boundsSystem;
    boundsSystem.Update(world);

    mesh_renderer::MeshCollisionPipeline pipeline;
    pipeline.Build(world);

    RaycastHit hit{};
    mesh_renderer::MeshCollisionRaycastStats stats{};
    CHECK(ctx, pipeline.Raycast(
        world, registry, { {0.25f,0.25f,1.f},{0.f,0.f,-1.f} }, 10.f, hit, {}, &stats));
    CHECK_EQ(ctx, hit.entity, precise);
    CHECK(ctx, hit.distance >= 1.9f && hit.distance <= 2.1f);
    CHECK_EQ(ctx, stats.broadphaseCandidates, 2u);
    CHECK_EQ(ctx, stats.triangleTests, 2u);
}

static void TestMeshBoundsDirtyFlagPreservesWorldRebuild(test::TestContext& ctx)
{
    ecs::ComponentMetaRegistry componentRegistry = CreateAssetCollisionRegistry();
    ecs::World world(componentRegistry);
    Scene scene(world);
    AssetRegistry registry;

    auto mesh = std::make_unique<MeshAsset>();
    mesh->state = AssetState::Loaded;
    SubMeshData sm;
    sm.positions = { 0.f,0.f,0.f, 2.f,0.f,0.f, 0.f,4.f,0.f };
    sm.indices = { 0,1,2 };
    mesh->submeshes.push_back(sm);
    MeshHandle mh = registry.GetOrAddMesh("dirty_bounds.mesh", std::move(mesh));

    EntityID e = scene.CreateEntity("DirtyBounds");
    world.Add<MeshComponent>(e, mh);
    auto& bounds = world.Add<BoundsComponent>(e);
    bounds.centerLocal = { 0.f, 0.f, 0.f };
    bounds.extentsLocal = { 1.f, 1.f, 1.f };
    bounds.centerWorld = { 0.f, 0.f, 0.f };
    bounds.extentsWorld = { 1.f, 1.f, 1.f };
    bounds.localDirty = true;

    scene.PropagateTransforms();

    mesh_renderer::UpdateLocalBoundsFromMeshes(world, registry);
    CHECK(ctx, world.Get<BoundsComponent>(e) != nullptr);
    CHECK(ctx, world.Get<BoundsComponent>(e)->localDirty);

    BoundsSystem boundsSystem;
    boundsSystem.Update(world);

    const BoundsComponent* updated = world.Get<BoundsComponent>(e);
    CHECK(ctx, updated != nullptr);
    CHECK(ctx, updated != nullptr && !updated->localDirty);
    CHECK(ctx, updated != nullptr && std::fabs(updated->centerLocal.x - 1.f) < 1e-6f);
    CHECK(ctx, updated != nullptr && std::fabs(updated->centerLocal.y - 2.f) < 1e-6f);
    CHECK(ctx, updated != nullptr && std::fabs(updated->extentsLocal.x - 1.f) < 1e-6f);
    CHECK(ctx, updated != nullptr && std::fabs(updated->extentsLocal.y - 2.f) < 1e-6f);
    CHECK(ctx, updated != nullptr && std::fabs(updated->centerWorld.x - 1.f) < 1e-6f);
    CHECK(ctx, updated != nullptr && std::fabs(updated->centerWorld.y - 2.f) < 1e-6f);
    CHECK(ctx, updated != nullptr && std::fabs(updated->extentsWorld.x - 1.f) < 1e-6f);
    CHECK(ctx, updated != nullptr && std::fabs(updated->extentsWorld.y - 2.f) < 1e-6f);
}

static void TestSceneDirectiveRegistrySupportsMultipleHandlers(test::TestContext& ctx)
{
    namespace fs = std::filesystem;
    const fs::path root = fs::temp_directory_path() / "krom_scene_directive_registry_test";
    fs::create_directories(root);

    {
        std::ofstream(root / "tri.mesh") << "v 0 0 0\nv 1 0 0\nv 0 1 0\ni 0 1 2\n";
        std::ofstream(root / "test.vert") << "void main(){}\n";
        std::ofstream(root / "test.frag") << "void main(){}\n";
        std::ofstream(root / "mat.mat") << "vertex test.vert\nfragment test.frag\nvec4 tint 1 0 0 1\n";
        std::ofstream(root / "scene.scene")
            << "entity Cube\n"
            << "mesh tri.mesh\n"
            << "material mat.mat\n"
            << "inactive true\n";
    }

    ecs::ComponentMetaRegistry componentRegistry = CreateAssetCollisionRegistry();
    ecs::World world(componentRegistry);
    Scene scene(world);

    AssetRegistry registry;
    AssetPipeline pipeline(registry, nullptr);
    pipeline.SetAssetRoot(root);
    mesh_renderer::ConfigureAssetPipeline(pipeline);
    pipeline.RegisterSceneDirectiveHandler(
        [](const std::string& directive,
           const std::vector<std::string>& parts,
           const AssetPipeline::SceneDirectiveContext& context) -> bool
        {
            if (directive != "inactive" || parts.size() < 2)
                return false;

            if (!context.world.Has<ActiveComponent>(context.entity))
                context.world.Add<ActiveComponent>(context.entity);

            context.world.Get<ActiveComponent>(context.entity)->active = !(parts[1] == "1" || parts[1] == "true");
            return true;
        });

    CHECK(ctx, pipeline.LoadScene("scene.scene", scene));
    CHECK_EQ(ctx, world.EntityCount(), 1u);

    EntityID found = NULL_ENTITY;
    world.View<MeshComponent, MaterialComponent, ActiveComponent>(
        [&](EntityID entity,
            const MeshComponent& mesh,
            const MaterialComponent& material,
            const ActiveComponent& active)
    {
        found = entity;
        CHECK(ctx, mesh.mesh.IsValid());
        CHECK(ctx, material.material.IsValid());
        CHECK(ctx, !active.active);
    });

    CHECK(ctx, found != NULL_ENTITY);
    fs::remove_all(root);
}

// ─── AssetPipeline: glTF materialHandles ─────────────────────────────────────

static void TestAssetPipelineGltfMaterialHandlesRegistered(test::TestContext& ctx)
{
    namespace fs = std::filesystem;
    const fs::path root = fs::temp_directory_path() / "krom_gltf_pipeline_mat_test";
    std::error_code cleanupError;
    fs::remove_all(root, cleanupError);
    fs::create_directories(root);

    {
        const float    positions[] = { 0.f,0.f,0.f,  1.f,0.f,0.f,  0.f,1.f,0.f };
        const uint32_t indices[]   = { 0u, 1u, 2u };
        std::ofstream bin(root / "mesh.bin", std::ios::binary);
        bin.write(reinterpret_cast<const char*>(positions), sizeof(positions));
        bin.write(reinterpret_cast<const char*>(indices),   sizeof(indices));
    }
    {
        std::ofstream out(root / "mesh.gltf");
        out << "{\n"
               "  \"asset\": { \"version\": \"2.0\" },\n"
               "  \"buffers\": [ { \"uri\": \"mesh.bin\", \"byteLength\": 48 } ],\n"
               "  \"bufferViews\": [\n"
               "    { \"buffer\": 0, \"byteOffset\":  0, \"byteLength\": 36, \"target\": 34962 },\n"
               "    { \"buffer\": 0, \"byteOffset\": 36, \"byteLength\": 12, \"target\": 34963 }\n"
               "  ],\n"
               "  \"accessors\": [\n"
               "    { \"bufferView\": 0, \"componentType\": 5126, \"count\": 3, \"type\": \"VEC3\",\n"
               "      \"min\": [0,0,0], \"max\": [1,1,0] },\n"
               "    { \"bufferView\": 1, \"componentType\": 5125, \"count\": 3, \"type\": \"SCALAR\" }\n"
               "  ],\n"
               "  \"materials\": [ { \"name\": \"TestMat\",\n"
               "    \"pbrMetallicRoughness\": {\n"
               "      \"baseColorFactor\": [1.0, 0.5, 0.25, 1.0],\n"
               "      \"metallicFactor\": 0.3,\n"
               "      \"roughnessFactor\": 0.7\n"
               "    } } ],\n"
               "  \"meshes\": [ { \"primitives\": [ {\n"
               "    \"attributes\": { \"POSITION\": 0 },\n"
               "    \"indices\": 1,\n"
               "    \"material\": 0\n"
               "  } ] } ],\n"
               "  \"nodes\": [ { \"mesh\": 0 } ],\n"
               "  \"scene\": 0,\n"
               "  \"scenes\": [ { \"nodes\": [0] } ]\n"
               "}\n";
    }

    {
        GltfImporter importer;
        ImportedAssetBundle bundle = importer.Import((root / "mesh.gltf").string());
        CHECK(ctx, bundle.Ok());
        CHECK_EQ(ctx, bundle.meshes.size(), 1u);
        CHECK_EQ(ctx, bundle.materials.size(), 1u);
        if (!bundle.materials.empty())
            CHECK(ctx, bundle.materials[0].debugName == std::string("TestMat"));
    }

    AssetRegistry registry;
    AssetPipeline pipeline(registry);
    pipeline.RegisterMeshImporter(std::make_unique<GltfImporter>());

    const MeshHandle mh = pipeline.LoadMesh((root / "mesh.gltf").string());
    CHECK(ctx, mh.IsValid());

    const MeshAsset* mesh = registry.meshes.Get(mh);
    CHECK(ctx, mesh != nullptr);
    if (!mesh) { fs::remove_all(root); return; }

    CHECK_EQ(ctx, mesh->materialHandles.size(), 1u);
    CHECK_EQ(ctx, mesh->submeshes.size(), 1u);
    if (!mesh->submeshes.empty())
        CHECK_EQ(ctx, mesh->submeshes[0].materialIndex, 0u);

    if (!mesh->materialHandles.empty())
    {
        const MaterialHandle matH = mesh->materialHandles[0];
        CHECK(ctx, matH.IsValid());

        const MaterialAsset* mat = registry.materials.Get(matH);
        CHECK(ctx, mat != nullptr);
        if (mat)
        {
            CHECK(ctx, mat->debugName == std::string("TestMat"));
            const auto Near = [](float a, float b){ return std::fabs(a - b) <= 1e-5f; };
            CHECK(ctx, Near(mat->baseColorFactor.x, 1.0f));
            CHECK(ctx, Near(mat->baseColorFactor.y, 0.5f));
            CHECK(ctx, Near(mat->metallicFactor, 0.3f));
            CHECK(ctx, Near(mat->roughnessFactor, 0.7f));
            CHECK(ctx, !mat->baseColorTexture.texture.IsValid());
            CHECK(ctx, mat->baseColorTexture.path.empty());
        }
    }

    fs::remove_all(root);
}

int RunAssetsCollisionTests()
{
    test::TestSuite suite("AssetsCollision");
    suite.Add("AssetPipelineLoadAndReload", TestAssetPipelineLoadAndReload)
        .Add("KMesh round-trip", TestKMeshRoundTrip)
        .Add("KMesh raw fast path fills interleaved bytes", TestKMeshRawFastPathFillsInterleavedBytes)
        .Add("Vertex layout contract preserves KMesh offsets", TestVertexLayoutContractPreservesKMeshOffsets)
        .Add("Vertex layout contract rejects missing required semantics", TestVertexLayoutContractRejectsMissingRequiredSemantics)
        .Add("SceneQueries", TestSceneQueries)
        .Add("RaySphere reports origin inside as immediate hit", TestRaySphereReportsOriginInsideAsImmediateHit)
        .Add("RaycastTriangles uses non-indexed triangle list", TestRaycastTrianglesUsesNonIndexedTriangleList)
        .Add("MeshCollisionPipeline prefers precise triangle over bounds false positive", TestMeshCollisionPipelinePrefersPreciseTriangleOverBoundsFalsePositive)
        .Add("MeshBounds dirty flag preserves world rebuild", TestMeshBoundsDirtyFlagPreservesWorldRebuild)
        .Add("Scene directive registry supports multiple handlers", TestSceneDirectiveRegistrySupportsMultipleHandlers)
        .Add("AssetPipeline glTF materialHandles registered", TestAssetPipelineGltfMaterialHandlesRegistered);
    return suite.Run();
}
