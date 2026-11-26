// =============================================================================
// KROM Engine - tests/test_gi.cpp
// Tests for the surface cache GI series (doc/PLAN_GI.md).
//
// Package 2 (end of file): trace shaders compile for DX11 and Vulkan; the HLSL
// trace kernel on WARP against SceneBvh (CPU reference).
//
// Package 1 (below): MeshBvh, SceneBvh and MeshSceneBvh against brute force,
// refit/rebuild, change list, resting bit, timings.
//
// Package 0: the radiance cache inserts keys into a hash table with
// InterlockedCompareExchange on a RWStructuredBuffer<uint>. This test compiles
// such a compute shader for DX11 (FXC, cs_5_0) and Vulkan (DXC -spirv), runs
// the DX11 bytecode on WARP and checks the result, and checks that the SPIR-V
// contains OpAtomicCompareExchange.
// =============================================================================
#include "TestFramework.hpp"
#include "GltfImporter.hpp"
#include "addons/mesh_renderer/MeshRendererComponents.hpp"
#include "addons/mesh_renderer/MeshSceneBvh.hpp"
#include "assets/AssetRegistry.hpp"
#include "collision/MeshBvh.hpp"
#include "collision/SceneBvh.hpp"
#include "core/Debug.hpp"
#include "core/PathUtf8.hpp"
#include "ecs/Components.hpp"
#include "jobs/JobSystem.hpp"
#include "renderer/ShaderCompiler.hpp"
#include "renderer/ShaderToolPaths.hpp"
#include "scene/Scene.hpp"
#include "SurfaceCachePass.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <memory>
#include <random>
#include <string>
#include <vector>

#if defined(_WIN32) && defined(KROM_DX11_BACKEND)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <d3d11.h>
#include <wrl/client.h>
#endif

using namespace engine;
using namespace engine::renderer;

namespace {

constexpr uint32_t kGiHashTableSize = 64u;
constexpr uint32_t kGiHashThreads   = 256u;   // 4 groups of 64
constexpr uint32_t kGiHashKeyCount  = 37u;    // distinct keys 1..37

// Open addressing with linear probing, as the cache will do it: claim an empty
// slot with CompareExchange, or find the slot that already holds the key, then
// count the thread there with InterlockedAdd.
const char* const kGiHashInsertCsHlsl = R"(
RWStructuredBuffer<uint> gKeys   : register(u0);
RWStructuredBuffer<uint> gCounts : register(u1);

static const uint kTableSize = 64u;
static const uint kEmpty     = 0u;

[numthreads(64, 1, 1)]
void main(uint3 dtid : SV_DispatchThreadID)
{
    const uint key = (dtid.x % 37u) + 1u;
    uint slot = (key * 2654435761u) & (kTableSize - 1u);
    [loop] for (uint probe = 0u; probe < kTableSize; ++probe)
    {
        uint previous = kEmpty;
        InterlockedCompareExchange(gKeys[slot], kEmpty, key, previous);
        if (previous == kEmpty || previous == key)
        {
            InterlockedAdd(gCounts[slot], 1u);
            return;
        }
        slot = (slot + 1u) & (kTableSize - 1u);
    }
}
)";

// CPU expectation: thread x inserts key (x % 37) + 1.
uint32_t ExpectedCountForKey(uint32_t key)
{
    uint32_t count = 0u;
    for (uint32_t x = 0u; x < kGiHashThreads; ++x)
        if ((x % kGiHashKeyCount) + 1u == key)
            ++count;
    return count;
}

// Checks a filled table: every key 1..37 exactly once, with the right count,
// and nothing else.
void CheckHashTable(test::TestContext& ctx, const uint32_t* keys, const uint32_t* counts)
{
    std::vector<uint32_t> seen(kGiHashKeyCount + 1u, 0u);
    uint32_t total = 0u;
    for (uint32_t i = 0u; i < kGiHashTableSize; ++i)
    {
        if (keys[i] == 0u)
        {
            CHECK_EQ(ctx, counts[i], 0u);
            continue;
        }
        CHECK(ctx, keys[i] >= 1u && keys[i] <= kGiHashKeyCount);
        if (keys[i] < 1u || keys[i] > kGiHashKeyCount)
            continue;
        ++seen[keys[i]];
        CHECK_EQ(ctx, counts[i], ExpectedCountForKey(keys[i]));
        total += counts[i];
    }
    for (uint32_t key = 1u; key <= kGiHashKeyCount; ++key)
        CHECK_EQ(ctx, seen[key], 1u);
    CHECK_EQ(ctx, total, kGiHashThreads);
}

assets::ShaderAsset MakeHashInsertShader()
{
    assets::ShaderAsset cs{};
    cs.debugName = "GiHashInsertCS";
    cs.stage = assets::ShaderStage::Compute;
    cs.sourceLanguage = assets::ShaderSourceLanguage::HLSL;
    cs.entryPoint = "main";
    cs.sourceCode = kGiHashInsertCsHlsl;
    return cs;
}

// True if the SPIR-V module contains an instruction with this opcode.
bool SpirvHasOpcode(const std::vector<uint8_t>& bytecode, uint16_t opcode)
{
    if (bytecode.size() < 20u || bytecode.size() % 4u != 0u)
        return false;
    std::vector<uint32_t> words(bytecode.size() / 4u);
    std::memcpy(words.data(), bytecode.data(), bytecode.size());
    if (words[0] != 0x07230203u)   // SPIR-V magic
        return false;
    size_t i = 5u;                 // after the header
    while (i < words.size())
    {
        const uint32_t wordCount = words[i] >> 16u;
        if ((words[i] & 0xFFFFu) == opcode)
            return true;
        if (wordCount == 0u)
            return false;
        i += wordCount;
    }
    return false;
}

void TestHashInsertCompareExchangeDx11(test::TestContext& ctx)
{
    const assets::ShaderAsset cs = MakeHashInsertShader();
    assets::CompiledShaderArtifact compiled{};
    std::string error;
    const bool ok = ShaderCompiler::CompileForTarget(cs, assets::ShaderTargetProfile::DirectX11_SM5, compiled, &error);
    if (!ok)
        std::fprintf(stderr, "test_gi.cpp: DX11 compile of GiHashInsertCS failed: %s\n", error.c_str());
    CHECK(ctx, ok);
    CHECK(ctx, !compiled.bytecode.empty());
    if (!ok || compiled.bytecode.empty())
        return;

#if defined(_WIN32) && defined(KROM_DX11_BACKEND)
    using Microsoft::WRL::ComPtr;
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> command;
    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0,
                                   D3D11_SDK_VERSION, &device, nullptr, &command);
    CHECK(ctx, SUCCEEDED(hr));
    if (FAILED(hr))
        return;

    ComPtr<ID3D11ComputeShader> shader;
    hr = device->CreateComputeShader(compiled.bytecode.data(), compiled.bytecode.size(), nullptr, &shader);
    CHECK(ctx, SUCCEEDED(hr));
    if (FAILED(hr))
        return;

    const auto makeBuffer = [&](ComPtr<ID3D11Buffer>& buffer, ComPtr<ID3D11UnorderedAccessView>& uav,
                                ComPtr<ID3D11Buffer>& staging) -> bool
    {
        D3D11_BUFFER_DESC desc{};
        desc.ByteWidth = kGiHashTableSize * sizeof(uint32_t);
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
        desc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
        desc.StructureByteStride = sizeof(uint32_t);
        const std::vector<uint32_t> zeros(kGiHashTableSize, 0u);
        D3D11_SUBRESOURCE_DATA init{ zeros.data(), 0u, 0u };
        if (FAILED(device->CreateBuffer(&desc, &init, &buffer)))
            return false;
        D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc{};
        uavDesc.Format = DXGI_FORMAT_UNKNOWN;
        uavDesc.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
        uavDesc.Buffer.NumElements = kGiHashTableSize;
        if (FAILED(device->CreateUnorderedAccessView(buffer.Get(), &uavDesc, &uav)))
            return false;
        D3D11_BUFFER_DESC stagingDesc = desc;
        stagingDesc.Usage = D3D11_USAGE_STAGING;
        stagingDesc.BindFlags = 0u;
        stagingDesc.MiscFlags = 0u;
        stagingDesc.StructureByteStride = 0u;
        stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        return SUCCEEDED(device->CreateBuffer(&stagingDesc, nullptr, &staging));
    };

    ComPtr<ID3D11Buffer> keys, counts, keysStaging, countsStaging;
    ComPtr<ID3D11UnorderedAccessView> keysUav, countsUav;
    const bool buffersOk = makeBuffer(keys, keysUav, keysStaging) && makeBuffer(counts, countsUav, countsStaging);
    CHECK(ctx, buffersOk);
    if (!buffersOk)
        return;

    ID3D11UnorderedAccessView* uavs[2] = { keysUav.Get(), countsUav.Get() };
    command->CSSetShader(shader.Get(), nullptr, 0u);
    command->CSSetUnorderedAccessViews(0u, 2u, uavs, nullptr);
    command->Dispatch(kGiHashThreads / 64u, 1u, 1u);
    ID3D11UnorderedAccessView* nullUavs[2] = {};
    command->CSSetUnorderedAccessViews(0u, 2u, nullUavs, nullptr);
    command->CopyResource(keysStaging.Get(), keys.Get());
    command->CopyResource(countsStaging.Get(), counts.Get());

    D3D11_MAPPED_SUBRESOURCE mappedKeys{}, mappedCounts{};
    const bool mapped = SUCCEEDED(command->Map(keysStaging.Get(), 0u, D3D11_MAP_READ, 0u, &mappedKeys)) &&
                        SUCCEEDED(command->Map(countsStaging.Get(), 0u, D3D11_MAP_READ, 0u, &mappedCounts));
    CHECK(ctx, mapped);
    if (mapped)
        CheckHashTable(ctx, static_cast<const uint32_t*>(mappedKeys.pData),
                       static_cast<const uint32_t*>(mappedCounts.pData));
    command->Unmap(keysStaging.Get(), 0u);
    command->Unmap(countsStaging.Get(), 0u);
#else
    std::fprintf(stdout, "  # test_gi.cpp: no DX11 backend, WARP run skipped\n");
#endif
}

void TestHashInsertCompareExchangeVulkan(test::TestContext& ctx)
{
    if (FindShaderTool(ShaderTool::Dxc).path.empty())
    {
        std::fprintf(stdout, "  # test_gi.cpp: dxc.exe not found, Vulkan compile skipped\n");
        return;
    }
    const assets::ShaderAsset cs = MakeHashInsertShader();
    assets::CompiledShaderArtifact compiled{};
    std::string error;
    const bool ok = ShaderCompiler::CompileForTarget(cs, assets::ShaderTargetProfile::Vulkan_SPIRV, compiled, &error);
    if (!ok)
        std::fprintf(stderr, "test_gi.cpp: Vulkan compile of GiHashInsertCS failed: %s\n", error.c_str());
    CHECK(ctx, ok);
    constexpr uint16_t kOpAtomicCompareExchange = 230u;
    constexpr uint16_t kOpAtomicIAdd = 234u;
    CHECK(ctx, SpirvHasOpcode(compiled.bytecode, kOpAtomicCompareExchange));
    CHECK(ctx, SpirvHasOpcode(compiled.bytecode, kOpAtomicIAdd));
}

// =============================================================================
// Package 1: BVH on the CPU
// =============================================================================
using collision::AABB;
using collision::BvhTriangle;
using collision::MeshBvh;
using collision::SceneBvh;
using collision::SceneBvhChangeKind;
using collision::SceneBvhInstanceDesc;
using collision::SceneBvhUpdateKind;
using collision::SceneRayHit;
using math::Mat4;
using math::Quat;
using math::Vec3;

struct TestMesh
{
    std::vector<float>    positions;
    std::vector<uint32_t> indices;
};

TestMesh MakeTriangle()
{
    return TestMesh{ { 0.f, 0.f, 0.f,  1.f, 0.f, 0.f,  0.f, 1.f, 0.f }, { 0u, 1u, 2u } };
}

TestMesh MakeCube(float h)
{
    TestMesh m;
    for (int i = 0; i < 8; ++i)
    {
        m.positions.push_back((i & 1) ? h : -h);
        m.positions.push_back((i & 2) ? h : -h);
        m.positions.push_back((i & 4) ? h : -h);
    }
    const uint32_t faces[6][4] = { {0,2,6,4}, {1,5,7,3}, {0,4,5,1}, {2,3,7,6}, {0,1,3,2}, {4,6,7,5} };
    for (const auto& f : faces)
    {
        m.indices.insert(m.indices.end(), { f[0], f[1], f[2], f[0], f[2], f[3] });
    }
    return m;
}

// n x n quads in XZ with a wavy height: many small triangles.
TestMesh MakeGrid(uint32_t n, float size)
{
    TestMesh m;
    for (uint32_t z = 0u; z <= n; ++z)
        for (uint32_t x = 0u; x <= n; ++x)
        {
            const float fx = (static_cast<float>(x) / n - 0.5f) * size;
            const float fz = (static_cast<float>(z) / n - 0.5f) * size;
            m.positions.insert(m.positions.end(), { fx, 0.3f * std::sin(fx * 3.f) * std::cos(fz * 2.f), fz });
        }
    for (uint32_t z = 0u; z < n; ++z)
        for (uint32_t x = 0u; x < n; ++x)
        {
            const uint32_t i = z * (n + 1u) + x;
            m.indices.insert(m.indices.end(), { i, i + 1u, i + n + 1u, i + 1u, i + n + 2u, i + n + 1u });
        }
    return m;
}

// UV sphere, fallback when the dragon cannot be loaded.
TestMesh MakeSphere(uint32_t rings, uint32_t segments, float r)
{
    TestMesh m;
    for (uint32_t y = 0u; y <= rings; ++y)
    {
        const float th = 3.14159265f * y / rings;
        for (uint32_t x = 0u; x <= segments; ++x)
        {
            const float ph = 6.2831853f * x / segments;
            m.positions.insert(m.positions.end(), { r * std::sin(th) * std::cos(ph), r * std::cos(th), r * std::sin(th) * std::sin(ph) });
        }
    }
    for (uint32_t y = 0u; y < rings; ++y)
        for (uint32_t x = 0u; x < segments; ++x)
        {
            const uint32_t i = y * (segments + 1u) + x;
            m.indices.insert(m.indices.end(), { i, i + segments + 1u, i + 1u, i + 1u, i + segments + 1u, i + segments + 2u });
        }
    return m;
}

// All submeshes of assets/Dragon/drache.glb merged into one mesh.
bool LoadDragon(TestMesh& out)
{
    const std::filesystem::path path = PathFromUtf8(KROM_SOURCE_DIR) / "assets" / "Dragon" / "drache.glb";
    if (!std::filesystem::exists(path))
        return false;
    engine::addons::gltf::GltfImporter importer;
    const assets::ImportedAssetBundle bundle = importer.Import(PathToUtf8(path));
    if (!bundle.Ok())
        return false;
    out = {};
    for (const assets::MeshAsset& mesh : bundle.meshes)
        for (const assets::SubMeshData& sub : mesh.submeshes)
        {
            const uint32_t base = static_cast<uint32_t>(out.positions.size() / 3u);
            out.positions.insert(out.positions.end(), sub.positions.begin(), sub.positions.end());
            for (const uint32_t i : sub.indices)
                out.indices.push_back(base + i);
        }
    return !out.indices.empty();
}

AABB MeshBounds(const TestMesh& m)
{
    AABB b{ Vec3{ 1e30f, 1e30f, 1e30f }, Vec3{ -1e30f, -1e30f, -1e30f } };
    for (size_t i = 0u; i + 2u < m.positions.size(); i += 3u)
    {
        b.min = Vec3{ std::min(b.min.x, m.positions[i]), std::min(b.min.y, m.positions[i + 1u]), std::min(b.min.z, m.positions[i + 2u]) };
        b.max = Vec3{ std::max(b.max.x, m.positions[i]), std::max(b.max.y, m.positions[i + 1u]), std::max(b.max.z, m.positions[i + 2u]) };
    }
    return b;
}

// Reference: every triangle, transformed to world, tested with the world ray.
struct BruteInstance
{
    const TestMesh* mesh = nullptr;
    Mat4 objectToWorld = Mat4::Identity();
};

float BruteForceClosest(const std::vector<BruteInstance>& instances, const Vec3& origin, const Vec3& dir,
                        float tMax, size_t* outInstance = nullptr)
{
    float best = -1.f;
    for (size_t k = 0u; k < instances.size(); ++k)
    {
        const TestMesh& m = *instances[k].mesh;
        const Mat4& w = instances[k].objectToWorld;
        for (size_t t = 0u; t + 2u < m.indices.size(); t += 3u)
        {
            BvhTriangle tri{};
            const uint32_t idx[3] = { m.indices[t], m.indices[t + 1u], m.indices[t + 2u] };
            float* dst[3] = { tri.v0, tri.v1, tri.v2 };
            for (int c = 0; c < 3; ++c)
            {
                const Vec3 p = w.TransformPoint(Vec3{ m.positions[idx[c] * 3u], m.positions[idx[c] * 3u + 1u], m.positions[idx[c] * 3u + 2u] });
                dst[c][0] = p.x; dst[c][1] = p.y; dst[c][2] = p.z;
            }
            float hitT = 0.f, u = 0.f, v = 0.f;
            if (collision::BvhRayTriangle(tri, origin, dir, 0.f, best >= 0.f ? best : tMax, hitT, u, v))
            {
                best = hitT;
                if (outInstance)
                    *outInstance = k;
            }
        }
    }
    return best;
}

// Rays from a sphere around the box towards random points inside it (mostly
// hits), every fourth one in a random direction (mostly misses).
struct TestRay { Vec3 origin; Vec3 dir; };

std::vector<TestRay> MakeRays(const AABB& box, size_t count, uint32_t seed)
{
    std::mt19937 rng(seed);
    std::uniform_real_distribution<float> u01(0.f, 1.f);
    const Vec3 c = (box.min + box.max) * 0.5f;
    const float radius = std::max((box.max - box.min).Length(), 1e-3f);
    std::vector<TestRay> rays;
    rays.reserve(count);
    for (size_t i = 0u; i < count; ++i)
    {
        Vec3 onSphere{ u01(rng) * 2.f - 1.f, u01(rng) * 2.f - 1.f, u01(rng) * 2.f - 1.f };
        if (onSphere.LengthSq() < 1e-6f)
            onSphere = Vec3{ 1.f, 0.f, 0.f };
        const Vec3 origin = c + onSphere.Normalized() * radius;
        Vec3 target{ box.min.x + (box.max.x - box.min.x) * u01(rng),
                     box.min.y + (box.max.y - box.min.y) * u01(rng),
                     box.min.z + (box.max.z - box.min.z) * u01(rng) };
        Vec3 dir = (target - origin).Normalized();
        if (i % 4u == 3u)
            dir = Vec3{ u01(rng) * 2.f - 1.f, u01(rng) * 2.f - 1.f, u01(rng) * 2.f - 1.f }.Normalized();
        if (dir.LengthSq() < 0.5f)
            dir = Vec3{ 0.f, 0.f, 1.f };
        rays.push_back({ origin, dir });
    }
    return rays;
}

bool SameDistance(float a, float b)
{
    return std::fabs(a - b) <= 1e-4f * std::max(1.f, std::fabs(b));
}

// Compares a mesh BVH with brute force; returns the number of rays that disagree.
size_t CompareMeshWithBruteForce(const MeshBvh& bvh, const TestMesh& mesh, const std::vector<TestRay>& rays,
                                 size_t* outHits = nullptr)
{
    const std::vector<BruteInstance> brute{ { &mesh, Mat4::Identity() } };
    size_t mismatches = 0u, hits = 0u;
    for (const TestRay& ray : rays)
    {
        collision::BvhTriangleHit hit{};
        const bool bvhHit = bvh.IntersectClosest(ray.origin, ray.dir, 0.f, 1e30f, hit);
        const float ref = BruteForceClosest(brute, ray.origin, ray.dir, 1e30f);
        const bool refHit = ref >= 0.f;
        hits += refHit ? 1u : 0u;
        if (bvhHit != refHit || (bvhHit && !SameDistance(hit.t, ref)))
            ++mismatches;
        else if (bvhHit != bvh.IntersectAny(ray.origin, ray.dir, 0.f, 1e30f))
            ++mismatches;
    }
    if (outHits)
        *outHits = hits;
    return mismatches;
}

void TestBvhSingleTriangle(test::TestContext& ctx)
{
    const TestMesh m = MakeTriangle();
    MeshBvh bvh;
    CHECK(ctx, bvh.Build(m.positions, m.indices));
    CHECK_EQ(ctx, bvh.Nodes().size(), static_cast<size_t>(1u));
    CHECK_EQ(ctx, bvh.Triangles().size(), static_cast<size_t>(1u));

    collision::BvhTriangleHit hit{};
    CHECK(ctx, bvh.IntersectClosest(Vec3{ 0.25f, 0.25f, 2.f }, Vec3{ 0.f, 0.f, -1.f }, 0.f, 100.f, hit));
    CHECK(ctx, std::fabs(hit.t - 2.f) < 1e-5f);
    CHECK(ctx, std::fabs(hit.u - 0.25f) < 1e-5f && std::fabs(hit.v - 0.25f) < 1e-5f);
    // From behind (both faces count), outside, beyond tMax, parallel.
    CHECK(ctx, bvh.IntersectClosest(Vec3{ 0.25f, 0.25f, -1.f }, Vec3{ 0.f, 0.f, 1.f }, 0.f, 100.f, hit));
    CHECK(ctx, !bvh.IntersectClosest(Vec3{ 0.8f, 0.8f, 2.f }, Vec3{ 0.f, 0.f, -1.f }, 0.f, 100.f, hit));
    CHECK(ctx, !bvh.IntersectClosest(Vec3{ 0.25f, 0.25f, 2.f }, Vec3{ 0.f, 0.f, -1.f }, 0.f, 1.5f, hit));
    CHECK(ctx, !bvh.IntersectAny(Vec3{ -1.f, 0.25f, 0.f }, Vec3{ 1.f, 0.f, 0.f }, 0.f, 100.f));
}

void TestBvhCube(test::TestContext& ctx)
{
    const TestMesh m = MakeCube(1.f);
    MeshBvh bvh;
    CHECK(ctx, bvh.Build(m.positions, m.indices));
    const AABB b = bvh.Bounds();
    CHECK(ctx, b.min.x == -1.f && b.max.z == 1.f);
    const Vec3 dirs[6] = { {1,0,0}, {-1,0,0}, {0,1,0}, {0,-1,0}, {0,0,1}, {0,0,-1} };
    for (const Vec3& d : dirs)
    {
        collision::BvhTriangleHit hit{};
        const Vec3 origin = d * -5.f + Vec3{ 0.1f, 0.2f, 0.3f } * 0.5f;
        CHECK(ctx, bvh.IntersectClosest(origin, d, 0.f, 100.f, hit));
        const float expected = 4.f - Vec3::Dot(Vec3{ 0.05f, 0.1f, 0.15f }, d);   // near face at -1 along d
        CHECK(ctx, std::fabs(hit.t - expected) < 1e-4f);
        // From inside: hits the far face at distance 1 - offset.
        CHECK(ctx, bvh.IntersectClosest(Vec3{ 0.f, 0.f, 0.f }, d, 0.f, 100.f, hit));
        CHECK(ctx, std::fabs(hit.t - 1.f) < 1e-5f);
    }
    collision::BvhTriangleHit miss{};
    CHECK(ctx, !bvh.IntersectClosest(Vec3{ 3.f, 3.f, -5.f }, Vec3{ 0.f, 0.f, 1.f }, 0.f, 100.f, miss));
    const std::vector<TestRay> rays = MakeRays(b, 500u, 7u);
    CHECK_EQ(ctx, CompareMeshWithBruteForce(bvh, m, rays), static_cast<size_t>(0u));
}

void TestBvhGridVsBruteForce(test::TestContext& ctx)
{
    const TestMesh m = MakeGrid(48u, 10.f);
    MeshBvh bvh;
    CHECK(ctx, bvh.Build(m.positions, m.indices));
    CHECK_EQ(ctx, bvh.Triangles().size(), static_cast<size_t>(48u * 48u * 2u));
    for (const collision::BvhNode& n : bvh.Nodes())
        if (n.count > 0u)
            CHECK(ctx, n.count <= MeshBvh::kMaxLeafTriangles);
    size_t hits = 0u;
    const std::vector<TestRay> rays = MakeRays(bvh.Bounds(), 1000u, 11u);
    CHECK_EQ(ctx, CompareMeshWithBruteForce(bvh, m, rays, &hits), static_cast<size_t>(0u));
    CHECK(ctx, hits > 300u);
    // Triangle index of a hit names the source triangle.
    collision::BvhTriangleHit hit{};
    CHECK(ctx, bvh.IntersectClosest(Vec3{ -4.99f, 5.f, -4.99f }, Vec3{ 0.f, -1.f, 0.f }, 0.f, 100.f, hit));
    CHECK(ctx, bvh.Triangles()[hit.triangle].primitive < 2u);
}

// Builds a BLAS, checks it against brute force and prints build time and CPU rays per second.
void MeasureBlas(test::TestContext& ctx, const char* name, const TestMesh& m, size_t compareRays)
{
    MeshBvh bvh;
    CHECK(ctx, bvh.Build(m.positions, m.indices));
    const collision::MeshBvhBuildStats& stats = bvh.BuildStats();
    std::fprintf(stdout, "  # test_gi.cpp: BLAS %s: %u triangles (%u degenerate dropped), %u nodes, %u leaves, depth %u, build %.2f ms\n",
                 name, stats.inputTriangles - stats.degenerateTriangles,
                 stats.degenerateTriangles, stats.nodes, stats.leaves, stats.maxDepth, stats.buildMs);

    const std::vector<TestRay> rays = MakeRays(bvh.Bounds(), compareRays, 23u);
    size_t hits = 0u;
    CHECK_EQ(ctx, CompareMeshWithBruteForce(bvh, m, rays, &hits), static_cast<size_t>(0u));
    CHECK(ctx, hits * 10u > compareRays);

    // CPU rays per second on one core (closest and any hit).
    const std::vector<TestRay> perfRays = MakeRays(bvh.Bounds(), 100000u, 29u);
    collision::BvhTraversalStats traversal{};
    auto start = std::chrono::steady_clock::now();
    size_t closestHits = 0u;
    for (const TestRay& ray : perfRays)
    {
        collision::BvhTriangleHit hit{};
        closestHits += bvh.IntersectClosest(ray.origin, ray.dir, 0.f, 1e30f, hit, &traversal) ? 1u : 0u;
    }
    const double closestS = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    start = std::chrono::steady_clock::now();
    size_t anyHits = 0u;
    for (const TestRay& ray : perfRays)
        anyHits += bvh.IntersectAny(ray.origin, ray.dir, 0.f, 1e30f) ? 1u : 0u;
    const double anyS = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    CHECK_EQ(ctx, closestHits, anyHits);
    CHECK_EQ(ctx, traversal.stackOverflows, 0u);
    std::fprintf(stdout, "  # test_gi.cpp: CPU rays/s (1 core): closest %.0f, any %.0f; %.1f nodes and %.1f triangle tests per closest ray, max stack %u\n",
                 perfRays.size() / closestS, perfRays.size() / anyS,
                 static_cast<double>(traversal.nodesVisited) / perfRays.size(),
                 static_cast<double>(traversal.triangleTests) / perfRays.size(), traversal.maxStackDepth);
}

void TestBvhDragonVsBruteForce(test::TestContext& ctx)
{
    TestMesh dragon;
    if (LoadDragon(dragon))
        MeasureBlas(ctx, "dragon (assets/Dragon/drache.glb)", dragon, 2000u);
    else
        std::fprintf(stdout, "  # test_gi.cpp: assets/Dragon/drache.glb not loadable, procedural mesh only\n");
    // The dragon is low-poly; a dense mesh gives meaningful build and ray numbers.
    MeasureBlas(ctx, "sphere 120x240", MakeSphere(120u, 240u, 1.f), 300u);
}

void TestBvhDegenerateAndEmpty(test::TestContext& ctx)
{
    MeshBvh empty;
    CHECK(ctx, !empty.Build(std::vector<float>{}, std::vector<uint32_t>{}));
    CHECK(ctx, empty.Empty());
    collision::BvhTriangleHit hit{};
    CHECK(ctx, !empty.IntersectClosest(Vec3{}, Vec3{ 0.f, 0.f, 1.f }, 0.f, 10.f, hit));
    CHECK(ctx, !empty.IntersectAny(Vec3{}, Vec3{ 0.f, 0.f, 1.f }, 0.f, 10.f));

    // Zero-area, repeated vertex, NaN, out-of-range index; one good triangle.
    const float nan = std::nanf("");
    const std::vector<float> positions = { 0,0,0, 1,0,0, 2,0,0, 0,1,0, nan,0,0 };
    const std::vector<uint32_t> indices = { 0,1,2,  0,0,1,  0,1,4,  0,1,9,  0,1,3,  7 };
    MeshBvh bvh;
    CHECK(ctx, bvh.Build(positions, indices));
    CHECK_EQ(ctx, bvh.BuildStats().inputTriangles, 5u);
    CHECK_EQ(ctx, bvh.BuildStats().degenerateTriangles, 4u);
    CHECK_EQ(ctx, bvh.Triangles().size(), static_cast<size_t>(1u));
    CHECK_EQ(ctx, bvh.Triangles()[0].primitive, 4u);
    CHECK(ctx, bvh.IntersectClosest(Vec3{ 0.2f, 0.2f, 1.f }, Vec3{ 0.f, 0.f, -1.f }, 0.f, 10.f, hit));

    // Only degenerate triangles: empty, no crash.
    MeshBvh allBad;
    CHECK(ctx, !allBad.Build(std::vector<float>{ 0,0,0, 1,1,1, 2,2,2 }, std::vector<uint32_t>{ 0,1,2 }));
    CHECK(ctx, allBad.Empty());

    // Many identical triangles (equal centroids): splits must still terminate.
    std::vector<uint32_t> same;
    for (int i = 0; i < 100; ++i)
        same.insert(same.end(), { 0u, 1u, 3u });
    MeshBvh stacked;
    CHECK(ctx, stacked.Build(positions, same));
    for (const collision::BvhNode& n : stacked.Nodes())
        if (n.count > 0u)
            CHECK(ctx, n.count <= MeshBvh::kMaxLeafTriangles);
    CHECK(ctx, stacked.IntersectAny(Vec3{ 0.2f, 0.2f, 1.f }, Vec3{ 0.f, 0.f, -1.f }, 0.f, 10.f));
}

std::shared_ptr<const MeshBvh> BuildShared(const TestMesh& m)
{
    auto bvh = std::make_shared<MeshBvh>();
    bvh->Build(m.positions, m.indices);
    return bvh;
}

SceneBvhInstanceDesc MakeInstance(uint64_t key, std::shared_ptr<const MeshBvh> blas, const Mat4& m, uint64_t version = 1u)
{
    SceneBvhInstanceDesc d{};
    d.key = key;
    d.entity = static_cast<uint32_t>(key);
    d.submesh = 0u;
    d.materialId = static_cast<uint32_t>(key) * 10u;
    d.blas = std::move(blas);
    d.objectToWorld = m;
    d.transformVersion = version;
    return d;
}

// Scene BVH against brute force; returns rays that disagree (distance or instance).
size_t CompareSceneWithBruteForce(const SceneBvh& scene, const std::vector<BruteInstance>& brute,
                                  const std::vector<uint64_t>& keys, const std::vector<TestRay>& rays,
                                  size_t* outHits = nullptr)
{
    size_t mismatches = 0u, hits = 0u;
    for (const TestRay& ray : rays)
    {
        SceneRayHit hit{};
        const bool bvhHit = scene.TraceClosest(ray.origin, ray.dir, 0.f, 1e30f, hit);
        size_t refInstance = 0u;
        const float ref = BruteForceClosest(brute, ray.origin, ray.dir, 1e30f, &refInstance);
        const bool refHit = ref >= 0.f;
        hits += refHit ? 1u : 0u;
        if (bvhHit != refHit)
            ++mismatches;
        else if (bvhHit && (!SameDistance(hit.distance, ref) ||
                            (hit.key != keys[refInstance] && std::fabs(hit.distance - ref) > 1e-5f)))
            ++mismatches;
        else if (bvhHit != scene.TraceAny(ray.origin, ray.dir, 0.f, 1e30f))
            ++mismatches;
    }
    if (outHits)
        *outHits = hits;
    return mismatches;
}

AABB SceneBounds(const SceneBvh& scene)
{
    AABB b{ Vec3{ 1e30f, 1e30f, 1e30f }, Vec3{ -1e30f, -1e30f, -1e30f } };
    for (uint32_t i = 0u; i < scene.InstanceCount(); ++i)
    {
        const AABB ib = scene.InstanceBounds(i);
        b.min = Vec3{ std::min(b.min.x, ib.min.x), std::min(b.min.y, ib.min.y), std::min(b.min.z, ib.min.z) };
        b.max = Vec3{ std::max(b.max.x, ib.max.x), std::max(b.max.y, ib.max.y), std::max(b.max.z, ib.max.z) };
    }
    return b;
}

void TestSceneBvhInstancesVsBruteForce(test::TestContext& ctx)
{
    const TestMesh grid = MakeGrid(16u, 2.f);
    const TestMesh cube = MakeCube(0.5f);
    const auto gridBlas = BuildShared(grid);
    const auto cubeBlas = BuildShared(cube);
    const Mat4 transforms[5] = {
        Mat4::Identity(),
        Mat4::Translation(Vec3{ 3.f, 0.5f, -1.f }),
        Mat4::TRS(Vec3{ -2.f, 1.f, 2.f }, Quat::FromEulerDeg(30.f, 45.f, 10.f), Vec3{ 1.f, 1.f, 1.f }),
        Mat4::TRS(Vec3{ 0.f, 2.f, 0.f }, Quat::FromEulerDeg(0.f, 20.f, 70.f), Vec3{ 2.f, 0.5f, 1.5f }),
        Mat4::TRS(Vec3{ 1.f, -1.f, 3.f }, Quat::FromEulerDeg(10.f, 0.f, 0.f), Vec3{ -1.f, 1.f, 0.7f }),  // mirrored
    };
    std::vector<SceneBvhInstanceDesc> descs;
    std::vector<BruteInstance> brute;
    std::vector<uint64_t> keys;
    for (uint64_t k = 0u; k < 5u; ++k)
    {
        const bool useGrid = (k % 2u) == 0u;
        descs.push_back(MakeInstance(k + 1u, useGrid ? gridBlas : cubeBlas, transforms[k]));
        brute.push_back({ useGrid ? &grid : &cube, transforms[k] });
        keys.push_back(k + 1u);
    }
    SceneBvh scene;
    scene.Update(descs);
    CHECK_EQ(ctx, scene.InstanceCount(), 5u);
    CHECK(ctx, scene.LastUpdate().kind == SceneBvhUpdateKind::Rebuild);
    // Two BLAS shared by five instances: packed once each.
    CHECK_EQ(ctx, scene.BlasNodes().size(), gridBlas->Nodes().size() + cubeBlas->Nodes().size());
    CHECK_EQ(ctx, scene.InstancedTriangleCount(), static_cast<uint64_t>(3u * grid.indices.size() / 3u + 2u * 12u));

    size_t hits = 0u;
    const std::vector<TestRay> rays = MakeRays(SceneBounds(scene), 2000u, 31u);
    const size_t mismatches = CompareSceneWithBruteForce(scene, brute, keys, rays, &hits);
    CHECK(ctx, mismatches * 1000u <= rays.size());   // at most 0.1 % (grazing edges)
    CHECK(ctx, hits > 300u);
    if (mismatches)
        std::fprintf(stdout, "  # test_gi.cpp: %zu of %zu instanced rays differ from brute force\n", mismatches, rays.size());

    // Hit details: straight down onto the translated cube's top face.
    SceneRayHit hit{};
    CHECK(ctx, scene.TraceClosest(Vec3{ 3.1f, 5.f, -0.9f }, Vec3{ 0.f, -1.f, 0.f }, 0.f, 100.f, hit));
    CHECK_EQ(ctx, hit.key, 2ull);
    CHECK_EQ(ctx, hit.entity, 2u);
    CHECK(ctx, std::fabs(hit.distance - 4.f) < 1e-4f);   // top face at y = 1
    CHECK(ctx, std::fabs(std::fabs(hit.normal.y) - 1.f) < 1e-4f);
    CHECK(ctx, std::fabs(hit.position.y - 1.f) < 1e-4f);
    CHECK_EQ(ctx, scene.Instances()[hit.instance].materialId, 20u);
}

void TestSceneBvhMoveRefitAndRebuild(test::TestContext& ctx)
{
    const TestMesh cube = MakeCube(0.4f);
    const auto blas = BuildShared(cube);
    // 20 x 20 cubes on a grid.
    std::vector<SceneBvhInstanceDesc> descs;
    std::vector<BruteInstance> brute;
    std::vector<uint64_t> keys;
    for (uint32_t i = 0u; i < 400u; ++i)
    {
        const Mat4 m = Mat4::Translation(Vec3{ static_cast<float>(i % 20u) * 2.f, 0.f, static_cast<float>(i / 20u) * 2.f });
        descs.push_back(MakeInstance(i + 1u, blas, m));
        brute.push_back({ &cube, m });
        keys.push_back(i + 1u);
    }
    SceneBvh scene;
    scene.Update(descs);
    const uint64_t v0 = scene.Version();

    // Move one cube up by 10: refit, and the hit moves with it.
    descs[5].objectToWorld = Mat4::Translation(Vec3{ 10.f, 10.f, 0.f });
    descs[5].transformVersion = 2u;
    brute[5].objectToWorld = descs[5].objectToWorld;
    scene.Update(descs);
    CHECK(ctx, scene.LastUpdate().kind == SceneBvhUpdateKind::Refit);
    CHECK(ctx, scene.Version() > v0);
    SceneRayHit hit{};
    CHECK(ctx, scene.TraceClosest(Vec3{ 10.f, 20.f, 0.f }, Vec3{ 0.f, -1.f, 0.f }, 0.f, 100.f, hit));
    CHECK_EQ(ctx, hit.key, 6ull);
    CHECK(ctx, std::fabs(hit.distance - 9.6f) < 1e-4f);
    const std::vector<TestRay> rays = MakeRays(SceneBounds(scene), 1500u, 41u);
    CHECK(ctx, CompareSceneWithBruteForce(scene, brute, keys, rays) * 1000u <= rays.size());

    // A version change without a new matrix is not a move.
    descs[7].transformVersion = 5u;
    const uint64_t v1 = scene.Version();
    scene.Update(descs);
    CHECK(ctx, scene.LastUpdate().kind == SceneBvhUpdateKind::None);
    CHECK_EQ(ctx, scene.Version(), v1);

    // Move a quarter of them: rebuild, same answers as brute force.
    for (uint32_t i = 0u; i < 400u; i += 4u)
    {
        descs[i].objectToWorld = Mat4::Translation(Vec3{ static_cast<float>(i % 20u) * 2.f, 3.f, static_cast<float>(i / 20u) * 2.f + 1.f });
        descs[i].transformVersion = 9u;
        brute[i].objectToWorld = descs[i].objectToWorld;
    }
    scene.Update(descs);
    CHECK(ctx, scene.LastUpdate().kind == SceneBvhUpdateKind::Rebuild);
    CHECK_EQ(ctx, scene.LastUpdate().moved, 100u);
    const std::vector<TestRay> rays2 = MakeRays(SceneBounds(scene), 1500u, 43u);
    CHECK(ctx, CompareSceneWithBruteForce(scene, brute, keys, rays2) * 1000u <= rays2.size());

    // Refit after the rebuild gives the same answers as a fresh rebuild.
    descs[1].objectToWorld = Mat4::Translation(Vec3{ 2.5f, 1.f, 0.5f });
    descs[1].transformVersion = 10u;
    brute[1].objectToWorld = descs[1].objectToWorld;
    scene.Update(descs);
    CHECK(ctx, scene.LastUpdate().kind == SceneBvhUpdateKind::Refit);
    SceneBvh fresh;
    fresh.Update(descs);
    const std::vector<TestRay> rays3 = MakeRays(SceneBounds(fresh), 1500u, 47u);
    size_t differ = 0u;
    for (const TestRay& ray : rays3)
    {
        SceneRayHit a{}, b{};
        const bool ha = scene.TraceClosest(ray.origin, ray.dir, 0.f, 1e30f, a);
        const bool hb = fresh.TraceClosest(ray.origin, ray.dir, 0.f, 1e30f, b);
        if (ha != hb || (ha && (a.key != b.key || a.distance != b.distance)))
            ++differ;
    }
    CHECK_EQ(ctx, differ, static_cast<size_t>(0u));
    CHECK(ctx, CompareSceneWithBruteForce(scene, brute, keys, rays3) * 1000u <= rays3.size());
}

void TestSceneBvhUnchangedScene(test::TestContext& ctx)
{
    const auto blas = BuildShared(MakeCube(0.5f));
    std::vector<SceneBvhInstanceDesc> descs;
    for (uint32_t i = 0u; i < 10u; ++i)
        descs.push_back(MakeInstance(i + 1u, blas, Mat4::Translation(Vec3{ static_cast<float>(i) * 2.f, 0.f, 0.f })));
    SceneBvh scene;
    scene.Update(descs);
    CHECK_EQ(ctx, scene.Changes().size(), static_cast<size_t>(10u));
    // Until kRestingFrames the instances are "moving"; then all become resting once.
    for (uint32_t frame = 1u; frame < SceneBvh::kRestingFrames; ++frame)
    {
        const uint64_t before = scene.Version();
        scene.Update(descs);
        CHECK(ctx, scene.Changes().empty());
        CHECK_EQ(ctx, scene.Version(), before);
        CHECK_EQ(ctx, scene.Instances()[0].flags & collision::kSceneBvhInstanceResting, 0u);
    }
    scene.Update(descs);
    CHECK_EQ(ctx, scene.LastUpdate().restingChanged, 10u);
    CHECK_EQ(ctx, scene.GeometryVersion(), 1ull);   // the resting bit is no geometry change
    CHECK(ctx, scene.LastUpdate().kind == SceneBvhUpdateKind::None);
    CHECK_EQ(ctx, scene.Instances()[0].flags & collision::kSceneBvhInstanceResting, collision::kSceneBvhInstanceResting);
    const uint64_t settled = scene.Version();
    const uint64_t blasVersion = scene.BlasPoolVersion();
    for (int frame = 0; frame < 50; ++frame)
    {
        scene.Update(descs);
        CHECK(ctx, scene.Changes().empty());
    }
    CHECK_EQ(ctx, scene.Version(), settled);
    CHECK_EQ(ctx, scene.BlasPoolVersion(), blasVersion);
    CHECK(ctx, scene.LastUpdate().kind == SceneBvhUpdateKind::None);

    // A resting instance that moves loses the bit.
    descs[3].objectToWorld = Mat4::Translation(Vec3{ 6.f, 1.f, 0.f });
    descs[3].transformVersion = 2u;
    scene.Update(descs);
    CHECK_EQ(ctx, scene.Instances()[scene.FindInstance(4u)].flags & collision::kSceneBvhInstanceResting, 0u);
    CHECK_EQ(ctx, scene.Instances()[scene.FindInstance(5u)].flags & collision::kSceneBvhInstanceResting,
             collision::kSceneBvhInstanceResting);
}

void TestSceneBvhChangeList(test::TestContext& ctx)
{
    const auto blas = BuildShared(MakeCube(0.5f));
    std::vector<SceneBvhInstanceDesc> descs;
    for (uint32_t i = 0u; i < 4u; ++i)
        descs.push_back(MakeInstance(i + 1u, blas, Mat4::Translation(Vec3{ static_cast<float>(i) * 3.f, 0.f, 0.f })));
    SceneBvh scene;
    scene.Update(descs);
    CHECK_EQ(ctx, scene.Changes().size(), static_cast<size_t>(4u));
    for (const auto& c : scene.Changes())
        CHECK(ctx, c.kind == SceneBvhChangeKind::Added && c.oldBounds.min.x > c.oldBounds.max.x);

    // Move instance 2, remove 3, add 9.
    descs[1].objectToWorld = Mat4::Translation(Vec3{ 3.f, 4.f, 0.f });
    descs[1].transformVersion = 2u;
    descs.erase(descs.begin() + 2);
    descs.push_back(MakeInstance(9u, blas, Mat4::Translation(Vec3{ 0.f, 0.f, 8.f })));
    scene.Update(descs);
    CHECK(ctx, scene.LastUpdate().kind == SceneBvhUpdateKind::Rebuild);
    CHECK_EQ(ctx, scene.Changes().size(), static_cast<size_t>(3u));
    bool moved = false, removed = false, added = false;
    for (const auto& c : scene.Changes())
    {
        if (c.kind == SceneBvhChangeKind::Moved)
        {
            moved = c.key == 2u && std::fabs(c.oldBounds.min.y + 0.5f) < 1e-5f && std::fabs(c.newBounds.min.y - 3.5f) < 1e-5f &&
                    std::fabs(c.newBounds.min.x - 2.5f) < 1e-5f;
        }
        if (c.kind == SceneBvhChangeKind::Removed)
            removed = c.key == 3u && std::fabs(c.oldBounds.min.x - 5.5f) < 1e-5f && c.newBounds.min.x > c.newBounds.max.x;
        if (c.kind == SceneBvhChangeKind::Added)
            added = c.key == 9u && std::fabs(c.newBounds.max.z - 8.5f) < 1e-5f;
    }
    CHECK(ctx, moved);
    CHECK(ctx, removed);
    CHECK(ctx, added);
    CHECK_EQ(ctx, scene.InstanceCount(), 4u);
    CHECK_EQ(ctx, scene.FindInstance(3u), UINT32_MAX);
    SceneRayHit hit{};
    CHECK(ctx, !scene.TraceClosest(Vec3{ 6.f, 5.f, 0.f }, Vec3{ 0.f, -1.f, 0.f }, 0.f, 100.f, hit));
    CHECK(ctx, scene.TraceClosest(Vec3{ 0.f, 5.f, 8.f }, Vec3{ 0.f, -1.f, 0.f }, 0.f, 100.f, hit));
    CHECK_EQ(ctx, hit.key, 9ull);

    // Next frame without changes: empty list.
    scene.Update(descs);
    CHECK(ctx, scene.Changes().empty());

    // Swapping the BLAS of an instance counts as a change at that place.
    descs[0].blas = BuildShared(MakeCube(1.f));
    scene.Update(descs);
    CHECK_EQ(ctx, scene.Changes().size(), static_cast<size_t>(1u));
    CHECK(ctx, scene.Changes()[0].kind == SceneBvhChangeKind::Moved && scene.Changes()[0].key == 1u);
    CHECK(ctx, std::fabs(scene.Changes()[0].newBounds.max.x - 1.f) < 1e-5f);
}

void TestSceneBvhEmpty(test::TestContext& ctx)
{
    SceneBvh scene;
    scene.Update({});
    SceneRayHit hit{};
    CHECK(ctx, !scene.TraceClosest(Vec3{}, Vec3{ 0.f, 0.f, 1.f }, 0.f, 100.f, hit));
    CHECK(ctx, !scene.TraceAny(Vec3{}, Vec3{ 0.f, 0.f, 1.f }, 0.f, 100.f));
    CHECK_EQ(ctx, scene.Version(), 0ull);
    CHECK(ctx, scene.TlasNodes().empty());

    // Instances without a usable BLAS are left out.
    std::vector<SceneBvhInstanceDesc> descs;
    descs.push_back(MakeInstance(1u, std::make_shared<MeshBvh>(), Mat4::Identity()));
    descs.push_back(MakeInstance(2u, nullptr, Mat4::Identity()));
    scene.Update(descs);
    CHECK_EQ(ctx, scene.InstanceCount(), 0u);
    CHECK(ctx, !scene.TraceAny(Vec3{}, Vec3{ 0.f, 0.f, 1.f }, 0.f, 100.f));

    // One instance, then none: back to empty.
    descs.push_back(MakeInstance(3u, BuildShared(MakeCube(1.f)), Mat4::Identity()));
    scene.Update(descs);
    CHECK_EQ(ctx, scene.InstanceCount(), 1u);
    CHECK(ctx, scene.TraceAny(Vec3{ 0.f, 0.f, -5.f }, Vec3{ 0.f, 0.f, 1.f }, 0.f, 100.f));
    scene.Update({});
    CHECK_EQ(ctx, scene.InstanceCount(), 0u);
    CHECK(ctx, scene.TlasNodes().empty());
    CHECK(ctx, !scene.TraceAny(Vec3{ 0.f, 0.f, -5.f }, Vec3{ 0.f, 0.f, 1.f }, 0.f, 100.f));
    // The unused BLAS data was compacted away.
    CHECK(ctx, scene.BlasNodes().empty());
}

void TestMeshSceneBvhFromWorld(test::TestContext& ctx)
{
    ecs::ComponentMetaRegistry components;
    RegisterCoreComponents(components);
    RegisterMeshRendererComponents(components);
    ecs::World world(components);
    Scene scene(world);
    assets::AssetRegistry registry;

    const auto makeMesh = [&](const char* path, const TestMesh& m, bool skinned)
    {
        auto asset = std::make_unique<assets::MeshAsset>();
        asset->state = assets::AssetState::Loaded;
        assets::SubMeshData sub;
        sub.positions = m.positions;
        sub.indices = m.indices;
        if (skinned)
        {
            sub.boneWeights.assign(m.positions.size() / 3u * 4u, 0.25f);
            sub.boneIndices.assign(m.positions.size() / 3u * 4u, 0u);
        }
        asset->submeshes.push_back(sub);
        asset->submeshes.push_back(sub);   // second submesh: second instance
        return registry.GetOrAddMesh(path, std::move(asset));
    };
    const MeshHandle cubeMesh = makeMesh("gi_cube.mesh", MakeCube(0.5f), false);
    const MeshHandle skinnedMesh = makeMesh("gi_skinned.mesh", MakeCube(0.5f), true);

    std::vector<EntityID> cubes;
    for (int i = 0; i < 3; ++i)
    {
        const EntityID e = scene.CreateEntity("Cube");
        world.Add<MeshComponent>(e, cubeMesh);
        scene.SetLocalPosition(e, Vec3{ static_cast<float>(i) * 3.f, 0.f, 0.f });
        cubes.push_back(e);
    }
    const EntityID skinned = scene.CreateEntity("Skinned");
    world.Add<MeshComponent>(skinned, skinnedMesh);
    const EntityID hidden = scene.CreateEntity("Hidden");
    world.Add<MeshComponent>(hidden, cubeMesh).visible = false;
    // Editor gizmo (light icon, transform arrows): not scene geometry.
    const EntityID gizmo = scene.CreateEntity("Gizmo");
    world.Add<MeshComponent>(gizmo, cubeMesh).layerMask = renderer::LAYER_EDITOR_GIZMO;
    scene.PropagateTransforms();

    jobs::JobSystem jobSystem;
    jobSystem.Initialize(2u);
    mesh_renderer::MeshSceneBvh meshBvh(&jobSystem);
    meshBvh.Update(world, registry);
    meshBvh.WaitForBlasBuilds();
    meshBvh.Update(world, registry);
    // 3 visible cubes x 2 submeshes; one BLAS per (mesh, submesh); skinned and hidden left out.
    CHECK_EQ(ctx, meshBvh.Scene().InstanceCount(), 6u);
    CHECK_EQ(ctx, meshBvh.CachedBlasCount(), static_cast<size_t>(2u));
    CHECK_EQ(ctx, meshBvh.PendingBlasBuilds(), 0u);
    CHECK_EQ(ctx, meshBvh.Scene().FindInstance(mesh_renderer::MeshSceneBvh::InstanceKey(skinned, 0u)), UINT32_MAX);
    CHECK_EQ(ctx, meshBvh.Scene().FindInstance(mesh_renderer::MeshSceneBvh::InstanceKey(hidden, 0u)), UINT32_MAX);
    CHECK_EQ(ctx, meshBvh.Scene().FindInstance(mesh_renderer::MeshSceneBvh::InstanceKey(gizmo, 0u)), UINT32_MAX);

    SceneRayHit hit{};
    CHECK(ctx, meshBvh.Scene().TraceClosest(Vec3{ 6.f, 5.f, 0.f }, Vec3{ 0.f, -1.f, 0.f }, 0.f, 100.f, hit));
    CHECK_EQ(ctx, hit.entity, cubes[2].value);

    // Unchanged world: no new version after the resting phase.
    for (uint32_t i = 0u; i <= SceneBvh::kRestingFrames; ++i)
    {
        scene.PropagateTransforms();
        meshBvh.Update(world, registry);
    }
    const uint64_t settled = meshBvh.Scene().Version();
    // A gizmo rescaled every frame (as the editor does) must not touch the BVH.
    scene.SetLocalScale(gizmo, Vec3{ 3.f, 3.f, 3.f });
    scene.PropagateTransforms();
    meshBvh.Update(world, registry);
    CHECK_EQ(ctx, meshBvh.Scene().Version(), settled);
    CHECK(ctx, meshBvh.Scene().Changes().empty());

    // Move one cube through the transform: both its submesh instances move.
    scene.SetLocalPosition(cubes[0], Vec3{ 0.f, 10.f, 0.f });
    scene.PropagateTransforms();
    meshBvh.Update(world, registry);
    CHECK_EQ(ctx, meshBvh.Scene().Changes().size(), static_cast<size_t>(2u));
    CHECK(ctx, meshBvh.Scene().Version() > settled);
    CHECK(ctx, meshBvh.Scene().TraceClosest(Vec3{ 0.f, 20.f, 0.f }, Vec3{ 0.f, -1.f, 0.f }, 0.f, 100.f, hit));
    CHECK(ctx, std::fabs(hit.distance - 9.5f) < 1e-4f);

    // Destroy one: its instances are removed.
    world.DestroyEntity(cubes[1]);
    scene.PropagateTransforms();
    meshBvh.Update(world, registry);
    CHECK_EQ(ctx, meshBvh.Scene().InstanceCount(), 4u);
    jobSystem.Shutdown();

    // Without a job system the BLAS are built right away.
    mesh_renderer::MeshSceneBvh direct;
    direct.Update(world, registry);
    CHECK_EQ(ctx, direct.Scene().InstanceCount(), 4u);
}

// Timings for the package notes (meaningful in Release builds).
void TestSceneBvhTimings(test::TestContext& ctx)
{
    const auto blas = BuildShared(MakeSphere(8u, 16u, 0.5f));
    for (const uint32_t count : { 1000u, 10000u })
    {
        std::mt19937 rng(count);
        std::uniform_real_distribution<float> pos(-100.f, 100.f);
        std::uniform_real_distribution<float> angle(0.f, 360.f);
        std::vector<SceneBvhInstanceDesc> descs;
        descs.reserve(count);
        for (uint32_t i = 0u; i < count; ++i)
            descs.push_back(MakeInstance(i + 1u, blas,
                Mat4::TRS(Vec3{ pos(rng), pos(rng) * 0.1f, pos(rng) }, Quat::FromEulerDeg(0.f, angle(rng), 0.f), Vec3{ 1.f, 1.f, 1.f })));

        SceneBvh scene;
        scene.Update(descs);
        const double firstMs = scene.LastUpdate().tlasMs;
        double rebuildMs = 0.0;
        constexpr int kRuns = 5;
        for (int run = 0; run < kRuns; ++run)
        {
            // Move a quarter: forced rebuild.
            for (uint32_t i = 0u; i < count; i += 4u)
            {
                descs[i].objectToWorld.m[3][1] += 0.5f;
                descs[i].transformVersion += 1u;
            }
            scene.Update(descs);
            CHECK(ctx, scene.LastUpdate().kind == SceneBvhUpdateKind::Rebuild);
            rebuildMs += scene.LastUpdate().tlasMs;
        }
        double refitMs = 0.0, refitUpdateMs = 0.0;
        for (int run = 0; run < kRuns; ++run)
        {
            for (uint32_t i = static_cast<uint32_t>(run); i < count; i += 100u)   // 1 %
            {
                descs[i].objectToWorld.m[3][0] += 0.25f;
                descs[i].transformVersion += 1u;
            }
            scene.Update(descs);
            CHECK(ctx, scene.LastUpdate().kind == SceneBvhUpdateKind::Refit);
            refitMs += scene.LastUpdate().tlasMs;
            refitUpdateMs += scene.LastUpdate().ms;
        }
        double idleMs = 0.0;
        for (int run = 0; run < kRuns; ++run)
        {
            scene.Update(descs);
            idleMs += scene.LastUpdate().ms;
        }
        std::fprintf(stdout, "  # test_gi.cpp: TLAS %u instances: first build %.3f ms, rebuild %.3f ms, refit (1 %% moved) %.3f ms "
                             "(whole update %.3f ms), unchanged update %.3f ms, %zu nodes\n",
                     count, firstMs, rebuildMs / kRuns, refitMs / kRuns, refitUpdateMs / kRuns, idleMs / kRuns,
                     scene.TlasNodes().size());
        CHECK_EQ(ctx, scene.TlasNodes().size(), static_cast<size_t>(count) * 2u - 1u);
    }
}

// =============================================================================
// Package 2: trace kernel on the GPU
// =============================================================================

void TestSurfaceCacheShadersCompile(test::TestContext& ctx)
{
    namespace sc = engine::renderer::addons::surface_cache;
    const bool haveDxc = !FindShaderTool(ShaderTool::Dxc).path.empty();
    const struct { const char* name; const char* source; assets::ShaderStage stage; } shaders[] = {
        { "surface_cache_trace.cs", sc::kTraceCsHlsl, assets::ShaderStage::Compute },
        { "surface_cache_overlay.ps", sc::kOverlayPsHlsl, assets::ShaderStage::Fragment },
        { "surface_cache_fullscreen.vs", sc::kFullscreenVsHlsl, assets::ShaderStage::Vertex },
    };
    for (const auto& entry : shaders)
    {
        assets::ShaderAsset shader{};
        shader.debugName = entry.name;
        shader.stage = entry.stage;
        shader.sourceLanguage = assets::ShaderSourceLanguage::HLSL;
        shader.entryPoint = "main";
        shader.sourceCode = entry.source;
        for (const assets::ShaderTargetProfile target : { assets::ShaderTargetProfile::DirectX11_SM5,
                                                          assets::ShaderTargetProfile::Vulkan_SPIRV })
        {
            if (target == assets::ShaderTargetProfile::Vulkan_SPIRV && !haveDxc)
            {
                std::fprintf(stdout, "  # test_gi.cpp: dxc.exe not found, Vulkan compile of %s skipped\n", entry.name);
                continue;
            }
            assets::CompiledShaderArtifact artifact{};
            std::string error;
            const bool ok = ShaderCompiler::CompileForTarget(shader, target, artifact, &error);
            if (!ok)
                std::fprintf(stderr, "test_gi.cpp: %s (%s): %s\n", entry.name, ShaderCompiler::ToString(target), error.c_str());
            CHECK(ctx, ok);
            CHECK(ctx, !artifact.bytecode.empty());
        }
    }
}

// The trace kernel of shaders/surface_cache_trace.hlsl on WARP, against
// SceneBvh::TraceClosest/TraceAny (package 1, the CPU reference).
void TestSurfaceCacheTraceOnWarp(test::TestContext& ctx)
{
#if defined(_WIN32) && defined(KROM_DX11_BACKEND)
    // A scene with shared BLAS, rotation, non-uniform and mirrored scale.
    const TestMesh grid = MakeGrid(24u, 4.f);
    const TestMesh cube = MakeCube(0.5f);
    const TestMesh sphere = MakeSphere(12u, 24u, 0.7f);
    const auto gridBlas = BuildShared(grid);
    const auto cubeBlas = BuildShared(cube);
    const auto sphereBlas = BuildShared(sphere);
    std::vector<SceneBvhInstanceDesc> descs;
    descs.push_back(MakeInstance(1u, gridBlas, Mat4::Identity()));
    for (uint32_t i = 0u; i < 24u; ++i)
    {
        const float x = static_cast<float>(i % 6u) * 1.4f - 3.5f;
        const float z = static_cast<float>(i / 6u) * 1.4f - 2.f;
        const Vec3 scale = (i % 5u == 0u) ? Vec3{ -1.f, 0.6f, 1.3f } : Vec3{ 1.f, 1.f + 0.1f * static_cast<float>(i % 3u), 0.8f };
        descs.push_back(MakeInstance(10u + i, (i % 2u) ? cubeBlas : sphereBlas,
            Mat4::TRS(Vec3{ x, 0.8f + 0.2f * static_cast<float>(i % 4u), z },
                      Quat::FromEulerDeg(10.f * static_cast<float>(i), 25.f * static_cast<float>(i), 5.f * static_cast<float>(i)), scale)));
    }
    SceneBvh scene;
    scene.Update(descs);

    // Rays into the scene from outside (every third one short), and upwards from
    // between the objects.
    struct GpuRay { float o[3]; float tMax; float d[3]; uint32_t pad; };
    std::vector<GpuRay> rays;
    std::mt19937 rng(77u);
    std::uniform_real_distribution<float> u(-1.f, 1.f);
    const std::vector<TestRay> random = MakeRays(SceneBounds(scene), 480u, 91u);
    for (size_t i = 0u; i < random.size(); ++i)
    {
        const float tMax = (i % 3u == 0u) ? 3.f : 1e30f;
        rays.push_back({ { random[i].origin.x, random[i].origin.y, random[i].origin.z }, tMax,
                         { random[i].dir.x, random[i].dir.y, random[i].dir.z }, 0u });
    }
    for (int i = 0; i < 32; ++i)
        rays.push_back({ { u(rng) * 3.f, 1.f, u(rng) * 2.f }, 1e30f, { 0.f, 1.f, 0.f }, 0u });
    const uint32_t rayCount = static_cast<uint32_t>(rays.size());

    static const char* const kTestCs = R"(#include "surface_cache_trace.hlsl"
struct TestRay { float3 o; float tMax; float3 d; uint pad; };
StructuredBuffer<TestRay> gRays : register(t5);
cbuffer TestParams : register(b0) { uint gRayCount; uint gTlasNodeCount; uint2 gPad; };
RWStructuredBuffer<uint4> gOut : register(u0);
[numthreads(64, 1, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= gRayCount)
        return;
    const TestRay r = gRays[id.x];
    TraceStats s = TraceStatsZero();
    TraceHit h;
    const bool closest = TraceClosest(r.o, r.d, 0.0f, r.tMax, gTlasNodeCount, h, s);
    const bool any = TraceAny(r.o, r.d, 0.0f, r.tMax, gTlasNodeCount, s);
    gOut[id.x] = uint4(closest ? asuint(h.t) : 0xFFFFFFFFu,
                       closest ? gInstances[h.instance].entity : 0xFFFFFFFFu,
                       any ? 1u : 0u, s.overflows);
}
)";
    assets::ShaderAsset cs{};
    cs.debugName = "GiTraceWarpCS";
    cs.stage = assets::ShaderStage::Compute;
    cs.sourceLanguage = assets::ShaderSourceLanguage::HLSL;
    cs.entryPoint = "main";
    cs.sourceCode = kTestCs;
    assets::CompiledShaderArtifact compiled{};
    std::string error;
    const bool compiledOk = ShaderCompiler::CompileForTarget(cs, assets::ShaderTargetProfile::DirectX11_SM5, compiled, &error);
    if (!compiledOk)
        std::fprintf(stderr, "test_gi.cpp: trace test shader: %s\n", error.c_str());
    CHECK(ctx, compiledOk);
    if (!compiledOk)
        return;

    using Microsoft::WRL::ComPtr;
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> command;
    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0,
                                   D3D11_SDK_VERSION, &device, nullptr, &command);
    CHECK(ctx, SUCCEEDED(hr));
    if (FAILED(hr))
        return;
    ComPtr<ID3D11ComputeShader> shader;
    CHECK(ctx, SUCCEEDED(device->CreateComputeShader(compiled.bytecode.data(), compiled.bytecode.size(), nullptr, &shader)));

    const auto makeSrv = [&](const void* data, uint32_t count, uint32_t stride, ComPtr<ID3D11Buffer>& buffer,
                             ComPtr<ID3D11ShaderResourceView>& srv)
    {
        count = std::max(count, 1u);
        std::vector<uint8_t> zero(static_cast<size_t>(count) * stride, 0u);
        D3D11_BUFFER_DESC desc{};
        desc.ByteWidth = count * stride;
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        desc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
        desc.StructureByteStride = stride;
        D3D11_SUBRESOURCE_DATA init{ data ? data : zero.data(), 0u, 0u };
        bool ok = SUCCEEDED(device->CreateBuffer(&desc, &init, &buffer));
        D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
        srvDesc.Format = DXGI_FORMAT_UNKNOWN;
        srvDesc.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
        srvDesc.Buffer.NumElements = count;
        ok = ok && SUCCEEDED(device->CreateShaderResourceView(buffer.Get(), &srvDesc, &srv));
        return ok;
    };
    ComPtr<ID3D11Buffer> b[5];
    ComPtr<ID3D11ShaderResourceView> srv[5];
    bool ok = makeSrv(scene.TlasNodes().data(), static_cast<uint32_t>(scene.TlasNodes().size()), sizeof(collision::BvhNode), b[0], srv[0]) &&
              makeSrv(scene.BlasNodes().data(), static_cast<uint32_t>(scene.BlasNodes().size()), sizeof(collision::BvhNode), b[1], srv[1]) &&
              makeSrv(scene.BlasTriangles().data(), static_cast<uint32_t>(scene.BlasTriangles().size()), sizeof(collision::BvhTriangle), b[2], srv[2]) &&
              makeSrv(scene.Instances().data(), static_cast<uint32_t>(scene.Instances().size()), sizeof(collision::GpuBvhInstance), b[3], srv[3]) &&
              makeSrv(rays.data(), rayCount, sizeof(GpuRay), b[4], srv[4]);
    CHECK(ctx, ok);
    if (!ok)
        return;

    D3D11_BUFFER_DESC outDesc{};
    outDesc.ByteWidth = rayCount * 16u;
    outDesc.Usage = D3D11_USAGE_DEFAULT;
    outDesc.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
    outDesc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
    outDesc.StructureByteStride = 16u;
    D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc{};
    uavDesc.Format = DXGI_FORMAT_UNKNOWN;
    uavDesc.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
    uavDesc.Buffer.NumElements = rayCount;
    D3D11_BUFFER_DESC stagingDesc{};
    stagingDesc.ByteWidth = rayCount * 16u;
    stagingDesc.Usage = D3D11_USAGE_STAGING;
    stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    const uint32_t paramsData[4] = { rayCount, static_cast<uint32_t>(scene.TlasNodes().size()), 0u, 0u };
    D3D11_BUFFER_DESC paramsDesc{};
    paramsDesc.ByteWidth = sizeof(paramsData);
    paramsDesc.Usage = D3D11_USAGE_DEFAULT;
    paramsDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    D3D11_SUBRESOURCE_DATA paramsInit{ paramsData, 0u, 0u };
    ComPtr<ID3D11Buffer> out, staging, params;
    ComPtr<ID3D11UnorderedAccessView> uav;
    ok = SUCCEEDED(device->CreateBuffer(&outDesc, nullptr, &out)) &&
         SUCCEEDED(device->CreateUnorderedAccessView(out.Get(), &uavDesc, &uav)) &&
         SUCCEEDED(device->CreateBuffer(&stagingDesc, nullptr, &staging)) &&
         SUCCEEDED(device->CreateBuffer(&paramsDesc, &paramsInit, &params));
    CHECK(ctx, ok);
    if (!ok)
        return;

    ID3D11ShaderResourceView* views[6] = { srv[0].Get(), srv[1].Get(), srv[2].Get(), srv[3].Get(), nullptr, srv[4].Get() };
    ID3D11UnorderedAccessView* uavs[1] = { uav.Get() };
    ID3D11Buffer* cbs[1] = { params.Get() };
    command->CSSetShader(shader.Get(), nullptr, 0u);
    command->CSSetShaderResources(0u, 6u, views);
    command->CSSetUnorderedAccessViews(0u, 1u, uavs, nullptr);
    command->CSSetConstantBuffers(0u, 1u, cbs);
    command->Dispatch((rayCount + 63u) / 64u, 1u, 1u);
    command->CopyResource(staging.Get(), out.Get());
    D3D11_MAPPED_SUBRESOURCE mapped{};
    CHECK(ctx, SUCCEEDED(command->Map(staging.Get(), 0u, D3D11_MAP_READ, 0u, &mapped)));
    if (!mapped.pData)
        return;
    const uint32_t* result = static_cast<const uint32_t*>(mapped.pData);

    size_t hits = 0u, mismatches = 0u, anyMismatches = 0u, overflows = 0u;
    float maxDiff = 0.f;
    for (uint32_t i = 0u; i < rayCount; ++i)
    {
        const GpuRay& r = rays[i];
        const Vec3 o{ r.o[0], r.o[1], r.o[2] };
        const Vec3 d{ r.d[0], r.d[1], r.d[2] };
        SceneRayHit cpu{};
        const bool cpuHit = scene.TraceClosest(o, d, 0.f, r.tMax, cpu);
        const bool cpuAny = scene.TraceAny(o, d, 0.f, r.tMax);
        const uint32_t* g = result + i * 4u;
        const bool gpuHit = g[0] != 0xFFFFFFFFu;
        overflows += g[3];
        hits += cpuHit ? 1u : 0u;
        if (cpuAny != (g[2] != 0u))
            ++anyMismatches;
        if (cpuHit != gpuHit)
        {
            ++mismatches;
            continue;
        }
        if (!cpuHit)
            continue;
        float gpuT = 0.f;
        std::memcpy(&gpuT, &g[0], sizeof(gpuT));
        const float diff = std::fabs(gpuT - cpu.distance);
        maxDiff = std::max(maxDiff, diff);
        if (diff > 1e-3f * std::max(1.f, cpu.distance) || (g[1] != cpu.entity && diff > 1e-4f))
            ++mismatches;
    }
    command->Unmap(staging.Get(), 0u);
    std::fprintf(stdout, "  # test_gi.cpp: WARP trace: %u rays, %zu hits, %zu closest mismatches, %zu any mismatches, max |dt| %.6f\n",
                 rayCount, hits, mismatches, anyMismatches, maxDiff);
    CHECK(ctx, hits > rayCount / 4u);
    CHECK(ctx, mismatches * 200u <= rayCount);      // at most 0.5 % (grazing edges, float order)
    CHECK(ctx, anyMismatches * 200u <= rayCount);
    CHECK_EQ(ctx, overflows, static_cast<size_t>(0u));
#else
    (void)ctx;
    std::fprintf(stdout, "  # test_gi.cpp: no DX11 backend, WARP trace skipped\n");
#endif
}

} // namespace

int RunGiTests()
{
    engine::Debug::MinLevel = engine::LogLevel::Fatal;
    test::TestSuite suite("GI");
    suite.Add("Hash insert with InterlockedCompareExchange on WARP (DX11)", TestHashInsertCompareExchangeDx11)
         .Add("Hash insert compiles to OpAtomicCompareExchange (Vulkan)", TestHashInsertCompareExchangeVulkan)
         .Add("BVH: single triangle", TestBvhSingleTriangle)
         .Add("BVH: cube", TestBvhCube)
         .Add("BVH: grid against brute force", TestBvhGridVsBruteForce)
         .Add("BVH: dragon against brute force, rays per second", TestBvhDragonVsBruteForce)
         .Add("BVH: degenerate triangles and empty meshes", TestBvhDegenerateAndEmpty)
         .Add("Scene BVH: instances with rotation and non-uniform scale", TestSceneBvhInstancesVsBruteForce)
         .Add("Scene BVH: move, refit and rebuild", TestSceneBvhMoveRefitAndRebuild)
         .Add("Scene BVH: unchanged scene keeps its version", TestSceneBvhUnchangedScene)
         .Add("Scene BVH: change list for move, add, remove", TestSceneBvhChangeList)
         .Add("Scene BVH: empty scene", TestSceneBvhEmpty)
         .Add("Scene BVH from the ECS (MeshSceneBvh)", TestMeshSceneBvhFromWorld)
         .Add("Scene BVH timings (1000 / 10000 instances)", TestSceneBvhTimings)
         .Add("Surface cache shaders compile for DX11 and Vulkan", TestSurfaceCacheShadersCompile)
         .Add("Surface cache trace kernel on WARP against the CPU BVH", TestSurfaceCacheTraceOnWarp);
    return suite.Run();
}
