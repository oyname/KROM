// =============================================================================
// KROM Engine - tests/test_probes.cpp
// Tests for the reflection probe add-on (component, .kprobe, cube-face bake).
// =============================================================================
#include "TestFramework.hpp"

#include "addons/lighting/LightingComponents.hpp"
#include "addons/mesh_renderer/MeshRendererComponents.hpp"
#include "addons/probes/ProbeBake.hpp"
#include "addons/probes/ProbeBoxFit.hpp"
#include "addons/probes/ProbeComponents.hpp"
#include "addons/probes/ProbeData.hpp"
#include "addons/probes/ProbeSerialization.hpp"
#include "assets/AssetRegistry.hpp"
#include "assets/TextureImporter.hpp"
#include "core/PathUtf8.hpp"
#include "ecs/Components.hpp"
#include "ecs/World.hpp"
#include "renderer/IBLBaker.hpp"
#include "serialization/SceneSerializer.hpp"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
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
    probe.boxOffset = { -1.5f, 0.25f, 2.f };
    probe.autoSizeLocked = true;
    probe.intensity = 0.75f;
    probe.resolution = 256u;
    probe.probeId = 0xF123456789ABCDEFull;   // above 2^53: must survive as hex
    probe.bakedDataPath = "Baked/Room 2/f123456789abcdef.kprobe";
    const EntityID e = world.CreateEntity();
    world.Add<ReflectionProbeComponent>(e, probe);

    auto bakeLockedProbe = probe;
    bakeLockedProbe.probeId = 7u;
    bakeLockedProbe.bakeLocked = true;
    bakeLockedProbe.autoSizeLocked = false;
    world.Add<ReflectionProbeComponent>(world.CreateEntity(), bakeLockedProbe);

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
        CHECK(ctx, *c == (c->probeId == probe.probeId ? probe : bakeLockedProbe));
    });
    CHECK_EQ(ctx, found, 2);
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
    CHECK(ctx, c.boxOffset == math::Vec3::Zero());
    CHECK(ctx, !c.bakeLocked);
    CHECK(ctx, !c.autoSizeLocked);
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

void TestDiffuseIrradianceFiltersHighFrequencyEnvironment(test::TestContext& ctx)
{
    // A fine black/white environment has mean radiance 0.5 in every direction.
    // Its diffuse lighting must not reproduce the texel pattern on a smooth sphere.
    constexpr uint32_t size = 64u;
    std::vector<float> faces(static_cast<size_t>(6u) * size * size * 3u);
    for (uint32_t face = 0u; face < 6u; ++face)
        for (uint32_t y = 0u; y < size; ++y)
            for (uint32_t x = 0u; x < size; ++x)
            {
                const float value = ((x + y) & 1u) ? 1.f : 0.f;
                const size_t index = ((static_cast<size_t>(face) * size + y) * size + x) * 3u;
                faces[index] = faces[index + 1u] = faces[index + 2u] = value;
            }
    renderer::IBLBakeParams params{};
    params.irradianceSize = 16u;
    params.irradianceSamples = 64u;
    params.prefilterBaseSize = 4u;
    params.prefilterMipCount = 3u;
    params.prefilterSamples = 16u;
    const auto baked = renderer::IBLBaker::BakeFromCubeFaces(faces.data(), size, params);
    CHECK(ctx, baked.IsValid());
    if (!baked.IsValid()) return;
    float maxError = 0.f;
    for (uint32_t face = 0u; face < 6u; ++face)
        for (uint32_t y = 0u; y < params.irradianceSize; ++y)
            for (uint32_t x = 0u; x < params.irradianceSize; ++x)
                maxError = std::max(maxError, std::fabs(CubeRed(baked.irradianceData,
                    params.irradianceSize, face, x, y) / math::PI - 0.5f));
    std::printf("# Diffuse high-frequency environment max radiance error: %.6f\n", maxError);
    CHECK(ctx, maxError < 0.01f);
}

void TestPureSkyBakeMeasurements(test::TestContext& ctx)
{
    assets::TextureImportRequest request{};
    request.sourcePath = std::filesystem::path(__FILE__).parent_path().parent_path()
                       / "assets/autumn_field_puresky_2k.hdr";
    if (const char* path = std::getenv("KROM_IBL_MEASURE_HDR"))
        request.sourcePath = PathFromUtf8(path);
    std::ifstream input(request.sourcePath, std::ios::binary);
    request.fileBytes.assign(std::istreambuf_iterator<char>(input), {});
    assets::TextureAsset texture{};
    CHECK(ctx, assets::TextureImporter::Import(request, texture));
    if (texture.pixelData.empty()) return;
    renderer::IBLBakeDiagnostics diagnostics{};
    renderer::IBLBakeParams params{};
    params.environmentSize = 128u;
    params.irradianceSize = 8u;
    params.prefilterBaseSize = 128u;
    params.prefilterSamples = 64u;
    params.diagnostics = &diagnostics;
    const auto baked = renderer::IBLBaker::BakeFromTexture(texture, 1.f, params);
    CHECK(ctx, baked.IsValid());
    if (!baked.IsValid()) return;
    std::printf("# puresky sun: sky median %.6f, removed peak %.6f, solid angle %.9f sr, original peak now %.6f\n",
        diagnostics.skyMedianLuminance, diagnostics.removedSunPeakLuminance,
        diagnostics.removedSunSolidAngle, diagnostics.sourcePeakAfterFiltering);
    for (const auto& stage : {std::pair{"source", diagnostics.source},
                             std::pair{"compressed", diagnostics.compressed},
                             std::pair{"lighting", diagnostics.lighting}})
        std::printf("# puresky %s: mean %.6f, max %.6f, changed %.6f%%\n", stage.first,
            stage.second.meanLuminance, stage.second.maxLuminance, stage.second.changedPixelFraction * 100.0);
    size_t offset = 0u;
    for (uint32_t mip = 0u, size = baked.prefilterBaseSize; mip < baked.prefilterMipCount;
         ++mip, size = std::max(size / 2u, 1u))
    {
        float maximum = 0.f;
        for (size_t pixel = 0u; pixel < static_cast<size_t>(6u) * size * size; ++pixel)
        {
            uint16_t h[4];
            std::memcpy(h, baked.prefilterData.data() + offset + pixel * 8u, 8u);
            maximum = std::max(maximum, HalfToFloatTest(h[0]) * .2126f
                + HalfToFloatTest(h[1]) * .7152f + HalfToFloatTest(h[2]) * .0722f);
        }
        if (mip == 0u || mip == 2u)
            std::printf("# puresky prefilter mip %u: max luminance %.6f\n", mip, maximum);
        offset += static_cast<size_t>(6u) * size * size * 8u;
    }
}

renderer::IBLBakeParams SunTestParams()
{
    renderer::IBLBakeParams params{};
    params.environmentSize = 64u;
    params.irradianceSize = 8u;
    params.prefilterBaseSize = 32u;
    params.prefilterMipCount = 4u;
    params.prefilterSamples = 32u;
    return params;
}

float MaximumCubeChannel(const std::vector<uint8_t>& bytes)
{
    float maximum = 0.f;
    for (size_t i = 0u; i + 8u <= bytes.size(); i += 8u)
    {
        uint16_t h[4];
        std::memcpy(h, bytes.data() + i, 8u);
        for (int channel = 0; channel < 3; ++channel)
            maximum = std::max(maximum, HalfToFloatTest(h[channel]));
    }
    return maximum;
}

assets::TextureAsset SyntheticSunTexture(bool sun)
{
    assets::TextureAsset texture{};
    texture.width = 256u;
    texture.height = 128u;
    texture.format = assets::TextureFormat::RGBA16F;
    texture.pixelData.resize(static_cast<size_t>(texture.width) * texture.height * 8u);
    for (uint32_t y = 0u; y < texture.height; ++y)
        for (uint32_t x = 0u; x < texture.width; ++x)
        {
            const uint16_t value = sun && x >= 125u && x < 131u && y >= 29u && y < 35u ? 0x5400u : 0x3C00u;
            const uint16_t rgba[] = {value, value, value, 0x3C00u}; // radiance 64 or 1
            std::memcpy(texture.pixelData.data() + (static_cast<size_t>(y) * texture.width + x) * 8u, rgba, 8u);
        }
    return texture;
}

void TestSeparateVisibleSkyAndLighting(test::TestContext& ctx)
{
    const auto baked = renderer::IBLBaker::BakeFromTexture(SyntheticSunTexture(true), 1.f, SunTestParams());
    CHECK(ctx, baked.IsValid());
    CHECK_GT(ctx, MaximumCubeChannel(baked.environmentData), 20.f);
    CHECK(ctx, MaximumCubeChannel(baked.prefilterData) < 12.f);
    const auto uniform = renderer::IBLBaker::BakeFromTexture(SyntheticSunTexture(false), 1.f, SunTestParams());
    CHECK(ctx, std::fabs(MaximumCubeChannel(uniform.environmentData) - 1.f) < .001f);
    CHECK(ctx, std::fabs(MaximumCubeChannel(uniform.prefilterData) - 1.f) < .001f);
    CHECK(ctx, std::fabs(MaximumCubeChannel(uniform.irradianceData) - math::PI) < .003f);
}

void TestProceduralSunOnlyInVisibleSky(test::TestContext& ctx)
{
    renderer::ProceduralSkyDesc sky{};
    sky.sunAngularRadius = .15f;
    sky.sunIntensity = 64.f;
    const auto sun = renderer::IBLBaker::BakeFromProceduralSky(sky, 1.f, SunTestParams());
    sky.sunIntensity = 0.f;
    const auto noSun = renderer::IBLBaker::BakeFromProceduralSky(sky, 1.f, SunTestParams());
    CHECK(ctx, sun.IsValid() && noSun.IsValid());
    CHECK(ctx, sun.environmentData != noSun.environmentData);
    CHECK_GT(ctx, MaximumCubeChannel(sun.environmentData), 20.f);
    CHECK(ctx, sun.irradianceData == noSun.irradianceData);
    CHECK(ctx, sun.prefilterData == noSun.prefilterData);
}

void TestSunRemovalSwitchAndSkyRegions(test::TestContext& ctx)
{
    const auto source = SyntheticSunTexture(true);
    auto params = SunTestParams();
    const auto removed = renderer::IBLBaker::BakeFromTexture(source, 1.f, params);
    params.removeSunFromLighting = false;
    const auto kept = renderer::IBLBaker::BakeFromTexture(source, 1.f, params);
    CHECK(ctx, removed.environmentData == kept.environmentData);
    CHECK(ctx, MaximumCubeChannel(removed.prefilterData) < 1.01f);
    CHECK(ctx, MaximumCubeChannel(removed.irradianceData) < math::PI + .01f);
    CHECK_GT(ctx, MaximumCubeChannel(kept.prefilterData), 1.5f);
    CHECK(ctx, renderer::IBLBaker::HashTextureSource(source, 1.f, true)
            != renderer::IBLBaker::HashTextureSource(source, 1.f, false));
    CHECK_EQ(ctx, renderer::IBLBaker::HashTextureSource(source, 1.f),
                  renderer::IBLBaker::HashTextureSource(source, 1.f, true));

    for (int shape = 0; shape < 3; ++shape)
    {
        auto texture = SyntheticSunTexture(false);
        for (uint32_t y = 0u; y < texture.height; ++y)
            for (uint32_t x = 0u; x < texture.width; ++x)
            {
                // Large cloud, seam-crossing disc, then a small polar cap.
                const bool bright = shape == 0 ? x >= 80u && x < 180u && y >= 20u && y < 60u
                                  : shape == 1 ? (x < 3u || x >= 253u) && y >= 29u && y < 35u
                                               : y < 3u;
                if (bright)
                {
                    const uint16_t rgba[] = {0x5400u, 0x5400u, 0x5400u, 0x3C00u};
                    std::memcpy(texture.pixelData.data() + (static_cast<size_t>(y) * texture.width + x) * 8u, rgba, 8u);
                }
            }
        params.removeSunFromLighting = true;
        const auto filtered = renderer::IBLBaker::BakeFromTexture(texture, 1.f, params);
        if (shape == 0)
        {
            params.removeSunFromLighting = false;
            const auto unfiltered = renderer::IBLBaker::BakeFromTexture(texture, 1.f, params);
            CHECK(ctx, filtered.prefilterData == unfiltered.prefilterData);
            CHECK(ctx, filtered.irradianceData == unfiltered.irradianceData);
        }
        else
            CHECK(ctx, MaximumCubeChannel(filtered.prefilterData) < 1.01f);
    }
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
    auto& probe = *w1->Get<ReflectionProbeComponent>(p1);
    probe.bakeLocked = true;
    CHECK_EQ(ctx, h1, ComputeProbeInputHash(*w1, p1));
    probe.autoSizeLocked = true;
    CHECK_EQ(ctx, h1, ComputeProbeInputHash(*w1, p1));
    probe.boxOffset = { 0.25f, -0.5f, 1.f };
    CHECK(ctx, h1 != ComputeProbeInputHash(*w1, p1));
    probe.boxOffset = math::Vec3::Zero();
    CHECK_EQ(ctx, h1, ComputeProbeInputHash(*w1, p1));
    probe.extents.x += 1.f;
    CHECK(ctx, h1 != ComputeProbeInputHash(*w1, p1));
    delete w1; delete w2; delete w3;
}

void TestFitProbeBox(test::TestContext& ctx)
{
    auto reg = MakeProbeRegistry();
    RegisterMeshRendererComponents(reg);
    ecs::World world(reg);
    assets::AssetRegistry assets;
    auto cube = std::make_unique<assets::MeshAsset>();
    cube->state = assets::AssetState::Loaded;
    auto& submesh = cube->submeshes.emplace_back();
    submesh.positions = {
        -0.5f, -0.5f, -0.5f,  0.5f, -0.5f, -0.5f,  0.5f,  0.5f, -0.5f, -0.5f,  0.5f, -0.5f,
        -0.5f, -0.5f,  0.5f,  0.5f, -0.5f,  0.5f,  0.5f,  0.5f,  0.5f, -0.5f,  0.5f,  0.5f,
    };
    submesh.indices = {
        0, 2, 1, 0, 3, 2,  4, 5, 6, 4, 6, 7,  0, 4, 7, 0, 7, 3,
        1, 2, 6, 1, 6, 5,  0, 1, 5, 0, 5, 4,  3, 7, 6, 3, 6, 2,
    };
    const MeshHandle mesh = assets.GetOrAddMesh("room-cube", std::move(cube));
    const math::Quat rotation = math::Quat::FromAxisAngleDeg({ 0.f, 1.f, 0.f }, 37.f);
    const math::Vec3 roomCenter{ 4.f, 2.f, -3.f };
    const auto setTransform = [&](EntityID entity, const math::Vec3& local, const math::Vec3& size) {
        WorldTransformComponent xf{};
        xf.position = roomCenter + rotation.Rotate(local);
        xf.rotation = rotation;
        xf.matrix = math::Mat4::TRS(xf.position, rotation, size);
        world.Add<WorldTransformComponent>(entity, xf);
    };
    const auto addBox = [&](const math::Vec3& local, const math::Vec3& size) {
        const EntityID entity = world.CreateEntity();
        setTransform(entity, local, size);
        world.Add<MeshComponent>(entity, mesh);
        auto& bounds = world.Add<BoundsComponent>(entity);
        bounds.centerWorld = world.Get<WorldTransformComponent>(entity)->position;
        bounds.boundingSphere = size.Length() * 0.5f;
        return entity;
    };

    const math::Vec3 capture{ 1.f, -0.4f, 0.7f };
    const EntityID probe = addBox(capture, { 0.2f, 0.2f, 0.2f });
    world.Add<ReflectionProbeComponent>(probe, ReflectionProbeComponent{});
    const EntityID hidden = addBox(capture, { 0.5f, 0.5f, 0.5f });
    world.Get<MeshComponent>(hidden)->visible = false;
    const EntityID inactive = addBox(capture, { 1.f, 1.f, 1.f });
    world.Add<ActiveComponent>(inactive, ActiveComponent{ false });
    const EntityID preview = addBox(capture, { 1.5f, 1.5f, 1.5f });
    world.Get<MeshComponent>(preview)->layerMask = renderer::LAYER_EDITOR_MATERIAL_PREVIEW;
    const EntityID gizmo = addBox(capture, { 2.f, 2.f, 2.f });
    world.Get<MeshComponent>(gizmo)->layerMask = renderer::LAYER_EDITOR_GIZMO_DEPTH;

    const auto empty = FitProbeBox(world, assets, probe);
    CHECK(ctx, empty.has_value());
    if (empty)
    {
        CHECK(ctx, empty->boxOffset == math::Vec3::Zero());
        CHECK(ctx, empty->extents == math::Vec3(49.98f));
    }

    addBox({ -4.1f, 0.f, 0.f }, { 0.2f, 5.f, 7.f });
    addBox({  4.1f, 0.f, 0.f }, { 0.2f, 5.f, 7.f });
    addBox({ 0.f, -2.1f, 0.f }, { 9.f, 0.2f, 7.f });
    addBox({ 0.f,  2.1f, 0.f }, { 9.f, 0.2f, 7.f });
    addBox({ 0.f, 0.f, -3.1f }, { 9.f, 5.f, 0.2f });
    addBox({ 0.f, 0.f,  3.1f }, { 9.f, 5.f, 0.2f });
    // A small obstruction hits the center ray towards +X.
    addBox(capture + math::Vec3{ 1.f, 0.f, 0.f }, { 0.05f, 0.05f, 0.05f });

    const auto fit = FitProbeBox(world, assets, probe);
    CHECK(ctx, fit.has_value());
    if (fit)
    {
        CHECK(ctx, (fit->extents - math::Vec3{ 3.98f, 1.98f, 2.98f }).Length() < 1e-4f);
        CHECK(ctx, (fit->boxOffset + capture).Length() < 1e-4f);
    }
}

void TestProbeDataPath(test::TestContext& ctx)
{
    CHECK(ctx, MakeProbeDataPath("Room 2", 0xABCull) == "Baked/Room 2/0000000000000abc.kprobe");
    CHECK(ctx, MakeProbeDataPath("", 1u) == "Baked/Scene/0000000000000001.kprobe");
    CHECK_NE(ctx, GenerateProbeId(), uint64_t{0});
}

void TestDeleteUnusedProbeFiles(test::TestContext& ctx)
{
    CHECK_EQ(ctx, ProbeIdFromFileName("0000000000000abc.kprobe"), uint64_t{0xABC});
    CHECK_EQ(ctx, ProbeIdFromFileName("abc.kprobe"), uint64_t{0});
    CHECK_EQ(ctx, ProbeIdFromFileName("000000000000zzzz.kprobe"), uint64_t{0});

    namespace fs = std::filesystem;
    const fs::path assets = fs::temp_directory_path() / "krom_probe_cleanup äö" / "Assets";
    std::error_code ec;
    fs::remove_all(assets.parent_path(), ec);
    fs::create_directories(assets / "Scenes", ec);
    auto touch = [&](uint64_t id) {
        const std::string path = PathToUtf8(assets / PathFromUtf8(MakeProbeDataPath("Räume", id)));
        fs::create_directories(PathFromUtf8(path).parent_path(), ec);
        std::FILE* f = OpenFileUtf8(path, "wb");
        if (f) std::fclose(f);
        return PathFromUtf8(path);
    };
    const fs::path inWorld = touch(0x1111u);
    const fs::path inOtherScene = touch(0x2222u);
    const fs::path orphan = touch(0x3333u);
    if (std::FILE* f = OpenFileUtf8(PathToUtf8(assets / "Scenes" / "Other.json"), "wb"))
    {
        const char text[] = R"({"probeId": "0000000000002222"})";
        std::fwrite(text, 1u, sizeof(text) - 1u, f);
        std::fclose(f);
    }

    const auto deleted = DeleteUnusedProbeFiles(PathToUtf8(assets), "Räume", { 0x1111u });
    CHECK_EQ(ctx, deleted.size(), size_t{1});
    CHECK(ctx, fs::exists(inWorld) && fs::exists(inOtherScene) && !fs::exists(orphan));
    CHECK(ctx, DeleteUnusedProbeFiles(PathToUtf8(assets), "Missing", {}).empty());

    fs::remove_all(assets.parent_path(), ec);
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
        .Add("Diffuse irradiance filters high-frequency environments", TestDiffuseIrradianceFiltersHighFrequencyEnvironment)
        .Add("Pure sky bake measurements", TestPureSkyBakeMeasurements)
        .Add("Separate visible sky and lighting", TestSeparateVisibleSkyAndLighting)
        .Add("Procedural sun only in visible sky", TestProceduralSunOnlyInVisibleSky)
        .Add("Sun removal switch, broad clouds, seams and poles", TestSunRemovalSwitchAndSkyRegions)
        .Add("Input hash",                     TestInputHash)
        .Add("Fit box from scene triangles",   TestFitProbeBox)
        .Add("Probe data path",                TestProbeDataPath)
        .Add("Delete unused probe files",      TestDeleteUnusedProbeFiles);
    return suite.Run();
}
