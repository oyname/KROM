#include "app/KromEditorApp.hpp"
#include "app/editor/EditorScene.hpp"
#include "core/Debug.hpp"

int main()
{
    using namespace engine;

    Debug::ResetMinLevelForBuild();

    app::KromEditorAppConfig config{};
    config.backend     = app::SelectAppBackend();
    config.windowTitle = std::string("KROM Editor (") + app::BackendDisplayName(config.backend) + ")";
    config.width       = 0u;   // 0 = automatisch vom primären Monitor abfragen
    config.height      = 0u;
    config.windowMode  = engine::platform::WindowMode::Maximized;

    config.enableDebugLayer  = false;
    config.clearColor        = { 0.05f, 0.05f, 0.05f, 1.f };
    config.enableForwardPlus = true;
    config.enableGtao        = true;

    config.ambientColor     = { 0.02f, 0.02f, 0.03f };
    config.ambientIntensity = 1.0f;

    app::KromEditorApp editorApp;
    if (!editorApp.Initialize(config))
        return 1;

    app::EditorScene scene;
    return editorApp.Run(scene);
}
