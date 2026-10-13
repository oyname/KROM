#include "TestFramework.hpp"

#include "addons/editor/EditorCommands.hpp"
#include "addons/editor/EditorComponents.hpp"
#include "addons/lighting/LightingComponents.hpp"
#include "assets/AssetRegistry.hpp"
#include "ecs/Components.hpp"
#include "ecs/World.hpp"
#include "renderer/MaterialSystem.hpp"

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

int RunEditorCommandTests()
{
    engine::Debug::MinLevel = engine::LogLevel::Fatal;

    test::TestSuite suite("EditorCommands");
    suite.Add("DuplicateEntity copies hierarchy", TestDuplicateEntityCopiesHierarchy);
    return suite.Run();
}
