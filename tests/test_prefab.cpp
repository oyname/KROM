#include "TestFramework.hpp"
#include "core/PathUtf8.hpp"
#include "addons/gltf/GltfImporter.hpp"
#include "addons/animation/AnimationSystem.hpp"
#include "assets/AssetPipeline.hpp"
#include "animation/ModelClipSettings.hpp"
#include <cmath>
#include <cstring>
#include <filesystem>

#include "addons/animation/AnimationComponents.hpp"
#include "addons/mesh_renderer/MeshRendererComponents.hpp"
#include "addons/mesh_renderer/MeshRendererSerialization.hpp"
#include "addons/physics/PhysicsComponents.hpp"
#include "addons/physics/PhysicsSerialization.hpp"
#include "addons/prefab/Prefab.hpp"
#include "addons/prefab/PrefabSerialization.hpp"
#include "addons/editor/EditorAssetBrowser.hpp"
#include "addons/editor/EditorCommands.hpp"
#include "addons/editor/EditorMaterialLibrary.hpp"
#include "addons/editor/EditorComponents.hpp"
#include "addons/editor/EditorPrefabWindow.hpp"
#include "addons/editor/EditorUIInternal.hpp"
#include "scene/TransformSystem.hpp"
#include "serialization/SceneSerializer.hpp"
#include "renderer/MaterialSystem.hpp"
#include "addons/script/ComponentScript.hpp"
#include "addons/script/ScriptList.hpp"
#include "assets/AssetRegistry.hpp"
#include "assets/MaterialProperties.hpp"
#include "ecs/Components.hpp"
#include "ecs/World.hpp"

#include <memory>
#include <algorithm>
#include <fstream>
#include <sstream>

using namespace engine;
using namespace engine::addons::prefab;

namespace {

class PrefabTestScript final : public script::ComponentScript
{
public:
    float speed = 3.f;
    std::string prefabPath = "Prefabs/Default.prefab";
    EntityID target = NULL_ENTITY;
};

assets::ImportedAssetBundle MakePrefabBundle()
{
    assets::ImportedAssetBundle bundle;

    assets::MaterialAsset material;
    material.debugName = "TestMaterial";
    material.state = assets::AssetState::Loaded;
    bundle.materials.push_back(std::move(material));

    assets::MeshAsset mesh;
    mesh.debugName = "TestMesh";
    mesh.path = "prefab_test.gltf";
    mesh.state = assets::AssetState::Loaded;
    assets::SubMeshData submesh;
    submesh.positions = { 0.f, 0.f, 0.f, 1.f, 0.f, 0.f, 0.f, 1.f, 0.f };
    submesh.normals = { 0.f, 0.f, 1.f, 0.f, 0.f, 1.f, 0.f, 0.f, 1.f };
    submesh.indices = { 0u, 1u, 2u };
    submesh.boneWeights = {
        1.f, 0.f, 0.f, 0.f,
        1.f, 0.f, 0.f, 0.f,
        1.f, 0.f, 0.f, 0.f,
    };
    submesh.boneIndices = {
        0u, 0u, 0u, 0u,
        0u, 0u, 0u, 0u,
        0u, 0u, 0u, 0u,
    };
    mesh.submeshes.push_back(std::move(submesh));
    bundle.meshes.push_back(std::move(mesh));
    bundle.meshSkinIndex.push_back(0);

    assets::SkeletonAsset skeleton;
    skeleton.bones.push_back({ "root", -1, math::Mat4::Identity() });
    bundle.skeletons.push_back(std::move(skeleton));

    assets::AnimationClip clip;
    clip.name = "Idle";
    clip.duration = 1.f;
    bundle.animations.push_back(std::move(clip));

    assets::ImportedSceneNode root;
    root.name = "Armature";
    root.parentIndex = -1;
    bundle.nodes.push_back(std::move(root));

    assets::ImportedSceneNode meshNode;
    meshNode.name = "Body";
    meshNode.parentIndex = 0;
    meshNode.meshIndex = 0;
    meshNode.skinIndex = 0;
    meshNode.translation = { 1.f, 2.f, 3.f };
    bundle.nodes.push_back(std::move(meshNode));

    return bundle;
}

assets::ImportedAssetBundle MakeMeshOnlyBundle()
{
    assets::ImportedAssetBundle bundle = MakePrefabBundle();
    bundle.nodes.clear();
    return bundle;
}

ecs::ComponentMetaRegistry MakePrefabComponentRegistry()
{
    ecs::ComponentMetaRegistry registry;
    RegisterCoreComponents(registry);
    RegisterMeshRendererComponents(registry);
    RegisterAnimationComponents(registry);
    ecs::RegisterComponent<engine::script::ScriptList>(registry, "ScriptList");
    RegisterPhysicsComponents(registry);
    RegisterAudioComponents(registry);
    RegisterParticleComponents(registry);
    RegisterCameraComponents(registry);
    RegisterLightingComponents(registry);
    RegisterPrefabInstanceComponent(registry);
    engine::renderer::addons::editor::RegisterEditorComponents(registry);
    return registry;
}

// Minimal skinned + animated glTF: one triangle, one joint, one translation clip.
void WriteRiggedTestModel(const std::filesystem::path& gltfPath)
{
    std::vector<uint8_t> bin(236u, 0u);
    const auto put = [&bin](size_t offset, const void* data, size_t size) {
        std::memcpy(bin.data() + offset, data, size);
    };
    const float positions[9] = {0.f, 0.f, 0.f, 1.f, 0.f, 0.f, 0.f, 1.f, 0.f};
    const float normals[9]   = {0.f, 0.f, 1.f, 0.f, 0.f, 1.f, 0.f, 0.f, 1.f};
    const uint16_t indices[3] = {0u, 1u, 2u};
    const uint8_t joints[12]  = {0u};
    const float weights[12]   = {1.f, 0.f, 0.f, 0.f, 1.f, 0.f, 0.f, 0.f, 1.f, 0.f, 0.f, 0.f};
    const float inverseBind[16] = {1.f, 0.f, 0.f, 0.f, 0.f, 1.f, 0.f, 0.f,
                                   0.f, 0.f, 1.f, 0.f, 0.f, 0.f, 0.f, 1.f};
    const float times[2] = {0.f, 1.f};
    const float translations[6] = {0.f, 0.f, 0.f, 0.f, 2.f, 0.f};
    put(0u, positions, sizeof(positions));
    put(36u, normals, sizeof(normals));
    put(72u, indices, sizeof(indices));
    put(80u, joints, sizeof(joints));
    put(92u, weights, sizeof(weights));
    put(140u, inverseBind, sizeof(inverseBind));
    put(204u, times, sizeof(times));
    put(212u, translations, sizeof(translations));
    {
        std::ofstream out(gltfPath.parent_path() / "rig.bin", std::ios::binary);
        out.write(reinterpret_cast<const char*>(bin.data()), static_cast<std::streamsize>(bin.size()));
    }
    std::ofstream out(gltfPath);
    out << R"({
  "asset": { "version": "2.0" },
  "buffers": [ { "uri": "rig.bin", "byteLength": 236 } ],
  "bufferViews": [
    { "buffer": 0, "byteOffset": 0,   "byteLength": 36 },
    { "buffer": 0, "byteOffset": 36,  "byteLength": 36 },
    { "buffer": 0, "byteOffset": 72,  "byteLength": 6 },
    { "buffer": 0, "byteOffset": 80,  "byteLength": 12 },
    { "buffer": 0, "byteOffset": 92,  "byteLength": 48 },
    { "buffer": 0, "byteOffset": 140, "byteLength": 64 },
    { "buffer": 0, "byteOffset": 204, "byteLength": 8 },
    { "buffer": 0, "byteOffset": 212, "byteLength": 24 }
  ],
  "accessors": [
    { "bufferView": 0, "componentType": 5126, "count": 3, "type": "VEC3", "min": [0,0,0], "max": [1,1,0] },
    { "bufferView": 1, "componentType": 5126, "count": 3, "type": "VEC3" },
    { "bufferView": 2, "componentType": 5123, "count": 3, "type": "SCALAR" },
    { "bufferView": 3, "componentType": 5121, "count": 3, "type": "VEC4" },
    { "bufferView": 4, "componentType": 5126, "count": 3, "type": "VEC4" },
    { "bufferView": 5, "componentType": 5126, "count": 1, "type": "MAT4" },
    { "bufferView": 6, "componentType": 5126, "count": 2, "type": "SCALAR", "min": [0], "max": [1] },
    { "bufferView": 7, "componentType": 5126, "count": 2, "type": "VEC3" }
  ],
  "meshes": [ { "name": "Tri", "primitives": [ { "attributes": {
      "POSITION": 0, "NORMAL": 1, "JOINTS_0": 3, "WEIGHTS_0": 4 }, "indices": 2 } ] } ],
  "skins": [ { "joints": [2], "inverseBindMatrices": 5 } ],
  "nodes": [
    { "name": "Root", "children": [1, 2] },
    { "name": "Body", "mesh": 0, "skin": 0 },
    { "name": "Joint" }
  ],
  "scenes": [ { "nodes": [0] } ],
  "scene": 0,
  "animations": [ { "name": "Move",
    "samplers": [ { "input": 6, "output": 7, "interpolation": "LINEAR" } ],
    "channels": [ { "sampler": 0, "target": { "node": 2, "path": "translation" } } ] } ]
})";
}

// A linked instance from a scene saved before skins/players were serialized has
// neither component; syncing with its unchanged prefab must restore both.
void TestPrefabSyncRestoresMissingAnimation(test::TestContext& ctx)
{
    namespace fs = std::filesystem;
    const fs::path root = fs::temp_directory_path() / "krom_prefab_sync_anim";
    fs::remove_all(root);
    fs::create_directories(root / "Models");
    WriteRiggedTestModel(root / "Models" / "rig.gltf");

    assets::AssetRegistry registry;
    assets::AssetPipeline pipeline(registry, nullptr);
    pipeline.RegisterMeshImporter(std::make_unique<engine::addons::gltf::GltfImporter>());
    pipeline.SetAssetRoot(root);
    engine::addons::prefab::SetPrefabAssetPipeline(&pipeline);

    const std::string json = std::string(R"({"type":"KromPrefab","version":2,"name":"Rig","rootIndex":0,"records":[)") +
        R"({"name":"Rig","parentIndex":-1},)" +
        R"({"name":"Body","parentIndex":0,"meshAssetPath":"Models/rig.gltf#mesh/0","skeletonAssetPath":"Models/rig.gltf#skeleton/0",)" +
        R"("animationAssetPath":"Models/rig.gltf#animation/0","playAnimation":true,"loopAnimation":true}]})";
    engine::addons::prefab::PrefabAsset prefab;
    CHECK(ctx, engine::addons::prefab::DeserializePrefabFromJson(json, registry, prefab, nullptr));

    auto components = MakePrefabComponentRegistry();
    ecs::World world(components);
    const auto instance = engine::addons::prefab::InstantiatePrefab(world, prefab);
    CHECK(ctx, instance.IsValid() && instance.entities.size() == 2u);
    if (instance.entities.size() == 2u)
    {
        LinkPrefabInstance(world, prefab, instance, "Prefabs/Rig.prefab");
        const EntityID body = instance.entities[1];
        world.Remove<SkinComponent>(body);               // old scene: never saved
        world.Remove<AnimationPlayerComponent>(body);
        CHECK(ctx, SynchronizePrefabInstance(world, instance.root, prefab, registry));
        const auto* skin = world.Get<SkinComponent>(body);
        const auto* player = world.Get<AnimationPlayerComponent>(body);
        CHECK(ctx, skin && registry.skeletons.Get(skin->skeleton) != nullptr);
        CHECK(ctx, player && registry.animations.Get(player->clip) != nullptr);
        CHECK(ctx, player && player->playing && player->looping);
        CHECK(ctx, !world.Has<AnimationPlayerComponent>(instance.root));
    }
    engine::addons::prefab::SetPrefabAssetPipeline(nullptr);
    fs::remove_all(root);
}

// Clip settings beside a model: whole clips keep the Blender name, cut clips are
// built from a frame range, a missing source is skipped, and "#animation/<n>"
// still resolves for older scenes. Editing the settings replaces clips in place.
void TestModelClipSettingsImport(test::TestContext& ctx)
{
    namespace fs = std::filesystem;
    const fs::path root = fs::temp_directory_path() / "krom_model_clip_settings";
    fs::remove_all(root);
    fs::create_directories(root / "Models");
    WriteRiggedTestModel(root / "Models" / "rig.gltf");
    {
        std::ofstream out(root / "Models" / "rig.gltf.clips");
        out << R"({"kromclips":1,"fps":10,"clips":[)"
               R"({"id":"whole","name":"Move","source":"Move","loop":true},)"
               R"({"id":"cut","name":"Second half","source":"Move","start":5,"end":10,"loop":false,"rootMotion":true},)"
               R"({"id":"lost","name":"Lost","source":"Renamed in Blender"}]})";
    }

    assets::AssetRegistry registry;
    assets::AssetPipeline pipeline(registry, nullptr);
    pipeline.RegisterMeshImporter(std::make_unique<engine::addons::gltf::GltfImporter>());
    pipeline.SetAssetRoot(root);

    CHECK(ctx, engine::addons::mesh_renderer::ImportModelAnimationAssets(pipeline, registry, "Models/rig.gltf#clip/cut"));
    const auto* legacy = registry.animations.Get(registry.FindAnimationByPath("Models/rig.gltf#animation/0"));
    const auto* whole = registry.animations.Get(registry.FindAnimationByPath("Models/rig.gltf#clip/whole"));
    const AnimationHandle cutHandle = registry.FindAnimationByPath("Models/rig.gltf#clip/cut");
    const auto* cut = registry.animations.Get(cutHandle);
    CHECK(ctx, legacy && legacy->name == "Move");
    CHECK(ctx, whole && whole->name == "Move" && whole->looping);
    CHECK(ctx, cut && cut->name == "Second half" && !cut->looping && cut->rootMotion);
    CHECK(ctx, cut && std::fabs(cut->duration - 0.5f) < 1e-4f);
    if (cut && !cut->channels.empty() && !cut->channels[0].vec3Values.empty())
        CHECK(ctx, std::fabs(cut->channels[0].vec3Values.front().y - 1.f) < 1e-4f); // halfway of 0 -> 2
    CHECK(ctx, !registry.FindAnimationByPath("Models/rig.gltf#clip/lost").IsValid());

    engine::animation::ModelClipSettings settings;
    CHECK(ctx, engine::addons::mesh_renderer::ReadModelClipSettings(pipeline, "Models/rig.gltf", settings));
    CHECK_EQ(ctx, settings.clips.size(), size_t{3});
    settings.Find("cut")->name = "Renamed";
    engine::addons::mesh_renderer::RegisterModelClips(registry, "Models/rig.gltf", settings);
    CHECK(ctx, registry.FindAnimationByPath("Models/rig.gltf#clip/cut") == cutHandle);
    CHECK(ctx, registry.animations.Get(cutHandle) && registry.animations.Get(cutHandle)->name == "Renamed");

    // No settings file: nothing to read, old references keep working.
    fs::remove(root / "Models" / "rig.gltf.clips");
    CHECK(ctx, engine::addons::mesh_renderer::ReadModelClipSettings(pipeline, "Models/rig.gltf", settings) && settings.clips.empty());
    fs::remove_all(root);
}

// Scene file round trip (save -> load in a fresh registry -> resolve): a skinned,
// animated entity keeps skeleton, clip and playback settings, absolute model
// paths come back relative, and the AnimationSystem produces a pose.
void TestSceneRoundTripKeepsSkinnedAnimation(test::TestContext& ctx)
{
    namespace fs = std::filesystem;
    const fs::path root = fs::temp_directory_path() / "krom_scene_skin_roundtrip";
    fs::remove_all(root);
    fs::create_directories(root / "Models");
    WriteRiggedTestModel(root / "Models" / "rig.gltf");
    const std::string absBase = PathToGenericUtf8(root / "Models" / "rig.gltf");

    auto components = MakePrefabComponentRegistry();
    std::string json;
    {
        ecs::World world(components);
        const EntityID relative = world.CreateEntity();
        SkinComponent skin{};
        skin.skeletonAssetPath = "Models/rig.gltf#skeleton/0";
        world.Add<SkinComponent>(relative, skin);
        AnimationPlayerComponent player{};
        player.clipAssetPath = "Models/rig.gltf#animation/0";
        player.playing = true;
        player.looping = true;
        player.speed = 1.5f;
        world.Add<AnimationPlayerComponent>(relative, player);

        const EntityID absolute = world.CreateEntity();
        SkinComponent absSkin{};
        absSkin.skeletonAssetPath = absBase + "#skeleton/0";
        world.Add<SkinComponent>(absolute, absSkin);

        serialization::SceneSerializer serializer(world);
        serializer.RegisterDefaultHandlers();
        engine::addons::mesh_renderer::RegisterMeshRendererSerializationHandlers(serializer);
        json = serializer.SerializeToJson("SkinRoundTrip");
    }
    CHECK(ctx, json.find("\"clipAssetPath\"") != std::string::npos);
    CHECK(ctx, json.find("Models/rig.gltf#skeleton/0") != std::string::npos);

    assets::AssetRegistry registry;
    assets::AssetPipeline pipeline(registry, nullptr);
    pipeline.RegisterMeshImporter(std::make_unique<engine::addons::gltf::GltfImporter>());
    pipeline.SetAssetRoot(root);

    ecs::World loaded(components);
    serialization::SceneDeserializer deserializer(loaded);
    deserializer.RegisterDefaultHandlers();
    engine::addons::mesh_renderer::RegisterMeshRendererDeserializationHandlers(deserializer);
    CHECK(ctx, deserializer.DeserializeFromJson(json).success);
    engine::addons::mesh_renderer::ResolveMeshAssetBindings(pipeline, registry, loaded);

    uint32_t skins = 0u, resolvedSkins = 0u, relativeSkinPaths = 0u;
    EntityID animated = NULL_ENTITY;
    loaded.View<SkinComponent>([&](EntityID id, SkinComponent& skin) {
        ++skins;
        if (registry.skeletons.Get(skin.skeleton)) ++resolvedSkins;
        if (skin.skeletonAssetPath == "Models/rig.gltf#skeleton/0") ++relativeSkinPaths;
        if (loaded.Has<AnimationPlayerComponent>(id)) animated = id;
    });
    CHECK(ctx, skins == 2u && resolvedSkins == 2u && relativeSkinPaths == 2u);
    const auto* player = animated.IsValid() ? loaded.Get<AnimationPlayerComponent>(animated) : nullptr;
    CHECK(ctx, player && registry.animations.Get(player->clip) != nullptr);
    CHECK(ctx, player && player->playing && player->looping && std::abs(player->speed - 1.5f) < 1e-6f);

    engine::addons::animation::AnimationSystem animation(registry);
    animation.Update(loaded, 0.25f);
    const auto* skin = animated.IsValid() ? loaded.Get<SkinComponent>(animated) : nullptr;
    CHECK(ctx, skin && !skin->bonePalette.empty());
    fs::remove_all(root);
}

// A prefab saved with absolute model paths in an earlier session: reading it in a
// fresh registry makes the paths relative and loads skeleton/animation from the model.
void TestPrefabResolvesModelAnimationAssets(test::TestContext& ctx)
{
    namespace fs = std::filesystem;
    const fs::path root = fs::temp_directory_path() / "krom_prefab_model_paths";
    fs::remove_all(root);
    fs::create_directories(root / "Models");
    WriteRiggedTestModel(root / "Models" / "rig.gltf");

    const std::string absBase = PathToGenericUtf8(root / "Models" / "rig.gltf");
    const std::string json = std::string(R"({"type":"KromPrefab","version":2,"name":"Rig","rootIndex":0,"records":[)") +
        R"({"name":"Rig","parentIndex":-1,"animationAssetPath":")" + absBase + R"(#animation/0","playAnimation":true},)" +
        R"({"name":"Body","parentIndex":0,"meshAssetPath":")" + absBase + R"(#mesh/0","skeletonAssetPath":")" +
        absBase + R"(#skeleton/0","animationAssetPath":")" + absBase + R"(#animation/0","playAnimation":true}]})";

    // Without a pipeline nothing is resolved or rewritten (previous behaviour).
    {
        assets::AssetRegistry registry;
        engine::addons::prefab::PrefabAsset prefab;
        CHECK(ctx, engine::addons::prefab::DeserializePrefabFromJson(json, registry, prefab, nullptr));
        CHECK(ctx, prefab.records.size() == 2u && !prefab.records[1].skeleton.IsValid());
        CHECK(ctx, prefab.records.size() == 2u && prefab.records[1].skeletonAssetPath == absBase + "#skeleton/0");
    }

    assets::AssetRegistry registry;
    assets::AssetPipeline pipeline(registry, nullptr);
    pipeline.RegisterMeshImporter(std::make_unique<engine::addons::gltf::GltfImporter>());
    pipeline.SetAssetRoot(root);
    engine::addons::prefab::SetPrefabAssetPipeline(&pipeline);

    engine::addons::prefab::PrefabAsset prefab;
    std::string error;
    const bool ok = engine::addons::prefab::DeserializePrefabFromJson(json, registry, prefab, &error);
    engine::addons::prefab::SetPrefabAssetPipeline(nullptr);
    CHECK(ctx, ok);
    CHECK(ctx, prefab.records.size() == 2u);
    if (prefab.records.size() != 2u)
        return;
    const auto& body = prefab.records[1];
    CHECK(ctx, body.meshAssetPath == "Models/rig.gltf#mesh/0");
    CHECK(ctx, body.skeletonAssetPath == "Models/rig.gltf#skeleton/0");
    CHECK(ctx, body.animationAssetPath == "Models/rig.gltf#animation/0");
    CHECK(ctx, body.skeleton.IsValid() && body.animation.IsValid());
    CHECK(ctx, prefab.records[0].animation == body.animation);

    // Saving writes the portable paths.
    const std::string saved = engine::addons::prefab::SerializePrefabToJson(prefab);
    CHECK(ctx, saved.find(absBase) == std::string::npos);
    CHECK(ctx, saved.find("Models/rig.gltf#skeleton/0") != std::string::npos);

    // The instance gets the components the AnimationSystem needs.
    auto components = MakePrefabComponentRegistry();
    ecs::World world(components);
    const auto instance = engine::addons::prefab::InstantiatePrefab(world, prefab);
    CHECK(ctx, instance.IsValid() && instance.entities.size() == 2u);
    if (instance.entities.size() == 2u)
    {
        CHECK(ctx, world.Has<SkinComponent>(instance.entities[1]));
        CHECK(ctx, world.Has<AnimationPlayerComponent>(instance.entities[1]));
    }
    fs::remove_all(root);
}

} // namespace

static void TestPrefabBuildsStableRecordRecipe(test::TestContext& ctx)
{
    assets::AssetRegistry assets;
    PrefabBuildOptions options;
    options.name = "Dragon";

    const PrefabAsset prefab =
        BuildPrefabFromImportedBundle(MakePrefabBundle(), assets, options);

    CHECK(ctx, !prefab.Empty());
    CHECK_EQ(ctx, prefab.rootIndex, 0u);
    CHECK_EQ(ctx, prefab.records.size(), size_t{3});
    CHECK_EQ(ctx, prefab.FindEntity("Body"), 2);

    const PrefabEntityRecord& root = prefab.records[0];
    CHECK(ctx, root.animation.IsValid());
    CHECK(ctx, root.playAnimation);

    const PrefabEntityRecord& body = prefab.records[2];
    CHECK(ctx, body.mesh.IsValid());
    CHECK(ctx, body.material.IsValid());
    CHECK(ctx, body.skeleton.IsValid());
    CHECK(ctx, body.animation.IsValid());
    CHECK(ctx, body.addBounds);
    CHECK_EQ(ctx, body.parentIndex, 1);
}

static void TestPrefabInstantiateReturnsRootAndEntityList(test::TestContext& ctx)
{
    assets::AssetRegistry assets;
    PrefabBuildOptions buildOptions;
    buildOptions.name = "Dragon";
    const PrefabAsset prefab =
        BuildPrefabFromImportedBundle(MakePrefabBundle(), assets, buildOptions);

    auto componentRegistry = MakePrefabComponentRegistry();
    ecs::World world(componentRegistry);

    PrefabInstantiateOptions instantiateOptions;
    instantiateOptions.position = { 10.f, 0.f, 0.f };
    const PrefabInstance instance =
        InstantiatePrefab(world, prefab, instantiateOptions);

    CHECK(ctx, instance.IsValid());
    CHECK_EQ(ctx, instance.entities.size(), prefab.records.size());
    CHECK_EQ(ctx, instance.root, instance.entities[prefab.rootIndex]);
    CHECK_EQ(ctx, world.EntityCount(), prefab.records.size());

    const EntityID body = instance.entities[static_cast<size_t>(prefab.FindEntity("Body"))];
    CHECK(ctx, world.Has<MeshComponent>(body));
    CHECK(ctx, world.Has<MaterialComponent>(body));
    CHECK(ctx, world.Has<SkinComponent>(body));
    CHECK(ctx, world.Has<AnimationPlayerComponent>(body));
    CHECK(ctx, world.Has<BoundsComponent>(body));

    const auto* bodyParent = world.Get<ParentComponent>(body);
    CHECK(ctx, bodyParent != nullptr);
    CHECK(ctx, bodyParent && bodyParent->parent == instance.entities[1]);

    const auto* rootTransform = world.Get<TransformComponent>(instance.root);
    CHECK(ctx, rootTransform != nullptr);
    CHECK(ctx, rootTransform && rootTransform->localPosition.x == 10.f);
}

static void TestPrefabResetRestoresRuntimeState(test::TestContext& ctx)
{
    assets::AssetRegistry assets;
    PrefabBuildOptions buildOptions;
    buildOptions.name = "Dragon";
    const PrefabAsset prefab =
        BuildPrefabFromImportedBundle(MakePrefabBundle(), assets, buildOptions);

    auto componentRegistry = MakePrefabComponentRegistry();
    ecs::World world(componentRegistry);
    const PrefabInstance instance = InstantiatePrefab(world, prefab);

    const EntityID body = instance.entities[static_cast<size_t>(prefab.FindEntity("Body"))];
    auto* bodyTransform = world.Get<TransformComponent>(body);
    auto* player = world.Get<AnimationPlayerComponent>(body);
    CHECK(ctx, bodyTransform != nullptr);
    CHECK(ctx, player != nullptr);
    if (!bodyTransform || !player)
        return;

    bodyTransform->localPosition = { 99.f, 99.f, 99.f };
    player->currentTime = 0.5f;
    player->bindingsDirty = false;
    player->playing = false;

    PrefabInstantiateOptions resetOptions;
    resetOptions.position = { -2.f, 0.f, 0.f };
    ResetPrefabInstance(world, prefab, instance, resetOptions);

    CHECK(ctx, bodyTransform->localPosition.x == 1.f);
    CHECK(ctx, bodyTransform->localPosition.y == 2.f);
    CHECK(ctx, bodyTransform->localPosition.z == 3.f);
    CHECK(ctx, player->currentTime == 0.f);
    CHECK(ctx, player->bindingsDirty);
    CHECK(ctx, player->playing);

    const auto* rootTransform = world.Get<TransformComponent>(instance.root);
    CHECK(ctx, rootTransform != nullptr);
    CHECK(ctx, rootTransform && rootTransform->localPosition.x == -2.f);
}

static void TestPrefabDestroyRemovesInstanceEntities(test::TestContext& ctx)
{
    assets::AssetRegistry assets;
    PrefabBuildOptions buildOptions;
    buildOptions.name = "Dragon";
    const PrefabAsset prefab =
        BuildPrefabFromImportedBundle(MakePrefabBundle(), assets, buildOptions);

    auto componentRegistry = MakePrefabComponentRegistry();
    ecs::World world(componentRegistry);
    const PrefabInstance instance = InstantiatePrefab(world, prefab);

    CHECK(ctx, instance.IsValid());
    CHECK_EQ(ctx, world.EntityCount(), prefab.records.size());

    DestroyPrefabInstance(world, instance);

    CHECK_EQ(ctx, world.EntityCount(), size_t{0});
    CHECK(ctx, !world.IsAlive(instance.root));
    for (EntityID entity : instance.entities)
        CHECK(ctx, !world.IsAlive(entity));
}

static void TestPrefabBuildWithoutSyntheticRoot(test::TestContext& ctx)
{
    assets::AssetRegistry assets;
    PrefabBuildOptions buildOptions;
    buildOptions.name = "Dragon";
    buildOptions.createSyntheticRoot = false;
    const PrefabAsset prefab =
        BuildPrefabFromImportedBundle(MakePrefabBundle(), assets, buildOptions);

    CHECK_EQ(ctx, prefab.rootIndex, 0u);
    CHECK_EQ(ctx, prefab.records.size(), size_t{2});
    CHECK_EQ(ctx, prefab.records[0].name, std::string("Armature"));
    CHECK_EQ(ctx, prefab.records[0].parentIndex, -1);
    CHECK_EQ(ctx, prefab.records[1].parentIndex, 0);

    auto componentRegistry = MakePrefabComponentRegistry();
    ecs::World world(componentRegistry);
    const PrefabInstance instance = InstantiatePrefab(world, prefab);

    CHECK(ctx, instance.IsValid());
    CHECK_EQ(ctx, instance.root, instance.entities[0]);
    const auto* childParent = world.Get<ParentComponent>(instance.entities[1]);
    CHECK(ctx, childParent != nullptr);
    CHECK(ctx, childParent && childParent->parent == instance.root);
}

static void TestPrefabBuildsMeshOnlyBundle(test::TestContext& ctx)
{
    assets::AssetRegistry assets;
    PrefabBuildOptions buildOptions;
    buildOptions.name = "MeshOnly";
    const PrefabAsset prefab =
        BuildPrefabFromImportedBundle(MakeMeshOnlyBundle(), assets, buildOptions);

    CHECK_EQ(ctx, prefab.rootIndex, 0u);
    CHECK_EQ(ctx, prefab.records.size(), size_t{2});
    CHECK_EQ(ctx, prefab.records[0].name, std::string("MeshOnly"));
    CHECK_EQ(ctx, prefab.records[1].parentIndex, 0);
    CHECK(ctx, prefab.records[1].mesh.IsValid());
    CHECK(ctx, prefab.records[1].material.IsValid());
    CHECK(ctx, prefab.records[1].skeleton.IsValid());

    auto componentRegistry = MakePrefabComponentRegistry();
    ecs::World world(componentRegistry);
    const PrefabInstance instance = InstantiatePrefab(world, prefab);

    CHECK(ctx, instance.IsValid());
    CHECK_EQ(ctx, instance.entities.size(), size_t{2});
    CHECK(ctx, world.Has<MeshComponent>(instance.entities[1]));
    CHECK(ctx, world.Has<SkinComponent>(instance.entities[1]));
}

static void TestPrefabHandlesEmptyBundle(test::TestContext& ctx)
{
    assets::AssetRegistry assets;
    PrefabBuildOptions buildOptions;
    buildOptions.name = "Empty";
    const PrefabAsset prefab =
        BuildPrefabFromImportedBundle(assets::ImportedAssetBundle{}, assets, buildOptions);

    CHECK_EQ(ctx, prefab.rootIndex, 0u);
    CHECK_EQ(ctx, prefab.records.size(), size_t{1});
    CHECK_EQ(ctx, prefab.records[0].name, std::string("Empty"));
    CHECK(ctx, !prefab.records[0].mesh.IsValid());
    CHECK(ctx, !prefab.records[0].material.IsValid());
    CHECK(ctx, !prefab.records[0].skeleton.IsValid());
    CHECK(ctx, !prefab.records[0].animation.IsValid());

    auto componentRegistry = MakePrefabComponentRegistry();
    ecs::World world(componentRegistry);
    const PrefabInstance instance = InstantiatePrefab(world, prefab);

    CHECK(ctx, instance.IsValid());
    CHECK_EQ(ctx, instance.entities.size(), size_t{1});
    CHECK(ctx, world.Has<TransformComponent>(instance.root));
    CHECK(ctx, !world.Has<MeshComponent>(instance.root));
    CHECK(ctx, !world.Has<AnimationPlayerComponent>(instance.root));
}

static void TestPrefabJsonRoundTripPreservesPersistentRecipe(test::TestContext& ctx)
{
    assets::AssetRegistry assets;
    PrefabBuildOptions buildOptions;
    buildOptions.name = "Dragon";
    const PrefabAsset prefab =
        BuildPrefabFromImportedBundle(MakePrefabBundle(), assets, buildOptions);

    const std::string json = SerializePrefabToJson(prefab);

    PrefabAsset loaded;
    std::string error;
    CHECK(ctx, DeserializePrefabFromJson(json, assets, loaded, &error));
    CHECK_EQ(ctx, error, std::string{});
    CHECK_EQ(ctx, loaded.name, std::string("Dragon"));
    CHECK_EQ(ctx, loaded.rootIndex, 0u);
    CHECK_EQ(ctx, loaded.records.size(), prefab.records.size());

    const PrefabEntityRecord& body = loaded.records[static_cast<size_t>(loaded.FindEntity("Body"))];
    CHECK_EQ(ctx, body.meshAssetPath, std::string("prefab_test.gltf#mesh/0"));
    CHECK_EQ(ctx, body.materialAssetPath, std::string("prefab_test.gltf#material/0"));
    CHECK_EQ(ctx, body.skeletonAssetPath, std::string("prefab_test.gltf#skeleton/0"));
    CHECK_EQ(ctx, body.animationAssetPath, std::string("prefab_test.gltf#animation/0"));
    CHECK(ctx, body.mesh.IsValid());
    CHECK(ctx, body.material.IsValid());
    CHECK(ctx, body.skeleton.IsValid());
    CHECK(ctx, body.animation.IsValid());
}

// Regression for mask2.prefab: an editor gizmo material path (absolute, into
// the engine's editor/ folder) was written into prefab records and into the
// baseline copy stored on prefab instances. Both go through
// SerializePrefabToJson, which must only store project-relative material paths.
static void TestPrefabDropsNonPortableMaterialPaths(test::TestContext& ctx)
{
    PrefabAsset prefab;
    prefab.name = "mask2";
    prefab.records.resize(3);
    prefab.records[0].id = "root"; prefab.records[0].name = "mask2";

    PrefabEntityRecord& leaked = prefab.records[1];
    leaked.id = "teeth"; leaked.name = "ZAEHNE LOW"; leaked.parentIndex = 0;
    leaked.materialAssetPath = "F:/working/krom codex/editor/dpad_gelb.mat";
    leaked.materialOverrideSource = "{\"leakedSource\":1}";
    PrefabEntityRecord::SlotOverride keptSlot;
    keptSlot.submeshIndex = 0u; keptSlot.materialAssetPath = "Materials/Mask/Zaehne.mat";
    leaked.slotOverrides.push_back(keptSlot);
    PrefabEntityRecord::SlotOverride leakedSlot;
    leakedSlot.submeshIndex = 1u; leakedSlot.materialAssetPath = "/opt/krom/editor/dpad_rot.mat";
    leaked.slotOverrides.push_back(leakedSlot);

    PrefabEntityRecord& portable = prefab.records[2];
    portable.id = "horns"; portable.name = "HOERNER LOW"; portable.parentIndex = 0;
    portable.materialAssetPath = "Materials/dpad_gelb.mat";
    portable.materialOverrideSource = "{\"keptSource\":1}";

    const std::string json = SerializePrefabToJson(prefab);
    CHECK(ctx, json.find("F:/working") == std::string::npos);
    CHECK(ctx, json.find("/opt/krom") == std::string::npos);
    CHECK(ctx, json.find("leakedSource") == std::string::npos);

    assets::AssetRegistry assets;
    PrefabAsset loaded;
    std::string error;
    CHECK(ctx, DeserializePrefabFromJson(json, assets, loaded, &error));
    CHECK_EQ(ctx, loaded.records.size(), 3u);
    if (loaded.records.size() != 3u)
        return;

    const PrefabEntityRecord& teeth = loaded.records[static_cast<size_t>(loaded.FindEntity("ZAEHNE LOW"))];
    CHECK(ctx, teeth.materialAssetPath.empty());
    CHECK(ctx, teeth.materialOverrideSource.empty());
    bool keptSlotFound = false;
    for (const PrefabEntityRecord::SlotOverride& slot : teeth.slotOverrides)
    {
        if (slot.submeshIndex == 0u)
        {
            keptSlotFound = true;
            CHECK_EQ(ctx, slot.materialAssetPath, std::string("Materials/Mask/Zaehne.mat"));
        }
        else
            CHECK(ctx, slot.materialAssetPath.empty());
    }
    CHECK(ctx, keptSlotFound);

    const PrefabEntityRecord& horns = loaded.records[static_cast<size_t>(loaded.FindEntity("HOERNER LOW"))];
    CHECK_EQ(ctx, horns.materialAssetPath, std::string("Materials/dpad_gelb.mat"));
    CHECK_EQ(ctx, horns.materialOverrideSource, std::string("{\"keptSource\":1}"));
}

static void TestPrefabBuildFromWorldEntityCapturesCollisionShape(test::TestContext& ctx)
{
    auto componentRegistry = MakePrefabComponentRegistry();
    ecs::World world(componentRegistry);

    const EntityID root = world.CreateEntity();
    world.Add<NameComponent>(root, NameComponent("Crate"));
    world.Add<ActiveComponent>(root, ActiveComponent{true});
    auto& transform = world.Add<TransformComponent>(root);
    transform.localPosition = { 4.f, 5.f, 6.f };

    BoundsComponent bounds{};
    bounds.centerLocal = { 0.f, 1.f, 0.f };
    bounds.extentsLocal = { 2.f, 3.f, 4.f };
    world.Add<BoundsComponent>(root, bounds);

    OBBComponent obb{};
    obb.centerOffset = { 1.f, 0.f, 0.f };
    obb.halfExtents = { 0.25f, 0.5f, 0.75f };
    obb.showInEditor = true;
    world.Add<OBBComponent>(root, obb);

    const PrefabAsset prefab = BuildPrefabFromWorldEntity(world, root);
    CHECK_EQ(ctx, prefab.name, std::string("Crate"));
    CHECK_EQ(ctx, prefab.records.size(), size_t{1});

    const PrefabEntityRecord& record = prefab.records[0];
    CHECK(ctx, record.addBounds);
    CHECK_EQ(ctx, record.boundsCenterLocal.y, 1.f);
    CHECK_EQ(ctx, record.boundsExtentsLocal.z, 4.f);
    CHECK(ctx, record.addObb);
    CHECK_EQ(ctx, record.obbCenterOffset.x, 1.f);
    CHECK_EQ(ctx, record.obbHalfExtents.z, 0.75f);
    CHECK(ctx, record.showObbInEditor);

    ecs::World dst(componentRegistry);
    const PrefabInstance instance = InstantiatePrefab(dst, prefab);
    CHECK(ctx, instance.IsValid());
    CHECK(ctx, dst.Has<BoundsComponent>(instance.root));
    CHECK(ctx, dst.Has<OBBComponent>(instance.root));
}

static void TestPrefabBuildFromWorldEntityCapturesTexturePath(test::TestContext& ctx)
{
    auto componentRegistry = MakePrefabComponentRegistry();
    ecs::World world(componentRegistry);

    const EntityID root = world.CreateEntity();
    world.Add<NameComponent>(root, NameComponent("TexturedCube"));
    world.Add<TransformComponent>(root);
    MaterialComponent material{};
    material.baseColorTexturePath = "Textures/crate_albedo.png";
    world.Add<MaterialComponent>(root, material);

    const PrefabAsset prefab = BuildPrefabFromWorldEntity(world, root);
    CHECK_EQ(ctx, prefab.records.size(), size_t{1});
    CHECK_EQ(ctx, prefab.records[0].baseColorTexturePath, std::string("Textures/crate_albedo.png"));

    assets::AssetRegistry assets;
    const std::string json = SerializePrefabToJson(prefab);
    PrefabAsset loaded;
    std::string error;
    CHECK(ctx, DeserializePrefabFromJson(json, assets, loaded, &error));
    CHECK_EQ(ctx, loaded.records[0].baseColorTexturePath, std::string("Textures/crate_albedo.png"));
}

static void TestPrefabPreservesScriptsAndFields(test::TestContext& ctx)
{
    auto componentRegistry = MakePrefabComponentRegistry();
    ecs::World world(componentRegistry);

    script::ScriptRegistry scripts;
    scripts.Register<PrefabTestScript>("PrefabTestScript");
    scripts.RegisterField<PrefabTestScript>(
        "PrefabTestScript", "speed", script::ScriptFieldType::Float, &PrefabTestScript::speed);
    scripts.RegisterField<PrefabTestScript>(
        "PrefabTestScript", "prefabPath", script::ScriptFieldType::Prefab, &PrefabTestScript::prefabPath);

    const EntityID root = world.CreateEntity();
    world.Add<NameComponent>(root, NameComponent("Scripted"));
    world.Add<TransformComponent>(root);

    script::ScriptList scriptList;
    CHECK(ctx, scriptList.Add("PrefabTestScript", root, scripts));
    script::ScriptInstance& srcInst = scriptList.Instances_Mutable().back();
    srcInst.instanceName = "Spawner";
    auto* srcScript = static_cast<PrefabTestScript*>(srcInst.script.get());
    srcScript->speed = 12.5f;
    srcScript->prefabPath = "Prefabs/Bullet.prefab";
    world.Add<script::ScriptList>(root, std::move(scriptList));

    const PrefabAsset prefab = BuildPrefabFromWorldEntity(world, root, {}, nullptr, &scripts);
    CHECK_EQ(ctx, prefab.records.size(), size_t{1});
    CHECK_EQ(ctx, prefab.records[0].scripts.size(), size_t{1});
    CHECK_EQ(ctx, prefab.records[0].scripts[0].className, std::string("PrefabTestScript"));
    CHECK_EQ(ctx, prefab.records[0].scripts[0].instanceName, std::string("Spawner"));

    assets::AssetRegistry assets;
    PrefabAsset loaded;
    std::string error;
    CHECK(ctx, DeserializePrefabFromJson(SerializePrefabToJson(prefab), assets, loaded, &error));
    CHECK_EQ(ctx, loaded.records[0].scripts.size(), size_t{1});

    ecs::World dst(componentRegistry);
    PrefabInstantiateOptions options{};
    options.scriptRegistry = &scripts;
    const PrefabInstance instance = InstantiatePrefab(dst, loaded, options);
    CHECK(ctx, instance.IsValid());
    const auto* dstScripts = dst.Get<script::ScriptList>(instance.root);
    CHECK(ctx, dstScripts != nullptr);
    if (!dstScripts)
        return;

    CHECK_EQ(ctx, dstScripts->Count(), size_t{1});
    const script::ScriptInstance& dstInst = dstScripts->Instances()[0];
    CHECK_EQ(ctx, dstInst.className, std::string("PrefabTestScript"));
    CHECK_EQ(ctx, dstInst.instanceName, std::string("Spawner"));
    CHECK(ctx, dstInst.script != nullptr);
    const auto* dstScript = static_cast<const PrefabTestScript*>(dstInst.script.get());
    CHECK_EQ(ctx, dstScript->speed, 12.5f);
    CHECK_EQ(ctx, dstScript->prefabPath, std::string("Prefabs/Bullet.prefab"));
}

static void TestPrefabBuildFromWorldEntityCapturesMeshSlotMaterial(test::TestContext& ctx)
{
    auto componentRegistry = MakePrefabComponentRegistry();
    ecs::World world(componentRegistry);
    assets::AssetRegistry assets;

    auto material = std::make_unique<assets::MaterialAsset>();
    material->baseColorTexture.path = "Textures/imported_albedo.png";
    const MaterialHandle materialHandle = assets.GetOrAddMaterial("Models/imported.glb#material/0", std::move(material));

    auto mesh = std::make_unique<assets::MeshAsset>();
    assets::SubMeshData submesh{};
    submesh.materialIndex = 0u;
    mesh->submeshes.push_back(std::move(submesh));
    mesh->materialHandles.push_back(materialHandle);
    const MeshHandle meshHandle = assets.GetOrAddMesh("Models/imported.glb#mesh/0", std::move(mesh));

    const EntityID root = world.CreateEntity();
    world.Add<NameComponent>(root, NameComponent("ImportedCube"));
    world.Add<MeshComponent>(root, MeshComponent{ meshHandle });

    const PrefabAsset prefab = BuildPrefabFromWorldEntity(world, root, {}, &assets);
    CHECK_EQ(ctx, prefab.records.size(), size_t{1});
    CHECK_EQ(ctx, prefab.records[0].material, materialHandle);
    CHECK_EQ(ctx, prefab.records[0].materialAssetPath, std::string("Models/imported.glb#material/0"));
    CHECK_EQ(ctx, prefab.records[0].baseColorTexturePath, std::string("Textures/imported_albedo.png"));
}

// A live MaterialComponent holds runtime material handles. Their index must not
// be resolved through the AssetRegistry, where it can name an unrelated asset
// (seen with editor gizmo materials like editor/dpad_rot.mat).
static void TestPrefabBuildFromWorldEntityIgnoresRuntimeMaterialHandles(test::TestContext& ctx)
{
    auto componentRegistry = MakePrefabComponentRegistry();
    ecs::World world(componentRegistry);
    assets::AssetRegistry assets;

    const MaterialHandle unrelated = assets.GetOrAddMaterial(
        "C:/engine/editor/dpad_rot.mat", std::make_unique<assets::MaterialAsset>());
    const MaterialHandle meshMaterial = assets.GetOrAddMaterial(
        "Models/mask.glb#material/0", std::make_unique<assets::MaterialAsset>());

    auto mesh = std::make_unique<assets::MeshAsset>();
    assets::SubMeshData submesh{};
    submesh.materialIndex = 0u;
    mesh->submeshes.push_back(std::move(submesh));
    mesh->materialHandles.push_back(meshMaterial);
    const MeshHandle meshHandle = assets.GetOrAddMesh("Models/mask.glb#mesh/0", std::move(mesh));

    const EntityID root = world.CreateEntity();
    world.Add<NameComponent>(root, NameComponent("Face"));
    world.Add<MeshComponent>(root, MeshComponent{ meshHandle });
    MaterialComponent material{};
    material.material = unrelated; // runtime handle whose index collides
    MaterialComponent::SlotOverride slot{};
    slot.submeshIndex = 0u;
    slot.material = unrelated;
    material.slotOverrides.push_back(slot);
    world.Add<MaterialComponent>(root, material);

    const PrefabAsset prefab = BuildPrefabFromWorldEntity(world, root, {}, &assets);
    CHECK_EQ(ctx, prefab.records.size(), size_t{1});
    CHECK_EQ(ctx, prefab.records[0].materialAssetPath, std::string("Models/mask.glb#material/0"));
    CHECK_EQ(ctx, prefab.records[0].slotOverrides.size(), size_t{1});
    CHECK(ctx, prefab.records[0].slotOverrides[0].materialAssetPath.empty());

    // The editor also overwrites mesh->materialHandles with runtime handles.
    auto editedMesh = std::make_unique<assets::MeshAsset>();
    assets::SubMeshData editedSubmesh{};
    editedSubmesh.materialIndex = 0u;
    editedMesh->submeshes.push_back(std::move(editedSubmesh));
    editedMesh->materialHandles.push_back(unrelated);
    const MeshHandle editedMeshHandle = assets.GetOrAddMesh("Models/mask.glb#mesh/1", std::move(editedMesh));
    const EntityID edited = world.CreateEntity();
    world.Add<NameComponent>(edited, NameComponent("Teeth"));
    world.Add<MeshComponent>(edited, MeshComponent{ editedMeshHandle });
    world.Add<MaterialComponent>(edited, material);

    const PrefabAsset editedPrefab = BuildPrefabFromWorldEntity(world, edited, {}, &assets);
    CHECK_EQ(ctx, editedPrefab.records.size(), size_t{1});
    CHECK(ctx, editedPrefab.records[0].materialAssetPath.empty());
}

static void TestPrefabPreservesPhysicsComponents(test::TestContext& ctx)
{
    auto componentRegistry = MakePrefabComponentRegistry();
    ecs::World world(componentRegistry);

    const EntityID root = world.CreateEntity();
    world.Add<NameComponent>(root, NameComponent("Crate"));
    world.Add<TransformComponent>(root);
    RigidBodyComponent rb{};
    rb.type = RigidBodyType::Kinematic;
    rb.mass = 3.f;
    rb.lockRotationY = true;
    world.Add<RigidBodyComponent>(root, rb);
    SphereColliderComponent sphere{};
    sphere.radius = 0.75f;
    world.Add<SphereColliderComponent>(root, sphere);

    const EntityID child = world.CreateEntity();
    world.Add<NameComponent>(child, NameComponent("Collider"));
    world.Add<TransformComponent>(child);
    world.Add<ParentComponent>(child, ParentComponent{root});
    world.Add<ChildrenComponent>(root).Add(child);
    CapsuleColliderComponent capsule{};
    capsule.axis = CapsuleAxis::X;
    capsule.height = 2.5f;
    world.Add<CapsuleColliderComponent>(child, capsule);

    const PrefabAsset prefab = BuildPrefabFromWorldEntity(world, root);
    CHECK_EQ(ctx, prefab.records.size(), size_t{2});

    assets::AssetRegistry assets;
    PrefabAsset loaded;
    std::string error;
    CHECK(ctx, DeserializePrefabFromJson(SerializePrefabToJson(prefab), assets, loaded, &error));

    ecs::World world2(componentRegistry);
    const PrefabInstance instance = InstantiatePrefab(world2, loaded);
    CHECK(ctx, instance.IsValid());
    CHECK_EQ(ctx, instance.entities.size(), size_t{2});
    if (instance.entities.size() != 2u)
        return;

    const auto* rb2 = world2.Get<RigidBodyComponent>(instance.entities[0]);
    CHECK(ctx, rb2 != nullptr);
    if (rb2)
    {
        CHECK(ctx, rb2->type == RigidBodyType::Kinematic);
        CHECK(ctx, rb2->mass == 3.f);
        CHECK(ctx, rb2->lockRotationY && !rb2->lockRotationX);
    }
    const auto* sphere2 = world2.Get<SphereColliderComponent>(instance.entities[0]);
    CHECK(ctx, sphere2 && sphere2->radius == 0.75f);
    const auto* capsule2 = world2.Get<CapsuleColliderComponent>(instance.entities[1]);
    CHECK(ctx, capsule2 && capsule2->axis == CapsuleAxis::X && capsule2->height == 2.5f);
    CHECK(ctx, world2.Get<RigidBodyComponent>(instance.entities[1]) == nullptr);
}


namespace {
class ChildLifecycleScript final : public script::ComponentScript
{
public:
    float speed = 3.f;
    int starts = 0;
    EntityID lastOwner = NULL_ENTITY;
    void OnStart(script::IScriptContext& ctx) override { ++starts; lastOwner = ctx.GetEntity(); }
    void OnUpdate(script::IScriptContext& ctx) override { lastOwner = ctx.GetEntity(); }
};

PrefabAsset MakeLinkedRecipe()
{
    PrefabAsset prefab;
    prefab.name = "Linked";
    prefab.records.resize(3);
    prefab.records[0].id = "root"; prefab.records[0].name = "Root";
    prefab.records[1].id = "child"; prefab.records[1].name = "Child";
    prefab.records[1].parentIndex = 0;
    prefab.records[1].localPosition = {1.f, 2.f, 3.f};
    prefab.records[1].rigidBody.emplace();
    prefab.records[2].id = "sibling"; prefab.records[2].name = "Sibling";
    prefab.records[2].parentIndex = 0;
    return prefab;
}
}

static void TestPrefabSyncPreservesIdentityAndLocalProperties(test::TestContext& ctx)
{
    auto meta = MakePrefabComponentRegistry(); ecs::World world(meta); assets::AssetRegistry registry;
    auto prefab = MakeLinkedRecipe();
    auto first = InstantiatePrefab(world, prefab); auto second = InstantiatePrefab(world, prefab);
    LinkPrefabInstance(world, prefab, first, "test.prefab");
    LinkPrefabInstance(world, prefab, second, "test.prefab");
    world.Get<RigidBodyComponent>(first.entities[1])->mass = 77.f;
    world.Get<TransformComponent>(first.root)->localPosition = {20.f, 0.f, 0.f};
    const auto extra = world.CreateEntity();
    world.Add<TransformComponent>(extra);
    world.Add<ParentComponent>(extra, ParentComponent{first.entities[1]});
    world.Add<ChildrenComponent>(first.entities[1]).Add(extra);
    prefab.records[1].name = "Renamed";
    prefab.records[1].localPosition.y = 8.f;
    prefab.records[1].rigidBody->mass = 4.f;
    prefab.records[1].rigidBody->friction = 0.9f;
    CHECK(ctx, SynchronizePrefabInstance(world, first.root, prefab, registry));
    CHECK(ctx, SynchronizePrefabInstance(world, second.root, prefab, registry));
    CHECK(ctx, world.IsAlive(first.entities[1]));
    CHECK_EQ(ctx, world.Get<NameComponent>(first.entities[1])->name, std::string("Renamed"));
    CHECK_EQ(ctx, world.Get<TransformComponent>(first.entities[1])->localPosition.y, 8.f);
    CHECK_EQ(ctx, world.Get<RigidBodyComponent>(first.entities[1])->mass, 77.f);
    CHECK_EQ(ctx, world.Get<RigidBodyComponent>(first.entities[1])->friction, 0.9f);
    CHECK_EQ(ctx, world.Get<RigidBodyComponent>(second.entities[1])->mass, 4.f);
    CHECK_EQ(ctx, world.Get<TransformComponent>(first.root)->localPosition.x, 20.f);
    CHECK(ctx, world.IsAlive(extra));
    CHECK_EQ(ctx, world.Get<ParentComponent>(extra)->parent, first.entities[1]);
    CHECK_EQ(ctx, world.EntityCount(), size_t{7});
    // A source temporarily matching a local override must not clear that override.
    prefab.records[1].rigidBody->mass = 77.f;
    CHECK(ctx, SynchronizePrefabInstance(world, first.root, prefab, registry));
    prefab.records[1].rigidBody->mass = 88.f;
    CHECK(ctx, SynchronizePrefabInstance(world, first.root, prefab, registry));
    CHECK_EQ(ctx, world.Get<RigidBodyComponent>(first.entities[1])->mass, 77.f);
}

static void TestPrefabChildScriptsSurviveSynchronization(test::TestContext& ctx)
{
    auto meta = MakePrefabComponentRegistry(); ecs::World world(meta); assets::AssetRegistry registry;
    script::ScriptRegistry scripts;
    scripts.Register<ChildLifecycleScript>("ChildLifecycle");
    scripts.RegisterField<ChildLifecycleScript>("ChildLifecycle", "speed", script::ScriptFieldType::Float, &ChildLifecycleScript::speed);
    auto prefab = MakeLinkedRecipe();
    PrefabEntityRecord::ScriptRecord record; record.className = "ChildLifecycle";
    record.instanceName = "Mover"; record.fieldValues["speed"].floatValue = 3.f;
    prefab.records[1].scripts.push_back(record);
    PrefabInstantiateOptions options; options.scriptRegistry = &scripts; options.prefabAssetPath = "child.prefab";
    const auto first = InstantiatePrefab(world, prefab, options);
    const auto second = InstantiatePrefab(world, prefab, options);
    auto* a = static_cast<ChildLifecycleScript*>(world.Get<script::ScriptList>(first.entities[1])->Instances()[0].script.get());
    auto* b = static_cast<ChildLifecycleScript*>(world.Get<script::ScriptList>(second.entities[1])->Instances()[0].script.get());
    world.Get<script::ScriptList>(first.entities[1])->Update(0.1f, first.entities[1]);
    world.Get<script::ScriptList>(second.entities[1])->Update(0.1f, second.entities[1]);
    b->speed = 9.f;
    CHECK(ctx, a != b);
    CHECK_EQ(ctx, a->GetEntity(), first.entities[1]);
    CHECK_EQ(ctx, b->GetEntity(), second.entities[1]);
    CHECK_EQ(ctx, a->lastOwner, first.entities[1]);
    CHECK(ctx, !world.Has<script::ScriptList>(first.root));
    CHECK(ctx, !world.Has<script::ScriptList>(first.entities[2]));
    prefab.records[1].scripts[0].fieldValues["speed"].floatValue = 6.f;
    CHECK(ctx, SynchronizePrefabInstance(world, first.root, prefab, registry, &scripts));
    CHECK(ctx, SynchronizePrefabInstance(world, second.root, prefab, registry, &scripts));
    CHECK(ctx, world.Get<script::ScriptList>(first.entities[1])->Instances()[0].script.get() == a);
    CHECK_EQ(ctx, a->speed, 6.f); CHECK_EQ(ctx, b->speed, 9.f);
    CHECK_EQ(ctx, a->starts, 1); CHECK_EQ(ctx, a->GetEntity(), first.entities[1]);
    world.Get<script::ScriptList>(first.entities[1])->Update(0.1f, first.entities[1]);
    CHECK_EQ(ctx, a->starts, 1);
    // Two instances of the same script class/name must remain distinct.
    prefab.records[1].scripts.push_back(prefab.records[1].scripts.front());
    prefab.records[1].scripts[1].fieldValues["speed"].floatValue = 10.f;
    CHECK(ctx, SynchronizePrefabInstance(world, first.root, prefab, registry, &scripts));
    CHECK_EQ(ctx, world.Get<script::ScriptList>(first.entities[1])->Count(), size_t{2});
    auto* added = static_cast<ChildLifecycleScript*>(world.Get<script::ScriptList>(first.entities[1])->Instances()[1].script.get());
    CHECK_EQ(ctx, added->speed, 10.f);
    world.Get<script::ScriptList>(first.entities[1])->Update(0.1f, first.entities[1]);
    CHECK_EQ(ctx, added->starts, 1); CHECK_EQ(ctx, added->lastOwner, first.entities[1]);
    prefab.records[1].scripts[1].fieldValues["speed"].floatValue = 12.f;
    CHECK(ctx, SynchronizePrefabInstance(world, first.root, prefab, registry, &scripts));
    CHECK_EQ(ctx, added->speed, 12.f); CHECK_EQ(ctx, a->speed, 6.f);
    prefab.records[1].scripts.pop_back();
    CHECK(ctx, SynchronizePrefabInstance(world, first.root, prefab, registry, &scripts));
    CHECK_EQ(ctx, world.Get<script::ScriptList>(first.entities[1])->Count(), size_t{1});
    CHECK(ctx, world.Get<script::ScriptList>(first.entities[1])->Instances()[0].script.get() == a);
}

static void TestPrefabSyncAddsRemovesAndReparents(test::TestContext& ctx)
{
    auto meta = MakePrefabComponentRegistry(); ecs::World world(meta); assets::AssetRegistry registry;
    auto prefab = MakeLinkedRecipe(); const auto instance = InstantiatePrefab(world, prefab);
    LinkPrefabInstance(world, prefab, instance, "hierarchy.prefab");
    const auto extra = world.CreateEntity(); world.Add<TransformComponent>(extra);
    world.Add<ParentComponent>(extra, ParentComponent{instance.entities[1]});
    world.Add<ChildrenComponent>(instance.entities[1]).Add(extra);
    prefab.records[2].parentIndex = 1;
    PrefabEntityRecord added; added.id = "new"; added.name = "Added"; added.parentIndex = 2;
    prefab.records.push_back(added);
    CHECK(ctx, SynchronizePrefabInstance(world, instance.root, prefab, registry));
    CHECK_EQ(ctx, world.Get<ParentComponent>(instance.entities[2])->parent, instance.entities[1]);
    CHECK_EQ(ctx, world.EntityCount(), size_t{5});
    // Reorder existing records while removing Child. Identity must not depend on index/name.
    const auto sibling = prefab.records[2];
    prefab.records = {prefab.records[0], prefab.records[3], sibling};
    prefab.records[1].parentIndex = 2; prefab.records[2].parentIndex = 0;
    CHECK(ctx, SynchronizePrefabInstance(world, instance.root, prefab, registry));
    CHECK(ctx, !world.IsAlive(instance.entities[1]));
    CHECK(ctx, world.IsAlive(instance.entities[2]));
    CHECK_EQ(ctx, world.Get<ParentComponent>(instance.entities[2])->parent, instance.root);
    CHECK(ctx, world.IsAlive(extra)); CHECK_EQ(ctx, world.Get<ParentComponent>(extra)->parent, instance.root);
    CHECK_EQ(ctx, world.EntityCount(), size_t{4});
}

static void TestPrefabSyncPreservesLocalDeletion(test::TestContext& ctx)
{
    auto meta = MakePrefabComponentRegistry(); ecs::World world(meta); assets::AssetRegistry registry;
    auto prefab = MakeLinkedRecipe(); const auto instance = InstantiatePrefab(world, prefab);
    LinkPrefabInstance(world, prefab, instance, "deleted.prefab");
    world.Get<ChildrenComponent>(instance.root)->Remove(instance.entities[1]);
    world.DestroyEntity(instance.entities[1]);
    prefab.records[1].name = "SourceRename";
    CHECK(ctx, SynchronizePrefabInstance(world, instance.root, prefab, registry));
    CHECK_EQ(ctx, world.EntityCount(), size_t{2});
    CHECK(ctx, SynchronizePrefabInstance(world, instance.root, prefab, registry));
    CHECK_EQ(ctx, world.EntityCount(), size_t{2});
}

static void TestPrefabLinksAndOverridesSurviveSceneRoundTrip(test::TestContext& ctx)
{
    auto meta = MakePrefabComponentRegistry(); ecs::World world(meta); assets::AssetRegistry registry;
    auto prefab = MakeLinkedRecipe(); const auto instance = InstantiatePrefab(world, prefab);
    LinkPrefabInstance(world, prefab, instance, "roundtrip.prefab");
    world.Get<RigidBodyComponent>(instance.entities[1])->mass = 55.f;
    serialization::SceneSerializer writer(world); writer.RegisterDefaultHandlers();
    RegisterPrefabSerializationHandlers(writer); physics::RegisterPhysicsSerializationHandlers(writer);
    ecs::World loaded(meta); serialization::SceneDeserializer reader(loaded); reader.RegisterDefaultHandlers();
    RegisterPrefabDeserializationHandlers(reader); physics::RegisterPhysicsDeserializationHandlers(reader);
    const auto result = reader.DeserializeFromJson(writer.SerializeToJson());
    CHECK(ctx, result.success);
    prefab.records[1].rigidBody->mass = 8.f;
    prefab.records[1].rigidBody->friction = 0.85f;
    const auto root = result.entityRemap.at(instance.root.value);
    const auto child = result.entityRemap.at(instance.entities[1].value);
    CHECK(ctx, SynchronizePrefabInstance(loaded, root, prefab, registry));
    CHECK_EQ(ctx, loaded.Get<RigidBodyComponent>(child)->mass, 55.f);
    CHECK_EQ(ctx, loaded.Get<RigidBodyComponent>(child)->friction, 0.85f);
    CHECK(ctx, loaded.IsAlive(child));
}

static void TestPrefabRootTransformAndPersistentComponents(test::TestContext& ctx)
{
    auto meta = MakePrefabComponentRegistry(); ecs::World world(meta); assets::AssetRegistry registry;
    auto prefab = MakeLinkedRecipe(); prefab.records[0].localScale = {2.f, 3.f, 4.f};
    prefab.records[0].localPosition = {5.f, 0.f, 0.f};
    prefab.records[1].camera.emplace(); prefab.records[2].light.emplace();
    const auto instance = InstantiatePrefab(world, prefab);
    CHECK_EQ(ctx, world.Get<TransformComponent>(instance.root)->localScale.y, 3.f);
    CHECK_EQ(ctx, world.Get<TransformComponent>(instance.root)->localPosition.x, 5.f);
    LinkPrefabInstance(world, prefab, instance, "transform.prefab", false);
    prefab.records[0].localScale = {4.f, 5.f, 6.f};
    prefab.records[0].localPosition.x = 10.f;
    prefab.records[1].camera->fovYDeg = 90.f;
    prefab.records[2].light->intensity = 8.f;
    CHECK(ctx, SynchronizePrefabInstance(world, instance.root, prefab, registry));
    CHECK_EQ(ctx, world.Get<TransformComponent>(instance.root)->localScale.y, 5.f);
    CHECK_EQ(ctx, world.Get<TransformComponent>(instance.root)->localPosition.x, 10.f);
    CHECK_EQ(ctx, world.Get<CameraComponent>(instance.entities[1])->fovYDeg, 90.f);
    CHECK_EQ(ctx, world.Get<LightComponent>(instance.entities[2])->intensity, 8.f);
    CHECK_EQ(ctx, world.Get<LightComponent>(instance.entities[2])->layerMask, uint32_t{0xFFFFFFFFu});
    PrefabAsset loaded; CHECK(ctx, DeserializePrefabFromJson(SerializePrefabToJson(prefab), registry, loaded));
    CHECK(ctx, loaded.records[1].camera.has_value()); CHECK(ctx, loaded.records[2].light.has_value());
}

static void TestNestedPrefabKeepsIndependentLink(test::TestContext& ctx)
{
    auto meta = MakePrefabComponentRegistry(); ecs::World source(meta); assets::AssetRegistry registry;
    const auto outer = source.CreateEntity(); source.Add<NameComponent>(outer, NameComponent{"Outer"}); source.Add<TransformComponent>(outer);
    auto inner = MakeLinkedRecipe(); PrefabInstantiateOptions opts; opts.prefabAssetPath = "inner.prefab";
    const auto child = InstantiatePrefab(source, inner, opts);
    source.Add<ParentComponent>(child.root, ParentComponent{outer}); source.Add<ChildrenComponent>(outer).Add(child.root);
    auto outerPrefab = BuildPrefabFromWorldEntity(source, outer, {}, &registry);
    PrefabAsset loaded; CHECK(ctx, DeserializePrefabFromJson(SerializePrefabToJson(outerPrefab), registry, loaded));
    ecs::World scene(meta); const auto first = InstantiatePrefab(scene, loaded); const auto second = InstantiatePrefab(scene, loaded);
    LinkPrefabInstance(scene, loaded, first, "outer.prefab"); LinkPrefabInstance(scene, loaded, second, "outer.prefab");
    CHECK(ctx, scene.Has<PrefabInstanceComponent>(first.entities[1]));
    CHECK(ctx, scene.Has<PrefabInstanceComponent>(second.entities[1]));
    inner.records[1].name = "UpdatedInner";
    CHECK(ctx, SynchronizePrefabInstance(scene, first.entities[1], inner, registry));
    CHECK_EQ(ctx, scene.Get<NameComponent>(first.entities[2])->name, std::string("UpdatedInner"));
    CHECK_EQ(ctx, scene.Get<NameComponent>(second.entities[2])->name, std::string("Child"));
    CHECK(ctx, SynchronizePrefabInstance(scene, second.entities[1], inner, registry));
    CHECK_EQ(ctx, scene.Get<NameComponent>(second.entities[2])->name, std::string("UpdatedInner"));
    CHECK_EQ(ctx, scene.EntityCount(), size_t{8});
    ecs::World edit(meta); const auto editedOuter = InstantiatePrefab(edit, loaded);
    PrefabEntityRecord added; added.id = "new-inner-node"; added.name = "NewInner"; added.parentIndex = 0;
    inner.records.push_back(added);
    CHECK(ctx, SynchronizePrefabInstance(scene, first.entities[1], inner, registry));
    CHECK(ctx, SynchronizePrefabInstance(scene, second.entities[1], inner, registry));
    CHECK(ctx, SynchronizePrefabInstance(edit, editedOuter.entities[1], inner, registry));
    const auto updatedOuter = BuildPrefabFromWorldEntity(edit, editedOuter.root, {}, &registry);
    CHECK(ctx, SynchronizePrefabInstance(scene, first.root, updatedOuter, registry));
    CHECK(ctx, SynchronizePrefabInstance(scene, second.root, updatedOuter, registry));
    CHECK_EQ(ctx, scene.EntityCount(), size_t{10});
}

static void TestPrefabModeKeepsSingleRoot(test::TestContext& ctx)
{
    namespace editor = engine::renderer::addons::editor;
    auto meta = MakePrefabComponentRegistry(); ecs::World world(meta); assets::AssetRegistry registry;
    renderer::MaterialSystem materials; editor::EditorState state;
    editor::EditorFrameContext frame{world, materials, registry, state};
    const auto path = std::filesystem::absolute("out/prefab-check/test-single-root.prefab");
    auto prefab = MakeLinkedRecipe(); CHECK(ctx, SavePrefabToFile(prefab, path));
    CHECK(ctx, editor::EnterPrefabMode(frame, path));
    const EntityID root = editor::PrefabModeRootEntity(frame);
    CHECK(ctx, root.IsValid());

    // Command path: the new entity is parented in the same undo step.
    const EntityID created = editor::CreateEmptyEntity(frame, "Added");
    CHECK(ctx, created.IsValid());
    const auto* parent = world.Get<ParentComponent>(created);
    CHECK(ctx, parent && parent->parent == root);
    CHECK(ctx, editor::PrefabModeRootEntity(frame) == root);

    // Raw path (e.g. asset drop): the per-frame call fixes it and keeps the world pose.
    const EntityID raw = world.CreateEntity();
    world.Add<NameComponent>(raw, NameComponent{"Dropped"});
    TransformComponent transform; transform.localPosition = {3.f, 0.f, 0.f};
    world.Add<TransformComponent>(raw, transform);
    world.Add<WorldTransformComponent>(raw);
    CHECK(ctx, editor::EnforcePrefabModeSingleRoot(frame));
    parent = world.Get<ParentComponent>(raw);
    CHECK(ctx, parent && parent->parent == root);
    CHECK(ctx, editor::PrefabModeRootEntity(frame) == root);
    CHECK(ctx, !editor::EnforcePrefabModeSingleRoot(frame));

    CHECK(ctx, editor::ExitPrefabMode(frame));
    std::filesystem::remove(path);
}

static void TestPrefabModeRebindsRenamedMesh(test::TestContext& ctx)
{
    namespace editor = engine::renderer::addons::editor;
    namespace fs = std::filesystem;
    const auto root = fs::absolute("out/prefab-check/mesh-rebind");
    fs::create_directories(root / "Models");
    const auto model = root / "Models" / "renamed.gltf";
    WriteRiggedTestModel(model);

    auto meta = MakePrefabComponentRegistry();
    ecs::World world(meta);
    assets::AssetRegistry registry;
    assets::AssetPipeline pipeline(registry);
    pipeline.SetAssetRoot(root);
    pipeline.RegisterMeshImporter(std::make_unique<engine::addons::gltf::GltfImporter>());
    renderer::MaterialSystem materials;
    editor::EditorState state;
    editor::EditorFrameContext frame{world, materials, registry, state};
    frame.assetPipeline = &pipeline;

    auto prefab = MakeLinkedRecipe();
    const auto path = root / "rebind.prefab";
    const std::string oldMeshPath = "Models/original.gltf#mesh/0";
    const std::string newMeshPath = "Models/renamed.gltf#mesh/0";
    prefab.records.front().meshAssetPath = oldMeshPath;
    CHECK(ctx, SavePrefabToFile(prefab, path));
    CHECK(ctx, editor::EnterPrefabMode(frame, path));

    const EntityID entity = editor::PrefabModeRootEntity(frame);
    CHECK(ctx, !editor::AssignModelMesh(frame, entity, root / "Models" / "missing.gltf"));
    CHECK(ctx, !editor::CanUndoEditorHistory(state));
    const auto* before = world.Get<MeshComponent>(entity);
    CHECK(ctx, before && before->meshAssetPath == oldMeshPath && !before->mesh.IsValid());
    CHECK(ctx, editor::AssignModelMesh(frame, entity, model));
    CHECK(ctx, editor::CanUndoEditorHistory(state));
    const auto* after = world.Get<MeshComponent>(entity);
    CHECK(ctx, after && after->meshAssetPath == newMeshPath && registry.meshes.Get(after->mesh));
    CHECK(ctx, editor::ExitPrefabMode(frame));

    PrefabAsset saved;
    CHECK(ctx, LoadPrefabFromFile(path, registry, saved));
    CHECK(ctx, !saved.records.empty() && saved.records.front().meshAssetPath == newMeshPath);
    fs::remove(path);
    fs::remove(model);
    fs::remove(model.parent_path() / "rig.bin");
}

static void TestParticleEffectBadgeFollowsRoot(test::TestContext& ctx)
{
    namespace editor = engine::renderer::addons::editor;
    // Emitter at the root.
    CHECK(ctx, editor::IsParticleEffectPrefabJson(
        R"({"rootIndex":0,"records":[{"name":"Fx","parentIndex":-1,"particleEmitter":{}}]})"));
    // Empty root with emitter children only (e.g. Staubwolke).
    CHECK(ctx, editor::IsParticleEffectPrefabJson(
        R"({"rootIndex":0,"records":[{"name":"Group","parentIndex":-1},)"
        R"({"name":"A","parentIndex":0,"particleEmitter":{}},{"name":"B","parentIndex":0,"particleEmitter":{}}]})"));
    // Mesh root with an added emitter child (car with dust) stays a prefab.
    CHECK(ctx, !editor::IsParticleEffectPrefabJson(
        R"({"rootIndex":0,"records":[{"name":"car","parentIndex":-1,"meshAssetPath":"car.glb#mesh/0"},)"
        R"({"name":"Dust","parentIndex":0,"particleEmitter":{}}]})"));
    // Empty root with a mesh child and an emitter child stays a prefab.
    CHECK(ctx, !editor::IsParticleEffectPrefabJson(
        R"({"rootIndex":0,"records":[{"name":"Group","parentIndex":-1},)"
        R"({"name":"Body","parentIndex":0,"meshAssetPath":"car.glb#mesh/0"},{"name":"Dust","parentIndex":0,"particleEmitter":{}}]})"));
    // Embedded prefab counts as content.
    CHECK(ctx, !editor::IsParticleEffectPrefabJson(
        R"({"rootIndex":0,"records":[{"name":"Group","parentIndex":-1},)"
        R"({"name":"Car","parentIndex":0,"nestedPrefabJson":"{}"},{"name":"Dust","parentIndex":0,"particleEmitter":{}}]})"));
    // No emitters, invalid JSON, bad root index.
    CHECK(ctx, !editor::IsParticleEffectPrefabJson(R"({"rootIndex":0,"records":[{"name":"Empty","parentIndex":-1}]})"));
    CHECK(ctx, !editor::IsParticleEffectPrefabJson("not json"));
    CHECK(ctx, !editor::IsParticleEffectPrefabJson(R"({"rootIndex":3,"records":[{"particleEmitter":{}}]})"));
}

static void TestPrefabModePreservesSceneLinks(test::TestContext& ctx)
{
    namespace editor = engine::renderer::addons::editor;
    auto meta = MakePrefabComponentRegistry(); ecs::World world(meta); assets::AssetRegistry registry;
    renderer::MaterialSystem materials; editor::EditorState state;
    editor::EditorFrameContext frame{world, materials, registry, state};
    const auto path = std::filesystem::absolute("out/prefab-check/test-mode.prefab");
    auto prefab = MakeLinkedRecipe(); CHECK(ctx, SavePrefabToFile(prefab, path));
    auto instance = InstantiatePrefab(world, prefab); LinkPrefabInstance(world, prefab, instance, path.string());
    CHECK(ctx, editor::EnterPrefabMode(frame, path));
    EntityID child = NULL_ENTITY;
    world.ForEachAlive([&](EntityID id) { if (auto* n = world.Get<NameComponent>(id); n && n->name == "Child") child = id; });
    CHECK(ctx, child.IsValid());
    world.Get<NameComponent>(child)->name = "EditedInPrefabMode";
    CHECK(ctx, editor::ExitPrefabMode(frame));
    size_t links = 0; bool updated = false;
    world.ForEachAlive([&](EntityID id) {
        if (world.Has<PrefabInstanceComponent>(id)) ++links;
        if (auto* n = world.Get<NameComponent>(id)) updated |= n->name == "EditedInPrefabMode";
    });
    CHECK_EQ(ctx, links, size_t{1}); CHECK(ctx, updated);
    std::filesystem::remove(path);
}


static void TestPrefabScaleSurvivesSaveSceneAndReopen(test::TestContext& ctx)
{
    namespace editor = engine::renderer::addons::editor;
    auto meta = MakePrefabComponentRegistry(); ecs::World world(meta); assets::AssetRegistry registry;
    renderer::MaterialSystem materials; editor::EditorState state;
    editor::EditorFrameContext frame{world, materials, registry, state};
    const auto path = std::filesystem::absolute("out/prefab-check/scale-roundtrip.prefab");
    const auto prefab = MakeLinkedRecipe(); CHECK(ctx, SavePrefabToFile(prefab, path));
    const auto normal = InstantiatePrefab(world, prefab);
    const auto overridden = InstantiatePrefab(world, prefab);
    LinkPrefabInstance(world, prefab, normal, path.string());
    LinkPrefabInstance(world, prefab, overridden, path.string());
    const std::string normalGuid = world.Get<GuidComponent>(normal.root)->guid;
    const std::string overriddenGuid = world.Get<GuidComponent>(overridden.root)->guid;
    world.Get<TransformComponent>(normal.root)->localPosition = {12.f, 3.f, 4.f};
    world.Get<TransformComponent>(overridden.root)->localScale = {0.25f, 0.25f, 0.25f};
    auto find = [&](const std::string& guid) {
        EntityID found = NULL_ENTITY;
        world.ForEachAlive([&](EntityID id) {
            if (const auto* g = world.Get<GuidComponent>(id); g && g->guid == guid) found = id;
        });
        return found;
    };
    CHECK(ctx, editor::EnterPrefabMode(frame, path));
    const auto root = editor::PrefabModeRootEntity(frame);
    world.Get<TransformComponent>(root)->localScale = {0.5f, 0.5f, 0.5f};
    CHECK(ctx, editor::SavePrefabMode(frame));
    PrefabAsset saved; CHECK(ctx, LoadPrefabFromFile(path, registry, saved));
    CHECK_EQ(ctx, saved.records[saved.rootIndex].localScale.x, 0.5f);
    CHECK(ctx, editor::ExitPrefabMode(frame));
    CHECK_EQ(ctx, world.Get<TransformComponent>(find(normalGuid))->localScale.x, 0.5f);
    CHECK_EQ(ctx, world.Get<TransformComponent>(find(normalGuid))->localPosition.x, 12.f);
    CHECK_EQ(ctx, world.Get<TransformComponent>(find(overriddenGuid))->localScale.x, 0.25f);
    CHECK(ctx, editor::EnterPrefabMode(frame, path));
    CHECK_EQ(ctx, world.Get<TransformComponent>(editor::PrefabModeRootEntity(frame))->localScale.x, 0.5f);
    CHECK(ctx, editor::ExitPrefabMode(frame));
    CHECK(ctx, LoadPrefabFromFile(path, registry, saved));
    CHECK_EQ(ctx, saved.records[saved.rootIndex].localScale.x, 0.5f);
    CHECK_EQ(ctx, world.Get<TransformComponent>(find(normalGuid))->localScale.x, 0.5f);
    CHECK_EQ(ctx, world.Get<TransformComponent>(find(overriddenGuid))->localScale.x, 0.25f);
    std::filesystem::remove(path);
}

// The editor lights prefab mode with a preview light rig. Those helper lights sit at the
// top level of the isolated world and must never become a prefab root or be saved.
static void TestPrefabModeIgnoresPreviewLightRig(test::TestContext& ctx)
{
    namespace editor = engine::renderer::addons::editor;
    auto meta = MakePrefabComponentRegistry(); ecs::World world(meta); assets::AssetRegistry registry;
    renderer::MaterialSystem materials; editor::EditorState state;
    editor::EditorFrameContext frame{world, materials, registry, state};
    const auto path = std::filesystem::absolute("out/prefab-check/preview-light-rig.prefab");
    const auto prefab = MakeLinkedRecipe(); CHECK(ctx, SavePrefabToFile(prefab, path));

    CHECK(ctx, editor::EnterPrefabMode(frame, path));
    const EntityID root = editor::PrefabModeRootEntity(frame);
    CHECK(ctx, root.IsValid());

    const EntityID rigLight = world.CreateEntity();
    world.Add<TransformComponent>(rigLight);
    world.Add<WorldTransformComponent>(rigLight);
    world.Add<LightComponent>(rigLight, LightComponent{});
    world.Add<editor::EditorPreviewLightComponent>(rigLight);

    CHECK(ctx, !editor::EnforcePrefabModeSingleRoot(frame));
    CHECK(ctx, !editor::GetEntityParent(frame, rigLight).IsValid());
    CHECK_EQ(ctx, editor::PrefabModeRootEntity(frame), root);
    CHECK(ctx, editor::SavePrefabMode(frame));

    PrefabAsset saved; CHECK(ctx, LoadPrefabFromFile(path, registry, saved));
    CHECK_EQ(ctx, saved.records.size(), prefab.records.size());
    bool anyLight = false;
    for (const auto& record : saved.records) anyLight |= record.light.has_value();
    CHECK(ctx, !anyLight);

    CHECK(ctx, editor::ExitPrefabMode(frame));
    std::filesystem::remove(path);
}

static void TestPrefabParentScaleAffectsEveryChild(test::TestContext& ctx)
{
    namespace editor = engine::renderer::addons::editor;
    auto meta = MakePrefabComponentRegistry(); ecs::World world(meta); assets::AssetRegistry registry;
    renderer::MaterialSystem materials; editor::EditorState state;
    editor::EditorFrameContext frame{world, materials, registry, state};
    const auto path = std::filesystem::absolute("out/prefab-check/parent-scale.prefab");
    auto prefab = MakeLinkedRecipe(); prefab.records[0].name = "Dragon";
    prefab.records[1].name = "Sitz"; prefab.records[1].localPosition = {4.f, 0.f, 0.f};
    prefab.records[2].name = "Drache"; prefab.records[2].localPosition = {-2.f, 2.f, 0.f};
    PrefabEntityRecord detail; detail.id = "detail"; detail.name = "Detail"; detail.parentIndex = 2;
    detail.localPosition = {0.f, 2.f, 0.f}; detail.localScale = {2.f, 3.f, 4.f};
    prefab.records.push_back(detail);
    CHECK(ctx, SavePrefabToFile(prefab, path));
    CHECK(ctx, editor::EnterPrefabMode(frame, path));
    const auto root = editor::PrefabModeRootEntity(frame);
    EntityID seat = NULL_ENTITY, dragon = NULL_ENTITY, nested = NULL_ENTITY;
    world.ForEachAlive([&](EntityID id) {
        const auto* name = world.Get<NameComponent>(id);
        if (!name) return;
        if (name->name == "Sitz") seat = id;
        if (name->name == "Drache") dragon = id;
        if (name->name == "Detail") nested = id;
    });
    TransformSystem transforms; transforms.Update(world);
    editor::detail::SetWorldScale(frame, root, *world.Get<TransformComponent>(root), {0.5f, 0.5f, 0.5f});
    transforms.Update(world);
    CHECK_EQ(ctx, world.Get<WorldTransformComponent>(root)->scale.x, 0.5f);
    CHECK_EQ(ctx, world.Get<WorldTransformComponent>(seat)->scale.x, 0.5f);
    CHECK_EQ(ctx, world.Get<WorldTransformComponent>(dragon)->scale.x, 0.5f);
    CHECK_EQ(ctx, world.Get<WorldTransformComponent>(seat)->position.x, 2.f);
    CHECK_EQ(ctx, world.Get<WorldTransformComponent>(dragon)->position.x, -1.f);
    CHECK_EQ(ctx, world.Get<WorldTransformComponent>(nested)->position.y, 2.f);
    CHECK_EQ(ctx, world.Get<WorldTransformComponent>(nested)->scale.y, 1.5f);
    CHECK_EQ(ctx, world.Get<TransformComponent>(seat)->localScale.x, 1.f);
    CHECK_EQ(ctx, world.Get<TransformComponent>(dragon)->localScale.x, 1.f);
    // A child can still be scaled separately; its sibling is unaffected.
    editor::detail::SetWorldScale(frame, seat, *world.Get<TransformComponent>(seat), {0.25f, 0.25f, 0.25f});
    transforms.Update(world);
    CHECK_EQ(ctx, world.Get<TransformComponent>(seat)->localScale.x, 0.5f);
    CHECK_EQ(ctx, world.Get<WorldTransformComponent>(seat)->scale.x, 0.25f);
    CHECK_EQ(ctx, world.Get<WorldTransformComponent>(dragon)->scale.x, 0.5f);
    CHECK(ctx, editor::SavePrefabMode(frame)); CHECK(ctx, editor::ExitPrefabMode(frame));
    CHECK(ctx, editor::EnterPrefabMode(frame, path));
    transforms.Invalidate(); transforms.Update(world);
    const auto reopened = editor::PrefabModeRootEntity(frame);
    CHECK_EQ(ctx, world.Get<WorldTransformComponent>(reopened)->scale.x, 0.5f);
    world.ForEachAlive([&](EntityID id) {
        if (const auto* name = world.Get<NameComponent>(id); name && name->name == "Sitz")
            CHECK_EQ(ctx, world.Get<WorldTransformComponent>(id)->scale.x, 0.25f);
        if (const auto* name = world.Get<NameComponent>(id); name && name->name == "Drache")
            CHECK_EQ(ctx, world.Get<WorldTransformComponent>(id)->scale.x, 0.5f);
    });
    CHECK(ctx, editor::ExitPrefabMode(frame));
    std::filesystem::remove(path);
}

static void TestDragonSceneScaleSaveAndReopen(test::TestContext& ctx)
{
    namespace editor = engine::renderer::addons::editor;
    auto meta = MakePrefabComponentRegistry(); ecs::World world(meta); assets::AssetRegistry registry;
    renderer::MaterialSystem materials; editor::EditorState state;
    editor::EditorFrameContext frame{world, materials, registry, state};
    const auto fixtures = std::filesystem::path(__FILE__).parent_path() / "fixtures";
    const auto project = std::filesystem::absolute("out/prefab-check/dragon-usercase");
    const auto path = project / "Assets/Prefabs/Dragon.prefab";
    PrefabAsset source; CHECK(ctx, LoadPrefabFromFile(fixtures / "prefab_dragon.prefab", registry, source));
    CHECK(ctx, SavePrefabToFile(source, path));
    const float expectedDragonScale = source.records[static_cast<size_t>(source.FindEntity("Drache"))].localScale.x * 0.5f;
    std::ifstream input(fixtures / "prefab_dragon_scene.json");
    std::ostringstream scene; scene << input.rdbuf();
    serialization::SceneDeserializer reader(world); reader.RegisterDefaultHandlers();
    RegisterPrefabDeserializationHandlers(reader);
    engine::addons::mesh_renderer::RegisterMeshRendererDeserializationHandlers(reader);
    physics::RegisterPhysicsDeserializationHandlers(reader);
    CHECK(ctx, reader.DeserializeFromJson(scene.str()).success);
    // The app rebuilds derived world-transform components after scene restores.
    frame.syncSceneState = [&]() {
        std::vector<EntityID> missing;
        world.ForEachAlive([&](EntityID id) {
            if (world.Has<TransformComponent>(id) && !world.Has<WorldTransformComponent>(id)) missing.push_back(id);
        });
        for (const auto id : missing) world.Add<WorldTransformComponent>(id);
    };
    frame.syncSceneState();
    frame.currentProjectRoot = project.string();
    std::string rootGuid;
    world.ForEachAlive([&](EntityID id) {
        if (const auto* n = world.Get<NameComponent>(id); n && n->name == "Dragon") rootGuid = world.Get<GuidComponent>(id)->guid;
    });
    CHECK(ctx, !rootGuid.empty());
    const math::Vec3 half{0.5f, 0.5f, 0.5f};
    CHECK(ctx, editor::EnterPrefabMode(frame, path));
    world.Get<TransformComponent>(editor::PrefabModeRootEntity(frame))->localScale = half;
    CHECK(ctx, editor::SavePrefabMode(frame)); CHECK(ctx, editor::ExitPrefabMode(frame));
    TransformSystem transforms; transforms.Update(world);
    bool rootFound = false, seatFound = false, dragonFound = false;
    world.ForEachAlive([&](EntityID id) {
        const auto* name = world.Get<NameComponent>(id);
        if (!name) return;
        if (name->name == "Dragon")
        {
            rootFound = true;
            CHECK(ctx, world.Get<TransformComponent>(id)->localScale == half);
            CHECK_EQ(ctx, world.Get<GuidComponent>(id)->guid, rootGuid);
            CHECK_EQ(ctx, world.Get<TransformComponent>(id)->localPosition.x, 7.2f);
        }
        if (name->name == "Sitz") { seatFound = true; CHECK_EQ(ctx, world.Get<WorldTransformComponent>(id)->scale.x, 0.5f); }
        if (name->name == "Drache") { dragonFound = true; CHECK_EQ(ctx, world.Get<WorldTransformComponent>(id)->scale.x, expectedDragonScale); }
    });
    CHECK(ctx, rootFound && seatFound && dragonFound);
    CHECK(ctx, editor::EnterPrefabMode(frame, path));
    CHECK(ctx, world.Get<TransformComponent>(editor::PrefabModeRootEntity(frame))->localScale == half);
    CHECK(ctx, editor::ExitPrefabMode(frame));
    // A second change must update the same scene instance, rather than compound scale.
    CHECK(ctx, editor::EnterPrefabMode(frame, path));
    world.Get<TransformComponent>(editor::PrefabModeRootEntity(frame))->localScale = {0.25f, 0.25f, 0.25f};
    CHECK(ctx, editor::ExitPrefabMode(frame));
    world.ForEachAlive([&](EntityID id) {
        if (const auto* g = world.Get<GuidComponent>(id); g && g->guid == rootGuid)
            CHECK_EQ(ctx, world.Get<TransformComponent>(id)->localScale.x, 0.25f);
    });
    CHECK(ctx, LoadPrefabFromFile(path, registry, source));
    CHECK_EQ(ctx, source.records[source.rootIndex].localScale.x, 0.25f);
    std::filesystem::remove(path);
}

static void TestPrefabDuplicateRemapsBindings(test::TestContext& ctx)
{
    namespace editor = engine::renderer::addons::editor;
    auto meta = MakePrefabComponentRegistry(); ecs::World world(meta); assets::AssetRegistry registry;
    renderer::MaterialSystem materials; editor::EditorState state;
    editor::EditorFrameContext frame{world, materials, registry, state};
    auto prefab = MakeLinkedRecipe();
    const auto original = InstantiatePrefab(world, prefab); LinkPrefabInstance(world, prefab, original, "duplicate.prefab");
    const auto copy = editor::DuplicateEntity(frame, original.root);
    CHECK(ctx, copy.IsValid()); CHECK(ctx, world.Has<PrefabInstanceComponent>(copy));
    CHECK(ctx, world.Get<GuidComponent>(copy)->guid != world.Get<GuidComponent>(original.root)->guid);
    prefab.records[1].name = "OnlyCopyChanged";
    CHECK(ctx, SynchronizePrefabInstance(world, copy, prefab, registry));
    CHECK_EQ(ctx, world.Get<NameComponent>(original.entities[1])->name, std::string("Child"));
    const auto* children = world.Get<ChildrenComponent>(copy);
    bool changed = false;
    for (const auto child : children->children) if (world.Get<NameComponent>(child)->name == "OnlyCopyChanged") changed = true;
    CHECK(ctx, changed); CHECK_EQ(ctx, world.EntityCount(), size_t{6});
    CHECK(ctx, editor::UndoEditorHistory(frame));
    CHECK_EQ(ctx, world.EntityCount(), size_t{3});
    CHECK(ctx, editor::RedoEditorHistory(frame));
    size_t links = 0; world.ForEachAlive([&](EntityID e) { links += world.Has<PrefabInstanceComponent>(e) ? 1u : 0u; });
    CHECK_EQ(ctx, links, size_t{2});
}

static void TestPrefabRefreshOnlyChangedAsset(test::TestContext& ctx)
{
    namespace editor = engine::renderer::addons::editor;
    auto meta = MakePrefabComponentRegistry(); ecs::World world(meta); assets::AssetRegistry registry;
    renderer::MaterialSystem materials; editor::EditorState state;
    editor::EditorFrameContext frame{world, materials, registry, state};
    auto a = MakeLinkedRecipe(); auto b = MakeLinkedRecipe();
    const auto pathA = std::filesystem::absolute("out/prefab-check/filter-a.prefab");
    const auto pathB = std::filesystem::absolute("out/prefab-check/filter-b.prefab");
    auto first = InstantiatePrefab(world, a); auto second = InstantiatePrefab(world, b);
    LinkPrefabInstance(world, a, first, pathA.string()); LinkPrefabInstance(world, b, second, pathB.string());
    a.records[1].name = "ChangedA"; b.records[1].name = "ChangedB";
    CHECK(ctx, SavePrefabToFile(a, pathA)); CHECK(ctx, SavePrefabToFile(b, pathB));
    engine::addons::editor::ResolvePrefabBindings(frame, pathA);
    CHECK_EQ(ctx, world.Get<NameComponent>(first.entities[1])->name, std::string("ChangedA"));
    CHECK_EQ(ctx, world.Get<NameComponent>(second.entities[1])->name, std::string("Child"));
    std::filesystem::remove(pathA); std::filesystem::remove(pathB);
}

static void TestPrefabRevertOverrides(test::TestContext& ctx)
{
    auto meta = MakePrefabComponentRegistry(); ecs::World world(meta); assets::AssetRegistry registry;
    auto prefab = MakeLinkedRecipe(); const auto instance = InstantiatePrefab(world, prefab);
    LinkPrefabInstance(world, prefab, instance, "revert.prefab");
    world.Get<TransformComponent>(instance.root)->localPosition.x = 50.f;
    world.Get<RigidBodyComponent>(instance.entities[1])->mass = 77.f;
    prefab.records[1].rigidBody->mass = 4.f;
    CHECK(ctx, SynchronizePrefabInstance(world, instance.root, prefab, registry));
    CHECK_EQ(ctx, world.Get<RigidBodyComponent>(instance.entities[1])->mass, 77.f);
    CHECK(ctx, SynchronizePrefabInstance(world, instance.root, prefab, registry, nullptr, nullptr, false));
    CHECK_EQ(ctx, world.Get<RigidBodyComponent>(instance.entities[1])->mass, 4.f);
    CHECK_EQ(ctx, world.Get<TransformComponent>(instance.root)->localPosition.x, 50.f);
    prefab.records[1].rigidBody->mass = 8.f;
    CHECK(ctx, SynchronizePrefabInstance(world, instance.root, prefab, registry));
    CHECK_EQ(ctx, world.Get<RigidBodyComponent>(instance.entities[1])->mass, 8.f);
}

static void TestPrefabSynchronizesInternalScriptReferences(test::TestContext& ctx)
{
    auto meta = MakePrefabComponentRegistry(); ecs::World world(meta); assets::AssetRegistry registry;
    script::ScriptRegistry scripts; scripts.Register<PrefabTestScript>("Refs");
    scripts.RegisterField<PrefabTestScript>("Refs", "target", script::ScriptFieldType::Entity, &PrefabTestScript::target);
    auto prefab = MakeLinkedRecipe();
    prefab.records[0].sourceGuid = "source-root"; prefab.records[1].sourceGuid = "source-child";
    prefab.records[2].sourceGuid = "source-sibling";
    PrefabEntityRecord::ScriptRecord script; script.className = "Refs"; script.instanceName = "Refs";
    script.fieldValues["target"].type = script::ScriptFieldType::Entity;
    script.fieldValues["target"].stringValue = "source-sibling";
    prefab.records[1].scripts.push_back(script);
    PrefabInstantiateOptions options; options.scriptRegistry = &scripts; options.prefabAssetPath = "refs.prefab";
    auto first = InstantiatePrefab(world, prefab, options); auto second = InstantiatePrefab(world, prefab, options);
    auto get = [&](const auto& inst) { return static_cast<PrefabTestScript*>(world.Get<script::ScriptList>(inst.entities[1])->Instances()[0].script.get()); };
    CHECK_EQ(ctx, get(first)->target, first.entities[2]); CHECK_EQ(ctx, get(second)->target, second.entities[2]);
    // Saving an edited prefab also replaces source GUIDs; stable record IDs still identify targets.
    prefab.records[0].sourceGuid = "edited-root"; prefab.records[1].sourceGuid = "edited-child";
    prefab.records[2].sourceGuid = "edited-sibling";
    prefab.records[1].scripts[0].fieldValues["target"].stringValue = "edited-root";
    CHECK(ctx, SynchronizePrefabInstance(world, first.root, prefab, registry, &scripts));
    CHECK_EQ(ctx, get(first)->target, first.root); CHECK_EQ(ctx, get(second)->target, second.entities[2]);
    CHECK_EQ(ctx, get(first)->GetEntity(), first.entities[1]);
}

static void TestPrefabImportedVariantsKeepAnimationPaths(test::TestContext& ctx)
{
    auto meta = MakePrefabComponentRegistry(); assets::AssetRegistry registry;
    for (int variant = 0; variant < 4; ++variant)
    {
        ecs::World world(meta); PrefabBuildOptions options; options.createSyntheticRoot = variant != 1;
        auto bundle = variant == 2 ? MakeMeshOnlyBundle() : (variant == 3 ? assets::ImportedAssetBundle{} : MakePrefabBundle());
        auto prefab = BuildPrefabFromImportedBundle(std::move(bundle), registry, options);
        auto instance = InstantiatePrefab(world, prefab); LinkPrefabInstance(world, prefab, instance, "imported.prefab");
        auto captured = BuildPrefabFromWorldEntity(world, instance.root, {}, &registry);
        PrefabAsset loaded; CHECK(ctx, DeserializePrefabFromJson(SerializePrefabToJson(captured), registry, loaded));
        if (variant != 3)
        {
            const auto mesh = std::find_if(loaded.records.begin(), loaded.records.end(), [](const auto& r) { return r.mesh.IsValid(); });
            CHECK(ctx, mesh != loaded.records.end());
            if (mesh != loaded.records.end())
            { CHECK(ctx, !mesh->skeletonAssetPath.empty()); CHECK(ctx, !mesh->animationAssetPath.empty()); }
        }
        prefab.records.back().active = false;
        CHECK(ctx, SynchronizePrefabInstance(world, instance.root, prefab, registry));
        CHECK(ctx, !world.Get<ActiveComponent>(instance.entities.back())->active);
    }
}

static void TestPrefabMalformedUpdateLeavesInstanceIntact(test::TestContext& ctx)
{
    auto meta = MakePrefabComponentRegistry(); ecs::World world(meta); assets::AssetRegistry registry;
    auto prefab = MakeLinkedRecipe(); auto instance = InstantiatePrefab(world, prefab);
    LinkPrefabInstance(world, prefab, instance, "invalid.prefab");
    prefab.records[1].parentIndex = 2; prefab.records[2].parentIndex = 1;
    std::string error;
    CHECK(ctx, !SynchronizePrefabInstance(world, instance.root, prefab, registry, nullptr, &error));
    CHECK(ctx, !error.empty()); CHECK_EQ(ctx, world.EntityCount(), size_t{3});
    CHECK(ctx, world.IsAlive(instance.entities[1]));
    CHECK_EQ(ctx, world.Get<ParentComponent>(instance.entities[1])->parent, instance.root);
}


static void TestPrefabAddonMergeAndComponentRemoval(test::TestContext& ctx)
{
    auto meta = MakePrefabComponentRegistry(); ecs::World world(meta); assets::AssetRegistry registry;
    auto prefab = MakeLinkedRecipe();
    prefab.records[1].audioSource.emplace(); prefab.records[1].particleEmitter.emplace();
    prefab.records[1].sphereCollider.emplace();
    prefab.records[1].baseColorTexturePath = "Textures/a.png"; // Material-only entity.
    auto instance = InstantiatePrefab(world, prefab); LinkPrefabInstance(world, prefab, instance, "addons.prefab");
    CHECK(ctx, world.Has<MaterialComponent>(instance.entities[1]));
    world.Get<AudioSourceComponent>(instance.entities[1])->volume = 0.7f;
    prefab.records[1].audioSource->volume = 0.2f; prefab.records[1].audioSource->pitch = 2.f;
    prefab.records[1].particleEmitter->emitRate = 123.f;
    prefab.records[1].rigidBody.reset(); prefab.records[1].sphereCollider.reset();
    prefab.records[1].baseColorTexturePath = "Textures/b.png";
    CHECK(ctx, SynchronizePrefabInstance(world, instance.root, prefab, registry));
    CHECK_EQ(ctx, world.Get<AudioSourceComponent>(instance.entities[1])->volume, 0.7f);
    CHECK_EQ(ctx, world.Get<AudioSourceComponent>(instance.entities[1])->pitch, 2.f);
    CHECK_EQ(ctx, world.Get<ParticleEmitterComponent>(instance.entities[1])->emitRate, 123.f);
    CHECK(ctx, !world.Has<RigidBodyComponent>(instance.entities[1]));
    CHECK(ctx, !world.Has<SphereColliderComponent>(instance.entities[1]));
    CHECK_EQ(ctx, world.Get<MaterialComponent>(instance.entities[1])->baseColorTexturePath, std::string("Textures/b.png"));
}

static void TestMaterialOverrideInheritance(test::TestContext& ctx)
{
    auto meta = MakePrefabComponentRegistry(); ecs::World world(meta); assets::AssetRegistry registry;
    auto prefab = MakeLinkedRecipe();
    prefab.records[1].materialAssetPath = "shared.kmat";
    prefab.records[1].materialOverrideSource = "uvScale 1 1\n";
    PrefabEntityRecord::SlotOverride slot; slot.submeshIndex = 1; slot.materialAssetPath = "shared.kmat";
    slot.materialOverrideSource = "uvScale 2 2\n"; prefab.records[1].slotOverrides.push_back(slot);
    const auto first = InstantiatePrefab(world, prefab), second = InstantiatePrefab(world, prefab);
    LinkPrefabInstance(world, prefab, first, "uv.prefab"); LinkPrefabInstance(world, prefab, second, "uv.prefab");
    world.Get<MaterialComponent>(first.entities[1])->materialOverrideSource = "uvScale 5 5\n";
    prefab.records[1].materialOverrideSource = "uvScale 3 3\n";
    CHECK(ctx, SynchronizePrefabInstance(world, first.root, prefab, registry));
    CHECK(ctx, SynchronizePrefabInstance(world, second.root, prefab, registry));
    CHECK_EQ(ctx, assets::MaterialPropertyLines(world.Get<MaterialComponent>(first.entities[1])->materialOverrideSource)["uvScale"], std::string("uvScale 5 5"));
    CHECK_EQ(ctx, assets::MaterialPropertyLines(world.Get<MaterialComponent>(second.entities[1])->materialOverrideSource)["uvScale"], std::string("uvScale 3 3"));
    const auto captured = BuildPrefabFromWorldEntity(world, first.root, {}, &registry);
    const auto json = SerializePrefabToJson(captured);
    PrefabAsset loaded;
    CHECK(ctx, DeserializePrefabFromJson(json, registry, loaded));
    const auto restored = InstantiatePrefab(world, loaded);
    CHECK_EQ(ctx, assets::MaterialPropertyLines(world.Get<MaterialComponent>(restored.entities[1])->materialOverrideSource)["uvScale"], std::string("uvScale 5 5"));
    CHECK_EQ(ctx, assets::MaterialPropertyLines(world.Get<MaterialComponent>(restored.entities[1])->FindSlotOverride(1)->materialOverrideSource)["uvScale"], std::string("uvScale 2 2"));
}

static void TestPrefabUVEditorRoundTrip(test::TestContext& ctx)
{
    namespace editor = engine::renderer::addons::editor;
    auto meta = MakePrefabComponentRegistry(); ecs::World world(meta); assets::AssetRegistry registry;
    renderer::MaterialSystem materials; editor::EditorState state;
    editor::EditorFrameContext frame{world, materials, registry, state};
    auto prefab = MakeLinkedRecipe(); prefab.records[1].materialAssetPath = "shared.kmat"; prefab.records[1].meshAssetPath = "missing.glb#mesh/0";
    const auto path = std::filesystem::absolute("out/prefab-check/uv-editor.prefab");
    CHECK(ctx, SavePrefabToFile(prefab, path));
    const auto first = InstantiatePrefab(world, prefab), second = InstantiatePrefab(world, prefab);
    LinkPrefabInstance(world, prefab, first, path.string()); LinkPrefabInstance(world, prefab, second, path.string());
    const auto firstGuid = world.Get<GuidComponent>(first.entities[1])->guid;
    const auto secondGuid = world.Get<GuidComponent>(second.entities[1])->guid;
    auto find = [&](const std::string& guid) {
        EntityID found{}; world.ForEachAlive([&](EntityID e) { if (auto* g=world.Get<GuidComponent>(e); g && g->guid==guid) found=e; }); return found;
    };
    assets::MaterialAsset material; material.uvScale = {5.f,5.f};
    CHECK(ctx, editor::SetEntityMaterialOverride(frame, first.entities[1], 0, material));
    CHECK(ctx, world.Get<MaterialComponent>(second.entities[1])->FindSlotOverride(0) == nullptr);
    // Real editor flow: enter isolated prefab, edit an inherited slot, save, exit.
    for (float value : {3.f,7.f})
    {
        if (value==7.f) {
            auto* parent=world.Get<ParentComponent>(find(firstGuid));
            auto* link=world.Get<PrefabInstanceComponent>(parent->parent);
            for (auto& binding : link->bindings)
                if (binding.entityGuid==firstGuid) binding.overriddenProperties.push_back("/slotOverrides/@0#0");
        }
        CHECK(ctx, editor::EnterPrefabMode(frame, path));
        EntityID child{}; world.ForEachAlive([&](EntityID e) { if (auto* n=world.Get<NameComponent>(e); n && n->name=="Child") child=e; });
        CHECK(ctx, child.IsValid());
        material.uvScale = {value,value}; material.roughnessFactor=value*.1f;
        CHECK(ctx, editor::SetEntityMaterialOverride(frame, child, 0, material));
        CHECK(ctx, editor::SavePrefabMode(frame));
        CHECK(ctx, editor::ExitPrefabMode(frame));
        const auto a = find(firstGuid), b = find(secondGuid);
        const auto* slotA = world.Get<MaterialComponent>(a)->FindSlotOverride(0);
        const auto* slotB = world.Get<MaterialComponent>(b)->FindSlotOverride(0);
        CHECK(ctx, slotA != nullptr); CHECK(ctx, slotB != nullptr);
        if (slotA && slotB) {
            const auto propertiesA=assets::MaterialPropertyLines(slotA->materialOverrideSource);
            const auto propertiesB=assets::MaterialPropertyLines(slotB->materialOverrideSource);
            CHECK_EQ(ctx, propertiesA.at("uvScale"), value==3.f ? std::string("uvScale 5 5") : std::string("uvScale 9 9"));
            CHECK_EQ(ctx, propertiesB.at("uvScale"), value==3.f ? std::string("uvScale 3 3") : std::string("uvScale 7 7"));
            CHECK_EQ(ctx, propertiesA.at("roughnessFactor"), propertiesB.at("roughnessFactor"));
        }
        material.uvScale = {9.f,9.f};
        CHECK(ctx, editor::SetEntityMaterialOverride(frame, a, 0, material));
        CHECK(ctx, world.Get<MaterialComponent>(a)->FindSlotOverride(0)->materialOverrideSource != world.Get<MaterialComponent>(b)->FindSlotOverride(0)->materialOverrideSource);
        engine::addons::editor::ResolvePrefabBindings(frame);
        CHECK(ctx, world.Get<MaterialComponent>(find(firstGuid))->FindSlotOverride(0)->materialOverrideSource != world.Get<MaterialComponent>(find(secondGuid))->FindSlotOverride(0)->materialOverrideSource);
    }
    CHECK(ctx,engine::addons::editor::ResetPrefabInstanceOverrides(frame,find(firstGuid)));
    CHECK_EQ(ctx,assets::MaterialPropertyLines(world.Get<MaterialComponent>(find(firstGuid))->FindSlotOverride(0)->materialOverrideSource).at("uvScale"),std::string("uvScale 7 7"));
    CHECK(ctx,editor::UndoEditorHistory(frame));
    CHECK_EQ(ctx,assets::MaterialPropertyLines(world.Get<MaterialComponent>(find(firstGuid))->FindSlotOverride(0)->materialOverrideSource).at("uvScale"),std::string("uvScale 9 9"));
    std::filesystem::remove(path);
}

static void TestUnifiedComponentPropertyOverrides(test::TestContext& ctx)
{
    auto meta=MakePrefabComponentRegistry(); ecs::World world(meta); assets::AssetRegistry registry;
    auto prefab=MakeLinkedRecipe();
    prefab.records[1].materialAssetPath="shared.kmat";
    prefab.records[1].materialOverrideSource="uvScale 1 1\nroughnessFactor 0.5\nbaseColorFactor 1 1 1 1\n";
    const auto a=InstantiatePrefab(world,prefab), b=InstantiatePrefab(world,prefab);
    LinkPrefabInstance(world,prefab,a,"unified.prefab"); LinkPrefabInstance(world,prefab,b,"unified.prefab");
    world.Get<RigidBodyComponent>(a.entities[1])->mass=42.f;
    world.Get<TransformComponent>(a.entities[1])->localScale={2.f,2.f,2.f};
    world.Get<MaterialComponent>(a.entities[1])->materialOverrideSource="uvScale 1 1\nroughnessFactor 0.5\nbaseColorFactor 1 0 0 1\n";
    prefab.records[1].rigidBody->mass=7.f; prefab.records[1].rigidBody->friction=.75f;
    prefab.records[1].localScale={.5f,.5f,.5f}; prefab.records[1].localPosition={4.f,5.f,6.f};
    prefab.records[1].materialOverrideSource="uvScale 3 3\nroughnessFactor 0.2\nbaseColorFactor 0 1 0 1\n";
    for (auto root : {a.root,b.root}) CHECK(ctx,SynchronizePrefabInstance(world,root,prefab,registry));
    CHECK_EQ(ctx,world.Get<RigidBodyComponent>(a.entities[1])->mass,42.f);
    CHECK_EQ(ctx,world.Get<RigidBodyComponent>(b.entities[1])->mass,7.f);
    CHECK_EQ(ctx,world.Get<RigidBodyComponent>(a.entities[1])->friction,.75f);
    CHECK_EQ(ctx,world.Get<TransformComponent>(a.entities[1])->localScale.x,2.f);
    CHECK_EQ(ctx,world.Get<TransformComponent>(b.entities[1])->localScale.x,.5f);
    CHECK_EQ(ctx,world.Get<TransformComponent>(a.entities[1])->localPosition.x,4.f);
    const auto first=assets::MaterialPropertyLines(world.Get<MaterialComponent>(a.entities[1])->materialOverrideSource);
    const auto second=assets::MaterialPropertyLines(world.Get<MaterialComponent>(b.entities[1])->materialOverrideSource);
    CHECK_EQ(ctx,first.at("baseColorFactor"),std::string("baseColorFactor 1 0 0 1"));
    CHECK_EQ(ctx,second.at("baseColorFactor"),std::string("baseColorFactor 0 1 0 1"));
    CHECK_EQ(ctx,first.at("uvScale"),std::string("uvScale 3 3"));
    CHECK_EQ(ctx,first.at("roughnessFactor"),std::string("roughnessFactor 0.2"));
    CHECK(ctx,SynchronizePrefabInstance(world,a.root,prefab,registry));
    CHECK_EQ(ctx,assets::MaterialPropertyLines(world.Get<MaterialComponent>(a.entities[1])->materialOverrideSource).at("baseColorFactor"),std::string("baseColorFactor 1 0 0 1"));
    CHECK(ctx,SynchronizePrefabInstance(world,a.root,prefab,registry,nullptr,nullptr,false));
    CHECK_EQ(ctx,world.Get<RigidBodyComponent>(a.entities[1])->mass,7.f);
    CHECK_EQ(ctx,assets::MaterialPropertyLines(world.Get<MaterialComponent>(a.entities[1])->materialOverrideSource).at("baseColorFactor"),std::string("baseColorFactor 0 1 0 1"));
}

int RunPrefabTests()
{
    test::TestSuite suite("Prefab");
    suite
        .Add("Builds stable record recipe", TestPrefabBuildsStableRecordRecipe)
        .Add("Instantiate returns root and entity list", TestPrefabInstantiateReturnsRootAndEntityList)
        .Add("Reset restores runtime state", TestPrefabResetRestoresRuntimeState)
        .Add("Destroy removes instance entities", TestPrefabDestroyRemovesInstanceEntities)
        .Add("Build without synthetic root", TestPrefabBuildWithoutSyntheticRoot)
        .Add("Builds mesh-only bundle", TestPrefabBuildsMeshOnlyBundle)
        .Add("Handles empty bundle", TestPrefabHandlesEmptyBundle)
        .Add("JSON round-trip preserves persistent recipe", TestPrefabJsonRoundTripPreservesPersistentRecipe)
        .Add("JSON drops non-portable material paths", TestPrefabDropsNonPortableMaterialPaths)
        .Add("Build from world entity captures collision shape", TestPrefabBuildFromWorldEntityCapturesCollisionShape)
        .Add("Build from world entity captures texture path", TestPrefabBuildFromWorldEntityCapturesTexturePath)
        .Add("Preserves scripts and fields", TestPrefabPreservesScriptsAndFields)
        .Add("Build from world entity captures mesh-slot material", TestPrefabBuildFromWorldEntityCapturesMeshSlotMaterial)
        .Add("Build from world entity ignores runtime material handles", TestPrefabBuildFromWorldEntityIgnoresRuntimeMaterialHandles)
        .Add("Preserves physics components", TestPrefabPreservesPhysicsComponents)
        .Add("Sync preserves identity and local properties", TestPrefabSyncPreservesIdentityAndLocalProperties)
        .Add("Child scripts survive synchronization", TestPrefabChildScriptsSurviveSynchronization)
        .Add("Sync adds removes and reparents", TestPrefabSyncAddsRemovesAndReparents)
        .Add("Sync preserves local deletion", TestPrefabSyncPreservesLocalDeletion)
        .Add("Links and overrides survive scene round trip", TestPrefabLinksAndOverridesSurviveSceneRoundTrip)
        .Add("Root transform and persistent components", TestPrefabRootTransformAndPersistentComponents)
        .Add("Nested prefab keeps independent link", TestNestedPrefabKeepsIndependentLink)
        .Add("Prefab mode preserves scene links", TestPrefabModePreservesSceneLinks)
        .Add("Prefab mode keeps a single root", TestPrefabModeKeepsSingleRoot)
        .Add("Prefab mode rebinds a renamed mesh", TestPrefabModeRebindsRenamedMesh)
        .Add("Particle effect badge follows the prefab root", TestParticleEffectBadgeFollowsRoot)
        .Add("Scale survives save scene and reopen", TestPrefabScaleSurvivesSaveSceneAndReopen)
        .Add("Prefab mode ignores preview light rig", TestPrefabModeIgnoresPreviewLightRig)
        .Add("Parent scale affects every child", TestPrefabParentScaleAffectsEveryChild)
        .Add("Dragon scene scale save and reopen", TestDragonSceneScaleSaveAndReopen)
        .Add("Duplicate remaps bindings and undo", TestPrefabDuplicateRemapsBindings)
        .Add("Refresh only changed asset", TestPrefabRefreshOnlyChangedAsset)
        .Add("Revert overrides", TestPrefabRevertOverrides)
        .Add("Synchronizes internal script references", TestPrefabSynchronizesInternalScriptReferences)
        .Add("Imported variants keep animation paths", TestPrefabImportedVariantsKeepAnimationPaths)
        .Add("Malformed update leaves instance intact", TestPrefabMalformedUpdateLeavesInstanceIntact)
        .Add("Unified component property overrides", TestUnifiedComponentPropertyOverrides)
        .Add("Prefab UV editor round trip", TestPrefabUVEditorRoundTrip)
        .Add("Material override inheritance", TestMaterialOverrideInheritance)
        .Add("Addon merge and component removal", TestPrefabAddonMergeAndComponentRemoval)
        .Add("Model paths: relative + animation import", TestPrefabResolvesModelAnimationAssets)
        .Add("Scene round trip keeps skinned animation", TestSceneRoundTripKeepsSkinnedAnimation)
        .Add("Prefab sync restores missing animation", TestPrefabSyncRestoresMissingAnimation)
        .Add("Model clip settings import", TestModelClipSettingsImport);
    return suite.Run();
}
