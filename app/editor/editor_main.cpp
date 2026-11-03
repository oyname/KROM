#include "app/KromEditorApp.hpp"
#include "app/editor/EditorScene.hpp"
#include "core/Debug.hpp"

#include <cstdlib>

int main()
{
    using namespace engine;

    Debug::ResetMinLevelForBuild();

    app::KromEditorAppConfig config{};
    config.backend     = app::SelectAppBackend();
    config.windowTitle = std::string("KROM Editor (") + app::BackendDisplayName(config.backend) + ")";
    config.width       = 1060u;
    config.height      = 620u;
    config.windowMode  = engine::platform::WindowMode::Windowed;

    // KROM_VALIDATION=1: Validierungsschicht des Backends (z. B. Vulkan) einschalten.
    config.enableDebugLayer  = std::getenv("KROM_VALIDATION") != nullptr;
    config.clearColor        = { 0.f, 0.f, 0.f, 1.f };
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
