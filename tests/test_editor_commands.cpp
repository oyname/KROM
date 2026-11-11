#include "addons/animation/AnimationComponents.hpp"
#include "TestFramework.hpp"



#include "addons/editor/EditorCommands.hpp"
#include "addons/editor/EditorAssetBrowser.hpp"
#include "addons/editor/EditorUI.hpp"
#include "addons/editor/EditorUIInternal.hpp"
#include "addons/editor/EditorScriptAssets.hpp"
#include "addons/script/ScriptList.hpp"

#include "addons/editor/EditorMaterialLibrary.hpp"
#include "addons/editor/EditorMaterialGraph.hpp"
#include "addons/gameplay/MaterialParameterInstances.hpp"
#include "renderer/MaterialGraphBinding.hpp"
#include "addons/lit/LitMaterial.hpp"
#include "addons/unlit/UnlitMaterial.hpp"
#include "assets/MaterialProperties.hpp"

#include "addons/mesh_renderer/MeshRendererComponents.hpp"

#include "addons/editor/EditorComponentClipboard.hpp"

#include "addons/editor/EditorValueClipboard.hpp"

#include "addons/editor/EditorThumbnailCache.hpp"
#include "addons/editor/EditorThumbnailFraming.hpp"

#include "addons/audio/AudioComponents.hpp"

#include "addons/camera/CameraComponents.hpp"

#include "addons/particles/ParticleComponents.hpp"

#include "addons/prefab/Prefab.hpp"

#include "addons/prefab/PrefabInstanceComponent.hpp"

#include "core/PathUtf8.hpp"

#include "addons/editor/EditorComponents.hpp"

#include "addons/lighting/LightingComponents.hpp"

#include "addons/physics/PhysicsComponents.hpp"

#include "assets/AssetRegistry.hpp"

#include "assets/AssetPipeline.hpp"

#include "ecs/Components.hpp"

#include "ecs/World.hpp"

#include "renderer/MaterialSystem.hpp"



#include <filesystem>

#include <fstream>

#include <chrono>

#include <cmath>

#include "scene/TransformSystem.hpp"



using namespace engine;

using namespace engine::ecs;

using namespace engine::renderer;

using namespace engine::renderer::addons::editor;



namespace {



static ComponentMetaRegistry CreateEditorCommandsRegistry()

{

    ComponentMetaRegistry registry;

    RegisterCoreComponents(registry);

    RegisterEditorComponents(registry);

    RegisterLightingComponents(registry);

    return registry;

}



static EditorFrameContext CreateEditorContext(World& world,

                                              MaterialSystem& materials,

                                              assets::AssetRegistry& registry,

                                              EditorState& state)

{

    return EditorFrameContext{world, materials, registry, state};

}



static void TestDuplicateEntityCopiesHierarchy(test::TestContext& ctx)

{

    ComponentMetaRegistry componentRegistry = CreateEditorCommandsRegistry();

    World world(componentRegistry);

    MaterialSystem materials;

    assets::AssetRegistry assetRegistry;

    EditorState state;

    EditorFrameContext editor = CreateEditorContext(world, materials, assetRegistry, state);



    const EntityID parent = world.CreateEntity();

    world.Add<NameComponent>(parent, NameComponent{"Parent"});

    world.Add<TransformComponent>(parent);

    world.Add<WorldTransformComponent>(parent);

    world.Add<ActiveComponent>(parent, ActiveComponent{true});

    world.Add<EditorHierarchyComponent>(parent, EditorHierarchyComponent{0u});



    const EntityID child = world.CreateEntity();

    world.Add<NameComponent>(child, NameComponent{"Child"});

    world.Add<TransformComponent>(child);

    world.Add<WorldTransformComponent>(child);

    world.Add<ActiveComponent>(child, ActiveComponent{true});

    world.Add<EditorHierarchyComponent>(child, EditorHierarchyComponent{0u});

    world.Add<ParentComponent>(child, ParentComponent{parent});

    world.Add<ChildrenComponent>(parent).Add(child);



    const EntityID grandChild = world.CreateEntity();

    world.Add<NameComponent>(grandChild, NameComponent{"GrandChild"});

    world.Add<TransformComponent>(grandChild);

    world.Add<WorldTransformComponent>(grandChild);

    world.Add<ActiveComponent>(grandChild, ActiveComponent{true});

    world.Add<EditorHierarchyComponent>(grandChild, EditorHierarchyComponent{0u});

    world.Add<ParentComponent>(grandChild, ParentComponent{child});

    world.Add<ChildrenComponent>(child).Add(grandChild);



    const EntityID parentCopy = DuplicateEntity(editor, parent);

    CHECK(ctx, parentCopy.IsValid());

    CHECK_NE(ctx, parentCopy, parent);



    EntityID childCopy = NULL_ENTITY;

    EntityID grandChildCopy = NULL_ENTITY;

    world.ForEachAlive([&](EntityID id) {

        if (id == parent || id == child || id == grandChild)

            return;



        const auto* name = world.Get<NameComponent>(id);

        const auto* parentComponent = world.Get<ParentComponent>(id);

        if (!name || !parentComponent)

            return;



        if (parentComponent->parent == parentCopy &&

            name->name.rfind("Child Kopie", 0u) == 0u)

        {

            childCopy = id;

        }

    });



    CHECK(ctx, childCopy.IsValid());

    CHECK_NE(ctx, childCopy, child);



    world.ForEachAlive([&](EntityID id) {

        if (id == parent || id == child || id == grandChild)

            return;



        const auto* name = world.Get<NameComponent>(id);

        const auto* parentComponent = world.Get<ParentComponent>(id);

        if (!name || !parentComponent)

            return;



        if (parentComponent->parent == childCopy &&

            name->name.rfind("GrandChild Kopie", 0u) == 0u)

        {

            grandChildCopy = id;

        }

    });



    CHECK(ctx, grandChildCopy.IsValid());

    CHECK_NE(ctx, grandChildCopy, grandChild);



    const auto* parentCopyChildren = world.Get<ChildrenComponent>(parentCopy);

    CHECK(ctx, parentCopyChildren != nullptr);

    CHECK(ctx, parentCopyChildren &&

                   std::find(parentCopyChildren->children.begin(),

                             parentCopyChildren->children.end(),

                             childCopy) != parentCopyChildren->children.end());



    const auto* childCopyChildren = world.Get<ChildrenComponent>(childCopy);

    CHECK(ctx, childCopyChildren != nullptr);

    CHECK(ctx, childCopyChildren &&

                   std::find(childCopyChildren->children.begin(),

                             childCopyChildren->children.end(),

                             grandChildCopy) != childCopyChildren->children.end());

}



} // namespace



static EntityID FindByName(World& world, const char* name)

{

    EntityID found = NULL_ENTITY;

    world.ForEachAlive([&](EntityID id) {

        if (const auto* n = world.Get<NameComponent>(id); n && n->name == name)

            found = id;

    });

    return found;

}



// Ablauf wie im Inspector: Komponente per ExecuteSceneMutation hinzufuegen,

// Feld-Aenderung per Begin/CommitPendingSceneEdit (ComponentUndoScope).

static void TestComponentAddAndFieldEditAreUndoable(test::TestContext& ctx)

{

    ComponentMetaRegistry componentRegistry = CreateEditorCommandsRegistry();

    RegisterPhysicsComponents(componentRegistry);

    World world(componentRegistry);

    MaterialSystem materials;

    assets::AssetRegistry assetRegistry;

    EditorState state;

    EditorFrameContext editor = CreateEditorContext(world, materials, assetRegistry, state);



    const EntityID crate = world.CreateEntity();

    world.Add<NameComponent>(crate, NameComponent{"Crate"});

    world.Add<TransformComponent>(crate);

    world.Add<WorldTransformComponent>(crate);

    world.Add<ActiveComponent>(crate, ActiveComponent{true});

    world.Add<EditorHierarchyComponent>(crate, EditorHierarchyComponent{0u});



    CHECK(ctx, ExecuteSceneMutation(editor, "Komponente hinzugefuegt", [&]() {

        world.Add<RigidBodyComponent>(crate, RigidBodyComponent{});

    }));

    CHECK(ctx, UndoEditorHistory(editor));

    CHECK(ctx, !world.Has<RigidBodyComponent>(FindByName(world, "Crate")));

    CHECK(ctx, RedoEditorHistory(editor));

    EntityID restored = FindByName(world, "Crate");

    CHECK(ctx, world.Has<RigidBodyComponent>(restored));



    // Feld-Aenderung: Schnappschuss mit altem Wert, neuer Wert, Abschluss.

    const std::string key = MakeEditHistoryKey(restored, "rigidbody");

    BeginPendingSceneEdit(editor, key, "Rigidbody geaendert");

    world.Get<RigidBodyComponent>(restored)->mass = 5.f;

    CommitPendingSceneEdit(editor, key);

    CHECK(ctx, UndoEditorHistory(editor));

    restored = FindByName(world, "Crate");

    const auto* rb = world.Get<RigidBodyComponent>(restored);

    CHECK(ctx, rb && rb->mass == 1.f);

}



// Duplizieren kopiert alle Komponenten; Zwischenablage kopiert/fuegt einzelne ein.

static void TestDuplicateAndComponentClipboard(test::TestContext& ctx)

{

    ComponentMetaRegistry componentRegistry = CreateEditorCommandsRegistry();

    RegisterParticleComponents(componentRegistry);

    RegisterPhysicsComponents(componentRegistry);

    RegisterAudioComponents(componentRegistry);

    RegisterCameraComponents(componentRegistry);

    World world(componentRegistry);

    MaterialSystem materials;

    assets::AssetRegistry assetRegistry;

    EditorState state;

    EditorFrameContext editor = CreateEditorContext(world, materials, assetRegistry, state);



    const EntityID source = world.CreateEntity();

    world.Add<NameComponent>(source, NameComponent{"Funken"});

    world.Add<TransformComponent>(source);

    world.Add<WorldTransformComponent>(source);

    world.Add<ActiveComponent>(source, ActiveComponent{true});

    world.Add<EditorHierarchyComponent>(source, EditorHierarchyComponent{0u});

    ParticleEmitterComponent emitter{};

    emitter.burstCount = 77u;

    world.Add<ParticleEmitterComponent>(source, emitter);

    OBBComponent obb{};

    obb.halfExtents = {2.f, 3.f, 4.f};

    world.Add<OBBComponent>(source, obb);

    AudioSourceComponent audio{};

    audio.volume = 0.25f;

    world.Add<AudioSourceComponent>(source, audio);



    const EntityID dup = DuplicateEntity(editor, source);

    CHECK(ctx, dup.IsValid() && dup != source);

    CHECK(ctx, world.Get<ParticleEmitterComponent>(dup) && world.Get<ParticleEmitterComponent>(dup)->burstCount == 77u);

    CHECK(ctx, world.Get<OBBComponent>(dup) && world.Get<OBBComponent>(dup)->halfExtents.y == 3.f);

    CHECK(ctx, world.Get<AudioSourceComponent>(dup) && world.Get<AudioSourceComponent>(dup)->volume == 0.25f);



    // Kopieren -> als neue Komponente einfuegen -> Werte einfuegen.

    const EntityID target = world.CreateEntity();

    world.Add<TransformComponent>(target);

    CopyComponentToClipboard<ParticleEmitterComponent>(editor, source, "Partikel-Emitter");

    CHECK(ctx, ClipboardHolds<ParticleEmitterComponent>());

    CHECK(ctx, !ComponentClipboard().apply(editor, target, false));   // fehlt am Ziel

    CHECK(ctx, ComponentClipboard().presentOn && !ComponentClipboard().presentOn(editor, target));

    CHECK(ctx, ComponentClipboard().apply(editor, target, true));

    CHECK(ctx, world.Get<ParticleEmitterComponent>(target)->burstCount == 77u);

    world.Get<ParticleEmitterComponent>(source)->burstCount = 5u;

    CHECK(ctx, ComponentClipboard().apply(editor, target, false));    // Kopie von vorher

    CHECK(ctx, world.Get<ParticleEmitterComponent>(target)->burstCount == 77u);



    // Transform: Werte einfuegen markiert die Transform als geaendert.

    world.Get<TransformComponent>(source)->localPosition = {1.f, 2.f, 3.f};

    CopyComponentToClipboard<TransformComponent>(editor, source, "Transform");

    world.Get<TransformComponent>(target)->dirty = false;

    CHECK(ctx, ComponentClipboard().apply(editor, target, false));

    CHECK(ctx, world.Get<TransformComponent>(target)->localPosition.z == 3.f);

    CHECK(ctx, world.Get<TransformComponent>(target)->dirty);



    // Kamera: Hauptkamera-Kennung bleibt am Ziel, wie sie war.

    CameraComponent mainCam{};

    mainCam.isMainCamera = true;

    world.Add<CameraComponent>(source, mainCam);

    CopyComponentToClipboard<CameraComponent>(editor, source, "Kamera");

    CHECK(ctx, ComponentClipboard().apply(editor, target, true));

    CHECK(ctx, !world.Get<CameraComponent>(target)->isMainCamera);

}



// Vorschaubilder auf der Platte: ein Bild je Asset, Stand, Umbenennen, Aufraeumen.

static void TestAssetThumbnailMeshFraming(test::TestContext& ctx)
{
    auto meta = CreateEditorCommandsRegistry();
    RegisterMeshRendererComponents(meta);
    World world(meta);
    assets::AssetRegistry registry;
    auto asset = std::make_unique<assets::MeshAsset>();
    asset->submeshes.emplace_back().positions = {
        -0.75f, 0.f, -0.75f, 0.75f, 4.5f, 0.75f};
    const auto mesh = registry.GetOrAddMesh("thumbnail-column", std::move(asset));

    const EntityID root = world.CreateEntity();
    world.Add<TransformComponent>(root);
    world.Add<WorldTransformComponent>(root);
    const EntityID child = world.CreateEntity();
    world.Add<TransformComponent>(child);
    world.Add<WorldTransformComponent>(child);
    world.Add<ParentComponent>(child, ParentComponent{root});
    world.Add<MeshComponent>(child, MeshComponent{mesh});
    // No serialized BoundsComponent: the actual mesh must still be framed.
    const EntityID group = world.CreateEntity();
    world.Add<TransformComponent>(group).localPosition = {10000.f, 10000.f, 10000.f};
    world.Add<WorldTransformComponent>(group);
    const std::vector<EntityID> entities{root, child, group};
    TransformSystem transforms;
    transforms.Update(world);
    math::Vec3 center{};
    math::Vec3 eyeOffset{};
    const float tanHalfFov = std::tan(kAssetThumbnailFov * 0.5f);
    const auto checkFraming = [&](std::initializer_list<EntityID> meshes) {
        const math::Mat4 view = math::Mat4::LookAtRH(center + eyeOffset, center, math::Vec3::Up());
        float coverage = 0.f;
        for (EntityID entity : meshes)
            for (float x : {-0.75f, 0.75f})
                for (float y : {0.f, 4.5f})
                    for (float z : {-0.75f, 0.75f})
                    {
                        const math::Vec3 p = world.Get<WorldTransformComponent>(entity)
                            ->matrix.TransformPoint({x, y, z});
                        const math::Vec3 cameraPoint = view.TransformPoint(p);
                        CHECK(ctx, cameraPoint.z < 0.f);
                        coverage = std::max(coverage, std::max(std::abs(cameraPoint.x), std::abs(cameraPoint.y))
                            / (-cameraPoint.z * tanHalfFov));
                    }
        CHECK(ctx, coverage > 0.85f && coverage < 0.90f);
    };

    CHECK(ctx, FitAssetThumbnailCamera(world, registry, entities, {}, center, eyeOffset));
    CHECK(ctx, eyeOffset.x > 0.f && eyeOffset.y > 0.f && eyeOffset.z > 0.f);
    const float oldRadius = math::Vec3{0.75f, 2.25f, 0.75f}.Length();
    CHECK(ctx, eyeOffset.Length() < oldRadius * 1.35f / tanHalfFov);
    checkFraming({child});

    auto* parentTransform = world.Get<TransformComponent>(root);
    parentTransform->localPosition = {10.f, -2.f, 5.f};
    parentTransform->localScale = {-2.f, 3.f, 0.5f};
    parentTransform->localRotation = math::Quat::FromEulerDeg(0.f, 90.f, 0.f);
    parentTransform->dirty = true;
    auto* childTransform = world.Get<TransformComponent>(child);
    childTransform->localPosition = {1.f, 2.f, -3.f};
    childTransform->dirty = true;
    transforms.Update(world);
    const math::Vec3 origin = parentTransform->localPosition;
    CHECK(ctx, FitAssetThumbnailCamera(world, registry, entities, origin, center, eyeOffset));
    checkFraming({child});

    // A second, distant mesh must expand the union; empty groups still do not.
    world.Add<MeshComponent>(root, MeshComponent{mesh});
    CHECK(ctx, FitAssetThumbnailCamera(world, registry, entities, origin, center, eyeOffset));
    checkFraming({root, child});
    world.Get<MeshComponent>(root)->visible = false;
    world.Add<ActiveComponent>(child, ActiveComponent{false});
    CHECK(ctx, !FitAssetThumbnailCamera(world, registry, entities, origin, center, eyeOffset));
}

static void TestThumbnailDiskCache(test::TestContext& ctx)

{

    const std::filesystem::path project = std::filesystem::temp_directory_path() / "krom_thumbnail_test";

    std::error_code ec;

    std::filesystem::remove_all(project, ec);

    std::filesystem::create_directories(project / "Assets" / "Materials", ec);

    auto touch = [&](const char* rel) {

        std::ofstream(project / "Assets" / rel) << "x";

    };

    touch("Materials/rot.mat");

    touch("Materials/blau.mat");



    std::vector<uint8_t> pixels(4u * 4u * 4u, 0u);

    for (size_t i = 0u; i < pixels.size(); ++i)

        pixels[i] = static_cast<uint8_t>(i);



    // Old lighting and camera settings are invalid even when the source asset did not change.
    for (const char* path : {"Materials/rot.mat", "tower.prefab", "tower.glb", "tower.gltf"})
    {
        CHECK(ctx, SaveCachedThumbnail(project, path, 111u, 4u, 4u, pixels));
        std::fstream old(ThumbnailCacheFile(project, path), std::ios::binary | std::ios::in | std::ios::out);
        const uint32_t oldVersion = std::string_view(path) == "tower.prefab" ? 5u : 3u;
        old.seekp(4);
        old.write(reinterpret_cast<const char*>(&oldVersion), sizeof(oldVersion));
        old.close();
        uint32_t oldWidth = 0u, oldHeight = 0u;
        std::vector<uint8_t> oldPixels;
        CHECK(ctx, !LoadCachedThumbnail(project, path, 111u, oldWidth, oldHeight, oldPixels));
    }
    RemoveCachedThumbnail(project, "tower.prefab");
    RemoveCachedThumbnail(project, "tower.glb");
    RemoveCachedThumbnail(project, "tower.gltf");
    CHECK(ctx, SaveCachedThumbnail(project, "Materials/rot.mat", 111u, 4u, 4u, pixels));

    CHECK(ctx, !SaveCachedThumbnail(project, "Materials/rot.mat", 111u, 4u, 5u, pixels));   // Groesse passt nicht

    uint32_t w = 0u, h = 0u;

    std::vector<uint8_t> loaded;

    CHECK(ctx, LoadCachedThumbnail(project, "Materials/rot.mat", 111u, w, h, loaded));

    CHECK(ctx, w == 4u && h == 4u && loaded == pixels);

    CHECK(ctx, !LoadCachedThumbnail(project, "Materials/rot.mat", 222u, w, h, loaded));    // Asset geaendert

    CHECK(ctx, !LoadCachedThumbnail(project, "Materials/blau.mat", 111u, w, h, loaded));   // nie gespeichert



    // Geaendertes Asset ueberschreibt sein Bild (keine zweite Datei).

    CHECK(ctx, SaveCachedThumbnail(project, "Materials/rot.mat", 222u, 4u, 4u, pixels));

    size_t files = 0u;

    for (const auto& entry : std::filesystem::directory_iterator(ThumbnailCacheDir(project), ec))

        files += entry.path().extension() == ".kthumb" ? 1u : 0u;

    CHECK_EQ(ctx, files, 1u);



    // Umbenennen nimmt das Bild mit, Loeschen entfernt es.

    RenameCachedThumbnail(project, "Materials/rot.mat", "Materials/gruen.mat");

    CHECK(ctx, !std::filesystem::exists(ThumbnailCacheFile(project, "Materials/rot.mat")));

    CHECK(ctx, LoadCachedThumbnail(project, "Materials/gruen.mat", 222u, w, h, loaded));

    RemoveCachedThumbnail(project, "Materials/gruen.mat");

    CHECK(ctx, !std::filesystem::exists(ThumbnailCacheFile(project, "Materials/gruen.mat")));



    // Aufraeumen: Bild ohne Asset fliegt, Bild mit Asset bleibt.

    CHECK(ctx, SaveCachedThumbnail(project, "Materials/blau.mat", 5u, 4u, 4u, pixels));

    CHECK(ctx, SaveCachedThumbnail(project, "Materials/weg.mat", 5u, 4u, 4u, pixels));

    CHECK_EQ(ctx, CleanupThumbnailCache(project), 1u);

    CHECK(ctx, std::filesystem::exists(ThumbnailCacheFile(project, "Materials/blau.mat")));

    CHECK(ctx, !std::filesystem::exists(ThumbnailCacheFile(project, "Materials/weg.mat")));



    // Obergrenze: zu klein -> auch gueltige Bilder werden entfernt.

    CHECK_EQ(ctx, CleanupThumbnailCache(project, 10u), 1u);

    CHECK(ctx, !std::filesystem::exists(ThumbnailCacheFile(project, "Materials/blau.mat")));



    // Verkleinern: 2x2-Bloecke werden gemittelt.

    const std::vector<uint8_t> small = DownscaleRgba(std::vector<uint8_t>{

        0, 0, 0, 255,   100, 0, 0, 255,

        0, 0, 0, 255,   100, 0, 0, 255}, 2u, 2u, 2u);

    CHECK(ctx, small.size() == 4u && small[0] == 50u && small[3] == 255u);

    std::filesystem::remove_all(project, ec);

}



static void TestParticlePrefabLiveLink(test::TestContext& ctx)

{

    auto components = CreateEditorCommandsRegistry();

    RegisterParticleComponents(components);

    RegisterPrefabInstanceComponent(components);

    World world(components);

    MaterialSystem materials;

    assets::AssetRegistry registry;

    EditorState state;

    auto editor = CreateEditorContext(world, materials, registry, state);

    const auto directory = std::filesystem::temp_directory_path() /

        ("krom-prefab-link-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));

    std::filesystem::create_directories(directory);

    const auto path = directory / "Particles.prefab";

    const EntityID root = world.CreateEntity();

    world.Add<NameComponent>(root, NameComponent{"PlacedParticles"});

    world.Add<TransformComponent>(root);

    world.Get<TransformComponent>(root)->localPosition = {4, 2, 7};

    world.Add<WorldTransformComponent>(root);

    world.Add<EditorHierarchyComponent>(root, EditorHierarchyComponent{0});

    world.Add<EditorSceneTagComponent>(root, EditorSceneTagComponent{"TestScene"});

    ParticleEmitterComponent emitter{};

    emitter.burstCount = 10;

    world.Add<ParticleEmitterComponent>(root, emitter);

    const auto prefab = engine::addons::prefab::BuildPrefabFromWorldEntity(world, root, "Particles", &registry);

    std::string error;

    CHECK(ctx, engine::addons::prefab::SavePrefabToFile(prefab, path, &error));

    world.Add<PrefabInstanceComponent>(root, PrefabInstanceComponent{PathToUtf8(path), true});

    state.selectedEntity = root;

    // Undo/Redo snapshots must preserve the live link as well.

    BeginPendingSceneEdit(editor, "particle-test", "Particle edit");

    world.Get<ParticleEmitterComponent>(root)->burstCount = 11;

    CommitPendingSceneEdit(editor, "particle-test");

    CHECK(ctx, UndoEditorHistory(editor));

    CHECK(ctx, EnterPrefabMode(editor, path));

    const EntityID editingRoot = PrefabModeRootEntity(editor);

    CHECK(ctx, editingRoot.IsValid());

    if (auto* editing = world.Get<ParticleEmitterComponent>(editingRoot))

        editing->burstCount = 77;

    else CHECK(ctx, false);

    CHECK(ctx, ExitPrefabMode(editor));

    unsigned instances = 0;

    world.ForEachAlive([&](EntityID entity) {

        const auto* link = world.Get<PrefabInstanceComponent>(entity);

        if (!link) return;

        ++instances;

        CHECK(ctx, link->prefabAssetPath == PathToUtf8(path));

        const auto* updated = world.Get<ParticleEmitterComponent>(entity);

        CHECK(ctx, updated && updated->burstCount == 77);

        const auto* transform = world.Get<TransformComponent>(entity);

        CHECK(ctx, transform && transform->localPosition.x == 4 && transform->localPosition.z == 7);

        const auto* tag = world.Get<EditorSceneTagComponent>(entity);

        CHECK(ctx, tag && tag->sceneName == "TestScene");

        const auto* hierarchy = world.Get<EditorHierarchyComponent>(entity);

        CHECK(ctx, hierarchy && hierarchy->order == 0);

        CHECK(ctx, state.selectedEntity == entity);

    });

    CHECK_EQ(ctx, instances, 1u);

    std::error_code ec;

    std::filesystem::remove_all(directory, ec);

}



static void TestTransformClipboardCoordinateSpaces(test::TestContext& ctx)

{

    auto meta = CreateEditorCommandsRegistry(); World world(meta);

    MaterialSystem materials; assets::AssetRegistry assets; EditorState state;

    auto editor = CreateEditorContext(world, materials, assets, state);

    ClearEditorHistory(state);

    auto make = [&](const char* name, EntityID parent, math::Vec3 position, math::Vec3 scale) {

        const auto entity = world.CreateEntity();

        world.Add<NameComponent>(entity, NameComponent{name});

        TransformComponent t; t.localPosition = position; t.localScale = scale;

        world.Add<TransformComponent>(entity, t); world.Add<WorldTransformComponent>(entity);

        world.Add<EditorHierarchyComponent>(entity);

        if (parent.IsValid()) world.Add<ParentComponent>(entity, ParentComponent{parent});

        return entity;

    };

    const auto parentA = make("ParentA", NULL_ENTITY, {10.f, 2.f, 3.f}, {2.f, 3.f, 4.f});

    const auto parentB = make("ParentB", NULL_ENTITY, {-5.f, -7.f, 4.f}, {3.f, 4.f, 5.f});

    world.Get<TransformComponent>(parentA)->localRotation = math::Quat::FromEulerDeg(0.f, 90.f, 0.f);

    world.Get<TransformComponent>(parentB)->localRotation = math::Quat::FromEulerDeg(0.f, -30.f, 0.f);

    const auto source = make("Source", parentA, {1.f, 2.f, 3.f}, {1.f, 2.f, 3.f});

    const auto target = make("Target", parentB, {4.f, 5.f, 6.f}, {.5f, .75f, 1.25f});

    world.Get<TransformComponent>(source)->localRotation = math::Quat::FromEulerDeg(0.f, 0.f, 45.f);

    world.Get<TransformComponent>(target)->localRotation = math::Quat::FromEulerDeg(20.f, 0.f, 0.f);

    TransformSystem transforms; transforms.Update(world);

    auto near = [](float a, float b) { return std::fabs(a-b) < 0.0001f; };

    auto sameVector = [&](math::Vec3 a, math::Vec3 b) { return near(a.x,b.x) && near(a.y,b.y) && near(a.z,b.z); };

    const auto originalLocal = *world.Get<TransformComponent>(target);

    const auto sourceWorld = *world.Get<WorldTransformComponent>(source);

    // Copy directly after an edit, before the render-frame world cache updates.

    world.Get<TransformComponent>(source)->localPosition.x += 1.f;

    const auto parentWorld = *world.Get<WorldTransformComponent>(parentA);

    const auto localPosition = world.Get<TransformComponent>(source)->localPosition;

    const auto freshPosition = parentWorld.position + parentWorld.rotation.Rotate(

        {localPosition.x*parentWorld.scale.x, localPosition.y*parentWorld.scale.y, localPosition.z*parentWorld.scale.z});

    state.transformSpace = EditorTransformSpace::World;

    CHECK(ctx, CopyTransformToClipboard(editor, source, TransformClipboardPart::Position));

    state.transformSpace = EditorTransformSpace::Local;

    CHECK(ctx, CopyTransformToClipboard(editor, source, TransformClipboardPart::Scale));

    CHECK(ctx, PasteTransformFromClipboard(editor, target, TransformClipboardPart::Position));

    transforms.Update(world);

    CHECK(ctx, sameVector(world.Get<WorldTransformComponent>(target)->position, freshPosition));

    CHECK(ctx, sameVector(world.Get<TransformComponent>(target)->localScale, originalLocal.localScale));

    CHECK(ctx, sameVector(world.Get<TransformComponent>(target)->localRotation.Rotate({1.f,0.f,0.f}), originalLocal.localRotation.Rotate({1.f,0.f,0.f})));

    // Position and scale retain separate copy-time coordinate spaces.

    state.transformSpace = EditorTransformSpace::World;

    CHECK(ctx, PasteTransformFromClipboard(editor, target, TransformClipboardPart::Scale));

    CHECK(ctx, sameVector(world.Get<TransformComponent>(target)->localScale, world.Get<TransformComponent>(source)->localScale));

    CHECK(ctx, CopyTransformToClipboard(editor, source, TransformClipboardPart::Rotation));

    state.transformSpace = EditorTransformSpace::Local;

    CHECK(ctx, PasteTransformFromClipboard(editor, target, TransformClipboardPart::Rotation));

    transforms.Update(world);

    CHECK(ctx, sameVector(world.Get<WorldTransformComponent>(target)->rotation.Rotate({1.f,0.f,0.f}), sourceWorld.rotation.Rotate({1.f,0.f,0.f})));

    // Full component copy uses World as well and remains a snapshot.

    state.transformSpace = EditorTransformSpace::World;

    world.Get<TransformComponent>(source)->dirty = true; transforms.Update(world);

    const auto expected = *world.Get<WorldTransformComponent>(source);

    CopyComponentToClipboard<TransformComponent>(editor, source, "Transform");

    world.Get<TransformComponent>(source)->localPosition.x += 100.f;

    state.transformSpace = EditorTransformSpace::Local;

    CHECK(ctx, PasteTransformFromClipboard(editor, target));

    transforms.Update(world);

    const auto pasted = *world.Get<WorldTransformComponent>(target);

    CHECK(ctx, sameVector(pasted.position, expected.position));

    CHECK(ctx, sameVector(pasted.scale, expected.scale));

    CHECK(ctx, sameVector(pasted.rotation.Rotate({0.f,0.f,1.f}), expected.rotation.Rotate({0.f,0.f,1.f})));

    // Explicit Local copying stays relative to the destination's own parent.

    const auto beforeLocalPaste = world.Get<TransformComponent>(target)->localPosition;

    CHECK(ctx, CopyTransformToClipboard(editor, source, TransformClipboardPart::Position));

    state.transformSpace = EditorTransformSpace::World;

    CHECK(ctx, PasteTransformFromClipboard(editor, target, TransformClipboardPart::Position));

    CHECK(ctx, sameVector(world.Get<TransformComponent>(target)->localPosition, world.Get<TransformComponent>(source)->localPosition));

    CHECK(ctx, UndoEditorHistory(editor));

    const auto restored = FindByName(world, "Target");

    CHECK(ctx, restored.IsValid());

    CHECK(ctx, sameVector(world.Get<TransformComponent>(restored)->localPosition, beforeLocalPaste));

    CHECK(ctx, RedoEditorHistory(editor));

    CHECK(ctx, sameVector(world.Get<TransformComponent>(FindByName(world, "Target"))->localPosition,

                          world.Get<TransformComponent>(FindByName(world, "Source"))->localPosition));

}



static void TestTypedPropertyClipboardAndUndo(test::TestContext& ctx)

{

    auto meta = CreateEditorCommandsRegistry(); RegisterPhysicsComponents(meta);

    World world(meta); MaterialSystem materials; assets::AssetRegistry assets; EditorState state;

    auto editor = CreateEditorContext(world, materials, assets, state); ClearEditorHistory(state);

    const auto entity = world.CreateEntity(); world.Add<NameComponent>(entity, NameComponent{"PropertyTarget"});

    world.Add<TransformComponent>(entity); world.Add<RigidBodyComponent>(entity).mass = 2.f;

    const float copied = 7.f; value_ui::Copy("float", &copied, sizeof(copied));

    bool boolean = false;

    CHECK(ctx, !value_ui::Paste("bool", &boolean, sizeof(boolean)));

    CHECK(ctx, !boolean);

    std::array<float,4> color{.1f,.2f,.3f,.4f}, destination{};

    value_ui::Copy("color4", color.data(), sizeof(color));

    CHECK(ctx, !value_ui::Paste("float4", destination.data(), sizeof(destination)));

    CHECK(ctx, value_ui::Paste("color4", destination.data(), sizeof(destination)));

    CHECK(ctx, destination == color);

    value_ui::CopyText("asset-texture", "Textures/test.png");

    std::string reference = "Textures/old.png";

    CHECK(ctx, !value_ui::PasteText("asset-material", reference));

    CHECK(ctx, value_ui::PasteText("asset-texture", reference));

    CHECK_EQ(ctx, reference, std::string("Textures/test.png"));

    value_ui::Copy("float", &copied, sizeof(copied));

    int before = 0, after = 0;

    const auto key = MakeEditHistoryKey(entity, "property.paste");

    {

        value_ui::Scope scope([&] { ++before; BeginPendingSceneEdit(editor,key,"Wert eingefuegt"); },

                              [&] { ++after; CommitPendingSceneEdit(editor,key); });

        CHECK(ctx, value_ui::Paste("float", &world.Get<RigidBodyComponent>(entity)->mass, sizeof(float)));

    }

    CHECK_EQ(ctx, before, 1); CHECK_EQ(ctx, after, 1);

    CHECK_EQ(ctx, world.Get<RigidBodyComponent>(entity)->mass, 7.f);

    CHECK(ctx, UndoEditorHistory(editor));

    CHECK_EQ(ctx, world.Get<RigidBodyComponent>(FindByName(world,"PropertyTarget"))->mass, 2.f);

    CHECK(ctx, RedoEditorHistory(editor));

    CHECK_EQ(ctx, world.Get<RigidBodyComponent>(FindByName(world,"PropertyTarget"))->mass, 7.f);

    ClearEditorHistory(state);

    const auto current = FindByName(world,"PropertyTarget");

    world.Get<RigidBodyComponent>(current)->mass = 2.f;

    const auto nativeKey = MakeEditHistoryKey(current,"native.mass");

    float editedMass = 2.f;

    value_ui::Copy("float", &copied, sizeof(copied));

    {

        value_ui::Scope scope([&] { BeginPendingSceneEdit(editor,key,"Wert eingefuegt"); },

                              [&] { CommitPendingSceneEdit(editor,key); });

        CHECK(ctx, value_ui::Paste("float",&editedMass,sizeof(float)));

        BeginPendingSceneEdit(editor,nativeKey,"Masse geaendert");

        world.Get<RigidBodyComponent>(current)->mass = editedMass;

        CommitPendingSceneEdit(editor,nativeKey);

    }

    CHECK(ctx, UndoEditorHistory(editor));

    CHECK_EQ(ctx, world.Get<RigidBodyComponent>(FindByName(world,"PropertyTarget"))->mass, 2.f);

    CHECK(ctx, !CanUndoEditorHistory(state));

    ClearEditorHistory(state);

    const math::Vec4 oldColor{.2f,.3f,.4f,1.f}, newColor{.6f,.7f,.8f,.5f};

    state.selectedMaterialAssetData.baseColorFactor = newColor;

    RecordEditorValueChange(editor,"Materialwert eingefuegt",

        [&] { state.selectedMaterialAssetData.baseColorFactor = oldColor; },

        [&] { state.selectedMaterialAssetData.baseColorFactor = newColor; });

    CHECK(ctx, UndoEditorHistory(editor)); CHECK(ctx, state.selectedMaterialAssetData.baseColorFactor == oldColor);

    CHECK(ctx, RedoEditorHistory(editor)); CHECK(ctx, state.selectedMaterialAssetData.baseColorFactor == newColor);

    ClearEditorHistory(state);

}



static void TestPropertyLabelContextMenu(test::TestContext& ctx)

{

    auto* previous = ImGui::GetCurrentContext();

    auto* gui = ImGui::CreateContext();

    auto& io = ImGui::GetIO(); io.DisplaySize = {640.f,480.f}; io.DeltaTime = 1.f/60.f;

    unsigned char* pixels = nullptr; int width = 0, height = 0;

    io.Fonts->GetTexDataAsRGBA32(&pixels,&width,&height);

    io.IniFilename = nullptr;

    float source = 8.f, target = .2f;

    ImVec2 sourceLabel{}, targetLabel{}, sourceColorLabel{}, targetColorLabel{};

    std::array<float,4> sourceColor{.1f,.2f,.3f,.4f}, targetColor{.9f,.8f,.7f,1.f};

    int before = 0, after = 0;

    auto frame = [&]() {

        ImGui::NewFrame();

        {

            value_ui::Scope scope([&] { ++before; }, [&] { ++after; });

            ImGui::SetNextWindowPos({0.f,0.f}); ImGui::SetNextWindowSize({500.f,400.f});

            ImGui::Begin("Property label test",nullptr,ImGuiWindowFlags_NoSavedSettings);

            value_ui::TextUnformatted("Source");

            sourceLabel = ImGui::GetItemRectMin(); sourceLabel.x += 12.f; sourceLabel.y += 6.f;

            ImGui::SameLine(100.f); value_ui::DragFloat("##source",&source,1.f,0.f,10.f);

            value_ui::TextUnformatted("Target");

            targetLabel = ImGui::GetItemRectMin(); targetLabel.x += 12.f; targetLabel.y += 6.f;

            ImGui::SameLine(100.f); value_ui::DragFloat("##target",&target,1.f,0.f,5.f);

            value_ui::ColorEdit4("Base Color",sourceColor.data());

            sourceColorLabel = {ImGui::GetItemRectMax().x-ImGui::CalcTextSize("Base Color").x*.5f,

                                ImGui::GetItemRectMin().y+ImGui::GetFrameHeight()*.5f};

            value_ui::ColorEdit4("Other Color",targetColor.data());

            targetColorLabel = {ImGui::GetItemRectMax().x-ImGui::CalcTextSize("Other Color").x*.5f,

                                ImGui::GetItemRectMin().y+ImGui::GetFrameHeight()*.5f};

            ImGui::End();

        }

        ImGui::Render();

    };

    auto click = [&](ImVec2 point, int button) {

        io.AddMousePosEvent(point.x,point.y); frame();

        io.AddMouseButtonEvent(button,true); frame();

        io.AddMouseButtonEvent(button,false); frame();

    };

    frame(); frame();

    click(sourceLabel,ImGuiMouseButton_Right);

    CHECK(ctx, gui->OpenPopupStack.Size == 1);

    if (gui->OpenPopupStack.Size == 1 && gui->OpenPopupStack[0].Window)

    {

        const auto start = gui->OpenPopupStack[0].Window->DC.CursorStartPos;

        click({start.x+35.f,start.y+ImGui::GetFontSize()*.5f},ImGuiMouseButton_Left);

        CHECK(ctx, value_ui::Compatible("float",sizeof(float)));

        float snapshot = 0.f;

        if (value_ui::Compatible("float",sizeof(float))) std::memcpy(&snapshot,value_ui::Clipboard().bytes.data(),sizeof(float));

        CHECK_EQ(ctx, snapshot, 8.f);

        CHECK_EQ(ctx, before, 0);

        click(targetLabel,ImGuiMouseButton_Right);

        CHECK(ctx, gui->OpenPopupStack.Size == 1);

        if (gui->OpenPopupStack.Size == 1 && gui->OpenPopupStack[0].Window)

        {

            const auto destination = gui->OpenPopupStack[0].Window->DC.CursorStartPos;

            click({destination.x+35.f,destination.y+ImGui::GetTextLineHeightWithSpacing()+ImGui::GetFontSize()*.5f},ImGuiMouseButton_Left);

            CHECK_EQ(ctx, target, 5.f); // Target limit is respected on paste.

            CHECK_EQ(ctx, before, 1); CHECK_EQ(ctx, after, 1);

        }

    }

    click(sourceColorLabel,ImGuiMouseButton_Right);

    CHECK(ctx, gui->OpenPopupStack.Size == 1);

    if (gui->OpenPopupStack.Size == 1 && gui->OpenPopupStack[0].Window)

    {

        const auto start = gui->OpenPopupStack[0].Window->DC.CursorStartPos;

        click({start.x+35.f,start.y+ImGui::GetFontSize()*.5f},ImGuiMouseButton_Left);

        CHECK(ctx, value_ui::Compatible("color4",sizeof(sourceColor)));

        click(targetColorLabel,ImGuiMouseButton_Right);

        CHECK(ctx, gui->OpenPopupStack.Size == 1);

        if (gui->OpenPopupStack.Size == 1 && gui->OpenPopupStack[0].Window)

        {

            const auto destination = gui->OpenPopupStack[0].Window->DC.CursorStartPos;

            click({destination.x+35.f,destination.y+ImGui::GetTextLineHeightWithSpacing()+ImGui::GetFontSize()*.5f},ImGuiMouseButton_Left);

            CHECK(ctx, targetColor == sourceColor);

            CHECK_EQ(ctx, before, 2); CHECK_EQ(ctx, after, 2);

        }

    }

    ImGui::DestroyContext(gui); ImGui::SetCurrentContext(previous);

}



static void TestIndependentMaterialInstances(test::TestContext& ctx)

{

    auto meta = CreateEditorCommandsRegistry(); RegisterMeshRendererComponents(meta);

    World world(meta); MaterialSystem materials; assets::AssetRegistry registry; EditorState state;

    auto editor = CreateEditorContext(world, materials, registry, state);

    const auto first = world.CreateEntity(), second = world.CreateEntity();

    for (auto entity : {first, second})

    {

        world.Add<TransformComponent>(entity);

        world.Add<MeshComponent>(entity);

        MaterialComponent c; c.materialAssetPath = "shared.kmat";

        MaterialComponent::SlotOverride slot; slot.submeshIndex = 1; slot.materialAssetPath = "shared.kmat";

        c.slotOverrides.push_back(slot); world.Add<MaterialComponent>(entity, c);

    }

    assets::MaterialAsset asset; asset.uvScale = {5.f, 3.f}; asset.uvOffset = {.2f, .4f};
    asset.boxProjection = true;
    asset.boxTileSize = 0.5f;

    state.selectedMaterialAssetData = asset;

    CHECK(ctx, SetEntityMaterialOverride(editor, second, -1, asset));
    CHECK(ctx, !world.Get<MaterialComponent>(second)->materialOverrideSource.empty());
    ResetEntityMaterialOverride(editor, second, -1, "shared.kmat");
    CHECK(ctx, world.Get<MaterialComponent>(second)->materialOverrideSource.empty());

    CHECK(ctx, SetEntityMaterialOverride(editor, first, -1, asset));

    const auto source = world.Get<MaterialComponent>(first)->materialOverrideSource;

    CHECK(ctx, source.find("uvScale 5 3") != std::string::npos);

    assets::AssetPipeline pipeline(registry);

    const auto parsed = pipeline.LoadMaterialSource(source);

    CHECK(ctx, parsed.IsValid());

    CHECK_EQ(ctx, registry.materials.Get(parsed)->uvScale.x, 5.f);

    CHECK_EQ(ctx, registry.materials.Get(parsed)->uvScale.y, 3.f);

    CHECK(ctx, registry.materials.Get(parsed)->boxProjection);
    CHECK_EQ(ctx, registry.materials.Get(parsed)->BoxProjectionTileSize(), 0.5f);

    const auto other = pipeline.LoadMaterialSource("template pbr-lit\nuvScale 2 2\n");

    CHECK_NE(ctx, parsed, other);

    CHECK_EQ(ctx, registry.materials.Get(parsed)->uvScale.x, 5.f);

    CHECK_EQ(ctx, registry.materials.Get(other)->uvScale.x, 2.f);

    CHECK(ctx, !registry.materials.Get(other)->boxProjection);
    CHECK_EQ(ctx, registry.materials.Get(other)->BoxProjectionTileSize(), 0.f);

    CHECK(ctx, world.Get<MaterialComponent>(second)->materialOverrideSource.empty());

    CHECK_EQ(ctx, world.Get<MaterialComponent>(first)->materialAssetPath, std::string("shared.kmat"));

    CHECK(ctx, !state.selectedMaterialAssetDirty);

    CHECK(ctx, UndoEditorHistory(editor));

    EntityID a{}, b{};

    world.ForEachAlive([&](EntityID e) { if (!a.IsValid()) a=e; else b=e; });

    CHECK(ctx, world.Get<MaterialComponent>(a)->materialOverrideSource.empty());

    CHECK(ctx, world.Get<MaterialComponent>(b)->materialOverrideSource.empty());

    CHECK(ctx, RedoEditorHistory(editor));

    world.ForEachAlive([&](EntityID e) { if (!world.Get<MaterialComponent>(e)->materialOverrideSource.empty()) a=e; else b=e; });

    CHECK_EQ(ctx, world.Get<MaterialComponent>(a)->materialOverrideSource, source);

    CHECK(ctx, world.Get<MaterialComponent>(b)->materialOverrideSource.empty());

    asset.uvScale = {2.f, 7.f};

    CHECK(ctx, SetEntityMaterialOverride(editor, b, 1, asset));

    CHECK(ctx, world.Get<MaterialComponent>(b)->materialOverrideSource.empty());

    CHECK(ctx, world.Get<MaterialComponent>(b)->FindSlotOverride(1)->materialOverrideSource.find("uvScale 2 7") != std::string::npos);

    CHECK(ctx, world.Get<MaterialComponent>(a)->FindSlotOverride(1)->materialOverrideSource.empty());

    const auto copy = DuplicateEntity(editor, a);

    CHECK(ctx, copy.IsValid());

    asset.uvScale = {9.f, 9.f};

    CHECK(ctx, SetEntityMaterialOverride(editor, copy, -1, asset));

    CHECK_EQ(ctx, world.Get<MaterialComponent>(a)->materialOverrideSource, source);

}



static void TestAssetFolderTileNavigation(test::TestContext& ctx)
{
    const auto root=std::filesystem::absolute("out/asset-folder-click/fixture");
    std::filesystem::create_directories(root / "Child" / "Nested");
    auto meta=CreateEditorCommandsRegistry(); World world(meta); MaterialSystem materials;
    assets::AssetRegistry registry; EditorState state; auto editor=CreateEditorContext(world,materials,registry,state);
    AssetBrowserState browser; browser.initialized=true; browser.currentDir=root;
    auto* previous=ImGui::GetCurrentContext(); auto* gui=ImGui::CreateContext();
    auto& io=ImGui::GetIO(); io.IniFilename=nullptr; io.DisplaySize={1280.f,900.f}; io.DeltaTime=1.f/60.f;
    io.Fonts->AddFontDefault(); unsigned char* pixels; int width,height; io.Fonts->GetTexDataAsRGBA32(&pixels,&width,&height);
    // Ohne Dockspace: Assets-Panel so platzieren, wie es im Editor unten angedockt ist.
    auto frame=[&] {
        ImGui::NewFrame();
        ImGui::SetNextWindowPos({0.f,640.f}); ImGui::SetNextWindowSize({1280.f,260.f});
        DrawAssetBrowser(editor,browser); ImGui::Render();
    };
    auto tile=[&] {
        ImVec2 result{};
        for (auto* window : gui->Windows)
            if (std::strstr(window->Name,"##content_panel")) result={window->DC.CursorStartPos.x+40.f,window->DC.CursorStartPos.y+35.f};
        return result;
    };
    frame(); frame(); const auto point=tile(); CHECK(ctx,point.x>0.f && point.y>0.f);
    io.AddMousePosEvent(point.x,point.y); frame();
    io.AddMouseButtonEvent(ImGuiMouseButton_Left,true); frame();
    CHECK_EQ(ctx,browser.currentDir,root);
    io.AddMouseButtonEvent(ImGuiMouseButton_Left,false); frame();
    CHECK_EQ(ctx,browser.currentDir,root);
    CHECK_EQ(ctx,browser.selectedFolder,root / "Child");
    io.AddMouseButtonEvent(ImGuiMouseButton_Left,true); frame();
    CHECK_EQ(ctx,browser.currentDir,root);
    io.AddMouseButtonEvent(ImGuiMouseButton_Left,false); frame();
    CHECK_EQ(ctx,browser.currentDir,root / "Child");
    frame(); const auto nested=tile();
    io.AddMousePosEvent(nested.x,nested.y); frame();
    io.AddMouseButtonEvent(ImGuiMouseButton_Left,true); frame();
    io.AddMouseButtonEvent(ImGuiMouseButton_Left,false); frame();
    CHECK_EQ(ctx,browser.currentDir,root / "Child");
    CHECK_EQ(ctx,browser.selectedFolder,root / "Child" / "Nested");
    io.AddMouseButtonEvent(ImGuiMouseButton_Left,true); frame();
    io.AddMouseButtonEvent(ImGuiMouseButton_Left,false); frame();
    CHECK_EQ(ctx,browser.currentDir,root / "Child" / "Nested");
    browser.currentDir=root; frame(); frame(); const auto drag=tile();
    io.AddMousePosEvent(drag.x,drag.y); frame();
    io.AddMouseButtonEvent(ImGuiMouseButton_Left,true); frame();
    io.AddMousePosEvent(drag.x+90.f,drag.y+5.f); frame();
    io.AddMouseButtonEvent(ImGuiMouseButton_Left,false); frame();
    CHECK_EQ(ctx,browser.currentDir,root);
    ImGui::DestroyContext(gui); ImGui::SetCurrentContext(previous);
}

// Angedocktes Assets-Panel laesst sich am Dock-Reiter herausziehen.
static void TestDockedAssetsPanelCanUndock(test::TestContext& ctx)
{
    const auto root=std::filesystem::absolute("out/asset-folder-click/dock");
    std::filesystem::create_directories(root);
    auto meta=CreateEditorCommandsRegistry(); World world(meta); MaterialSystem materials;
    assets::AssetRegistry registry; EditorState state; auto editor=CreateEditorContext(world,materials,registry,state);
    AssetBrowserState browser; browser.initialized=true; browser.currentDir=root;
    auto* previous=ImGui::GetCurrentContext(); auto* gui=ImGui::CreateContext();
    auto& io=ImGui::GetIO(); io.IniFilename=nullptr; io.DisplaySize={1280.f,900.f}; io.DeltaTime=1.f/60.f;
    io.ConfigFlags|=ImGuiConfigFlags_DockingEnable;
    io.Fonts->AddFontDefault(); unsigned char* pixels; int width,height; io.Fonts->GetTexDataAsRGBA32(&pixels,&width,&height);
    auto frame=[&] { ImGui::NewFrame(); DrawEditorDockSpace(editor); DrawAssetBrowser(editor,browser); ImGui::Render(); };
    frame(); frame();
    ImGuiWindow* assets=ImGui::FindWindowByName("Assets##editor");
    CHECK(ctx,assets!=nullptr && assets->DockIsActive);
    if (!assets || !assets->DockNode || !assets->DockNode->TabBar) { ImGui::DestroyContext(gui); ImGui::SetCurrentContext(previous); return; }

    // Am Dock-Reiter herausziehen.
    const ImRect bar=assets->DockNode->TabBar->BarRect;
    const ImVec2 grab{bar.Min.x+20.f,(bar.Min.y+bar.Max.y)*.5f};
    io.AddMousePosEvent(grab.x,grab.y); frame();
    io.AddMouseButtonEvent(ImGuiMouseButton_Left,true); frame();
    for (int i=1;i<=10;++i) { io.AddMousePosEvent(grab.x+20.f*i,grab.y-40.f*i); frame(); }
    io.AddMouseButtonEvent(ImGuiMouseButton_Left,false); frame(); frame();
    CHECK(ctx,!assets->DockIsActive);
    ImGui::DestroyContext(gui); ImGui::SetCurrentContext(previous);
}

static void TestSceneSettingsDockTab(test::TestContext& ctx)
{
    using namespace engine::renderer::addons::editor::detail;

    auto meta = CreateEditorCommandsRegistry();
    World world(meta);
    MaterialSystem materials;
    assets::AssetRegistry registry;
    EditorState state;
    auto editor = CreateEditorContext(world, materials, registry, state);

    auto* previous = ImGui::GetCurrentContext();
    auto* gui = ImGui::CreateContext();
    auto& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    io.DisplaySize = {1280.f, 900.f};
    io.DeltaTime = 1.f / 60.f;
    unsigned char* pixels;
    int width, height;
    io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
    auto frame = [&](bool openViewMenu = false) {
        ImGui::NewFrame();
        if (openViewMenu && ImGui::BeginMainMenuBar())
        {
            ImGui::OpenPopup("###menu.view");
            ImGui::EndMainMenuBar();
        }
        DrawFileMenuBar(editor);
        DrawEditorDockSpace(editor);
        if (editor.inspectorPanelVisible)
            DrawInspector(editor);
        if (editor.sceneSettingsPanelVisible)
            DrawSceneSettingsPanel(editor);
        ImGui::Render();
    };

    frame();
    frame();
    const auto* inspector = ImGui::FindWindowByName("Inspector##editor");
    const auto* scene = ImGui::FindWindowByName("Scene##editor");
    auto* bar = scene && scene->DockNode ? scene->DockNode->TabBar : nullptr;
    const auto* sceneTab = bar ? ImGui::TabBarFindTabByID(bar, scene->TabId) : nullptr;
    CHECK(ctx, inspector && sceneTab && inspector->DockNode == scene->DockNode);
    if (sceneTab)
    {
        io.AddMousePosEvent(bar->BarRect.Min.x + sceneTab->Offset + sceneTab->Width * 0.5f,
                            bar->BarRect.GetCenter().y);
        frame();
        io.AddMouseButtonEvent(ImGuiMouseButton_Middle, true);
        frame();
        io.AddMouseButtonEvent(ImGuiMouseButton_Middle, false);
        frame();
        CHECK(ctx, !editor.sceneSettingsPanelVisible);
        CHECK(ctx, editor.inspectorPanelVisible);

        frame(true);
        frame();
        auto* viewMenu = gui->OpenPopupStack.empty() ? nullptr : gui->OpenPopupStack.back().Window;
        CHECK(ctx, viewMenu);
        if (viewMenu)
        {
            ImGui::ActivateItemByID(viewMenu->GetID("###panel.scene"));
            frame();
            frame();
            CHECK(ctx, editor.sceneSettingsPanelVisible);
            CHECK(ctx, scene->DockTabIsVisible);
            CHECK(ctx, inspector && inspector->DockNode == scene->DockNode);
        }
    }

    ImGui::DestroyContext(gui);
    ImGui::SetCurrentContext(previous);
}

static void TestMaterialGraphRoundTrip(test::TestContext& ctx)
{
    using Node = assets::MaterialNodeType;
    assets::MaterialAsset material;
    material.graph.nodes = {
        {5, Node::Add, 4, 3},
        {4, Node::Multiply, 1, 2},
        {3, Node::Value, 0, 0, {0.1f, 0.f, 0.f}},
        {2, Node::Color, 0, 0, {0.5f, 0.25f, 1.f}},
        {1, Node::Texture, 0, 0, {}, {23.f, 54.f}, "Textures/base color.png"},
    };
    material.graph.baseColor = 5;
    material.graph.emissive = 4;
    material.graph.normal = 1;
    material.graph.orm = 1;
    material.graph.metallic = 3;
    material.graph.roughness = 3;
    material.graph.normalStrength = 3;
    material.graph.occlusion = 3;
    material.roughnessFactor = 0.6f;
    material.normalTexture.path = "Textures/original.png";
    material.emissiveFactor = {0.2f, 0.4f, 0.f};
    material.emissiveStrength = 5.f;
    assets::AssetRegistry registry;
    assets::AssetPipeline pipeline(registry);
    const auto handle = pipeline.LoadMaterialSource(assets::SerializeMaterialProperties(material));
    const auto* loaded = registry.materials.Get(handle);
    CHECK(ctx, loaded != nullptr);
    if (!loaded)
        return;
    CHECK_EQ(ctx, loaded->emissiveStrength, 5.f);
    CHECK_EQ(ctx, loaded->RuntimeEmissive().x, 5.f); // Linked graph: strength only.
    assets::MaterialAsset plainEmissive = material;
    plainEmissive.graph = {};
    CHECK_EQ(ctx, plainEmissive.RuntimeEmissive().y, 2.f); // Color times strength.
    assets::MaterialAsset linkedStrength = material;
    linkedStrength.graph.nodes.push_back({9, Node::Value, 0, 0, {7.f, 0.f, 0.f}});
    linkedStrength.graph.emissiveStrength = 9;
    CHECK_EQ(ctx, linkedStrength.RuntimeEmissive().x, 7.f);
    std::ostringstream writtenOutputs;
    linkedStrength.graph.Write(writtenOutputs);
    assets::MaterialGraph readOutputs;
    std::istringstream outputLines(writtenOutputs.str());
    for (std::string line; std::getline(outputLines, line);)
        readOutputs.Read(line);
    CHECK_EQ(ctx, readOutputs.emissiveStrength, 9u);
    auto graph = loaded->graph;
    const auto compiled = assets::CompileMaterialGraph(graph);
    CHECK(ctx, compiled.Active());
    CHECK_EQ(ctx, compiled.textures[0], std::string("Textures/base color.png"));
    CHECK_EQ(ctx, compiled.outputs.x, 4.f);
    CHECK_EQ(ctx, compiled.outputs.y, 2.f);
    CHECK_EQ(ctx, compiled.outputs.z, 5.f);
    CHECK_EQ(ctx, compiled.instructions[2].w, 3.f);
    CHECK_EQ(ctx, compiled.instructions[4].w, 2.f);
    CHECK_EQ(ctx, graph.nodes.back().position.y, 54.f);
    CHECK_EQ(ctx, loaded->PbrMetallic(), 0.1f);
    CHECK_EQ(ctx, loaded->PbrRoughness(), 0.1f);
    CHECK_EQ(ctx, loaded->PbrNormalStrength(), 0.1f);
    CHECK_EQ(ctx, loaded->PbrOcclusion(), 0.1f);
    CHECK_EQ(ctx, loaded->PbrNormalPath(), std::string("Textures/base color.png"));
    CHECK_EQ(ctx, loaded->PbrOrmPath(), std::string("Textures/base color.png"));
    CHECK_EQ(ctx, graph.ResolveValue(2, material.roughnessFactor, 1.f), 0.6f);
    graph.normal = 0;
    CHECK_EQ(ctx, graph.ResolveTexture(graph.normal, material.normalTexture.path), std::string("Textures/original.png"));

    assets::MaterialGraph oldGraph;
    oldGraph.Read("graphOutputs 5 4");
    CHECK_EQ(ctx, oldGraph.baseColor, 5u);
    CHECK_EQ(ctx, oldGraph.emissive, 4u);
    CHECK_EQ(ctx, oldGraph.metallic, 0u);

    assets::MaterialGraph emptyTexture;
    emptyTexture.nodes.push_back({1, Node::Texture});
    emptyTexture.baseColor = 1;
    const auto compiledEmpty = assets::CompileMaterialGraph(emptyTexture);
    CHECK(ctx, compiledEmpty.Active());
    CHECK(ctx, compiledEmpty.textures[0].empty());
    CHECK_EQ(ctx, compiledEmpty.instructions[0].x, 1.f);
    CHECK_EQ(ctx, compiledEmpty.instructions[0].w, 0.f);

    assets::MaterialGraph strengthGraph;
    strengthGraph.nodes.push_back({1, Node::Texture, 0, 0, {}, {}, "Textures/a.png"});
    strengthGraph.nodes.push_back({2, Node::Color, 0, 0, {0.2f, 0.3f, 0.4f}});
    strengthGraph.nodes.push_back({3, Node::Multiply, 1, 2, {0.5f, 0.25f, 1.f}});
    strengthGraph.baseColor = 3;
    const auto compiledStrength = assets::CompileMaterialGraph(strengthGraph);
    CHECK_EQ(ctx, compiledStrength.instructions[2].y, 0.5f); // Scales the linked texture.
    CHECK_EQ(ctx, compiledStrength.instructions[2].z, 1.f);  // A linked color is edited at its node.

    assets::MaterialGraph mixGraph;
    mixGraph.nodes.push_back({1, Node::Texture, 0, 0, {}, {}, "Textures/a.png"});
    mixGraph.nodes.push_back({2, Node::Separate, 1, 0, {2.f, 1.f, 1.f}});
    mixGraph.nodes.push_back({3, Node::Color, 0, 0, {0.2f, 0.3f, 0.4f}});
    mixGraph.nodes.push_back({4, Node::Mix, 3, 1, {1.f, 1.f, 0.25f}});
    mixGraph.nodes.back().c = 2;
    mixGraph.baseColor = 4;
    mixGraph.roughness = 2;
    const auto compiledMix = assets::CompileMaterialGraph(mixGraph);
    CHECK(ctx, compiledMix.Active());
    CHECK_EQ(ctx, compiledMix.outputs.z, 4.f);
    CHECK_EQ(ctx, compiledMix.outputs.w, 96.f); // Roughness reads instruction 2.
    CHECK_EQ(ctx, compiledMix.instructions[2].w, 5.f);
    CHECK_EQ(ctx, compiledMix.instructions[2].y, 2.f);
    CHECK_EQ(ctx, compiledMix.instructions[3].x, 3137.f);
    CHECK(ctx, std::floor(compiledMix.instructions[3].w) == 4.f);
    CHECK(ctx, compiledMix.instructions[3].w > 4.99f); // The stored 0.25 is ignored with a mask.
    CHECK(ctx, !compiledMix.textureLinear[0]); // Also used as color.

    std::ostringstream writtenGraph;
    mixGraph.Write(writtenGraph);
    assets::MaterialGraph readGraph;
    std::istringstream graphLines(writtenGraph.str());
    for (std::string line; std::getline(graphLines, line);)
        readGraph.Read(line);
    CHECK(ctx, readGraph.Find(4) != nullptr && readGraph.Find(4)->c == 2u);
    CHECK(ctx, readGraph.Find(4) != nullptr && readGraph.Find(4)->type == Node::Mix);

    assets::MaterialGraph dataGraph;
    dataGraph.nodes.push_back({1, Node::Texture, 0, 0, {}, {}, "Textures/rough.png"});
    dataGraph.metallic = 1;
    const auto compiledData = assets::CompileMaterialGraph(dataGraph);
    CHECK(ctx, compiledData.Active());
    CHECK_EQ(ctx, compiledData.outputs.w, 1.f);
    CHECK(ctx, compiledData.textureLinear[0]);

    assets::MaterialGraph uvGraph;
    uvGraph.nodes.push_back({1, Node::Texture, 0, 0, {}, {}, "Textures/a.png"});
    uvGraph.nodes.push_back({2, Node::Texture, 0, 0, {}, {}, "Textures/a.png"});
    uvGraph.nodes.push_back({3, Node::Multiply, 1, 2});
    uvGraph.nodes[1].uvScale = {4.f, 2.f};
    uvGraph.nodes[1].uvOffset = {0.5f, 0.25f};
    uvGraph.baseColor = 3;
    const auto compiledUV = assets::CompileMaterialGraph(uvGraph);
    CHECK_EQ(ctx, compiledUV.textures[1], std::string("Textures/a.png")); // Other UVs, own slot.
    CHECK_EQ(ctx, compiledUV.textureUV[0].x, 1.f);
    CHECK_EQ(ctx, compiledUV.textureUV[1].x, 4.f);
    CHECK_EQ(ctx, compiledUV.textureUV[1].w, 0.25f);
    uvGraph.nodes[1].uvScale = {1.f, 1.f};
    uvGraph.nodes[1].uvOffset = {0.f, 0.f};
    CHECK(ctx, assets::CompileMaterialGraph(uvGraph).textures[1].empty()); // Same UVs share a slot.

    uvGraph.nodes[1].uvScale = {3.f, 5.f};
    std::ostringstream writtenUV;
    uvGraph.Write(writtenUV);
    assets::MaterialGraph readUV;
    std::istringstream uvLines(writtenUV.str());
    for (std::string line; std::getline(uvLines, line);)
        readUV.Read(line);
    CHECK(ctx, readUV.Find(2) != nullptr && readUV.Find(2)->uvScale.y == 5.f);
    assets::MaterialGraph oldFormat;
    oldFormat.Read("graphNode 1 0 0 0 1 1 1 0 0 \"Textures/a.png\"");
    CHECK(ctx, oldFormat.Find(1) != nullptr && oldFormat.Find(1)->uvScale.x == 1.f && oldFormat.Find(1)->c == 0u);

    assets::MaterialGraph mathGraph;
    mathGraph.nodes.push_back({1, Node::Time, 0, 0, {2.f, 1.f, 1.f}});
    mathGraph.nodes.push_back({2, Node::Sine, 1});
    mathGraph.nodes.push_back({3, Node::Remap, 2, 7, {1.f, 0.25f, 0.75f}}); // Unused B is ignored.
    mathGraph.nodes.push_back({4, Node::Invert, 3});
    mathGraph.nodes.push_back({5, Node::Subtract, 4, 0, {1.f, 0.5f, 1.f}});
    mathGraph.baseColor = 5;
    const auto compiledMath = assets::CompileMaterialGraph(mathGraph);
    CHECK(ctx, compiledMath.Active());
    CHECK_EQ(ctx, compiledMath.instructions[0].w, 12.f);
    CHECK_EQ(ctx, compiledMath.instructions[0].y, 2.f);
    CHECK_EQ(ctx, compiledMath.instructions[1].w, 11.f);
    CHECK_EQ(ctx, compiledMath.instructions[2].w, 10.f);
    CHECK_EQ(ctx, compiledMath.instructions[2].y, 0.25f);
    CHECK_EQ(ctx, compiledMath.instructions[3].w, 8.f);
    CHECK_EQ(ctx, compiledMath.instructions[4].w, 6.f);

    assets::MaterialGraph textureLimit;
    uint32_t chainId = 100;
    for (uint32_t i = 1; i <= 5; ++i)
    {
        textureLimit.nodes.push_back({i, Node::Texture, 0, 0, {}, {}, "Textures/t" + std::to_string(i) + ".png"});
    }
    textureLimit.nodes.push_back({chainId, Node::Add, 1, 2});
    textureLimit.nodes.push_back({chainId + 1, Node::Add, chainId, 3});
    textureLimit.nodes.push_back({chainId + 2, Node::Add, chainId + 1, 4});
    textureLimit.baseColor = chainId + 2;
    const auto compiledFour = assets::CompileMaterialGraph(textureLimit);
    CHECK(ctx, compiledFour.Active());
    CHECK_EQ(ctx, compiledFour.textures[3], std::string("Textures/t4.png"));
    textureLimit.nodes.push_back({chainId + 3, Node::Add, chainId + 2, 5});
    textureLimit.baseColor = chainId + 3;
    CHECK(ctx, !assets::CompileMaterialGraph(textureLimit).valid); // A fifth texture is refused.

    assets::MaterialGraph mixCycle = mixGraph;
    mixCycle.Find(1)->type = Node::Multiply;
    mixCycle.Find(1)->a = 4;
    CHECK(ctx, !assets::CompileMaterialGraph(mixCycle).valid);

    assets::MaterialAsset customAsset;
    customAsset.baseColorTexture.path = "Textures/direct.png";
    customAsset.emissiveTexture.path = "Textures/emissive.png";
    customAsset.graph.nodes.push_back({1, Node::Texture, 0, 0, {}, {}, "Textures/node.png"});
    customAsset.graph.baseColor = 1;
    const auto customResolved = customAsset.WithGraphTextures();
    CHECK_EQ(ctx, customResolved.baseColorTexture.path, std::string("Textures/node.png"));
    CHECK_EQ(ctx, customResolved.emissiveTexture.path, std::string("Textures/emissive.png"));
    CHECK(ctx, customResolved.graph.nodes.empty());

    {
        // PerMaterial ends at 64 in the Lit and Unlit shaders; the graph block follows it.
        MaterialSystem graphMaterials;
        renderer::unlit::UnlitMaterialCreateInfo unlitInfo{};
        unlitInfo.materialGraph = true;
        const auto unlitMaterial = renderer::unlit::UnlitMaterial::Register(graphMaterials, unlitInfo);
        CHECK_EQ(ctx, graphMaterials.GetCBLayout(unlitMaterial).GetOffset("graphOutputs"), 64u);
        CHECK_EQ(ctx, graphMaterials.GetCBLayout(unlitMaterial).GetOffset("graphNode7"), 192u);
        renderer::lit::LitMaterialCreateInfo litInfo{};
        litInfo.materialGraph = true;
        const auto litMaterial = renderer::lit::LitMaterial::Register(graphMaterials, litInfo);
        CHECK_EQ(ctx, graphMaterials.GetCBLayout(litMaterial).GetOffset("graphOutputs"), 64u);
        CHECK_EQ(ctx, graphMaterials.GetCBLayout(litMaterial).GetOffset("graphNode7"), 192u);
    }

    graph.nodes[1].a = 5;
    CHECK(ctx, !assets::CompileMaterialGraph(graph).valid);
}

// UV, Position and Vector2 nodes: encoding, a texture sampled at a linked UV,
// UV links of the normal and ORM maps, files with the new types, cycles.
static void TestMaterialGraphUVNodes(test::TestContext& ctx)
{
    using Node = assets::MaterialNodeType;
    assets::MaterialGraph graph;
    graph.nodes = {
        {1, Node::UV, 0, 0, {4.f, 2.f, 1.f}},
        {2, Node::Vector2, 0, 0, {0.5f, 0.25f, 0.f}},
        {3, Node::Add, 1, 2},
        {4, Node::Texture, 3, 0, {}, {}, "Textures/a.png"},
        {5, Node::Position, 0, 0, {0.1f, 2.f, 0.f}},
        {6, Node::Texture, 5, 0, {}, {}, "Textures/normal.png"},
        {7, Node::Texture, 0, 0, {}, {}, "Textures/a.png"},
    };
    graph.nodes[6].uvScale = {3.f, 3.f};
    graph.baseColor = 4;
    graph.emissive = 7;
    graph.normal = 6;
    const auto compiled = assets::CompileMaterialGraph(graph);
    CHECK(ctx, compiled.valid && compiled.Active());
    // Order: UV, Vector2, Add, Texture (base), Texture (emissive), Position (normal UV).
    CHECK_EQ(ctx, compiled.instructions[0].w, 13.f);
    CHECK(ctx, compiled.instructions[0].x == 4.f && compiled.instructions[0].y == 2.f);
    CHECK(ctx, compiled.instructions[1].w == 0.f && compiled.instructions[1].x == 0.5f && compiled.instructions[1].y == 0.25f);
    CHECK_EQ(ctx, compiled.instructions[3].w, 1.f);
    CHECK_EQ(ctx, compiled.instructions[3].y, 3.f); // linked UV = instruction 2, stored + 1
    CHECK(ctx, compiled.textureUV[0].x == 1.f && compiled.textureUV[0].z == 0.f); // linked: identity transform
    // The same file unlinked with its own tiling needs a slot of its own.
    CHECK_EQ(ctx, compiled.instructions[4].y, 0.f);
    CHECK_EQ(ctx, compiled.instructions[4].x, 1.f);
    CHECK_EQ(ctx, compiled.textureUV[1].x, 3.f);
    CHECK(ctx, compiled.instructions[5].w == 14.f && compiled.instructions[5].y == 2.f && compiled.instructions[5].x == 0.1f);
    const int bits = static_cast<int>(compiled.outputs.w);
    CHECK_EQ(ctx, (bits >> 10) & 31, 6); // normal-map UV = instruction 5, stored + 1
    CHECK_EQ(ctx, (bits >> 15) & 31, 0); // ORM map: mesh UV
    CHECK_EQ(ctx, (bits >> 5) & 31, 0);  // roughness unaffected

    // Files: the new types and the texture's UV link survive a round trip.
    std::ostringstream written;
    graph.Write(written);
    assets::MaterialGraph read;
    std::istringstream lines(written.str());
    for (std::string line; std::getline(lines, line);)
        read.Read(line);
    CHECK_EQ(ctx, read.nodes.size(), graph.nodes.size());
    if (read.nodes.size() == graph.nodes.size())
    {
        CHECK(ctx, read.nodes[0].type == Node::UV && read.nodes[4].type == Node::Position && read.nodes[1].type == Node::Vector2);
        CHECK_EQ(ctx, read.nodes[3].a, 3u);
        CHECK_EQ(ctx, read.normal, 6u);
    }
    // An unknown, newer type is skipped instead of misread.
    assets::MaterialGraph future;
    future.Read("graphNode 1 99 0 0 1 1 1 0 0 \"\"");
    CHECK(ctx, future.nodes.empty());

    // A texture whose UV comes from itself is a cycle.
    assets::MaterialGraph cycle;
    cycle.nodes = {{1, Node::Texture, 2, 0, {}, {}, "Textures/a.png"}, {2, Node::Add, 1, 0}};
    cycle.baseColor = 1;
    CHECK(ctx, !assets::CompileMaterialGraph(cycle).valid);

    // The editor draws the new nodes and the texture's UV socket (also zoomed).
    auto meta = CreateEditorCommandsRegistry();
    World world(meta);
    MaterialSystem materials;
    assets::AssetRegistry registry;
    EditorState state;
    auto editor = CreateEditorContext(world, materials, registry, state);
    assets::MaterialAsset material;
    material.graph = graph;
    auto* previous = ImGui::GetCurrentContext();
    auto* gui = ImGui::CreateContext();
    auto& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.DisplaySize = {1000.f, 800.f};
    unsigned char* pixels;
    int width, height;
    io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
    bool drawn = true;
    for (int frame = 0; frame < 4; ++frame)
    {
        if (frame == 2)
            io.AddMouseWheelEvent(0.f, -2.f); // zoom out while hovering nothing in particular
        ImGui::NewFrame();
        ImGui::SetNextWindowPos(ImVec2(10.f, 10.f));
        ImGui::SetNextWindowSize(ImVec2(950.f, 700.f));
        ImGui::Begin("UV graph test");
        DrawMaterialGraph(editor, material, EditorMaterialTemplate::PbrLit);
        ImGui::End();
        ImGui::Render();
        drawn = drawn && ImGui::GetDrawData() != nullptr;
    }
    CHECK(ctx, drawn && material.graph.nodes.size() == graph.nodes.size() && material.graph.nodes[3].a == 3u);
    ImGui::DestroyContext(gui);
    ImGui::SetCurrentContext(previous);
}

static void TestMaterialGraphSockets(test::TestContext& ctx)
{
    auto meta = CreateEditorCommandsRegistry();
    World world(meta);
    MaterialSystem materials;
    assets::AssetRegistry registry;
    EditorState state;
    auto editor = CreateEditorContext(world, materials, registry, state);
    assets::MaterialAsset material;
    material.baseColorTexture.path = "Textures/original.png";
    material.normalTexture.path = "Textures/normal.png";
    auto& graph = material.graph;
    graph.nodes.push_back({1, assets::MaterialNodeType::Value, 0, 0, {}, {14.f, 14.f}});

    auto* previous = ImGui::GetCurrentContext();
    auto* gui = ImGui::CreateContext();
    auto& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.DisplaySize = {1000.f, 800.f};
    unsigned char* pixels;
    int width, height;
    io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
    auto materialType = EditorMaterialTemplate::PbrLit;
    const auto frame = [&](const char* texture = nullptr, std::string* drawnText = nullptr) {
        ImGui::NewFrame();
        if (texture && ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceExtern))
        {
            ImGui::SetDragDropPayload("KROM_ASSET_TEXTURE", texture, std::strlen(texture) + 1);
            ImGui::EndDragDropSource();
        }
        ImGui::SetNextWindowPos(ImVec2(10.f, 10.f));
        ImGui::SetNextWindowSize(ImVec2(850.f, 620.f));
        ImGui::Begin("Graph test");
        if (drawnText)
            ImGui::LogToBuffer(0);
        DrawMaterialGraph(editor, material, materialType);
        if (drawnText)
        {
            *drawnText = gui->LogBuffer.c_str();
            ImGui::LogFinish();
        }
        ImGui::End();
        ImGui::Render();
    };
    frame();
    frame();
    const ImGuiWindow* canvas = nullptr;
    for (auto* window : gui->Windows)
        if (std::strstr(window->Name, "##material_graph_canvas"))
            canvas = window;
    CHECK(ctx, canvas != nullptr);
    if (canvas)
    {
        const ImVec2 origin = canvas->DC.CursorStartPos;
        const float step = ImGui::GetFrameHeightWithSpacing();
        const float y = origin.y + 14.f + 1.5f * step;
        // The material card has a title row above its inputs.
        const float cardY = y + step;
        const float outputX = origin.x + 14.f + 181.f;
        const float inputX = std::max(origin.x + 220.f, canvas->ContentRegionRect.Max.x - 329.f) + 3.f;
        const auto link = [&](float inputY) {
            io.AddMousePosEvent(outputX, y);
            frame();
            io.AddMouseButtonEvent(ImGuiMouseButton_Left, true);
            frame();
            io.AddMousePosEvent(inputX, inputY);
            frame();
            frame();
            io.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
            frame();
        };
        const auto rightClick = [&](float x, float atY) {
            io.AddMousePosEvent(x, atY);
            frame();
            io.AddMouseButtonEvent(ImGuiMouseButton_Right, true);
            frame();
            io.AddMouseButtonEvent(ImGuiMouseButton_Right, false);
            frame();
        };
        link(cardY);
        CHECK_EQ(ctx, graph.baseColor, 1u);
        std::string linkedColorText;
        frame(nullptr, &linkedColorText);
        CHECK(ctx, linkedColorText.find("Alpha") != std::string::npos);
        rightClick(inputX, cardY);
        CHECK_EQ(ctx, graph.baseColor, 0u);
        frame(nullptr, &linkedColorText);
        CHECK(ctx, linkedColorText.find("Alpha") == std::string::npos);
        CHECK_EQ(ctx, material.baseColorTexture.path, std::string("Textures/original.png"));
        CHECK(ctx, gui->OpenPopupStack.empty());

        io.AddMousePosEvent(inputX + 308.f - ImGui::GetFrameHeight() * 0.5f, cardY + step);
        frame();
        io.AddMouseButtonEvent(ImGuiMouseButton_Left, true);
        frame();
        io.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
        frame();
        CHECK(ctx, material.normalTexture.path.empty());

        io.AddMousePosEvent(inputX + 40.f, cardY + step);
        io.AddMouseButtonEvent(ImGuiMouseButton_Left, true);
        frame("Textures/replacement.png");
        frame("Textures/replacement.png");
        io.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
        frame("Textures/replacement.png");
        CHECK_EQ(ctx, material.normalTexture.path, std::string("Textures/replacement.png"));
        CHECK_EQ(ctx, graph.nodes.size(), size_t(1));

        link(cardY + 5.f * step);
        CHECK_EQ(ctx, graph.metallic, 1u);
        link(cardY + 6.f * step);
        CHECK_EQ(ctx, graph.roughness, 1u);
        std::string drawnText;
        frame(nullptr, &drawnText);
        const std::string linkedTitle = std::string(Translate("material.metallic")) + " / " +
                                        Translate("material.roughness");
        CHECK(ctx, drawnText.find(linkedTitle) != std::string::npos);
        rightClick(inputX, cardY + 5.f * step);
        CHECK_EQ(ctx, graph.metallic, 0u);
        CHECK_EQ(ctx, graph.roughness, 1u);
        CHECK(ctx, gui->OpenPopupStack.empty());

        io.AddMousePosEvent(inputX, cardY + 6.f * step);
        frame();
        io.AddMouseButtonEvent(ImGuiMouseButton_Left, true);
        frame();
        io.AddMousePosEvent(origin.x + 230.f, origin.y + 100.f);
        frame();
        io.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
        frame();
        CHECK_EQ(ctx, graph.roughness, 0u);
        frame(nullptr, &drawnText);
        CHECK(ctx, drawnText.find(linkedTitle) == std::string::npos);
        CHECK(ctx, drawnText.find(Translate("material.graph_value")) != std::string::npos);

        rightClick(origin.x + 230.f, origin.y + 100.f);
        CHECK(ctx, !gui->OpenPopupStack.empty());
        frame();
        if (!gui->OpenPopupStack.empty())
        {
            const auto* popup = gui->OpenPopupStack.back().Window;
            CHECK(ctx, popup != nullptr);
            if (popup)
            {
                const ImVec2 start = popup->DC.CursorStartPos;
                const float addY = start.y + ImGui::GetTextLineHeightWithSpacing() + step + ImGui::GetFrameHeight() * 0.5f;
                io.AddMousePosEvent(start.x + 10.f, addY);
                frame();
                io.AddMouseButtonEvent(ImGuiMouseButton_Left, true);
                frame();
                io.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
                frame();
                CHECK_EQ(ctx, graph.nodes.size(), size_t(2));
                if (graph.nodes.size() == 2)
                {
                    // Placed where the canvas was clicked, snapped to the 20-unit grid.
                    CHECK_EQ(ctx, graph.nodes.back().position.x, 240.f);
                    CHECK_EQ(ctx, graph.nodes.back().position.y, 100.f);
                }
            }
        }

        for (auto type : {EditorMaterialTemplate::Unlit, EditorMaterialTemplate::LegacyLit, EditorMaterialTemplate::Custom})
        {
            materialType = type;
            frame();
            CHECK(ctx, gui->OpenPopupStack.empty());
            rightClick(origin.x + 230.f, origin.y + 420.f);
            CHECK(ctx, !gui->OpenPopupStack.empty());
            io.AddMousePosEvent(origin.x + 5.f, origin.y + 5.f);
            frame();
            io.AddMouseButtonEvent(ImGuiMouseButton_Left, true);
            frame();
            io.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
            frame();
        }
    }
    ImGui::DestroyContext(gui);
    ImGui::SetCurrentContext(previous);
}

static void TestMaterialParameterPerObject(test::TestContext& ctx)
{
    auto meta = CreateEditorCommandsRegistry();
    RegisterMeshRendererComponents(meta);
    World world(meta);
    MaterialSystem materials;
    renderer::unlit::UnlitMaterialCreateInfo info{};
    info.materialGraph = true;
    const auto base = renderer::unlit::UnlitMaterial::Register(materials, info);

    assets::MaterialGraph graph;
    graph.nodes.push_back({1, assets::MaterialNodeType::Parameter, 0, 0, {1.f, 0.f, 0.f}, {}, "Glow"});
    graph.baseColor = 1;
    const auto compiled = assets::CompileMaterialGraph(graph);
    CHECK_EQ(ctx, compiled.parameters.size(), size_t(1));
    renderer::SetMaterialGraph(materials, base, compiled, std::array<TextureHandle, assets::kMaterialGraphMaxTextures>{});

    const auto nodeValue = [&](MaterialHandle handle) {
        const auto* instance = materials.GetInstance(handle);
        for (const auto& param : instance->parameterOverrides)
            if (param.name == "graphNode0")
                return param.value.f[0];
        return -1.f;
    };
    const EntityID a = world.CreateEntity();
    const EntityID b = world.CreateEntity();
    world.Add<MaterialComponent>(a).material = base;
    world.Add<MaterialComponent>(b).material = base;

    gameplay::MaterialParameterInstances instances;
    CHECK(ctx, instances.Set(materials, world, a, "Glow", {5.f, 5.f, 5.f}));
    const MaterialHandle own = world.Get<MaterialComponent>(a)->material;
    CHECK(ctx, own != base);
    CHECK(ctx, world.Get<MaterialComponent>(b)->material == base);
    CHECK_EQ(ctx, nodeValue(own), 5.f);
    CHECK_EQ(ctx, nodeValue(base), 1.f); // Other objects keep the shared value.

    CHECK(ctx, instances.Set(materials, world, a, "Glow", {2.f, 2.f, 2.f}));
    CHECK(ctx, world.Get<MaterialComponent>(a)->material == own);
    CHECK_EQ(ctx, nodeValue(own), 2.f);
    CHECK_EQ(ctx, instances.Count(), size_t(1));
    CHECK(ctx, !instances.Set(materials, world, a, "Unknown", {1.f, 1.f, 1.f}));
    CHECK_EQ(ctx, instances.Count(), size_t(1));

    world.DestroyEntity(a);
    instances.CollectDestroyed(world);
    CHECK_EQ(ctx, instances.Count(), size_t(0));
    CHECK(ctx, materials.GetInstance(own) == nullptr);
}

static void TestMaterialGraphUndo(test::TestContext& ctx)
{
    auto meta = CreateEditorCommandsRegistry();
    World world(meta);
    MaterialSystem materials;
    assets::AssetRegistry registry;
    EditorState state;
    auto editor = CreateEditorContext(world, materials, registry, state);
    const auto root = std::filesystem::absolute("out/material-graph-undo/" +
        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(root);
    assets::AssetPipeline pipeline(registry);
    pipeline.SetAssetRoot(root);
    editor.assetPipeline = &pipeline;

    state.materialWindowOpen = true;
    state.selectedMaterialAssetLoaded = true;
    state.selectedMaterialAssetPath = PathToUtf8(root / "test.mat");
    auto& asset = state.selectedMaterialAssetData;
    asset.graph.nodes.push_back({1, assets::MaterialNodeType::Value, 0, 0, {}, {14.f, 14.f}});

    auto* previous = ImGui::GetCurrentContext();
    auto* gui = ImGui::CreateContext();
    auto& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.DisplaySize = {1600.f, 900.f};
    unsigned char* pixels;
    int width, height;
    io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
    const auto frame = [&] {
        ImGui::NewFrame();
        DrawMaterialLibrary(editor);
        ImGui::Render();
    };
    frame();
    frame();
    ImGuiWindow* canvas = nullptr;
    for (auto* window : gui->Windows)
        if (std::strstr(window->Name, "##material_graph_canvas"))
            canvas = window;
    CHECK(ctx, canvas != nullptr);
    if (canvas)
    {
        const ImVec2 origin = canvas->DC.CursorStartPos;
        const float titleY = origin.y + 14.f + ImGui::GetFrameHeight() * 0.5f;
        io.AddMousePosEvent(origin.x + 40.f, titleY);
        frame();
        io.AddMouseButtonEvent(ImGuiMouseButton_Left, true);
        frame();
        for (int i = 1; i <= 3; ++i)
        {
            io.AddMousePosEvent(origin.x + 40.f + 20.f * static_cast<float>(i), titleY);
            frame();
        }
        CHECK(ctx, !CanUndoEditorHistory(state)); // Still dragging.
        io.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
        frame();
        frame();
        CHECK(ctx, asset.graph.nodes[0].position.x > 14.f);
        const float moved = asset.graph.nodes[0].position.x;

        CHECK(ctx, UndoEditorHistory(editor));
        CHECK_EQ(ctx, asset.graph.nodes[0].position.x, 14.f);
        CHECK(ctx, !CanUndoEditorHistory(state)); // The whole drag was one step.
        CHECK(ctx, RedoEditorHistory(editor));
        CHECK_EQ(ctx, asset.graph.nodes[0].position.x, moved);
        CHECK(ctx, UndoEditorHistory(editor));
        frame();

        io.AddMousePosEvent(origin.x + 14.f + 184.f - 13.f, titleY);
        frame();
        io.AddMouseButtonEvent(ImGuiMouseButton_Left, true);
        frame();
        io.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
        frame();
        frame();
        CHECK(ctx, asset.graph.nodes.empty());
        CHECK(ctx, UndoEditorHistory(editor));
        CHECK_EQ(ctx, asset.graph.nodes.size(), size_t(1));
    }
    ImGui::DestroyContext(gui);
    ImGui::SetCurrentContext(previous);
}

static void TestCustomMaterialGraph(test::TestContext& ctx)
{
    auto meta = CreateEditorCommandsRegistry();
    World world(meta);
    MaterialSystem materials;
    assets::AssetRegistry registry;
    EditorState state;
    auto editor = CreateEditorContext(world, materials, registry, state);
    const auto root = std::filesystem::absolute("out/custom-material-graph/" +
        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(root);
    assets::AssetPipeline pipeline(registry);
    pipeline.SetAssetRoot(root);
    editor.assetPipeline = &pipeline;
    const std::string shaderFile = PathToUtf8(root / "existing.vs.hlsl");
    const std::string oldSource = "float4 main(float3 p : POSITION) : SV_Position { return float4(p, 1); }";
    std::ofstream(PathFromUtf8(shaderFile)) << oldSource;

    state.materialWindowOpen = true;
    state.selectedMaterialAssetLoaded = true;
    state.selectedMaterialAssetPath = PathToUtf8(root / "test.mat");
    auto& asset = state.selectedMaterialAssetData;

    auto* previous = ImGui::GetCurrentContext();
    auto* gui = ImGui::CreateContext();
    auto& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.DisplaySize = {1600.f, 900.f};
    unsigned char* pixels;
    int width, height;
    io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
    const auto frame = [&](const char* file = nullptr) {
        ImGui::NewFrame();
        if (file && ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceExtern |
                                                ImGuiDragDropFlags_SourceNoPreviewTooltip))
        {
            ImGui::SetDragDropPayload("KROM_MOVE_FILE", file, std::strlen(file) + 1u);
            ImGui::EndDragDropSource();
        }
        DrawMaterialLibrary(editor);
        ImGui::Render();
    };
    const auto click = [&](ImVec2 pos) {
        io.AddMousePosEvent(pos.x, pos.y);
        frame();
        io.AddMouseButtonEvent(ImGuiMouseButton_Left, true);
        frame();
        io.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
        frame();
    };

    frame();
    frame();
    ImGuiWindow* canvas = nullptr;
    for (auto* window : gui->Windows)
        if (std::strstr(window->Name, "##material_graph_canvas"))
            canvas = window;
    CHECK(ctx, canvas != nullptr);
    if (canvas)
    {
        const ImGuiID typeId = canvas->GetID("##material_type");
        ImGui::ActivateItemByID(typeId);
        frame();
        frame();
        CHECK(ctx, !gui->OpenPopupStack.empty());
        if (!gui->OpenPopupStack.empty() && gui->OpenPopupStack.back().Window)
        {
            const ImVec2 comboStart = gui->OpenPopupStack.back().Window->DC.CursorStartPos;
            click({comboStart.x + 12.f, comboStart.y + 3.f * ImGui::GetTextLineHeightWithSpacing() +
                                                       ImGui::GetTextLineHeight() * 0.5f});
        }
        CHECK_EQ(ctx, asset.templateName, std::string("custom"));
        frame();
        CHECK(ctx, canvas->Active);
        CHECK_EQ(ctx, canvas->GetID("##material_type"), typeId);

        const ImVec2 origin = canvas->DC.CursorStartPos;
        const float step = ImGui::GetFrameHeightWithSpacing();
        io.AddMousePosEvent(origin.x + 48.f, origin.y + 14.f + 1.5f * step);
        frame();
        io.AddMouseButtonEvent(ImGuiMouseButton_Left, true);
        frame(shaderFile.c_str());
        frame(shaderFile.c_str());
        io.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
        frame(shaderFile.c_str());
        CHECK_EQ(ctx, PathFromUtf8(asset.vertexShaderPath).filename(), PathFromUtf8("existing.vs.hlsl"));
        const auto* vertex = registry.shaders.Get(asset.vertexShader);
        CHECK(ctx, vertex && vertex->stage == assets::ShaderStage::Vertex);

        const ImGuiID stageId = canvas->GetID("material.graph_pixel_shader");
        ImGui::ActivateItemByID(ImHashStr(Translate("material.graph_shader_text"), 0, stageId));
        frame();
        frame();
        CHECK(ctx, !gui->OpenPopupStack.empty());
        if (!gui->OpenPopupStack.empty() && gui->OpenPopupStack.back().Window)
        {
            const std::string code = "float4 main() : SV_Target { return float4(1, 0, 0, 1); }";
            state.materialShaderDraft = code;
            state.materialShaderDraftLanguage = 0;
            auto* modal = gui->OpenPopupStack.back().Window;
            ImGui::ActivateItemByID(modal->GetID(Translate("material.graph_shader_save")));
            frame();
            frame();
            CHECK_EQ(ctx, PathFromUtf8(asset.fragmentShaderPath).filename(), PathFromUtf8("test.ps.hlsl"));
            const auto* pixel = registry.shaders.Get(asset.fragmentShader);
            CHECK(ctx, pixel && pixel->stage == assets::ShaderStage::Fragment);
            std::ifstream saved(root / "test.ps.hlsl", std::ios::binary);
            const std::string savedCode((std::istreambuf_iterator<char>(saved)), std::istreambuf_iterator<char>());
            CHECK_EQ(ctx, savedCode, code);
            std::ifstream original(PathFromUtf8(shaderFile), std::ios::binary);
            const std::string originalCode((std::istreambuf_iterator<char>(original)), std::istreambuf_iterator<char>());
            CHECK_EQ(ctx, originalCode, oldSource);
        }
    }
    ImGui::DestroyContext(gui);
    ImGui::SetCurrentContext(previous);
}

static void TestMaterialPropertyWidthStaysFixed(test::TestContext& ctx)
{
    auto meta = CreateEditorCommandsRegistry();
    World world(meta);
    MaterialSystem materials;
    assets::AssetRegistry registry;
    EditorState state;
    auto editor = CreateEditorContext(world, materials, registry, state);
    state.materialWindowOpen = true;
    state.selectedMaterialAssetLoaded = true;
    state.selectedMaterialAssetPath = "out/material-property-width/test.mat";
    auto& material = state.selectedMaterialAssetData;
    material.metallicFactor = 0.7f;
    material.graph.nodes.push_back({1, assets::MaterialNodeType::Value, 0, 0, {0.8f, 0.f, 0.f}});
    material.graph.metallic = 1;

    auto* previous = ImGui::GetCurrentContext();
    auto* gui = ImGui::CreateContext();
    auto& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.DisplaySize = {1800.f, 1100.f};
    unsigned char* pixels;
    int width, height;
    io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
    const auto frame = [&] {
        ImGui::NewFrame();
        DrawMaterialLibrary(editor);
        ImGui::Render();
    };
    frame();
    frame();

    auto* window = ImGui::FindWindowByName("Materialeditor##editor");
    ImGuiWindow* graph = nullptr;
    bool oldPanelVisible = false;
    for (auto* child : gui->Windows)
    {
        if (std::strstr(child->Name, "##material_graph_canvas"))
            graph = child;
        oldPanelVisible |= child->Active && (std::strstr(child->Name, "##material_properties_pane") ||
                                             std::strstr(child->Name, "##material_texture_cards"));
    }
    CHECK(ctx, window && graph);
    CHECK(ctx, !oldPanelVisible);
    if (window && graph)
    {
        const ImGuiID slider = ImHashStr("##value", 0, graph->GetID("material.metallic"));
        const auto sliderWidth = [&](float windowWidth) {
            ImGui::SetWindowSize(window->Name, ImVec2(windowWidth, 900.f));
            ImGui::ActivateItemByID(slider);
            frame();
            CHECK_EQ(ctx, gui->NavId, slider);
            return graph->NavRectRel[ImGuiNavLayer_Main].GetWidth();
        };
        const float normal = sliderWidth(980.f);
        const float wide = sliderWidth(1480.f);
        CHECK_GT(ctx, normal, 0.f);
        CHECK(ctx, wide < 400.f);
        CHECK_EQ(ctx, wide, normal);

        const ImRect track = ImGui::WindowRectRelToAbs(graph, graph->NavRectRel[ImGuiNavLayer_Main]);
        const ImVec2 mouse(track.Min.x + track.GetWidth() * 0.2f, track.GetCenter().y);
        io.AddMousePosEvent(mouse.x, mouse.y);
        frame();
        io.AddMouseButtonEvent(ImGuiMouseButton_Left, true);
        frame();
        io.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
        frame();
        CHECK(ctx, material.PbrMetallic() < 0.4f);
        CHECK_EQ(ctx, material.metallicFactor, 0.7f);
        material.graph.metallic = 0;
        CHECK_EQ(ctx, material.PbrMetallic(), 0.7f);
    }

    ImGui::DestroyContext(gui);
    ImGui::SetCurrentContext(previous);
}

static void TestMaterialPreviewMouseWheel(test::TestContext& ctx)
{
    auto meta = CreateEditorCommandsRegistry();
    World world(meta);
    MaterialSystem materials;
    assets::AssetRegistry registry;
    EditorState state;
    auto editor = CreateEditorContext(world, materials, registry, state);
    state.materialWindowOpen = true;
    state.selectedMaterialAssetLoaded = true;
    state.selectedMaterialAssetPath = "out/material-preview-scroll/test.mat";
    state.selectedMaterialAssetData.graph.nodes.push_back(
        {1, assets::MaterialNodeType::Value, 0, 0, {}, {14.f, 650.f}});

    auto* previous = ImGui::GetCurrentContext();
    auto* gui = ImGui::CreateContext();
    auto& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.DisplaySize = {1600.f, 540.f}; // A node lies below the visible area.
    io.DeltaTime = 1.f / 60.f;
    unsigned char* pixels;
    int width, height;
    io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
    const auto frame = [&] {
        ImGui::NewFrame();
        DrawMaterialLibrary(editor);
        ImGui::Render();
    };
    frame();
    frame();

    const ImGuiWindow* preview = nullptr;
    const ImGuiWindow* board = nullptr;
    for (auto* child : gui->Windows)
    {
        if (std::strstr(child->Name, "##material_preview_pane"))
            preview = child;
        if (std::strstr(child->Name, "##material_graph_canvas"))
            board = child;
    }
    CHECK(ctx, preview && board);
    if (preview && board)
    {
        CHECK(ctx, preview->Pos.x < board->Pos.x);
        const float boardBottom = board->Pos.y + board->Size.y;
        CHECK_GT(ctx, 2.f, std::abs(boardBottom - (preview->Pos.y + preview->Size.y)));
        const float initialDistance = state.materialPreviewDistance;
        io.AddMousePosEvent(preview->DC.CursorStartPos.x + 40.f, preview->Pos.y + 100.f);
        frame();
        io.AddMouseWheelEvent(0.f, -1.f);
        frame();
        CHECK_GT(ctx, state.materialPreviewDistance, initialDistance);
        CHECK_EQ(ctx, board->Scroll.y, 0.f);

        // Over the graph the wheel zooms the graph (it never scrolls), not the preview.
        const float zoomedDistance = state.materialPreviewDistance;
        auto* boardWindow = const_cast<ImGuiWindow*>(board);
        const ImGuiID zoomId = boardWindow->GetID("##canvas_zoom");
        io.AddMousePosEvent(board->DC.CursorStartPos.x + 40.f, board->InnerRect.Max.y - 12.f);
        frame();
        io.AddMouseWheelEvent(0.f, -1.f);
        frame();
        CHECK_GT(ctx, 1.f, boardWindow->StateStorage.GetFloat(zoomId, 1.f));
        CHECK_EQ(ctx, board->Scroll.y, 0.f);
        CHECK_EQ(ctx, state.materialPreviewDistance, zoomedDistance);
    }
    ImGui::DestroyContext(gui);
    ImGui::SetCurrentContext(previous);
}

static void TestInspectorRevealsScriptAsset(test::TestContext& ctx)
{
    const auto root = std::filesystem::absolute("out/script-reveal/" +
        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    const auto scripts = root / "Assets" / "Scripts" / PathFromUtf8("Steuerung ä");
    std::filesystem::create_directories(scripts);
    // Dateiname und Instanzname unterscheiden sich bewusst von der Klasse.
    const auto source = scripts / "Movement.cpp";
    const auto header = scripts / "Movement.hpp";
    for (int i = 0; i < 30; ++i)
    {
        std::ofstream out(scripts / ("A" + std::to_string(i) + ".cpp"));
        out << "// Other asset\n";
    }
    { std::ofstream out(header); out << "// KROM_SCRIPT(PlayerController)\n"; }
    { std::ofstream out(source); out << "#include \"Movement.hpp\"\n"; }
    { std::ofstream out(scripts / "Inline.h"); out << "// KROM_SCRIPT(InlineController)\n"; }

    auto meta = CreateEditorCommandsRegistry();
    RegisterComponent<script::ScriptList>(meta, "ScriptList");
    World world(meta);
    MaterialSystem materials;
    assets::AssetRegistry registry;
    EditorState state;
    auto editor = CreateEditorContext(world, materials, registry, state);
    editor.currentProjectRoot = PathToUtf8(root);
    AssetBrowserState browser;
    state.assetBrowser = &browser;
    CHECK_EQ(ctx, FindCppScriptAssetPath(editor, "PlayerController"), source);
    CHECK_EQ(ctx, FindCppScriptAssetPath(editor, "InlineController"), scripts / "Inline.h");
    CHECK(ctx, FindCppScriptAssetPath(editor, "MissingController").empty());
    const auto entity = world.CreateEntity();
    world.Add<script::ScriptList>(entity).Instances_Mutable().emplace_back(
        "PlayerController", "Custom instance", nullptr);
    state.selectedEntity = entity;

    auto* previous = ImGui::GetCurrentContext();
    auto* gui = ImGui::CreateContext();
    auto& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.DisplaySize = {1280.f, 900.f};
    io.DeltaTime = 1.f / 60.f;
    unsigned char* pixels;
    int width, height;
    io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
    auto frame = [&]() {
        ImGui::NewFrame();
        if (editor.assetsPanelVisible)
        {
            ImGui::SetNextWindowPos({500.f, 0.f});
            ImGui::SetNextWindowSize({780.f, 300.f});
            DrawAssetBrowser(editor, browser);
        }
        ImGui::SetNextWindowPos({500.f, 320.f});
        ImGui::SetNextWindowSize({780.f, 250.f});
        DrawEditorConsole(editor);
        ImGui::SetNextWindowPos({0.f, 0.f});
        ImGui::SetNextWindowSize({480.f, 850.f});
        engine::renderer::addons::editor::detail::DrawInspector(editor);
        ImGui::Render();
    };
    frame(); frame();
    auto* inspector = ImGui::FindWindowByName("Inspector##editor");
    const ImGuiID entityScope = inspector->GetID(static_cast<int>(entity.value));
    const ImGuiID sectionScope = ImHashStr("ScriptSection", 0, entityScope);
    const int instanceIndex = 0;
    const ImGuiID instanceScope = ImHashData(&instanceIndex, sizeof(instanceIndex), sectionScope);
    const ImGuiID treeId = ImHashStr("Custom instance##si0", 0, instanceScope);
    const ImGuiID linkId = ImHashStr("PlayerController", 0, treeId);

    // Auch bei verstecktem Panel wird der Browser wieder eingeblendet.
    editor.assetsPanelVisible = false;
    ImGui::ActivateItemByID(linkId);
    frame();
    CHECK(ctx, editor.assetsPanelVisible);
    CHECK_EQ(ctx, browser.highlightPath, source);
    CHECK_EQ(ctx, gui->NavId, linkId);
    const ImRect linkRect = ImGui::WindowRectRelToAbs(inspector, inspector->NavRectRel[ImGuiNavLayer_Main]);
    const ImVec2 headerPoint{
        linkRect.Min.x,
        linkRect.Min.y - ImGui::GetStyle().ItemSpacing.y - ImGui::GetFontSize() * 0.5f};
    frame(); frame();
    CHECK_EQ(ctx, browser.currentDir, scripts);
    CHECK_EQ(ctx, browser.selectedScriptPath, source);
    CHECK(ctx, browser.highlightPath.empty());
    CHECK(ctx, browser.expandedDirs.count(PathToUtf8(scripts.parent_path())) > 0);
    CHECK_EQ(ctx, state.selectedEntity, entity);
    for (auto* window : gui->Windows)
        if (std::strstr(window->Name, "##content_panel"))
            CHECK_GT(ctx, window->Scroll.y, 0.f);

    // Klick auf den Instanznamen fuehrt ebenfalls zum Script und klappt es nicht ein.
    io.AddMousePosEvent(headerPoint.x, headerPoint.y);
    frame();
    CHECK_EQ(ctx, gui->HoveredId, treeId);
    io.AddMouseButtonEvent(ImGuiMouseButton_Left, true);
    frame();
    CHECK_EQ(ctx, browser.highlightPath, source);
    CHECK_EQ(ctx, inspector->StateStorage.GetInt(treeId, 1), 1);
    io.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
    frame(); frame();

    // Showing the same asset moves focus from the separate console to Assets.
    state.consoleFocusRequest = true;
    frame(); frame();
    const auto* consoleWindow = ImGui::FindWindowByName("Console##editor");
    CHECK(ctx, consoleWindow && gui->NavWindow == consoleWindow);
    ImGui::ActivateItemByID(linkId);
    frame(); frame(); frame();
    const auto* assetsWindow = ImGui::FindWindowByName("Assets##editor");
    CHECK(ctx, assetsWindow && gui->NavWindow == assetsWindow);
    CHECK_EQ(ctx, browser.selectedScriptPath, source);
    // Nach dem Sprung kann der Nutzer wieder frei navigieren.
    browser.currentDir = root;
    frame();
    CHECK_EQ(ctx, browser.currentDir, root);
    ImGui::DestroyContext(gui);
    ImGui::SetCurrentContext(previous);
}

static void TestInspectorRevealsEntityReference(test::TestContext& ctx)
{
    struct References final : script::ComponentScript { EntityID door{}; };
    auto meta = CreateEditorCommandsRegistry();
    RegisterComponent<script::ScriptList>(meta, "ScriptList");
    World world(meta);
    MaterialSystem materials;
    assets::AssetRegistry assets;
    EditorState state;
    auto editor = CreateEditorContext(world, materials, assets, state);
    script::ScriptRegistry registry;
    registry.Register<References>("References");
    registry.RegisterField<References>("References", "door", script::ScriptFieldType::Entity, &References::door);
    editor.scriptRegistry = &registry;
    const EntityID source = world.CreateEntity();
    for (int i = 0; i < 45; ++i)
        world.Add<NameComponent>(world.CreateEntity(), NameComponent{"Other object"});
    const EntityID parent = world.CreateEntity();
    const EntityID target = world.CreateEntity();
    const EntityID replacement = world.CreateEntity();
    world.Add<NameComponent>(parent, NameComponent{"Closed parent"});
    world.Add<NameComponent>(target, NameComponent{"Gate"});
    world.Add<NameComponent>(replacement, NameComponent{"Replacement"});
    world.Add<GuidComponent>(target, GuidComponent{"gate-guid"});
    world.Add<ParentComponent>(target, ParentComponent{parent});
    auto& scripts = world.Add<script::ScriptList>(source);
    scripts.Add("References", source, registry);
    script::ScriptFieldValue reference;
    reference.type = script::ScriptFieldType::Entity;
    reference.stringValue = "gate-guid"; // Referenz nach dem Laden, noch ohne EntityID.
    scripts.Instances_Mutable()[0].fieldValues["door"] = reference;
    state.selectedEntity = source;
    state.selectionSource = SelectionSource::Viewport;

    auto* previous = ImGui::GetCurrentContext();
    auto* gui = ImGui::CreateContext();
    auto& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.DisplaySize = {1280.f, 900.f};
    io.DeltaTime = 1.f / 60.f;
    unsigned char* pixels;
    int width, height;
    io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
    auto frame = [&]() {
        ImGui::NewFrame();
        if (editor.hierarchyPanelVisible)
        {
            ImGui::SetNextWindowPos({500.f, 0.f});
            ImGui::SetNextWindowSize({380.f, 300.f});
            engine::renderer::addons::editor::detail::DrawEntityList(editor);
        }
        ImGui::SetNextWindowPos({0.f, 0.f});
        ImGui::SetNextWindowSize({480.f, 850.f});
        engine::renderer::addons::editor::detail::DrawInspector(editor);
        ImGui::Render();
    };
    frame(); frame();
    auto* inspector = ImGui::FindWindowByName("Inspector##editor");
    auto* hierarchy = ImGui::FindWindowByName("Hierarchie##editor");
    const ImGuiID parentId = hierarchy->GetID(reinterpret_cast<void*>(static_cast<uintptr_t>(parent.value)));
    hierarchy->StateStorage.SetInt(parentId, 0);
    const ImGuiID entityScope = inspector->GetID(static_cast<int>(source.value));
    const ImGuiID sectionScope = ImHashStr("ScriptSection", 0, entityScope);
    const int index = 0;
    const ImGuiID instanceScope = ImHashData(&index, sizeof(index), sectionScope);
    const ImGuiID treeId = ImHashStr("References##si0", 0, instanceScope);
    const ImGuiID fieldScope = ImHashStr("door", 0, treeId);
    const ImGuiID linkId = ImHashStr("###reveal", 0, fieldScope);
    const ImGuiID comboId = ImHashStr("##value", 0, fieldScope);

    editor.hierarchyPanelVisible = false;
    ImGui::ActivateItemByID(linkId);
    frame();
    CHECK(ctx, editor.hierarchyPanelVisible);
    CHECK_EQ(ctx, state.hierarchyRevealEntity, target);
    frame(); frame();
    CHECK_EQ(ctx, state.selectedEntity, source);
    CHECK_EQ(ctx, state.selectionSource, SelectionSource::Viewport);
    CHECK_EQ(ctx, state.hierarchyHighlightedEntity, target);
    CHECK(ctx, !state.hierarchyRevealEntity.IsValid());
    CHECK_EQ(ctx, hierarchy->StateStorage.GetInt(parentId), 1);
    CHECK_GT(ctx, hierarchy->Scroll.y, 0.f);
    CHECK_EQ(ctx, scripts.Instances()[0].fieldValues.at("door").stringValue, std::string("gate-guid"));

    // Den sichtbaren Rahmen anklicken und damit erst jetzt die Auswahl wechseln.
    const ImU32 highlightColor = ImGui::GetColorU32(theme::kAccentHover);
    ImRect highlightRect(FLT_MAX, FLT_MAX, -FLT_MAX, -FLT_MAX);
    for (const auto& vertex : hierarchy->DrawList->VtxBuffer)
        if (vertex.col == highlightColor)
            highlightRect.Add(vertex.pos);
    CHECK(ctx, !highlightRect.IsInverted());
    if (!highlightRect.IsInverted())
    {
        const ImVec2 click = highlightRect.GetCenter();
        io.AddMousePosEvent(click.x, click.y);
        frame();
        io.AddMouseButtonEvent(ImGuiMouseButton_Left, true);
        frame();
        io.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
        frame();
    }
    CHECK_EQ(ctx, state.selectedEntity, target);
    CHECK_EQ(ctx, state.selectionSource, SelectionSource::Outliner);
    CHECK(ctx, !state.hierarchyHighlightedEntity.IsValid());

    // Der Pfeil oeffnet weiterhin die Zuweisung, ohne zum Objekt zu navigieren.
    state.selectedEntity = source;
    frame();
    ImGui::ActivateItemByID(comboId);
    frame(); frame();
    CHECK_EQ(ctx, state.selectedEntity, source);
    CHECK(ctx, !state.hierarchyRevealEntity.IsValid());
    ImGuiWindow* popup = nullptr;
    for (auto* window : gui->Windows)
        if (window->Active && std::strstr(window->Name, "##Combo_")) popup = window;
    CHECK(ctx, popup != nullptr);
    if (popup)
    {
        const auto label = "Replacement #" + std::to_string(replacement.value);
        ImGui::ActivateItemByID(popup->GetID(label.c_str()));
        frame(); frame();
        CHECK_EQ(ctx, scripts.Instances()[0].fieldValues.at("door").entityValue, replacement);
        CHECK_EQ(ctx, state.selectedEntity, source);
        CHECK(ctx, !state.hierarchyRevealEntity.IsValid());
    }
    ImGui::DestroyContext(gui);
    ImGui::SetCurrentContext(previous);
}

static void TestHierarchyMarksCarriedPersistentObjects(test::TestContext& ctx)
{
    auto meta = CreateEditorCommandsRegistry();
    World world(meta);
    MaterialSystem materials;
    assets::AssetRegistry registry;
    EditorState state;
    auto editor = CreateEditorContext(world, materials, registry, state);
    const EntityID light = world.CreateEntity();
    world.Add<NameComponent>(light, NameComponent{"Directional Light"});
    world.Add<EditorSceneTagComponent>(light, EditorSceneTagComponent{"Room 2"});
    world.Add<EditorPersistentComponent>(light);

    auto* previous = ImGui::GetCurrentContext();
    auto* gui = ImGui::CreateContext();
    auto& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.DisplaySize = {1280.f, 900.f};
    unsigned char* pixels;
    int width, height;
    io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
    const auto hasMarker = [&] {
        ImGui::NewFrame();
        ImGui::SetNextWindowPos({0.f, 0.f});
        ImGui::SetNextWindowSize({380.f, 300.f});
        engine::renderer::addons::editor::detail::DrawEntityList(editor);
        ImGui::Render();
        const auto* hierarchy = ImGui::FindWindowByName("Hierarchie##editor");
        const ImU32 marker = ImGui::GetColorU32(theme::kWarning);
        return hierarchy && std::any_of(hierarchy->DrawList->VtxBuffer.begin(),
            hierarchy->DrawList->VtxBuffer.end(), [marker](const ImDrawVert& vertex) {
                return vertex.col == marker;
            });
    };

    editor.currentEditorSceneName = "Room 2";
    editor.loadedEditorScenes = {"Room 2"};
    CHECK(ctx, !hasMarker());
    editor.currentEditorSceneName = "Car";
    editor.loadedEditorScenes = {"Car"};
    CHECK(ctx, hasMarker());
    editor.loadedEditorScenes.push_back("Room 2");
    CHECK(ctx, !hasMarker());
    editor.loadedEditorScenes.pop_back();
    world.Remove<EditorPersistentComponent>(light);
    CHECK(ctx, !hasMarker());

    ImGui::DestroyContext(gui);
    ImGui::SetCurrentContext(previous);
}

static void TestHierarchyFoldoutsSurvivePlay(test::TestContext& ctx)
{
    auto meta = CreateEditorCommandsRegistry();
    World world(meta);
    MaterialSystem materials;
    assets::AssetRegistry assets;
    EditorState state;
    auto editor = CreateEditorContext(world, materials, assets, state);
    const EntityID root = world.CreateEntity();
    const EntityID branch = world.CreateEntity();
    const EntityID leaf = world.CreateEntity();
    world.Add<ParentComponent>(branch, ParentComponent{root});
    world.Add<ParentComponent>(leaf, ParentComponent{branch});
    state.selectedEntity = branch;

    auto* previous = ImGui::GetCurrentContext();
    auto* gui = ImGui::CreateContext();
    auto& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.DisplaySize = {1280.f, 900.f};
    io.DeltaTime = 1.f / 60.f;
    unsigned char* pixels;
    int width, height;
    io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
    auto frame = [&]() {
        ImGui::NewFrame();
        ImGui::SetNextWindowPos({0.f, 0.f});
        ImGui::SetNextWindowSize({380.f, 300.f});
        engine::renderer::addons::editor::detail::DrawEntityList(editor);
        ImGui::Render();
    };
    frame();
    auto* hierarchy = ImGui::FindWindowByName("Hierarchie##editor");
    const uintptr_t rootKey = root.value;
    const uintptr_t branchKey = branch.value;
    const ImGuiID rootId = ImHashData(&rootKey, sizeof(rootKey), hierarchy->ID);
    const ImGuiID branchId = ImHashData(&branchKey, sizeof(branchKey), rootId);
    auto toggle = [&](ImGuiID id, int expectedOpen) {
        ImGui::ActivateItemByID(id);
        frame();
        CHECK_EQ(ctx, hierarchy->StateStorage.GetInt(id, -1), expectedOpen);
    };

    toggle(branchId, 0);
    CHECK(ctx, BeginPlaySession(editor));
    CHECK(ctx, EndPlaySession(editor));
    CHECK(ctx, state.selectedEntity.IsValid() && state.selectedEntity != branch);
    toggle(branchId, 1); // The restored branch was visible and still closed.

    toggle(branchId, 0);
    toggle(rootId, 0);
    CHECK(ctx, BeginPlaySession(editor));
    CHECK(ctx, EndPlaySession(editor));
    toggle(rootId, 1);
    toggle(branchId, 1);

    ClearEditorHistory(state);
    ImGui::DestroyContext(gui);
    ImGui::SetCurrentContext(previous);
}

static void TestInspectorComponentStaysOpenAcrossPlayAndPrefabEdit(test::TestContext& ctx)
{
    auto meta = CreateEditorCommandsRegistry();
    RegisterPhysicsComponents(meta);
    World world(meta);
    MaterialSystem materials;
    assets::AssetRegistry assets;
    EditorState state;
    auto editor = CreateEditorContext(world, materials, assets, state);
    const EntityID entity = CreateEmptyEntity(editor);
    world.Add<RigidBodyComponent>(entity);
    state.selectedEntity = entity;

    auto* previous = ImGui::GetCurrentContext();
    auto* gui = ImGui::CreateContext();
    auto& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.DisplaySize = {1280.f, 900.f};
    io.DeltaTime = 1.f / 60.f;
    unsigned char* pixels;
    int width, height;
    io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
    auto frame = [&]() {
        ImGui::NewFrame();
        ImGui::SetNextWindowPos({0.f, 0.f});
        ImGui::SetNextWindowSize({480.f, 850.f});
        engine::renderer::addons::editor::detail::DrawInspector(editor);
        ImGui::Render();
    };
    frame();
    frame();

    auto* inspector = ImGui::FindWindowByName("Inspector##editor");
    const ImGuiID entityScope = inspector->GetID(static_cast<int>(entity.value));
    const ImGuiID headerId = ImHashStr("Rigidbody##rb_hdr", 0, entityScope);
    const ImGuiID enabledId = ImHashStr("Aktiv##rb_enabled", 0, entityScope);
    ImGui::ActivateItemByID(headerId);
    frame();
    CHECK_EQ(ctx, inspector->StateStorage.GetInt(headerId), 1);

    CHECK(ctx, BeginPlaySession(editor));
    frame();
    CHECK(ctx, EndPlaySession(editor));
    CHECK(ctx, state.selectedEntity.IsValid() && state.selectedEntity != entity);
    frame();
    // Das Feld ist nur dann anklickbar, wenn der Abschnitt weiterhin offen ist.
    ImGui::ActivateItemByID(enabledId);
    frame();
    const auto* body = world.Get<RigidBodyComponent>(state.selectedEntity);
    CHECK(ctx, body && !body->enabled);

    CHECK(ctx, BeginPlaySession(editor));
    ImGui::ActivateItemByID(headerId);
    frame();
    CHECK_EQ(ctx, inspector->StateStorage.GetInt(headerId), 0);
    CHECK(ctx, EndPlaySession(editor));
    frame();
    ImGui::ActivateItemByID(headerId);
    frame();
    CHECK_EQ(ctx, inspector->StateStorage.GetInt(headerId), 1);

    const auto prefabPath = std::filesystem::temp_directory_path() /
        ("krom-inspector-foldout-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".prefab");
    const auto prefab = engine::addons::prefab::BuildPrefabFromWorldEntity(
        world, state.selectedEntity, "Foldout", &assets);
    CHECK(ctx, engine::addons::prefab::SavePrefabToFile(prefab, prefabPath));
    CHECK(ctx, EnterPrefabMode(editor, prefabPath));
    const EntityID editingEntity = state.selectedEntity;
    const ImGuiID prefabScope = ImHashStr(prefab.records.front().id.c_str(), 0,
        inspector->GetID(state.prefabModePath.c_str()));
    const ImGuiID prefabHeaderId = ImHashStr("Rigidbody##rb_hdr", 0, prefabScope);
    const ImGuiID prefabEnabledId = ImHashStr("Aktiv##rb_enabled", 0, prefabScope);
    frame();
    ImGui::ActivateItemByID(prefabHeaderId);
    frame();
    CHECK_EQ(ctx, inspector->StateStorage.GetInt(prefabHeaderId), 1);
    CHECK(ctx, ExitPrefabMode(editor));
    frame();
    CHECK(ctx, EnterPrefabMode(editor, prefabPath));
    CHECK(ctx, state.selectedEntity.IsValid() && state.selectedEntity != editingEntity);
    frame();
    const auto* prefabBody = world.Get<RigidBodyComponent>(state.selectedEntity);
    CHECK(ctx, prefabBody && !prefabBody->enabled);
    ImGui::ActivateItemByID(prefabEnabledId);
    frame();
    prefabBody = world.Get<RigidBodyComponent>(state.selectedEntity);
    CHECK(ctx, prefabBody && prefabBody->enabled);
    CHECK(ctx, ExitPrefabMode(editor));
    std::error_code ec;
    std::filesystem::remove(prefabPath, ec);

    ClearEditorHistory(state);
    ImGui::DestroyContext(gui);
    ImGui::SetCurrentContext(previous);
}

// Play/Stop restores the edit scene from a serialized snapshot: skin and player
// must survive it (asset paths and playback settings; handles resolve on sync).
static void TestPlaySnapshotKeepsSkinAndAnimation(test::TestContext& ctx)
{
    auto meta = CreateEditorCommandsRegistry();
    RegisterAnimationComponents(meta);
    World world(meta);
    MaterialSystem materials;
    assets::AssetRegistry assets;
    EditorState state;
    auto editor = CreateEditorContext(world, materials, assets, state);
    const EntityID rig = CreateEmptyEntity(editor);
    SkinComponent skin{};
    skin.skeletonAssetPath = "Models/rig.glb#skeleton/0";
    world.Add<SkinComponent>(rig, skin);
    AnimationPlayerComponent player{};
    player.clipAssetPath = "Models/rig.glb#animation/0";
    player.playing = true;
    player.looping = true;
    world.Add<AnimationPlayerComponent>(rig, player);

    CHECK(ctx, BeginPlaySession(editor));
    CHECK(ctx, EndPlaySession(editor));

    uint32_t found = 0u;
    world.View<SkinComponent>([&](EntityID id, SkinComponent& restored) {
        const auto* restoredPlayer = world.Get<AnimationPlayerComponent>(id);
        if (restored.skeletonAssetPath == "Models/rig.glb#skeleton/0" && restoredPlayer &&
            restoredPlayer->clipAssetPath == "Models/rig.glb#animation/0" &&
            restoredPlayer->playing && restoredPlayer->looping)
            ++found;
    });
    CHECK(ctx, found == 1u);
    ClearEditorHistory(state);
}

// Opening a prefab during Play must be refused: otherwise Stop restores the edit
// scene into the prefab mode and the prefab root swallows the whole scene.
static void TestPrefabModeBlockedDuringPlay(test::TestContext& ctx)
{
    auto meta = CreateEditorCommandsRegistry();
    World world(meta);
    MaterialSystem materials;
    assets::AssetRegistry assets;
    EditorState state;
    auto editor = CreateEditorContext(world, materials, assets, state);
    const EntityID a = CreateEmptyEntity(editor);
    (void)CreateEmptyEntity(editor);
    (void)CreateEmptyEntity(editor);

    const auto countRoots = [&world]() {
        uint32_t entities = 0u, roots = 0u;
        world.ForEachAlive([&](EntityID id) {
            ++entities;
            if (!world.Has<ParentComponent>(id)) ++roots;
        });
        return std::pair<uint32_t, uint32_t>{entities, roots};
    };
    const auto before = countRoots();

    const auto prefabPath = std::filesystem::temp_directory_path() /
        ("krom-play-prefab-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".prefab");
    const auto prefab = engine::addons::prefab::BuildPrefabFromWorldEntity(world, a, "PlayGuard", &assets);
    CHECK(ctx, engine::addons::prefab::SavePrefabToFile(prefab, prefabPath));
    std::error_code ec;
    const auto prefabSize = std::filesystem::file_size(prefabPath, ec);

    CHECK(ctx, BeginPlaySession(editor));
    CHECK(ctx, !EnterPrefabMode(editor, prefabPath));
    CHECK(ctx, !IsInPrefabMode(state));
    CHECK(ctx, !editor.lastFileMessage.empty());
    CHECK(ctx, EndPlaySession(editor));

    CHECK(ctx, !IsInPrefabMode(state));
    CHECK(ctx, countRoots() == before);
    CHECK(ctx, std::filesystem::file_size(prefabPath, ec) == prefabSize);

    // After Stop the prefab opens normally again.
    CHECK(ctx, EnterPrefabMode(editor, prefabPath));
    CHECK(ctx, ExitPrefabMode(editor));
    CHECK(ctx, countRoots() == before);
    std::filesystem::remove(prefabPath, ec);
    ClearEditorHistory(state);
}

static void TestParticleAtlasDragAndUndo(test::TestContext& ctx)
{
    auto meta = CreateEditorCommandsRegistry();
    RegisterParticleComponents(meta);
    World world(meta);
    MaterialSystem materials;
    assets::AssetRegistry registry;
    assets::AssetPipeline pipeline(registry);
    EditorState state;
    AssetBrowserState browser;
    state.assetBrowser = &browser;
    auto editor = CreateEditorContext(world, materials, registry, state);
    editor.assetPipeline = &pipeline;
    void* textureID = reinterpret_cast<void*>(uintptr_t(901u));
    editor.editorTextureId = [textureID](TextureHandle) { return textureID; };
    const auto image = std::filesystem::temp_directory_path() /
        ("krom-atlas-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".png");
    { std::ofstream file(image); file << "cached thumbnail"; }
    pipeline.SetAssetRoot(image.parent_path());
    const uint64_t stamp = static_cast<uint64_t>(std::filesystem::last_write_time(image).time_since_epoch().count());
    browser.textureThumbnails[PathToGenericUtf8(image.lexically_normal())] = {TextureHandle{1u}, textureID, stamp, 1.f, 128u, 128u};
    const auto entity = CreateEmptyEntity(editor);
    ParticleEmitterComponent emitter;
    emitter.texturePath = PathToUtf8(image.filename());
    emitter.flipbookColumns = emitter.flipbookRows = 2u;
    world.Add<ParticleEmitterComponent>(entity, emitter);
    state.selectedEntity = entity;
    auto* previous = ImGui::GetCurrentContext();
    auto* gui = ImGui::CreateContext();
    auto& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.DisplaySize = {1200.f, 1800.f};
    io.DeltaTime = 1.f / 60.f;
    unsigned char* pixels;
    int width, height;
    io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
    io.Fonts->SetTexID(ImTextureID(1u));
    auto frame = [&]() {
        ImGui::NewFrame();
        ImGui::SetNextWindowPos({0.f, 0.f});
        ImGui::SetNextWindowSize({600.f, 1750.f});
        engine::renderer::addons::editor::detail::DrawInspector(editor);
        ImGui::Render();
    };
    frame(); frame();
    auto* inspector = ImGui::FindWindowByName("Inspector##editor");
    const ImGuiID scope = inspector->GetID(static_cast<int>(entity.value));
    ImGui::ActivateItemByID(ImHashStr("Partikel-Emitter##particles_hdr", 0, scope));
    frame();
    ImGui::ActivateItemByID(ImHashStr("Atlas Frames##pe_frames", 0, scope));
    frame(); frame();
    ImVec4 atlas{};
    for (const auto& command : inspector->DrawList->CmdBuffer)
        if (command.GetTexID() == reinterpret_cast<ImTextureID>(textureID) &&
            std::abs(command.ClipRect.z - command.ClipRect.x - 128.f) < 1.f)
        {
            atlas = command.ClipRect;
            break;
        }
    CHECK(ctx, atlas.z > atlas.x && atlas.w > atlas.y);
    if (atlas.z > atlas.x)
    {
        io.AddMousePosEvent(atlas.x + 12.f, atlas.y + 12.f);
        frame();
        io.AddMouseButtonEvent(ImGuiMouseButton_Left, true);
        frame();
        io.AddMousePosEvent(atlas.x + 22.f, atlas.y + 18.f);
        frame();
        io.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
        frame();
        const auto* edited = world.Get<ParticleEmitterComponent>(state.selectedEntity);
        CHECK_EQ(ctx, edited->frames.size(), size_t(4));
        if (edited->frames.size() == 4u)
        {
            CHECK(ctx, edited->frames.front().x == 10u && edited->frames.front().y == 6u);
            CHECK(ctx, edited->frames.front().width == 64u && edited->frames.front().height == 64u);
            CHECK_EQ(ctx, edited->frames[1].x, 64u);
            CHECK(ctx, UndoEditorHistory(editor));
            CHECK(ctx, world.Get<ParticleEmitterComponent>(state.selectedEntity)->frames.empty());
        }
        frame();
        const ImGuiID atlasScope = ImHashStr("Atlas Frames##pe_frames", 0, scope);
        const ImGuiID clipsScope = ImHashStr("Clips##pe_clips", 0, atlasScope);
        ImGui::ActivateItemByID(clipsScope);
        frame();
        ImGui::ActivateItemByID(ImHashStr("Add Clip##add", 0, clipsScope));
        frame(); frame();
        auto* clips = &world.Get<ParticleEmitterComponent>(state.selectedEntity)->clips;
        CHECK_EQ(ctx, clips->size(), size_t(1));
        if (clips->size() == 1u)
        {
            const int zero = 0, one = 1;
            const ImGuiID rowZero = ImHashData(&zero, sizeof(zero), clipsScope);
            ImGui::ActivateItemByID(ImHashStr("Duplicate##duplicate", 0, rowZero));
            frame(); frame();
            CHECK_EQ(ctx, clips->size(), size_t(2));
            if (clips->size() == 2u)
            {
                const std::string duplicateName = clips->back().name;
                CHECK_NE(ctx, clips->front().name, duplicateName);
                const ImGuiID rowOne = ImHashData(&one, sizeof(one), clipsScope);
                ImGui::ActivateItemByID(ImHashStr("Move Up##up", 0, rowOne));
                frame(); frame();
                CHECK_EQ(ctx, clips->front().name, duplicateName);
                CHECK_EQ(ctx, inspector->StateStorage.GetInt(ImHashStr("##preview_source", 0, atlasScope), -1), 0);
                const auto beforePreview = *world.Get<ParticleEmitterComponent>(state.selectedEntity);
                ImGui::ActivateItemByID(ImHashStr("Play / Pause##play_pause", 0, atlasScope));
                frame(); frame();
                CHECK(ctx, *world.Get<ParticleEmitterComponent>(state.selectedEntity) == beforePreview);
                ImGui::ActivateItemByID(ImHashStr("Delete##delete", 0, rowZero));
                frame(); frame();
                CHECK_EQ(ctx, clips->size(), size_t(1));
                CHECK(ctx, UndoEditorHistory(editor));
                CHECK_EQ(ctx, world.Get<ParticleEmitterComponent>(state.selectedEntity)->clips.size(), size_t(2));
            }
        }
    }
    ClearEditorHistory(state);
    ImGui::DestroyContext(gui);
    ImGui::SetCurrentContext(previous);
    std::error_code ec;
    std::filesystem::remove(image, ec);
}

static void TestInspectorCameraDeleteConfirmation(test::TestContext& ctx)
{
    auto meta = CreateEditorCommandsRegistry();
    RegisterCameraComponents(meta);
    World world(meta);
    MaterialSystem materials;
    assets::AssetRegistry registry;
    EditorState state;
    auto editor = CreateEditorContext(world, materials, registry, state);
    const EntityID camera = CreateCameraEntity(editor);
    const std::string undoBefore = GetUndoEditorHistoryLabel(state);

    auto* previous = ImGui::GetCurrentContext();
    auto* gui = ImGui::CreateContext();
    auto& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.DisplaySize = {1280.f, 900.f};
    io.DeltaTime = 1.f / 60.f;
    unsigned char* pixels = nullptr;
    int width = 0, height = 0;
    io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
    auto frame = [&]() {
        ImGui::NewFrame();
        engine::renderer::addons::editor::detail::DrawDeleteConfirmDialog(editor);
        ImGui::SetNextWindowPos({0.f, 0.f});
        ImGui::SetNextWindowSize({480.f, 750.f});
        engine::renderer::addons::editor::detail::DrawInspector(editor);
        ImGui::Render();
    };
    frame();
    frame();
    auto* inspector = ImGui::FindWindowByName("Inspector##editor");
    CHECK(ctx, inspector != nullptr);
    if (inspector)
    {
        // Delete is requested from the hierarchy context menu, which only sets the flag.
        state.deleteConfirmOpen = true;
        frame();
        CHECK(ctx, world.IsAlive(camera));
        CHECK_EQ(ctx, GetUndoEditorHistoryLabel(state), undoBefore);
        frame();
        auto* popup = ImGui::FindWindowByName("Objekt löschen?##editor");
        CHECK(ctx, popup != nullptr && popup->Active);
        CHECK(ctx, world.IsAlive(camera));
        if (popup && popup->Active)
        {
            ImGui::ActivateItemByID(popup->GetID("Abbrechen"));
            frame();
            frame();
            CHECK(ctx, world.IsAlive(camera));
            CHECK(ctx, world.Has<CameraComponent>(camera));
            CHECK_EQ(ctx, state.selectedEntity, camera);
            CHECK_EQ(ctx, GetUndoEditorHistoryLabel(state), undoBefore);

            state.deleteConfirmOpen = true;
            frame();
            frame();
            CHECK(ctx, world.IsAlive(camera));
            ImGui::ActivateItemByID(popup->GetID("Löschen"));
            frame();
            CHECK(ctx, !world.IsAlive(camera));
            CHECK(ctx, !state.selectedEntity.IsValid());
            CHECK(ctx, UndoEditorHistory(editor));
            CHECK(ctx, state.selectedEntity.IsValid() && world.Has<CameraComponent>(state.selectedEntity));
        }
    }
    ImGui::DestroyContext(gui);
    ImGui::SetCurrentContext(previous);
}

static void TestTransformInspectorSelectionSource(test::TestContext& ctx)
{
    auto meta = CreateEditorCommandsRegistry();
    World world(meta);
    MaterialSystem materials;
    assets::AssetRegistry registry;
    EditorState state;
    auto editor = CreateEditorContext(world, materials, registry, state);
    const EntityID parent = CreateEmptyEntity(editor, "Parent");
    const EntityID child = CreateEmptyEntity(editor, "Child");
    CHECK(ctx, ReparentEntity(editor, child, parent));
    state.selectedEntity = child;

    auto* previous = ImGui::GetCurrentContext();
    auto* gui = ImGui::CreateContext();
    auto& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.DisplaySize = {1280.f, 900.f};
    io.DeltaTime = 1.f / 60.f;
    unsigned char* pixels = nullptr;
    int width = 0, height = 0;
    io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);

    auto draw = [&](SelectionSource source) {
        state.selectionSource = source;
        ImGui::NewFrame();
        ImGui::LogToBuffer(0);
        ImGui::SetNextWindowPos({0.f, 0.f});
        ImGui::SetNextWindowSize({480.f, 850.f});
        engine::renderer::addons::editor::detail::DrawInspector(editor);
        const std::string text = gui->LogBuffer.c_str();
        ImGui::LogFinish();
        ImGui::Render();
        return text;
    };

    draw(SelectionSource::Viewport);
    const std::string viewport = draw(SelectionSource::Viewport);
    const std::string hierarchy = draw(SelectionSource::Outliner);
    CHECK_EQ(ctx, viewport, hierarchy);
    CHECK(ctx, viewport.find(ICON_LC_LOCATE_FIXED "  " + std::string(Translate("transform.local"))) != std::string::npos);
    CHECK(ctx, viewport.find(ICON_LC_EARTH "  " + std::string(Translate("transform.world"))) != std::string::npos);

    ImGui::DestroyContext(gui);
    ImGui::SetCurrentContext(previous);
    ClearEditorHistory(state);
}

static void TestDeleteSelectionShortcut(test::TestContext& ctx)
{
    auto meta = CreateEditorCommandsRegistry();
    World world(meta);
    MaterialSystem materials;
    assets::AssetRegistry registry;
    EditorState state;
    auto editor = CreateEditorContext(world, materials, registry, state);
    const EntityID parent = CreateEmptyEntity(editor, "Parent");
    const EntityID child = CreateEmptyEntity(editor, "Child");
    const EntityID otherSelected = CreateEmptyEntity(editor, "Other Selected");
    const EntityID unselected = CreateEmptyEntity(editor, "Unselected");
    CHECK(ctx, ReparentEntity(editor, child, parent));
    state.selectedEntity = parent;
    AddToMultiSelection(editor, child);
    AddToMultiSelection(editor, otherSelected);
    ClearEditorHistory(state);

    auto* previous = ImGui::GetCurrentContext();
    auto* gui = ImGui::CreateContext();
    auto& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.DisplaySize = {1280.f, 900.f};
    io.DeltaTime = 1.f / 60.f;
    unsigned char* pixels = nullptr;
    int width = 0, height = 0;
    io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
    // Delete works on scene objects only while the hierarchy (or viewport) has the focus.
    const std::string hierarchyTitle = TranslateLabel("menu.hierarchy", "Hierarchie##editor");
    const char* focusWindow = hierarchyTitle.c_str();
    auto frame = [&]() {
        ImGui::NewFrame();
        ImGui::Begin(hierarchyTitle.c_str());
        ImGui::End();
        ImGui::Begin("Other##delete_test");
        ImGui::End();
        if (focusWindow)
        {
            ImGui::SetWindowFocus(focusWindow);
            focusWindow = nullptr;
        }
        engine::renderer::addons::editor::detail::DrawFileMenuBar(editor);
        engine::renderer::addons::editor::detail::DrawDeleteConfirmDialog(editor);
        ImGui::Render();
    };

    // Another window focused (graph, animation, inspector): nothing is deleted or asked.
    focusWindow = "Other##delete_test";
    frame();
    io.AddKeyEvent(ImGuiKey_Delete, true);
    frame();
    io.AddKeyEvent(ImGuiKey_Delete, false);
    frame();
    CHECK(ctx, !state.deleteConfirmOpen);

    focusWindow = hierarchyTitle.c_str();
    frame();
    io.AddKeyEvent(ImGuiKey_Delete, true);
    frame();
    io.AddKeyEvent(ImGuiKey_Delete, false);
    frame();
    auto* popup = ImGui::FindWindowByName("Objekt löschen?##editor");
    CHECK(ctx, popup && popup->Active);
    CHECK(ctx, world.IsAlive(parent) && world.IsAlive(child) && world.IsAlive(otherSelected));
    if (popup && popup->Active)
    {
        ImGui::ActivateItemByID(popup->GetID("Löschen"));
        frame();
        CHECK(ctx, !world.IsAlive(parent) && !world.IsAlive(child) && !world.IsAlive(otherSelected));
        CHECK(ctx, world.IsAlive(unselected));
        CHECK(ctx, !state.selectedEntity.IsValid() && state.multiSelection.empty());

        CHECK(ctx, UndoEditorHistory(editor));
        const EntityID restoredParent = FindByName(world, "Parent");
        const EntityID restoredChild = FindByName(world, "Child");
        CHECK(ctx, restoredParent.IsValid() && restoredChild.IsValid());
        CHECK_EQ(ctx, GetEntityParent(editor, restoredChild), restoredParent);
        CHECK(ctx, FindByName(world, "Other Selected").IsValid());
        CHECK(ctx, !CanUndoEditorHistory(state));
    }
    ClearEditorHistory(state);
    ImGui::DestroyContext(gui);
    ImGui::SetCurrentContext(previous);
}

static void TestMoveEntityInHierarchy(test::TestContext& ctx)
{
    ComponentMetaRegistry componentRegistry = CreateEditorCommandsRegistry();
    World world(componentRegistry);
    MaterialSystem materials;
    assets::AssetRegistry assetRegistry;
    EditorState state;
    EditorFrameContext editor = CreateEditorContext(world, materials, assetRegistry, state);

    auto make = [&](const char* name, uint32_t order) {
        const EntityID e = world.CreateEntity();
        world.Add<NameComponent>(e, NameComponent{name});
        world.Add<TransformComponent>(e);
        world.Add<WorldTransformComponent>(e);
        world.Add<ActiveComponent>(e, ActiveComponent{true});
        world.Add<EditorHierarchyComponent>(e, EditorHierarchyComponent{order});
        return e;
    };
    const EntityID a = make("A", 0u);
    const EntityID b = make("B", 1u);
    const EntityID c = make("C", 2u);
    const EntityID child = make("Child", 0u);
    world.Add<ParentComponent>(child, ParentComponent{a});
    world.Add<ChildrenComponent>(a).Add(child);

    auto order = [&](EntityID e) { return GetEntityHierarchyOrder(editor, e); };
    auto findByName = [&](const char* name) {
        EntityID found = NULL_ENTITY;
        world.ForEachAlive([&](EntityID id) {
            if (const auto* n = world.Get<NameComponent>(id); n && n->name == name)
                found = id;
        });
        return found;
    };

    // Reorder among root siblings: C before A.
    CHECK(ctx, MoveEntityInHierarchy(editor, c, NULL_ENTITY, a, false));
    CHECK(ctx, order(c) < order(a) && order(a) < order(b));

    // Dropping on the same spot again is a no-op without an undo entry.
    CHECK(ctx, !MoveEntityInHierarchy(editor, c, NULL_ENTITY, a, false));

    // Take the child out of A: place it after B on the root level.
    CHECK(ctx, MoveEntityInHierarchy(editor, child, NULL_ENTITY, b, true));
    CHECK(ctx, GetEntityParent(editor, child) == NULL_ENTITY);
    CHECK(ctx, order(b) < order(child));
    const auto* aChildren = world.Get<ChildrenComponent>(a);
    CHECK(ctx, !aChildren || std::find(aChildren->children.begin(), aChildren->children.end(), child) == aChildren->children.end());

    // Cycles are rejected: A cannot go under its own former child once child is under A.
    CHECK(ctx, MoveEntityInHierarchy(editor, child, a, NULL_ENTITY, true));
    CHECK(ctx, GetEntityParent(editor, child) == a);
    CHECK(ctx, !MoveEntityInHierarchy(editor, a, child, NULL_ENTITY, true));

    // Undo restores the root placement after B.
    CHECK(ctx, UndoEditorHistory(editor));
    const EntityID restored = findByName("Child");
    CHECK(ctx, restored.IsValid() && GetEntityParent(editor, restored) == NULL_ENTITY);
}

static void TestUnpackPrefabInstance(test::TestContext& ctx)
{
    ComponentMetaRegistry componentRegistry = CreateEditorCommandsRegistry();
    RegisterPrefabInstanceComponent(componentRegistry);
    World world(componentRegistry);
    MaterialSystem materials;
    assets::AssetRegistry assetRegistry;
    EditorState state;
    EditorFrameContext editor = CreateEditorContext(world, materials, assetRegistry, state);

    auto make = [&](const char* name, EntityID parent) {
        const EntityID e = world.CreateEntity();
        world.Add<NameComponent>(e, NameComponent{name});
        world.Add<TransformComponent>(e);
        world.Add<WorldTransformComponent>(e);
        world.Add<PrefabSourceComponent>(e, PrefabSourceComponent{name});
        if (parent.IsValid())
        {
            world.Add<ParentComponent>(e, ParentComponent{parent});
            if (auto* children = world.Get<ChildrenComponent>(parent)) children->Add(e);
            else world.Add<ChildrenComponent>(parent).Add(e);
        }
        return e;
    };
    const EntityID root = make("Root", NULL_ENTITY);
    world.Add<PrefabInstanceComponent>(root, PrefabInstanceComponent{"Prefabs/Outer.prefab"});
    const EntityID part = make("Part", root);
    const EntityID nested = make("Nested", root);
    world.Add<PrefabInstanceComponent>(nested, PrefabInstanceComponent{"Prefabs/Inner.prefab"});
    const EntityID nestedPart = make("NestedPart", nested);
    const EntityID plain = world.CreateEntity();

    // A child (even inside the nested instance) resolves to the outermost instance.
    CHECK_EQ(ctx, FindOutermostPrefabInstanceRoot(editor, nestedPart), root);
    CHECK(ctx, !FindOutermostPrefabInstanceRoot(editor, plain).IsValid());
    CHECK(ctx, !UnpackPrefabInstance(editor, plain));

    CHECK(ctx, UnpackPrefabInstance(editor, part));
    CHECK(ctx, !world.Has<PrefabInstanceComponent>(root));
    CHECK(ctx, !world.Has<PrefabSourceComponent>(root));
    CHECK(ctx, !world.Has<PrefabSourceComponent>(part));
    CHECK(ctx, world.IsAlive(part));
    // The nested instance keeps its own link.
    CHECK(ctx, world.Has<PrefabInstanceComponent>(nested));
    CHECK(ctx, world.Has<PrefabSourceComponent>(nestedPart));
    CHECK_EQ(ctx, FindOutermostPrefabInstanceRoot(editor, nestedPart), nested);

    CHECK(ctx, UndoEditorHistory(editor));
    bool relinked = false;
    world.ForEachAlive([&](EntityID id) {
        if (const auto* n = world.Get<NameComponent>(id); n && n->name == "Root")
            relinked = world.Has<PrefabInstanceComponent>(id);
    });
    CHECK(ctx, relinked);
}

static void TestLiveScriptInspectorValues(test::TestContext& ctx)
{
    struct Counter final : script::ComponentScript {
        int32_t spawnCount = 0;
        EntityID target{};
        void OnUpdate(script::IScriptContext&) override { ++spawnCount; }
    };
    auto meta = CreateEditorCommandsRegistry();
    RegisterComponent<script::ScriptList>(meta, "ScriptList");
    World world(meta);
    MaterialSystem materials;
    assets::AssetRegistry assets;
    EditorState state;
    auto editor = CreateEditorContext(world, materials, assets, state);
    script::ScriptRegistry registry;
    registry.Register<Counter>("Counter");
    registry.RegisterField<Counter>("Counter", "spawnCount", script::ScriptFieldType::Int, &Counter::spawnCount);
    registry.RegisterField<Counter>("Counter", "target", script::ScriptFieldType::Entity, &Counter::target);
    editor.scriptRegistry = &registry;
    const auto entity = world.CreateEntity();
    world.Add<GuidComponent>(entity, GuidComponent{"counter-guid"});
    auto& list = world.Add<script::ScriptList>(entity);
    CHECK(ctx, list.Add("Counter", entity, registry));
    auto& instance = list.Instances_Mutable()[0];
    auto& object = static_cast<Counter&>(*instance.script);
    object.target = entity;
    script::ScriptFieldValue stored;
    stored.type = script::ScriptFieldType::Int;
    instance.fieldValues["spawnCount"] = stored;
    const auto& fields = *registry.GetFields("Counter");
    script::ScriptFieldValue value = stored;
    list.Update(0.016f, entity);
    CHECK(ctx, !ReadLiveEditorScriptField(editor, instance, fields[0], value));
    editor.playActive = true;
    CHECK(ctx, ReadLiveEditorScriptField(editor, instance, fields[0], value));
    CHECK_EQ(ctx, value.intValue, 1);
    list.Update(0.016f, entity);
    CHECK(ctx, ReadLiveEditorScriptField(editor, instance, fields[0], value));
    CHECK_EQ(ctx, value.intValue, 2);
    editor.playPaused = true;
    CHECK(ctx, ReadLiveEditorScriptField(editor, instance, fields[0], value));
    CHECK_EQ(ctx, value.intValue, 2);
    CHECK_EQ(ctx, instance.fieldValues.at("spawnCount").intValue, 0);
    CHECK(ctx, ReadLiveEditorScriptField(editor, instance, fields[1], value));
    CHECK_EQ(ctx, value.stringValue, std::string("counter-guid"));
    editor.playActive = false;
    CHECK(ctx, !ReadLiveEditorScriptField(editor, instance, fields[0], value));
}

int RunEditorCommandTests()

{

    engine::Debug::MinLevel = engine::LogLevel::Fatal;



    test::TestSuite suite("EditorCommands");
    suite.Add("Inspector reveals script asset in browser", TestInspectorRevealsScriptAsset);
    suite.Add("Inspector entity reference reveals target and preserves dropdown assignment", TestInspectorRevealsEntityReference);
    suite.Add("Camera deletion requires confirmation", TestInspectorCameraDeleteConfirmation);
    suite.Add("Delete shortcut removes the whole selection in one undo step", TestDeleteSelectionShortcut);
    suite.Add("Transform Inspector is consistent across selection sources", TestTransformInspectorSelectionSource);
    suite.Add("Particle atlas dragging converts grid and supports undo", TestParticleAtlasDragAndUndo);
    suite.Add("Hierarchy foldouts survive Play and Stop", TestHierarchyFoldoutsSurvivePlay);
    suite.Add("Hierarchy marks persistent objects from unloaded scenes", TestHierarchyMarksCarriedPersistentObjects);
    suite.Add("Inspector component foldout survives Play and reopening a prefab", TestInspectorComponentStaysOpenAcrossPlayAndPrefabEdit);
    suite.Add("Prefab mode blocked during Play", TestPrefabModeBlockedDuringPlay);
    suite.Add("Play snapshot keeps skin and animation", TestPlaySnapshotKeepsSkinAndAnimation);
    suite.Add("Inspector displays current Play and Pause script values", TestLiveScriptInspectorValues);

    suite.Add("DuplicateEntity copies hierarchy", TestDuplicateEntityCopiesHierarchy);
    suite.Add("Hierarchy drop reorders and unparents", TestMoveEntityInHierarchy);
    suite.Add("Unpack prefab instance keeps nested instances", TestUnpackPrefabInstance);

    suite.Add("Component add and field edit undoable", TestComponentAddAndFieldEditAreUndoable);

    suite.Add("Duplicate and component clipboard", TestDuplicateAndComponentClipboard);

    suite.Add("Transform clipboard respects Local and World", TestTransformClipboardCoordinateSpaces);

    suite.Add("Typed property clipboard and undo", TestTypedPropertyClipboardAndUndo);

    suite.Add("Property label context menu", TestPropertyLabelContextMenu);

    suite.Add("Material preview zoom takes precedence over window scroll", TestMaterialPreviewMouseWheel);
    suite.Add("Material property fields keep their width as the window grows", TestMaterialPropertyWidthStaysFixed);
    suite.Add("Material type switches in the graph and Custom connects shader files and text", TestCustomMaterialGraph);
    suite.Add("Material parameters apply per object", TestMaterialParameterPerObject);
    suite.Add("Material graph edits undo as one step", TestMaterialGraphUndo);
    suite.Add("Material graph compiles and round-trips", TestMaterialGraphRoundTrip);
    suite.Add("Material graph UV, Position and Vector2 nodes", TestMaterialGraphUVNodes);
    suite.Add("Material graph sockets connect and disconnect", TestMaterialGraphSockets);
    suite.Add("Material instances stay independent", TestIndependentMaterialInstances);

    suite.Add("Asset folder tile navigation",TestAssetFolderTileNavigation);
    suite.Add("Docked assets panel can undock",TestDockedAssetsPanelCanUndock);
    suite.Add("Scene settings close independently and reopen from View", TestSceneSettingsDockTab);
    suite.Add("Thumbnail disk cache", TestThumbnailDiskCache);
    suite.Add("Asset thumbnail fits mesh geometry and hierarchy", TestAssetThumbnailMeshFraming);

    suite.Add("Particle prefab live link survives snapshots", TestParticlePrefabLiveLink);

    return suite.Run();

}

