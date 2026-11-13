// =============================================================================
// KROM Engine - tests/test_probes.cpp
// Tests for the reflection probe add-on (component, .kprobe, cube-face bake).
// =============================================================================
#include "TestFramework.hpp"

#include "addons/lighting/LightingComponents.hpp"
#include "addons/mesh_renderer/MeshRendererComponents.hpp"
#include "addons/probes/ProbeBake.hpp"
#include "addons/probes/ProbeComponents.hpp"
#include "addons/probes/ProbeData.hpp"
#include "addons/probes/ProbeSerialization.hpp"
#include "core/PathUtf8.hpp"
#include "ecs/Components.hpp"
#include "ecs/World.hpp"
#include "renderer/IBLBaker.hpp"
#include "serialization/SceneSerializer.hpp"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

using namespace engine;
using namespace engine::addons::probes;

namespace {

float HalfToFloatTest(uint16_t h)
{
    const uint32_t sign = (h >> 15u) & 1u;
    const uint32_t exp = (h >> 10u) & 0x1Fu;
    const uint32_t mant = h & 0x3FFu;
    float v = 0.f;
    if (exp == 0u)
        v = std::ldexp(static_cast<float>(mant), -24);
    else if (exp == 31u)
        v = mant ? NAN : INFINITY;
    else
        v = std::ldexp(static_cast<float>(mant | 0x400u), static_cast<int>(exp) - 25);
    return sign ? -v : v;
}

// Red channel of texel (x, y) of face f in an RGBA16F cube (mip 0).
float CubeRed(const std::vector<uint8_t>& data, uint32_t size, uint32_t f, uint32_t x, uint32_t y)
{
    const size_t index = ((static_cast<size_t>(f) * size + y) * size + x) * 4u;
    uint16_t h = 0u;
    std::memcpy(&h, data.data() + index * sizeof(uint16_t), sizeof(h));
    return HalfToFloatTest(h);
}

ecs::ComponentMetaRegistry MakeProbeRegistry()
{
    ecs::ComponentMetaRegistry reg;
    RegisterCoreComponents(reg);
    RegisterProbeComponents(reg);
    return reg;
}

void TestSerializationRoundTrip(test::TestContext& ctx)
{
    auto reg = MakeProbeRegistry();
    ecs::World world(reg);
    ReflectionProbeComponent probe{};
    probe.extents = { 2.f, 1.5f, 4.f };
    probe.intensity = 0.75f;
    probe.resolution = 256u;
    probe.probeId = 0xF123456789ABCDEFull;   // above 2^53: must survive as hex
    probe.bakedDataPath = "Baked/Room 2/f123456789abcdef.kprobe";
    const EntityID e = world.CreateEntity();
    world.Add<ReflectionProbeComponent>(e, probe);

    serialization::SceneSerializer ser(world);
    ser.RegisterDefaultHandlers();
    RegisterProbeSerializationHandlers(ser);
    const std::string json = ser.SerializeToJson("Test");

    auto reg2 = MakeProbeRegistry();
    ecs::World world2(reg2);
    serialization::SceneDeserializer deser(world2);
    deser.RegisterDefaultHandlers();
    RegisterProbeDeserializationHandlers(deser);
    CHECK(ctx, deser.DeserializeFromJson(json).success);

    int found = 0;
    world2.ForEachAlive([&](EntityID id) {
        const auto* c = world2.Get<ReflectionProbeComponent>(id);
        if (!c) return;
        ++found;
        CHECK(ctx, *c == probe);
    });
    CHECK_EQ(ctx, found, 1);
}

void TestOldScenesAndSanitizing(test::TestContext& ctx)
{
    std::string error;
    const serialization::JsonValue v = serialization::JsonParser::Parse(
        R"({"type":"ReflectionProbeComponent","extents":[0,-1,3],"resolution":100})", error);
    CHECK(ctx, error.empty());
    const ReflectionProbeComponent c = ReadProbeFields(v);
    CHECK(ctx, c.extents.x > 0.f && c.extents.y > 0.f && c.extents.z == 3.f);
    CHECK_EQ(ctx, c.resolution, 128u);
    CHECK_EQ(ctx, c.probeId, uint64_t{0});
    CHECK(ctx, c.bakedDataPath.empty());
}

void TestProbeFileRoundTrip(test::TestContext& ctx)
{
    ProbeBakedData data{};
    data.inputHash = 0x1234u;
    data.capturePosition = { 1.f, 2.f, 3.f };
    data.extents = { 4.f, 5.f, 6.f };
    data.irradianceSize = 4u;
    data.prefilterBaseSize = 8u;
    data.prefilterMipCount = 2u;
    data.irradianceData.assign(6u * 4u * 4u * 8u, 0x11u);
    data.prefilterData.assign(6u * (8u * 8u + 4u * 4u) * 8u, 0x22u);
    CHECK(ctx, data.IsValid());

    const std::filesystem::path dir = std::filesystem::temp_directory_path() / "krom_probe_test äö";
    const std::string path = PathToUtf8(dir / "sub" / "probe.kprobe");
    std::string error;
    CHECK(ctx, WriteProbeFile(path, data, &error));

    ProbeBakedData read{};
    CHECK(ctx, ReadProbeFile(path, read, &error));
    CHECK_EQ(ctx, read.inputHash, data.inputHash);
    CHECK(ctx, read.capturePosition == data.capturePosition && read.extents == data.extents);
    CHECK(ctx, read.irradianceData == data.irradianceData && read.prefilterData == data.prefilterData);
    CHECK_EQ(ctx, read.prefilterMipCount, 2u);

    // Wrong format version must be rejected.
    if (std::FILE* f = OpenFileUtf8(path, "r+b"))
    {
        std::fseek(f, 4, SEEK_SET);
        const uint32_t badVersion = 999u;
        std::fwrite(&badVersion, sizeof(badVersion), 1u, f);
        std::fclose(f);
    }
    ProbeBakedData rejected{};
    CHECK(ctx, !ReadProbeFile(path, rejected, &error));
    CHECK(ctx, !ReadProbeFile(PathToUtf8(dir / "missing.kprobe"), rejected, &error));

    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}

void TestBakeFromCubeFaces(test::TestContext& ctx)
{
    constexpr uint32_t size = 16u;
    renderer::IBLBakeParams params{};
    params.irradianceSize = 8u;
    params.irradianceSamples = 64u;
    params.prefilterBaseSize = 8u;
    params.prefilterMipCount = 4u;
    params.prefilterSamples = 32u;

    // Constant environment: prefilter stays that constant; irradiance is stored
    // as pi * radiance (same convention as the global IBL).
    std::vector<float> faces(6u * size * size * 3u, 0.5f);
    const renderer::IBLBakedData flat = renderer::IBLBaker::BakeFromCubeFaces(faces.data(), size, params);
    CHECK(ctx, flat.IsValid());
    CHECK_EQ(ctx, flat.environmentSize, size);
    CHECK(ctx, std::fabs(CubeRed(flat.irradianceData, 8u, 0u, 4u, 4u) - 0.5f * 3.14159265f) < 0.08f);
    CHECK(ctx, std::fabs(CubeRed(flat.prefilterData, 8u, 3u, 2u, 5u) - 0.5f) < 0.02f);

    // Only the +X face lit: irradiance looking along +X is bright, along -X dark.
    std::fill(faces.begin(), faces.end(), 0.f);
    for (size_t i = 0u; i < size * size * 3u; ++i)
        faces[i] = 1.f;
    const renderer::IBLBakedData lit = renderer::IBLBaker::BakeFromCubeFaces(faces.data(), size, params);
    const float plusX = CubeRed(lit.irradianceData, 8u, 0u, 4u, 4u);
    const float minusX = CubeRed(lit.irradianceData, 8u, 1u, 4u, 4u);
    CHECK_GT(ctx, plusX, 0.2f);
    CHECK(ctx, minusX < 0.02f);
    CHECK(ctx, renderer::IBLBaker::BakeFromCubeFaces(nullptr, size, params).irradianceData.empty());
}

void TestInputHash(test::TestContext& ctx)
{
    auto makeWorld = [](bool reversed, float meshX, ecs::World*& out, EntityID& outProbe,
                        ecs::ComponentMetaRegistry& reg) {
        RegisterCoreComponents(reg);
        RegisterProbeComponents(reg);
        RegisterMeshRendererComponents(reg);
        RegisterLightingComponents(reg);
        out = new ecs::World(reg);
        auto addMesh = [&](float x, const char* path) {
            const EntityID e = out->CreateEntity();
            MeshComponent mesh{};
            mesh.meshAssetPath = path;
            out->Add<MeshComponent>(e, mesh);
            WorldTransformComponent xf{};
            xf.position = { x, 0.f, 0.f };
            out->Add<WorldTransformComponent>(e, xf);
        };
        auto addLight = [&]() {
            const EntityID e = out->CreateEntity();
            out->Add<LightComponent>(e, LightComponent{});
            out->Add<WorldTransformComponent>(e, WorldTransformComponent{});
        };
        if (reversed) { addLight(); addMesh(2.f, "b.kmesh"); addMesh(meshX, "a.kmesh"); }
        else          { addMesh(meshX, "a.kmesh"); addMesh(2.f, "b.kmesh"); addLight(); }
        outProbe = out->CreateEntity();
        out->Add<ReflectionProbeComponent>(outProbe, ReflectionProbeComponent{});
        out->Add<WorldTransformComponent>(outProbe, WorldTransformComponent{});
    };
    ecs::ComponentMetaRegistry r1, r2, r3;
    ecs::World* w1 = nullptr; ecs::World* w2 = nullptr; ecs::World* w3 = nullptr;
    EntityID p1{}, p2{}, p3{};
    makeWorld(false, 1.f, w1, p1, r1);
    makeWorld(true, 1.00001f, w2, p2, r2);   // other order, sub-millimetre difference
    makeWorld(false, 1.5f, w3, p3, r3);      // a mesh moved
    const uint64_t h1 = ComputeProbeInputHash(*w1, p1);
    CHECK(ctx, h1 == ComputeProbeInputHash(*w2, p2));
    CHECK(ctx, h1 != ComputeProbeInputHash(*w3, p3));
    w1->Get<ReflectionProbeComponent>(p1)->extents.x += 1.f;
    CHECK(ctx, h1 != ComputeProbeInputHash(*w1, p1));
    delete w1; delete w2; delete w3;
}

void TestProbeDataPath(test::TestContext& ctx)
{
    CHECK(ctx, MakeProbeDataPath("Room 2", 0xABCull) == "Baked/Room 2/0000000000000abc.kprobe");
    CHECK(ctx, MakeProbeDataPath("", 1u) == "Baked/Scene/0000000000000001.kprobe");
    CHECK_NE(ctx, GenerateProbeId(), uint64_t{0});
}

} // namespace

int RunProbeTests()
{
    test::TestSuite suite("Probes");
    suite
        .Add("Serialization round trip",       TestSerializationRoundTrip)
        .Add("Old scenes and sanitizing",      TestOldScenesAndSanitizing)
        .Add(".kprobe round trip and versions", TestProbeFileRoundTrip)
        .Add("Bake from cube faces",           TestBakeFromCubeFaces)
        .Add("Input hash",                     TestInputHash)
        .Add("Probe data path",                TestProbeDataPath);
    return suite.Run();
}
