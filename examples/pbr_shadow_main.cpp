#include "examples/framework/ExampleApp.hpp"
#include "examples/framework/PbrShadowScene.hpp"
#include "core/Debug.hpp"

int main()
{
    using namespace engine;

    Debug::ResetMinLevelForBuild();

    examples::ExampleAppConfig config{};

    // Window
    config.backend          = examples::SelectExampleBackend();
    config.windowTitle      = std::string("KROM - PBR Shadow Demo (") + examples::BackendDisplayName(config.backend) + ")";
    config.width            = 1280u;
    config.height           = 720u;
    config.windowMode       = engine::platform::WindowMode::Windowed;

    // Rendering
    config.enableDebugLayer = false;
    config.clearColor       = { 0.0f, 0.0f, 0.0f, 1.f };

    // World
    config.ambientColor     = { 0.06f, 0.06f, 0.08f };
    config.ambientIntensity = 1.0f;

    examples::ExampleApp app;
    if (!app.Initialize(config))
        return 1;

    examples::PbrShadowScene scene;
    return app.Run(scene);
}
