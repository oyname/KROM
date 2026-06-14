#include "TestFramework.hpp"

#include "addons/animation/AnimationComponents.hpp"
#include "addons/mesh_renderer/MeshRendererComponents.hpp"
#include "addons/prefab/Prefab.hpp"
#include "addons/script/ComponentScript.hpp"
#include "addons/script/ScriptList.hpp"
#include "assets/AssetRegistry.hpp"
#include "ecs/Components.hpp"
#include "ecs/World.hpp"

#include <memory>

using namespace engine;
using namespace engine::addons::prefab;

namespace {

class PrefabTestScript final : public script::ComponentScript
{
public:
    float speed = 3.f;
    std::string prefabPath = "Prefabs/Default.prefab";
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
    return registry;
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
        .Add("Build from world entity captures collision shape", TestPrefabBuildFromWorldEntityCapturesCollisionShape)
        .Add("Build from world entity captures texture path", TestPrefabBuildFromWorldEntityCapturesTexturePath)
        .Add("Preserves scripts and fields", TestPrefabPreservesScriptsAndFields)
        .Add("Build from world entity captures mesh-slot material", TestPrefabBuildFromWorldEntityCapturesMeshSlotMaterial);
    return suite.Run();
}
