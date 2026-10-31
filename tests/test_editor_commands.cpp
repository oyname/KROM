#include "TestFramework.hpp"

#include "addons/editor/EditorCommands.hpp"
#include "addons/editor/EditorComponentClipboard.hpp"
#include "addons/editor/EditorThumbnailCache.hpp"
#include "addons/audio/AudioComponents.hpp"
#include "addons/camera/CameraComponents.hpp"
#include "addons/particles/ParticleComponents.hpp"
#include "addons/editor/EditorComponents.hpp"
#include "addons/lighting/LightingComponents.hpp"
#include "addons/physics/PhysicsComponents.hpp"
#include "assets/AssetRegistry.hpp"
#include "ecs/Components.hpp"
#include "ecs/World.hpp"
#include "renderer/MaterialSystem.hpp"

#include <filesystem>
#include <fstream>

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

int RunEditorCommandTests()
{
    engine::Debug::MinLevel = engine::LogLevel::Fatal;

    test::TestSuite suite("EditorCommands");
    suite.Add("DuplicateEntity copies hierarchy", TestDuplicateEntityCopiesHierarchy);
    suite.Add("Component add and field edit undoable", TestComponentAddAndFieldEditAreUndoable);
    suite.Add("Duplicate and component clipboard", TestDuplicateAndComponentClipboard);
    suite.Add("Thumbnail disk cache", TestThumbnailDiskCache);
    return suite.Run();
}
