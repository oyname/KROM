#include "TestFramework.hpp"



#include "addons/editor/EditorCommands.hpp"
#include "addons/editor/EditorAssetBrowser.hpp"
#include "addons/editor/EditorUI.hpp"
#include "addons/editor/EditorUIInternal.hpp"
#include "addons/editor/EditorScriptAssets.hpp"
#include "addons/script/ScriptList.hpp"

#include "addons/editor/EditorMaterialLibrary.hpp"

#include "addons/mesh_renderer/MeshRendererComponents.hpp"

#include "addons/editor/EditorComponentClipboard.hpp"

#include "addons/editor/EditorValueClipboard.hpp"

#include "addons/editor/EditorThumbnailCache.hpp"

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

    state.selectedMaterialAssetData = asset;

    CHECK(ctx, SetEntityMaterialOverride(editor, first, -1, asset));

    const auto source = world.Get<MaterialComponent>(first)->materialOverrideSource;

    CHECK(ctx, source.find("uvScale 5 3") != std::string::npos);

    assets::AssetPipeline pipeline(registry);

    const auto parsed = pipeline.LoadMaterialSource(source);

    CHECK(ctx, parsed.IsValid());

    CHECK_EQ(ctx, registry.materials.Get(parsed)->uvScale.x, 5.f);

    CHECK_EQ(ctx, registry.materials.Get(parsed)->uvScale.y, 3.f);

    const auto other = pipeline.LoadMaterialSource("template pbr-lit\nuvScale 2 2\n");

    CHECK_NE(ctx, parsed, other);

    CHECK_EQ(ctx, registry.materials.Get(parsed)->uvScale.x, 5.f);

    CHECK_EQ(ctx, registry.materials.Get(other)->uvScale.x, 2.f);

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
        const ImGuiID entityScope = inspector->GetID(static_cast<int>(camera.value));
        const ImGuiID deleteId = ImHashStr("Loeschen", 0, entityScope);
        ImGui::ActivateItemByID(deleteId);
        frame();
        CHECK(ctx, world.IsAlive(camera));
        CHECK(ctx, state.deleteConfirmOpen);
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

            ImGui::ActivateItemByID(deleteId);
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
    suite.Add("Inspector camera deletion requires confirmation", TestInspectorCameraDeleteConfirmation);
    suite.Add("Inspector displays current Play and Pause script values", TestLiveScriptInspectorValues);

    suite.Add("DuplicateEntity copies hierarchy", TestDuplicateEntityCopiesHierarchy);

    suite.Add("Component add and field edit undoable", TestComponentAddAndFieldEditAreUndoable);

    suite.Add("Duplicate and component clipboard", TestDuplicateAndComponentClipboard);

    suite.Add("Transform clipboard respects Local and World", TestTransformClipboardCoordinateSpaces);

    suite.Add("Typed property clipboard and undo", TestTypedPropertyClipboardAndUndo);

    suite.Add("Property label context menu", TestPropertyLabelContextMenu);

    suite.Add("Material instances stay independent", TestIndependentMaterialInstances);

    suite.Add("Asset folder tile navigation",TestAssetFolderTileNavigation);
    suite.Add("Docked assets panel can undock",TestDockedAssetsPanelCanUndock);
    suite.Add("Thumbnail disk cache", TestThumbnailDiskCache);

    suite.Add("Particle prefab live link survives snapshots", TestParticlePrefabLiveLink);

    return suite.Run();

}

