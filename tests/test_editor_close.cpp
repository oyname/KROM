// Closing the editor: unsaved changes ask save / don't save / cancel first.
#include "TestFramework.hpp"

#include "addons/editor/EditorComponents.hpp"
#include "addons/editor/EditorUIInternal.hpp"
#include "assets/AssetRegistry.hpp"
#include "ecs/Components.hpp"
#include "ecs/World.hpp"
#include "renderer/MaterialSystem.hpp"

#include "imgui.h"
#include "imgui_internal.h"

using namespace engine;
using namespace engine::ecs;
using namespace engine::renderer;
using namespace engine::renderer::addons::editor;
namespace ui = engine::renderer::addons::editor::detail;

namespace {

static void TestEditorCloseAsksAboutUnsavedScene(test::TestContext& ctx)
{
    ComponentMetaRegistry meta;
    RegisterCoreComponents(meta);
    RegisterEditorComponents(meta);
    World world(meta);
    MaterialSystem materials;
    assets::AssetRegistry registry;
    EditorState state;
    EditorFrameContext editor{world, materials, registry, state};
    editor.currentProjectName = "Project";
    editor.currentEditorSceneName = "Main";

    bool modified = false;
    bool saveResult = true;
    int closeCount = 0;
    int saveCount = 0;
    int stopCount = 0;
    editor.isEditorSceneModified = [&] { return modified; };
    editor.requestClose = [&] { ++closeCount; };
    editor.saveScene = [&] {
        ++saveCount;
        if (saveResult)
            modified = false;
        return saveResult;
    };
    editor.stopPlay = [&] {
        ++stopCount;
        editor.playActive = false;
        modified = true; // the restored edit scene has unsaved changes
    };

    auto* previous = ImGui::GetCurrentContext();
    auto* gui = ImGui::CreateContext();
    auto& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.DisplaySize = {1280.f, 900.f};
    io.DeltaTime = 1.f / 60.f;
    unsigned char* pixels = nullptr;
    int width = 0, height = 0;
    io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
    auto frame = [&] {
        ImGui::NewFrame();
        ui::DrawCloseConfirmDialog(editor);
        ImGui::Render();
    };
    auto popup = [] { return ImGui::FindWindowByName("###close.title"); };
    auto popupOpen = [&] { ImGuiWindow* window = popup(); return window && window->Active; };
    auto click = [&](const char* id) {
        if (ImGuiWindow* window = popup())
            ImGui::ActivateItemByID(window->GetID(id));
        frame();
        frame();
    };

    // Unchanged scene: closes at once, no question.
    state.closeConfirmOpen = true;
    frame();
    frame();
    CHECK_EQ(ctx, closeCount, 1);
    CHECK(ctx, !popupOpen());

    // Changed scene: the close waits for the answer.
    modified = true;
    CHECK(ctx, ui::EditorCloseNeedsSaveQuestion(editor));
    state.closeConfirmOpen = true;
    frame();
    frame();
    CHECK_EQ(ctx, closeCount, 1);
    CHECK(ctx, popupOpen());

    // Cancel keeps the editor open.
    click("###common.cancel");
    CHECK_EQ(ctx, closeCount, 1);
    CHECK_EQ(ctx, saveCount, 0);
    CHECK(ctx, !popupOpen());

    // Don't save closes without saving.
    state.closeConfirmOpen = true;
    frame();
    frame();
    click("###scene.dont_save");
    CHECK_EQ(ctx, closeCount, 2);
    CHECK_EQ(ctx, saveCount, 0);

    // Save: a failed save keeps the dialog open, a successful one closes.
    saveResult = false;
    state.closeConfirmOpen = true;
    frame();
    frame();
    click("###common.save");
    CHECK_EQ(ctx, saveCount, 1);
    CHECK_EQ(ctx, closeCount, 2);
    CHECK(ctx, state.closeSaveFailed);
    CHECK(ctx, popupOpen());
    saveResult = true;
    click("###common.save");
    CHECK_EQ(ctx, saveCount, 2);
    CHECK_EQ(ctx, closeCount, 3);
    CHECK(ctx, !state.closeSaveFailed);
    CHECK(ctx, !popupOpen());

    // Play is stopped first, so the question sees the restored edit scene.
    modified = false;
    editor.playActive = true;
    state.closeConfirmOpen = true;
    frame();
    frame();
    CHECK_EQ(ctx, stopCount, 1);
    CHECK_EQ(ctx, closeCount, 3);
    CHECK(ctx, popupOpen());
    click("###common.cancel");

    // An editable prefab has no change tracking and always asks.
    modified = false;
    state.prefabModePath = "C:/Project/Assets/Thing.prefab";
    CHECK(ctx, ui::EditorCloseNeedsSaveQuestion(editor));
    state.prefabModeReadOnly = true;
    CHECK(ctx, !ui::EditorCloseNeedsSaveQuestion(editor));
    state.prefabModePath.clear();
    state.prefabModeReadOnly = false;

    // Start screen and scene loading: nothing to save.
    modified = true;
    editor.sceneLoadActive = true;
    CHECK(ctx, !ui::EditorCloseNeedsSaveQuestion(editor));
    editor.sceneLoadActive = false;
    editor.currentProjectName.clear();
    CHECK(ctx, !ui::EditorCloseNeedsSaveQuestion(editor));
    state.closeConfirmOpen = true;
    frame();
    CHECK_EQ(ctx, closeCount, 4);

    ImGui::DestroyContext(gui);
    ImGui::SetCurrentContext(previous);
}

} // namespace

int RunEditorCloseTests()
{
    test::TestSuite suite("EditorClose");
    suite.Add("Closing the editor asks about unsaved changes", TestEditorCloseAsksAboutUnsavedScene);
    return suite.Run();
}
