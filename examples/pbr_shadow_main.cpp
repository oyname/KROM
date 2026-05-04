#include "examples/framework/ExampleApp.hpp"
#include "examples/framework/LightingValidationScene.hpp"
#include "core/Debug.hpp"

int main()
{
    using namespace engine;

    Debug::ResetMinLevelForBuild();

    examples::ExampleAppConfig config{};

    // Window
    config.backend          = examples::SelectExampleBackend();
    config.windowTitle      = std::string("KROM - Lighting Validation (") + examples::BackendDisplayName(config.backend) + ")";
    config.width            = 1280u;
    config.height           = 720u;
    config.windowMode       = engine::platform::WindowMode::Windowed;

    // Rendering
    config.enableDebugLayer = false;
    config.clearColor       = { 0.0f, 0.0f, 0.0f, 1.f };

    // World
    config.ambientColor     = { 0.0f, 0.0f, 0.0f };
    config.ambientIntensity = 0.0f;

    examples::ExampleApp app;
    if (!app.Initialize(config))
        return 1;

    examples::LightingValidationScene scene;
    return app.Run(scene);
}
