#include "krom.h"
#include "core/Debug.hpp"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <string>

#ifdef KROM_HAS_GAME_SCRIPTS
extern "C" void KromRegisterGameScripts(engine::script::ScriptRegistry& registry);
#endif

namespace {

struct SceneLaunchConfig
{
    std::string projectFile;
    int sceneIndex = 1;
    bool editorLiveState = false;
};

int SelectBackend() noexcept
{
#if defined(KROM_APP_BACKEND_DX11)
    return Krom::Renderer::DX11;
#elif defined(KROM_APP_BACKEND_OPENGL)
    return Krom::Renderer::OpenGL;
#elif defined(KROM_APP_BACKEND_VULKAN)
    return Krom::Renderer::Vulkan;
#else
    return Krom::DefaultRenderer();
#endif
}

SceneLaunchConfig ResolveSceneLaunchConfig(int argc, char** argv)
{
    SceneLaunchConfig launch{};

    // Projektdatei: Argument → neben der EXE (exportiertes Spiel) → Entwicklungs-Default.
    std::error_code ec;
    const std::filesystem::path exeDir = (argc >= 1 && argv[0])
        ? std::filesystem::absolute(argv[0], ec).parent_path()
        : std::filesystem::current_path(ec);
    if (std::filesystem::exists(exeDir / "krom-project.json", ec))
        launch.projectFile = (exeDir / "krom-project.json").generic_string();
#ifdef KROM_DEFAULT_PROJECT_FILE
    else if (std::filesystem::exists(KROM_DEFAULT_PROJECT_FILE, ec))
        launch.projectFile = KROM_DEFAULT_PROJECT_FILE;
#endif

    if (argc >= 2 && argv[1] && argv[1][0] != '\0')
        launch.projectFile = argv[1];

    for (int i = 2; i < argc; ++i)
    {
        const std::string arg = argv[i] ? argv[i] : "";
        if (arg == "--editor-live-state")
        {
            launch.editorLiveState = true;
            continue;
        }
        if (!arg.empty())
            launch.sceneIndex = std::max(1, std::atoi(arg.c_str()));
    }

    return launch;
}

} // namespace

int main(int argc, char** argv)
{
    engine::Debug::ResetMinLevelForBuild();

    const SceneLaunchConfig launch = ResolveSceneLaunchConfig(argc, argv);
    if (!Krom::LoadProject(launch.projectFile.c_str()))
        return 1;
    Krom::SetEditorLiveStateEnabled(launch.editorLiveState);

#ifdef KROM_HAS_GAME_SCRIPTS
    KromRegisterGameScripts(Krom::GetScriptRegistry());
#endif

    Krom::GraphicsConfig config{};
    config.backend = SelectBackend();
    config.width = 1280u;
    config.height = 720u;
    config.title = "KROM Editor Scene Runtime";
    config.clearColor = {0.04f, 0.05f, 0.06f, 1.0f};
    config.vsync = false;
    config.enableGtao = true;

    if (!Krom::Graphics(config))
        return 1;

    const LPSCENE scene = Krom::LoadScene(launch.sceneIndex);
    if (!scene.IsValid())
        return 2;

    return Krom::Run([](float)
    {
        if (Krom::KeyHit(Krom::Key::Escape))
            Krom::Quit();
    });
}
