#include "TestFramework.hpp"
#include "GltfImporter.hpp"

#include "animation/AnimationEvaluator.hpp"
#include "assets/AnimationClip.hpp"

#include <cmath>
#include <filesystem>
#include <fstream>

using namespace engine;
using namespace engine::assets;
using namespace engine::addons::gltf;

namespace {

[[nodiscard]] bool Near(float a, float b, float eps = 1e-5f) noexcept
{
    return std::fabs(a - b) <= eps;
}

static void TestChannelLinearVec3(test::TestContext& ctx)
{
    AnimationChannel channel;
    channel.property = AnimationTargetProperty::Translation;
    channel.interpolation = AnimationInterpolation::Linear;
    channel.times = { 0.f, 2.f };
    channel.vec3Values = { { 0.f, 0.f, 0.f }, { 4.f, 8.f, 12.f } };

    const math::Vec3 mid =
        animation::SampleVec3(channel.times, channel.vec3Values, channel.interpolation, 1.f);

    CHECK(ctx, Near(mid.x, 2.f));
    CHECK(ctx, Near(mid.y, 4.f));
    CHECK(ctx, Near(mid.z, 6.f));
}

static void TestChannelStepVec3(test::TestContext& ctx)
{
    AnimationChannel channel;
    channel.property = AnimationTargetProperty::Scale;
    channel.interpolation = AnimationInterpolation::Step;
    channel.times = { 0.f, 1.f, 2.f };
    channel.vec3Values = {
        { 1.f, 1.f, 1.f },
        { 2.f, 2.f, 2.f },
        { 3.f, 3.f, 3.f },
    };

    const math::Vec3 beforeNext =
        animation::SampleVec3(channel.times, channel.vec3Values, channel.interpolation, 0.99f);
    const math::Vec3 atNext =
        animation::SampleVec3(channel.times, channel.vec3Values, channel.interpolation, 1.f);

    CHECK(ctx, Near(beforeNext.x, 1.f));
    CHECK(ctx, Near(atNext.x, 2.f));
}

static void TestChannelLinearQuat(test::TestContext& ctx)
{
    AnimationChannel channel;
    channel.property = AnimationTargetProperty::Rotation;
    channel.interpolation = AnimationInterpolation::Linear;
    channel.times = { 0.f, 1.f };
    channel.quatValues = {
        math::Quat::Identity(),
        math::Quat::FromAxisAngleDeg(math::Vec3{ 0.f, 1.f, 0.f }, 90.f),
    };

    const math::Quat q =
        animation::SampleQuat(channel.times, channel.quatValues, channel.interpolation, 0.5f);

    CHECK(ctx, Near(q.LengthSq(), 1.f, 1e-5f));
}

static void TestNativeClipKeepsSeparateTRSChannels(test::TestContext& ctx)
{
    AnimationClip clip;
    clip.name = "native";

    AnimationChannel t;
    t.targetIndex = 7;
    t.targetName = "Node";
    t.property = AnimationTargetProperty::Translation;
    t.interpolation = AnimationInterpolation::Linear;
    t.times = { 0.f, 2.f };
    t.vec3Values = { { 0.f, 0.f, 0.f }, { 10.f, 0.f, 0.f } };

    AnimationChannel r;
    r.targetIndex = 7;
    r.targetName = "Node";
    r.property = AnimationTargetProperty::Rotation;
    r.interpolation = AnimationInterpolation::Step;
    r.times = { 1.f };
    r.quatValues = { math::Quat::FromAxisAngleDeg(math::Vec3{ 0.f, 1.f, 0.f }, 90.f) };

    clip.duration = 2.f;
    clip.channels.push_back(t);
    clip.channels.push_back(r);

    CHECK_EQ(ctx, clip.channels.size(), 2u);
    CHECK_EQ(ctx, clip.channels[0].targetIndex, 7);
    CHECK(ctx, clip.channels[0].property == AnimationTargetProperty::Translation);
    CHECK(ctx, clip.channels[1].property == AnimationTargetProperty::Rotation);
    CHECK(ctx, Near(clip.duration, 2.f));
}

static void TestWeightNormalization(test::TestContext& ctx)
{
    float w[4] = { 2.f, 2.f, 2.f, 2.f };
    const float sum = w[0] + w[1] + w[2] + w[3];
    const float inv = 1.f / sum;
    for (float& wi : w) wi *= inv;
    const float normalizedSum = w[0] + w[1] + w[2] + w[3];
    CHECK(ctx, Near(normalizedSum, 1.f));
}

static void TestGltfImporterImportsPbrMaterials(test::TestContext& ctx)
{
    namespace fs = std::filesystem;
    const fs::path root = fs::temp_directory_path() / "krom_gltf_material_test";
    fs::create_directories(root / "textures");

    const fs::path gltf = root / "mat_only.gltf";
    {
        std::ofstream out(gltf);
        out <<
            "{\n"
            "  \"asset\": { \"version\": \"2.0\" },\n"
            "  \"images\": [ { \"uri\": \"textures/base.png\" }, { \"uri\": \"textures/mr.png\" } ],\n"
            "  \"textures\": [ { \"source\": 0 }, { \"source\": 1 } ],\n"
            "  \"materials\": [ {\n"
            "    \"name\": \"Paint\",\n"
            "    \"pbrMetallicRoughness\": {\n"
            "      \"baseColorFactor\": [0.25, 0.5, 0.75, 0.8],\n"
            "      \"metallicFactor\": 0.2,\n"
            "      \"roughnessFactor\": 0.6,\n"
            "      \"baseColorTexture\": { \"index\": 0 },\n"
            "      \"metallicRoughnessTexture\": { \"index\": 1 }\n"
            "    },\n"
            "    \"emissiveFactor\": [0.1, 0.2, 0.3],\n"
            "    \"alphaMode\": \"BLEND\",\n"
            "    \"doubleSided\": true\n"
            "  } ]\n"
            "}\n";
    }

    GltfImporter importer;
    const ImportedAssetBundle bundle = importer.Import(gltf.string());

    CHECK(ctx, bundle.Ok());
    CHECK(ctx, bundle.materials.size() == 1u);
    if (!bundle.materials.empty())
    {
        const MaterialAsset& mat = bundle.materials[0];
        CHECK(ctx, mat.debugName == std::string("Paint"));
        CHECK(ctx, Near(mat.baseColorFactor.x, 0.25f));
        CHECK(ctx, Near(mat.baseColorFactor.w, 0.8f));
        CHECK(ctx, Near(mat.metallicFactor, 0.2f));
        CHECK(ctx, Near(mat.roughnessFactor, 0.6f));
        CHECK(ctx, mat.alphaMode == MaterialAlphaMode::Blend);
        CHECK(ctx, mat.transparent);
        CHECK(ctx, mat.doubleSided);
        CHECK(ctx, mat.baseColorTexture.path.find("textures") != std::string::npos);
        CHECK(ctx, mat.metallicRoughnessTexture.path.find("mr.png") != std::string::npos);
    }

    fs::remove_all(root);
}

} // namespace

int RunGltfImporterTests()
{
    test::TestSuite suite("GltfImporter");
    suite
        .Add("Channel linear Vec3", TestChannelLinearVec3)
        .Add("Channel step Vec3", TestChannelStepVec3)
        .Add("Channel linear Quat", TestChannelLinearQuat)
        .Add("Native clip keeps separate TRS channels", TestNativeClipKeepsSeparateTRSChannels)
        .Add("Weight normalization: sum to 1", TestWeightNormalization)
        .Add("GltfImporter imports PBR materials", TestGltfImporterImportsPbrMaterials);
    return suite.Run();
}
