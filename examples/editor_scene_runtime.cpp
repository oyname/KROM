#include "krom.h"
#include "core/Debug.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <mutex>
#include <string>

#ifdef KROM_HAS_GAME_SCRIPTS
extern "C" void KromRegisterGameScripts(engine::script::ScriptRegistry& registry);
#endif

namespace {

// Frames pro Szene im Warm-up — Pipelines entstehen beim ersten Zeichnen, ein
// paar Frames mehr erfassen auch Passes, die erst nach dem Start anlaufen.
constexpr int kWarmupFramesPerScene = 10;

struct SceneLaunchConfig
{
    std::string projectFile;
    std::filesystem::path exeDir;
    int sceneIndex = 1;
    bool editorLiveState = false;
    bool warmupShaders = false;
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
    launch.exeDir = (argc >= 1 && argv[0])
        ? std::filesystem::absolute(argv[0], ec).parent_path()
        : std::filesystem::current_path(ec);
    if (std::filesystem::exists(launch.exeDir / "krom-project.json", ec))
        launch.projectFile = (launch.exeDir / "krom-project.json").generic_string();
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
        if (arg == "--warmup-shaders")
        {
            launch.warmupShaders = true;
            continue;
        }
        if (!arg.empty())
            launch.sceneIndex = std::max(1, std::atoi(arg.c_str()));
    }

    return launch;
}

#if !defined(NDEBUG)
void InstallLogFile(const std::filesystem::path&) {}
#else
// Release-Runtimes haben kein Konsolenfenster: Warnungen und Fehler landen in
// krom.log neben der EXE, damit z. B. fehlende Shader nachvollziehbar bleiben.
void InstallLogFile(const std::filesystem::path& exeDir)
{
    static std::mutex mutex;
#if defined(_WIN32)
    static FILE* file = _wfopen((exeDir / "krom.log").c_str(), L"w");
#else
    static FILE* file = std::fopen((exeDir / "krom.log").c_str(), "w");
#endif
    if (!file)
        return;
    engine::Debug::onLog = [](engine::LogLevel level, const char* message) {
        if (level < engine::LogLevel::Warning)
            return;
        std::lock_guard<std::mutex> lock(mutex);
        std::fprintf(file, "%s %s\n", level >= engine::LogLevel::Error ? "[ERR ]" : "[WARN]", message);
        std::fflush(file);
    };
}
#endif

} // namespace

int main(int argc, char** argv)
{
    engine::Debug::ResetMinLevelForBuild();

    const SceneLaunchConfig launch = ResolveSceneLaunchConfig(argc, argv);
    InstallLogFile(launch.exeDir);
    if (launch.warmupShaders)
    {
        // Relative Caches (.krom/ibl) sollen im Spielordner landen, nicht im
        // Arbeitsverzeichnis des Editors, der den Warm-up startet.
        std::error_code ec;
        std::filesystem::current_path(launch.exeDir, ec);
    }
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
    if (launch.warmupShaders)
    {
        // Export: unsichtbar und stumm jede Szene kurz rendern, damit der
        // Shader-Cache alles enthaelt, was das Spiel braucht.
        config.visible = false;
        config.audioDevice = false;
    }

    if (!Krom::Graphics(config))
        return 1;

    const int firstScene = launch.warmupShaders ? 1 : launch.sceneIndex;
    const LPSCENE scene = Krom::LoadScene(firstScene);
    if (!scene.IsValid())
        return 2;

    if (launch.warmupShaders)
    {
        int frame = 0;
        int sceneIndex = firstScene;
        return Krom::Run([&frame, &sceneIndex](float)
        {
            if (++frame < kWarmupFramesPerScene)
                return;
            frame = 0;
            if (++sceneIndex > Krom::GetSceneCount() || !Krom::LoadScene(sceneIndex).IsValid())
                Krom::Quit();
        });
    }

    return Krom::Run([](float)
    {
        if (Krom::KeyHit(Krom::Key::Escape))
            Krom::Quit();
    });
}
