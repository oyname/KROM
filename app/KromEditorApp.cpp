#include "renderer/MaterialDomain.hpp"
#include "core/PathUtf8.hpp"
#include "renderer/RenderFrameConstants.hpp"
#include "renderer/RenderLayers.hpp"
#include "renderer/ShaderCompiler.hpp"
#include "renderer/runtime/MaterialRuntimeBridge.hpp"
#include "renderer/runtime/MaterialRuntimeDesc.hpp"
#include "app/KromEditorApp.hpp"
#include "app/editor/BuildTools.hpp"
#include "renderer/ShaderToolPaths.hpp"

#include "addons/camera/CameraComponents.hpp"
#include "addons/animation/AnimationComponents.hpp"
#include "addons/animation/AnimationSystem.hpp"
#include "addons/camera/CameraViewBuilder.hpp"
#include "addons/camera/CameraSerialization.hpp"
#include "addons/debug_draw/DebugDraw.hpp"
#include "addons/forward/ForwardFeature.hpp"
#include "addons/lighting/LightingComponents.hpp"
#include "addons/lighting/LightingFeature.hpp"
#include "addons/lighting/LightingSerialization.hpp"
#include "addons/gltf/GltfImporter.hpp"
#include "addons/mesh_renderer/MeshAssetSceneBindings.hpp"
#include "addons/mesh_renderer/MeshRendererComponents.hpp"
#include "assets/MeshTangents.hpp"
#include "addons/mesh_renderer/MeshRendererFeature.hpp"
#include "addons/mesh_renderer/MeshRendererSerialization.hpp"
#include "addons/editor/EditorControllerGraph.hpp"
#include "addons/prefab/Prefab.hpp"
#include "addons/prefab/PrefabInstanceComponent.hpp"
#include "addons/prefab/PrefabSerialization.hpp"
#include "addons/script/ScriptList.hpp"
#include "addons/script/ScriptSerialization.hpp"
#include "addons/physics/PhysicsComponents.hpp"
#include "addons/physics/PhysicsSerialization.hpp"
#include "addons/audio/AudioComponents.hpp"
#include "addons/audio/AudioSerialization.hpp"
#include "addons/particles/ParticleComponents.hpp"
#include "addons/particles/ParticleSerialization.hpp"
#include "addons/editor/EditorFileNaming.hpp"
#include "addons/editor/EditorProjectHistory.hpp"
#include "addons/editor/EditorPreferences.hpp"
#include "core/AtomicTextFile.hpp"
#include "addons/editor/EditorLocalization.hpp"
#include "addons/editor/EditorPrefabWindow.hpp"
#include "addons/editor/EditorScriptAssets.hpp"
#include "addons/shadow/ShadowFeature.hpp"
#include "addons/gtao/GtaoFeature.hpp"
#include "addons/probes/ProbeComponents.hpp"
#include "addons/probes/ProbeData.hpp"
#include "addons/probes/ProbeFeature.hpp"
#include "addons/probes/ProbeSerialization.hpp"
#include "addons/outline/OutlineFeature.hpp"
#include "core/Debug.hpp"
#include "serialization/SceneSerializer.hpp"
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <fstream>
#include <filesystem>
#include <functional>
#include <mutex>
#include <optional>
#if defined(_WIN32)
#include <process.h>
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#ifdef CreateWindow
#undef CreateWindow
#endif
#ifdef small
#undef small
#endif
#endif
#include <random>
#include <regex>
#include <span>
#include <thread>
#include <utility>
#include <unordered_map>
#include <tuple>
#include <unordered_set>

using engine::PathFromUtf8;
using engine::PathToUtf8;
using engine::PathToGenericUtf8;
using engine::renderer::addons::editor::Translate;
using engine::renderer::addons::editor::TranslateWithValue;

#ifdef KROM_HAS_GAME_SCRIPTS
extern "C" void KromRegisterGameScripts(engine::script::ScriptRegistry& registry);
#endif

namespace {
engine::ecs::World* g_editorScriptWorld = nullptr;
engine::script::ScriptRegistry* g_editorScriptRegistry = nullptr;

std::string GenerateGuidString()
{
    static std::mt19937_64 rng{std::random_device{}()};
    const uint64_t a = rng();
    const uint64_t b = rng();

    char buf[37];
    std::snprintf(buf, sizeof(buf),
        "%08x-%04x-%04x-%04x-%012llx",
        static_cast<unsigned>((a >> 32) & 0xffffffffu),
        static_cast<unsigned>((a >> 16) & 0xffffu),
        static_cast<unsigned>(a & 0xffffu),
        static_cast<unsigned>((b >> 48) & 0xffffu),
        static_cast<unsigned long long>(b & 0xffffffffffffull));
    return buf;
}

void EnsureEntityGuids(engine::ecs::World& world)
{
    std::unordered_set<std::string> used;
    world.ForEachAlive([&](engine::EntityID id) {
        if (const auto* guid = world.Get<engine::GuidComponent>(id))
            if (!guid->guid.empty())
                used.insert(guid->guid);
    });

    world.ForEachAlive([&](engine::EntityID id) {
        auto* guid = world.Get<engine::GuidComponent>(id);
        if (guid && !guid->guid.empty())
            return;

        std::string value;
        do {
            value = GenerateGuidString();
        } while (used.count(value) > 0u);
        used.insert(value);

        if (guid)
            guid->guid = std::move(value);
        else
            world.Add<engine::GuidComponent>(id, engine::GuidComponent{std::move(value)});
    });
}

const char* RuntimeBackendId(engine::renderer::DeviceFactory::BackendType backend) noexcept
{
    using BackendType = engine::renderer::DeviceFactory::BackendType;
    switch (backend)
    {
        case BackendType::DirectX11: return "dx11";
        case BackendType::Vulkan:    return "vulkan";
        case BackendType::OpenGL:    return "opengl";
        default:                     return "dx11";
    }
}

std::string SanitizeRuntimeTargetName(std::string name)
{
    for (char& ch : name)
    {
        const bool alphaNum = (ch >= 'a' && ch <= 'z') ||
                              (ch >= 'A' && ch <= 'Z') ||
                              (ch >= '0' && ch <= '9');
        if (!(alphaNum || ch == '_'))
            ch = '_';
    }
    if (name.empty())
        name = "KromGame";
    if (name[0] >= '0' && name[0] <= '9')
        name = "Krom_" + name;
    return name;
}

std::string RuntimeTargetName(const std::string& runtimeTarget,
                              engine::renderer::DeviceFactory::BackendType backend)
{
    return "krom_runtime_" + SanitizeRuntimeTargetName(runtimeTarget) + "_" + RuntimeBackendId(backend);
}

std::string RuntimeExecutableName(const std::string& runtimeTarget,
                                  engine::renderer::DeviceFactory::BackendType backend)
{
    return SanitizeRuntimeTargetName(runtimeTarget) + "_" + RuntimeBackendId(backend) + ".exe";
}

std::filesystem::path RuntimeExecutablePath(const std::filesystem::path& buildDir,
                                            const std::string& runtimeTarget,
                                            engine::renderer::DeviceFactory::BackendType backend)
{
    return buildDir / "bin" / PathFromUtf8(RuntimeExecutableName(runtimeTarget, backend));
}

std::filesystem::path RuntimeProfileDir(const char* configuration)
{
    return PathFromUtf8(KROM_BUILD_DIR).parent_path() / PathFromUtf8(std::string("runtime-") + configuration);
}

constexpr size_t kMaxConsoleLogs = 512u;

void AddEditorConsoleLog(engine::renderer::addons::editor::EditorState* state,
                         engine::LogLevel level,
                         std::string message)
{
    if (!state)
        return;

    if (state->consoleLogs.size() >= kMaxConsoleLogs)
        state->consoleLogs.erase(state->consoleLogs.begin());
    state->consoleLogs.push_back({ level, std::move(message) });
    state->consoleScrollToBottom = true;
}

#ifdef KROM_EDITOR_HAS_IMGUI
using ConsoleEntry = engine::renderer::addons::editor::EditorState::LogEntry;
std::mutex g_consoleLogMutex;
std::deque<ConsoleEntry> g_consoleLogQueue;

void CaptureConsoleLog(engine::LogLevel level, const char* message)
{
    std::lock_guard lock(g_consoleLogMutex);
    if (g_consoleLogQueue.size() >= kMaxConsoleLogs)
        g_consoleLogQueue.pop_front();
    g_consoleLogQueue.push_back({level, message});
}

void DrainConsoleLogs(engine::renderer::addons::editor::EditorState* state)
{
    std::deque<ConsoleEntry> logs;
    {
        std::lock_guard lock(g_consoleLogMutex);
        logs.swap(g_consoleLogQueue);
    }
    for (auto& entry : logs)
        AddEditorConsoleLog(state, entry.level, std::move(entry.message));
}
#endif

void RememberProject(const std::filesystem::path& projectFile)
{
    using namespace engine::renderer::addons::editor;
    const std::string path = PathToUtf8(projectFile);
    if (GetEditorPreferences().lastProject == path)
        return;
    auto preferences = GetEditorPreferences();
    preferences.lastProject = path;
    if (!SaveEditorPreferences(preferences))
        engine::Debug::LogWarning("KromEditorApp: failed to record the last opened project");
}

std::string RuntimeScriptFieldKey(const std::string& entityGuid,
                                  uint32_t scriptIndex,
                                  const std::string& fieldName)
{
    return entityGuid + "|" + std::to_string(scriptIndex) + "|" + fieldName;
}

bool RuntimeScriptFieldTypeFromString(const std::string& type,
                                      engine::script::ScriptFieldType& outType) noexcept
{
    using engine::script::ScriptFieldType;
    if (type == "Float")  { outType = ScriptFieldType::Float; return true; }
    if (type == "Int")    { outType = ScriptFieldType::Int; return true; }
    if (type == "Bool")   { outType = ScriptFieldType::Bool; return true; }
    if (type == "Vec3")   { outType = ScriptFieldType::Vec3; return true; }
    if (type == "String") { outType = ScriptFieldType::String; return true; }
    if (type == "Prefab") { outType = ScriptFieldType::Prefab; return true; }
    if (type == "Entity") { outType = ScriptFieldType::Entity; return true; }
    return false;
}

bool ReadRuntimeScriptFieldValue(const engine::serialization::JsonValue& node,
                                 engine::script::ScriptFieldType type,
                                 engine::script::ScriptFieldValue& outValue) noexcept
{
    outValue.type = type;
    switch (type)
    {
    case engine::script::ScriptFieldType::Float:
        if (!node.IsNumber()) return false;
        outValue.floatValue = node.AsFloat();
        return true;
    case engine::script::ScriptFieldType::Int:
        if (!node.IsNumber()) return false;
        outValue.intValue = node.AsInt();
        return true;
    case engine::script::ScriptFieldType::Bool:
        if (!node.IsBool()) return false;
        outValue.boolValue = node.AsBool();
        return true;
    case engine::script::ScriptFieldType::Vec3:
        if (!node.IsArray() || node.arrayVal.size() < 3u) return false;
        outValue.vec3Value = node.AsVec3();
        return true;
    case engine::script::ScriptFieldType::String:
    case engine::script::ScriptFieldType::Prefab:
    case engine::script::ScriptFieldType::Entity:
        if (!node.IsString()) return false;
        outValue.stringValue = node.AsString();
        return true;
    }
    return false;
}

void PollRuntimeStateFile(engine::renderer::addons::editor::EditorState& state,
                          const std::filesystem::path& projectRoot,
                          double deltaSeconds)
{
    if (projectRoot.empty())
        return;

    state.runtimeStatePollTimer += deltaSeconds;
    if (state.runtimeStatePollTimer < 0.2)
        return;
    state.runtimeStatePollTimer = 0.0;

    const std::filesystem::path path = projectRoot / "editor" / "runtime-state.json";
    state.runtimeStateFilePath = PathToUtf8(path);

    std::error_code ec;
    const auto writeTime = std::filesystem::last_write_time(path, ec);
    if (ec)
    {
        state.runtimeScriptFieldValues.clear();
        return;
    }
    const auto runtimeAge = std::filesystem::file_time_type::clock::now() - writeTime;
    if (runtimeAge > std::chrono::seconds(2))
    {
        state.runtimeScriptFieldValues.clear();
        return;
    }
    if (writeTime == state.runtimeStateLastWriteTime)
        return;
    state.runtimeStateLastWriteTime = writeTime;

    std::ifstream in(path, std::ios::binary);
    if (!in)
        return;
    const std::string json((std::istreambuf_iterator<char>(in)),
                           std::istreambuf_iterator<char>());

    std::string error;
    const engine::serialization::JsonValue root =
        engine::serialization::JsonParser::Parse(json, error);
    if (!error.empty() || !root.IsObject())
    {
        state.runtimeStateLastError = error.empty() ? "runtime-state root is not an object" : error;
        return;
    }

    const auto* fields = root.Get("fields");
    if (!fields || !fields->IsArray())
        return;

    std::unordered_map<std::string, engine::script::ScriptFieldValue> nextValues;
    for (const engine::serialization::JsonValue& item : fields->arrayVal)
    {
        if (!item.IsObject())
            continue;
        const auto* guidNode = item.Get("entityGuid");
        const auto* indexNode = item.Get("scriptIndex");
        const auto* fieldNode = item.Get("field");
        const auto* typeNode = item.Get("type");
        const auto* valueNode = item.Get("value");
        if (!guidNode || !guidNode->IsString() ||
            !indexNode || !indexNode->IsNumber() ||
            !fieldNode || !fieldNode->IsString() ||
            !typeNode || !typeNode->IsString() ||
            !valueNode)
            continue;

        engine::script::ScriptFieldType type{};
        if (!RuntimeScriptFieldTypeFromString(typeNode->AsString(), type))
            continue;

        engine::script::ScriptFieldValue value{};
        if (!ReadRuntimeScriptFieldValue(*valueNode, type, value))
            continue;

        nextValues[RuntimeScriptFieldKey(guidNode->AsString(), indexNode->AsUint(), fieldNode->AsString())] =
            std::move(value);
    }

    state.runtimeScriptFieldValues = std::move(nextValues);
    state.runtimeStateLastError.clear();
}

std::string ToLowerAscii(std::string value)
{
    for (char& ch : value)
        if (ch >= 'A' && ch <= 'Z')
            ch = static_cast<char>(ch - 'A' + 'a');
    return value;
}

engine::LogLevel BuildOutputLevel(const std::string& line)
{
    const std::string lower = ToLowerAscii(line);
    if (lower.find("fatal error") != std::string::npos ||
        lower.find("cmake error") != std::string::npos ||
        lower.find(": error ") != std::string::npos ||
        lower.find(" error c") != std::string::npos ||
        lower.find(" error lnk") != std::string::npos ||
        lower.find("failed:") != std::string::npos)
    {
        return engine::LogLevel::Error;
    }
    if (lower.find(": warning ") != std::string::npos ||
        lower.find(" warning c") != std::string::npos ||
        lower.find(" warning lnk") != std::string::npos)
    {
        return engine::LogLevel::Warning;
    }
    return engine::LogLevel::Verbose;
}

bool ShouldShowBuildOutputLine(const std::string& line)
{
    return BuildOutputLevel(line) >= engine::LogLevel::Warning;
}

// What to build. Tool discovery is platform-specific; the resolved configuration
// and build steps carry only executable paths, arguments, a working directory and
// an environment.
struct RuntimeBuildRequest
{
    std::filesystem::path sourceDir;
    std::filesystem::path buildDir;
    std::filesystem::path scriptProject;
    std::string target;
    std::string configuration;
    bool scriptBuild = false;
    // runtime-<config> folders belong to the editor and may be reconfigured with new
    // tools; the editor's own build folder (KROM_BUILD_DIR) never is.
    bool editorOwnedBuildDir = false;
    // Tool paths from the personal editor settings, copied on the main thread.
    engine::app::build::BuildToolOverrides toolOverrides;
};

engine::app::build::BuildToolOverrides BuildToolOverridesFrom(
    const engine::renderer::addons::editor::EditorPreferences& preferences)
{
    return {PathFromUtf8(preferences.toolVisualStudio), PathFromUtf8(preferences.toolCMake),
            PathFromUtf8(preferences.toolNinja)};
}

// Main thread: only checks that need no build tools. Discovery (vswhere, VsDevCmd,
// compiler probe: seconds) runs on the build worker so the editor never blocks.
bool PrepareRuntimeBuild(const std::string& runtimeTarget,
                         engine::renderer::DeviceFactory::BackendType backend,
                         const std::filesystem::path& buildDir,
                         engine::renderer::addons::editor::EditorState* editorState,
                         RuntimeBuildRequest& outRequest,
                         const std::string& scriptTarget = {},
                         const std::filesystem::path& scriptProject = {},
                         const char* configuration = "Release")
{
    const bool scriptBuild = !scriptTarget.empty();
    if (scriptBuild && !std::filesystem::exists(buildDir / "CMakeCache.txt"))
    {
        AddEditorConsoleLog(editorState, engine::LogLevel::Error, Translate("build.editor_folder_missing"));
        return false;
    }
    outRequest = {};
    outRequest.sourceDir = PathFromUtf8(KROM_SOURCE_DIR);
    outRequest.buildDir = buildDir;
    outRequest.scriptProject = scriptProject;
    outRequest.target = scriptBuild ? scriptTarget : RuntimeTargetName(runtimeTarget, backend);
    outRequest.configuration = configuration;
    outRequest.scriptBuild = scriptBuild;
    outRequest.editorOwnedBuildDir =
        buildDir.lexically_normal() != PathFromUtf8(KROM_BUILD_DIR).lexically_normal();
    outRequest.toolOverrides =
        BuildToolOverridesFrom(engine::renderer::addons::editor::GetEditorPreferences());
    engine::Debug::LogVerbose("%s", TranslateWithValue("build.start_target", outRequest.target).c_str());
    return true;
}

// Build-job log lines get their "[Build] " prefix when the editor shows them.
std::string BuildLogText(std::string text, const std::string& detail = {})
{
    constexpr std::string_view prefix = "[Build] ";
    if (text.starts_with(prefix)) text.erase(0, prefix.size());
    if (!detail.empty()) text += " " + detail;
    return text;
}

// Build worker: discovers the tools and turns the request into process steps.
bool ResolveRuntimeBuildCommands(const RuntimeBuildRequest& request,
                                 const std::function<void(engine::LogLevel, std::string)>& log,
                                 std::vector<engine::app::build::ProcessCommand>& commands)
{
    using namespace engine::app::build;
    auto fail = [&](const std::string& key, const std::string& detail = {}) {
        log(engine::LogLevel::Error, BuildLogText(Translate(key.c_str()), detail));
        return false;
    };
#ifdef _MSC_FULL_VER
    constexpr uint32_t editorCompiler = _MSC_FULL_VER;
#else
    constexpr uint32_t editorCompiler = 0;
#endif
    const auto discovery = GetBuildTools(editorCompiler, request.toolOverrides);
    if (!discovery.Succeeded()) return fail(discovery.errorKey, discovery.detail);
    const auto& tools = discovery.tools;
    if (editorCompiler && !CompilerCompatible(editorCompiler, tools.compilerVersion, request.scriptBuild))
        return fail(request.scriptBuild ? "build.script_compiler_mismatch" : "build.compiler_too_old",
            "Editor: " + std::to_string(editorCompiler) + ", MSVC: " + std::to_string(tools.compilerVersion));
    std::string key, detail;
    std::error_code ec;
    if (!CheckBuildCache(request.buildDir, tools, key, detail))
    {
        if (!request.editorOwnedBuildDir) return fail(key, detail);
        // The editor created this folder itself (e.g. before a Visual Studio update):
        // configure it again with the current tools instead of asking the user.
        std::filesystem::remove(request.buildDir / "CMakeCache.txt", ec);
        if (!ec) std::filesystem::remove_all(request.buildDir / "CMakeFiles", ec);
        if (ec) return fail("build.directory_failed", ec.message());
        log(engine::LogLevel::Warning,
            BuildLogText(TranslateWithValue("build.cache_reset", PathToUtf8(request.buildDir))));
    }
    const bool needsConfigure = !std::filesystem::exists(request.buildDir / "CMakeCache.txt");
    std::filesystem::create_directories(request.buildDir, ec);
    if (ec) return fail("build.directory_failed", ec.message());

    commands.clear();
    auto configure = [&](std::vector<std::string> options) {
        ProcessCommand command{tools.cmake, {"-S", PathToUtf8(request.sourceDir), "-B", PathToUtf8(request.buildDir)},
            request.sourceDir, tools.environment};
        command.arguments.insert(command.arguments.end(), options.begin(), options.end());
        commands.push_back(std::move(command));
    };
    if (needsConfigure)
    {
        log(engine::LogLevel::Info,
            BuildLogText(TranslateWithValue("build.configuring_path", PathToUtf8(request.buildDir))));
        configure({"-G", "Ninja", "-DCMAKE_BUILD_TYPE=" + request.configuration,
            "-DCMAKE_MAKE_PROGRAM=" + PathToGenericUtf8(tools.ninja),
            "-DCMAKE_CXX_COMPILER=" + PathToGenericUtf8(tools.compiler)});
    }
    // External-project runtime support remains a separate change (point 2).
    if (request.scriptBuild &&
        request.scriptProject.parent_path().lexically_normal() != (request.sourceDir / "projects").lexically_normal())
        configure({"-DKROM_PROJECT_SCRIPT_ROOT=" + PathToUtf8(request.scriptProject)});
    commands.push_back({tools.ninja, {"-C", PathToUtf8(request.buildDir), request.target},
        request.sourceDir, tools.environment});
    return true;
}

bool RunRuntimeBuildCommands(const std::vector<engine::app::build::ProcessCommand>& commands,
                            const std::function<void(engine::LogLevel, std::string)>& log,
                            const std::function<void(std::string)>& progress,
                            const std::atomic<bool>& cancel)
{
    using namespace engine::app::build;
    for (const auto& command : commands)
    {
        bool showedDiagnostic = false;
        std::string pending;
        std::deque<std::string> lastLines;
        auto consumeLine = [&](std::string line) {
            while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) line.pop_back();
            if (line.empty()) return;
            if (lastLines.size() == 12) lastLines.pop_front();
            lastLines.push_back(line);
            if (line.front() == '[' && line.find('/') != std::string::npos && line.find(']') != std::string::npos)
                progress(line);
            if (ShouldShowBuildOutputLine(line))
            {
                showedDiagnostic = true;
                log(BuildOutputLevel(line), std::move(line));
            }
        };
        const auto result = RunProcess(command, [&](const std::string& chunk) {
            pending += chunk;
            size_t newline;
            while ((newline = pending.find('\n')) != std::string::npos)
            {
                consumeLine(pending.substr(0, newline));
                pending.erase(0, newline + 1);
            }
        }, &cancel);
        if (!pending.empty()) consumeLine(std::move(pending));
        if (result.cancelled || cancel.load(std::memory_order_acquire))
        {
            log(engine::LogLevel::Warning, Translate("build.cancelled"));
            return false;
        }
        if (result.Succeeded()) continue;
        if (!showedDiagnostic)
            for (const auto& line : lastLines) log(engine::LogLevel::Error, line);
        if (!result.error.empty()) log(engine::LogLevel::Error, result.error);
        log(engine::LogLevel::Error, TranslateWithValue("build.process_failed", std::to_string(result.exitCode)));
        return false;
    }
    return true;
}

// Startet die exportierte Runtime mit --warmup-shaders und wartet auf das Ende.
// Fehler und Warnungen des Laufs stehen in krom.log neben der EXE.
bool RunShaderWarmup(const std::filesystem::path& exePath,
                     const std::filesystem::path& projectFile,
                     engine::renderer::addons::editor::EditorState* editorState)
{
#if defined(_WIN32)
    // _wspawnv setzt die Argumente nur mit Leerzeichen zusammen — Pfade quoten.
    const std::wstring exeArg     = L"\"" + exePath.wstring() + L"\"";
    const std::wstring projectArg = L"\"" + projectFile.wstring() + L"\"";
    const std::wstring warmupArg  = L"--warmup-shaders";
    const wchar_t* args[] = { exeArg.c_str(), projectArg.c_str(), warmupArg.c_str(), nullptr };
    const intptr_t exitCode = _wspawnv(_P_WAIT, exePath.c_str(), args);
    if (exitCode == -1)
    {
        AddEditorConsoleLog(editorState, engine::LogLevel::Error,
            Translate("export.warmup_launch_failed"));
        return false;
    }

    const std::filesystem::path logPath = exePath.parent_path() / "krom.log";
    std::ifstream log(logPath);
    std::string line;
    size_t errors = 0u;
    while (std::getline(log, line))
    {
        if (line.rfind("[ERR ]", 0u) != 0u)
            continue;
        if (++errors <= 10u)
            AddEditorConsoleLog(editorState, engine::LogLevel::Error, "[Export] " + line);
    }
    if (errors > 10u)
        AddEditorConsoleLog(editorState, engine::LogLevel::Error,
            "[Export] ... " + std::to_string(errors - 10u) + " weitere Fehler in " + PathToUtf8(logPath));

    if (exitCode != 0)
    {
        AddEditorConsoleLog(editorState, engine::LogLevel::Error,
            TranslateWithValue("export.warmup_exit", std::to_string(exitCode)));
        return false;
    }
    if (errors > 0u)
        AddEditorConsoleLog(editorState, engine::LogLevel::Warning,
            Translate("export.warmup_log") + PathToUtf8(logPath));
    return true;
#else
    (void)exePath;
    (void)projectFile;
    (void)editorState;
    return true;
#endif
}

bool LaunchRuntimeProcess(const std::filesystem::path& exePath,
                          const std::filesystem::path& projectFile,
                          bool editorLiveState)
{
#if defined(_WIN32)
    if (!std::filesystem::exists(exePath) || !std::filesystem::exists(projectFile))
        return false;
    std::wstring command = L"cmd /c start \"KROM Play\" /D \"";
    command += exePath.parent_path().wstring();
    command += L"\" \"";
    command += exePath.wstring();
    command += L"\" \"";
    command += projectFile.wstring();
    command += L"\" 1";
    if (editorLiveState)
        command += L" --editor-live-state";
    return _wsystem(command.c_str()) == 0;
#else
    (void)exePath;
    (void)projectFile;
    (void)editorLiveState;
    return false;
#endif
}
}

namespace Krom {

engine::ecs::World& GetWorld()
{
    return *g_editorScriptWorld;
}

engine::script::ScriptRegistry& GetScriptRegistry()
{
    return *g_editorScriptRegistry;
}

bool KeyDown(engine::platform::Key)
{
    return false;
}

bool KeyHit(engine::platform::Key)
{
    return false;
}

bool MouseButtonDown(engine::platform::MouseButton)
{
    return false;
}

int MouseDeltaX()
{
    return 0;
}

int MouseDeltaY()
{
    return 0;
}

float MouseScrollDelta()
{
    return 0.0f;
}

} // namespace Krom

#if defined(KROM_APP_USE_WIN32_PLATFORM)
#include "platform/Win32Platform.hpp"
#elif defined(KROM_APP_USE_GLFW_PLATFORM)
#include "platform/GLFWPlatform.hpp"
#else
#error App platform not configured. Define KROM_APP_USE_WIN32_PLATFORM or KROM_APP_USE_GLFW_PLATFORM.
#endif

#ifdef KROM_EDITOR_HAS_IMGUI
#include "addons/editor/EditorAssetBrowser.hpp"
#include "addons/editor/EditorComponents.hpp"
#include "addons/editor/EditorCommands.hpp"
#include "addons/editor/EditorThumbnailCache.hpp"
#include "addons/editor/EditorThumbnailFraming.hpp"
#include "addons/editor/EditorFeature.hpp"
#include "addons/editor/EditorSerialization.hpp"
#include "addons/editor/EditorMaterialLibrary.hpp"
#include "addons/editor/EditorAnimationWindow.hpp"
#include "addons/editor/EditorIcons.hpp"
#include "addons/editor/EditorTheme.hpp"
#include "addons/editor/EditorUI.hpp"
#include "app/editor/EditorWin32Hook.hpp"
#include "imgui.h"

// Platform-Backend: GLFW
#if defined(KROM_APP_USE_GLFW_PLATFORM) && defined(KROM_IMGUI_HAS_GLFW)
#include "imgui_impl_glfw.h"
#include <GLFW/glfw3.h>
#endif

// Platform-Backend: Win32 (nur fuer NewFrame/Init, kein HWND noetig)
#if defined(KROM_APP_USE_WIN32_PLATFORM) && defined(KROM_IMGUI_HAS_WIN32)
#include "imgui_impl_win32.h"
#endif

// Renderer-Backend
#if defined(KROM_APP_BACKEND_DX11) && defined(KROM_IMGUI_HAS_DX11)
#include "imgui_impl_dx11.h"
#include "addons/dx11/DX11Device.hpp"
#include <dxgi.h>
// Undocumented in imgui_impl_dx11.h; the backend asks callers to declare it.
// Note: the backend copies only the first template, so pass exactly one.
void ImGui_ImplDX11_SetSwapChainDescs(const DXGI_SWAP_CHAIN_DESC* desc_templates, int desc_templates_count);
#elif defined(KROM_APP_BACKEND_OPENGL) && defined(KROM_IMGUI_HAS_OPENGL3)
#include "imgui_impl_opengl3.h"
#elif defined(KROM_APP_BACKEND_VULKAN) && defined(KROM_IMGUI_HAS_VULKAN)
#include "imgui_impl_vulkan.h"
#include "addons/vulkan/VulkanDevice.hpp"
#endif

namespace {

// Multi-viewports: floating editor windows may leave the editor window and
// become OS windows (e.g. the material editor on a second monitor). Which
// panels may do that is decided in the editor UI (KeepNextWindowInMainViewport).
void EnableEditorMultiViewports(ImGuiIO& io)
{
    io.ConfigFlags |= ImGuiConfigFlags_ViewportsEnable;
    // Own OS windows get a task bar entry so they can be found again
    // behind the editor window.
    io.ConfigViewportsNoTaskBarIcon = false;
}

#if defined(KROM_APP_BACKEND_DX11) && defined(KROM_IMGUI_HAS_DX11)
// Secondary viewports must use the same (sRGB) format as the editor swapchain,
// otherwise the linear theme colors come out too dark. The bitblt swap effect
// allows sRGB buffer formats.
void ConfigureDx11ViewportSwapchains(engine::renderer::Format backbufferFormat)
{
    using engine::renderer::Format;
    DXGI_SWAP_CHAIN_DESC sd{};
    switch (backbufferFormat)
    {
    case Format::RGBA8_UNORM_SRGB: sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB; break;
    case Format::BGRA8_UNORM_SRGB: sd.BufferDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM_SRGB; break;
    case Format::BGRA8_UNORM:      sd.BufferDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM; break;
    default:                       sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM; break;
    }
    sd.SampleDesc.Count = 1u;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.BufferCount = 1u;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
    ImGui_ImplDX11_SetSwapChainDescs(&sd, 1);
}
#endif

#if defined(KROM_APP_BACKEND_VULKAN) && defined(KROM_IMGUI_HAS_VULKAN) && \
    defined(KROM_APP_USE_WIN32_PLATFORM) && defined(KROM_IMGUI_HAS_WIN32) && defined(_WIN32)
} // namespace
// vulkan.h came in through imgui_impl_vulkan.h before VK_USE_PLATFORM_WIN32_KHR
// was defined, so the Win32 surface API needs its own header.
#include <vulkan/vulkan_win32.h>
namespace {
// imgui_impl_win32 has no Vulkan surface hook (only the GLFW backend does);
// the Vulkan backend needs one to create swapchains for secondary viewports.
int CreateWin32ViewportVkSurface(ImGuiViewport* viewport, ImU64 instance,
                                 const void* allocator, ImU64* outSurface)
{
    HWND hwnd = static_cast<HWND>(viewport->PlatformHandleRaw
        ? viewport->PlatformHandleRaw : viewport->PlatformHandle);
    VkWin32SurfaceCreateInfoKHR sci{ VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR };
    sci.hinstance = GetModuleHandleW(nullptr);
    sci.hwnd = hwnd;
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    const VkResult result = vkCreateWin32SurfaceKHR(
        reinterpret_cast<VkInstance>(instance), &sci,
        static_cast<const VkAllocationCallbacks*>(allocator), &surface);
    *outSurface = reinterpret_cast<ImU64>(surface);
    return static_cast<int>(result);
}
#endif

} // namespace

#endif // KROM_EDITOR_HAS_IMGUI

#include "platform/NativeFileDialog.hpp"

#include <filesystem>
#include <algorithm>
#include <cmath>
#include <fstream>
#include <memory>
#include <cstdint>
#include <cstdio>
#include <unordered_set>

namespace engine::app {

// Runtime-Build im Hintergrund-Thread. Der Worker schreibt nur in logs/progress
// (unter mutex) und setzt am Ende ok + finished; alles Weitere passiert im
// Main-Thread in KromEditorApp::PollRuntimeBuild.
struct RuntimeBuildJob
{
    std::thread worker;
    std::mutex  mutex;
    std::vector<std::pair<LogLevel, std::string>> logs;
    std::string progress;
    std::atomic<bool> finished{ false };
    std::atomic<bool> cancel{ false };
    bool scriptBuild = false;
    bool ok = false;

    std::string target;
    std::filesystem::path projectRoot;
    std::chrono::steady_clock::time_point startTime;
    std::chrono::steady_clock::time_point lastTerminalReport;
    std::function<void(bool)> onFinished;
};

// Nur Datei-I/O und JSON-Parsing laufen im Worker. World, Editor, Assets und
// GPU bleiben im Hauptthread; jede Aufbauphase erhaelt einen normalen UI-Frame.
struct SceneLoadJob
{
    enum class Stage { Save, Textures, Read, Apply, Hierarchy, Meshes, Materials, Prefabs, Finalize, Done };
    Stage stage = Stage::Save;
    std::string name;
    bool additive = false;
    bool openingProject = false;
    bool hasSceneFile = false;
    std::vector<std::filesystem::path> textures;
    std::vector<EntityID> assetEntities;
    size_t nextAsset = 0;
    std::atomic<bool> readFinished{false};
    serialization::JsonValue root;
    std::string error;
};

namespace {

std::filesystem::path ResolveAssetRoot()
{
    return PathFromUtf8(__FILE__).parent_path().parent_path() / "assets";
}

#ifdef KROM_EDITOR_HAS_IMGUI
void ConfigureEditorUiFont(ImGuiIO& io)
{
    std::vector<std::filesystem::path> candidates{ResolveAssetRoot() / "fonts" / "ui.ttf"};
#if defined(_WIN32)
    const wchar_t* windir = _wgetenv(L"WINDIR");
    const std::filesystem::path fonts = std::filesystem::path(windir ? windir : L"C:/Windows") / "Fonts";
    candidates.push_back(fonts / "segoeui.ttf");
    candidates.push_back(fonts / "arial.ttf");
#else
    candidates.emplace_back("/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf");
    candidates.emplace_back("/usr/share/fonts/truetype/liberation2/LiberationSans-Regular.ttf");
    candidates.emplace_back("/usr/share/fonts/TTF/DejaVuSans.ttf");
    candidates.emplace_back("/usr/share/fonts/dejavu-sans-fonts/DejaVuSans.ttf");
#endif
    // Bereich der Symbole (EditorIcons.hpp): nie aus der Textschrift nehmen.
    static const ImWchar iconRanges[] = { ICON_LC_FIRST, ICON_LC_LAST, 0 };
    ImFontConfig config{};
    config.OversampleH = 2;
    config.OversampleV = 2;
    config.GlyphExcludeRanges = iconRanges;
    bool loaded = false;
    for (const auto& path : candidates)
    {
        std::error_code ec;
        if (std::filesystem::is_regular_file(path, ec) &&
            io.Fonts->AddFontFromFileTTF(PathToUtf8(path).c_str(), 16.f, &config))
        {
            loaded = true;
            break;
        }
    }
    if (!loaded)
    {
        config.SizePixels = 16.f;
        io.Fonts->AddFontDefault(&config);
    }

    // Symbole (Lucide) in dieselbe Schrift mischen, siehe EditorIcons.hpp.
    const std::filesystem::path iconFont = ResolveAssetRoot() / "fonts" / "lucide.ttf";
    std::error_code ec;
    if (!std::filesystem::is_regular_file(iconFont, ec))
        return;
    ImFontConfig iconConfig{};
    iconConfig.MergeMode = true;
    iconConfig.PixelSnapH = true;
    iconConfig.GlyphMinAdvanceX = 16.f;
    iconConfig.GlyphOffset = ImVec2(0.f, 2.f);
    iconConfig.GlyphRanges = iconRanges;
    io.Fonts->AddFontFromFileTTF(PathToUtf8(iconFont).c_str(), 16.f, &iconConfig);
}

std::filesystem::path ResolveProjectsRoot()
{
    return PathFromUtf8(__FILE__).parent_path().parent_path() / "projects";
}

const char* ProjectBackendId(renderer::DeviceFactory::BackendType backend) noexcept
{
    switch (backend)
    {
    case renderer::DeviceFactory::BackendType::DirectX11: return "dx11";
    case renderer::DeviceFactory::BackendType::OpenGL: return "opengl";
    case renderer::DeviceFactory::BackendType::Vulkan: return "vulkan";
    default: return "unknown";
    }
}

renderer::DeviceFactory::BackendType BackendFromProjectId(std::string_view id) noexcept
{
    if (id == "dx11") return renderer::DeviceFactory::BackendType::DirectX11;
    if (id == "opengl") return renderer::DeviceFactory::BackendType::OpenGL;
    if (id == "vulkan") return renderer::DeviceFactory::BackendType::Vulkan;
    return renderer::DeviceFactory::BackendType::Vulkan;
}

std::unique_ptr<assets::MeshAsset> CreateMaterialPreviewSphereMesh()
{
    constexpr uint32_t kSlices = 48u;
    constexpr uint32_t kStacks = 32u;

    auto mesh = std::make_unique<assets::MeshAsset>();
    mesh->path = "__editor/material_preview_sphere";
    mesh->debugName = "Material Preview Sphere";
    mesh->state = assets::AssetState::Loaded;
    mesh->gpuStatus.dirty = true;
    mesh->gpuStatus.uploaded = false;
    mesh->materialHandles.resize(1u);

    assets::SubMeshData sub{};
    sub.materialIndex = 0u;
    sub.positions.reserve((kSlices + 1u) * (kStacks + 1u) * 3u);
    sub.normals.reserve((kSlices + 1u) * (kStacks + 1u) * 3u);
    sub.uvs.reserve((kSlices + 1u) * (kStacks + 1u) * 2u);
    sub.indices.reserve(kSlices * kStacks * 6u);

    for (uint32_t y = 0u; y <= kStacks; ++y)
    {
        const float v = static_cast<float>(y) / static_cast<float>(kStacks);
        const float phi = v * math::PI;
        const float sy = std::sin(phi);
        const float cy = std::cos(phi);

        for (uint32_t x = 0u; x <= kSlices; ++x)
        {
            const float u = static_cast<float>(x) / static_cast<float>(kSlices);
            const float theta = u * math::TWO_PI;
            const math::Vec3 n{
                sy * std::cos(theta),
                cy,
                sy * std::sin(theta)
            };

            sub.positions.push_back(n.x);
            sub.positions.push_back(n.y);
            sub.positions.push_back(n.z);
            sub.normals.push_back(n.x);
            sub.normals.push_back(n.y);
            sub.normals.push_back(n.z);
            sub.uvs.push_back(u);
            sub.uvs.push_back(1.0f - v);
        }
    }

    for (uint32_t y = 0u; y < kStacks; ++y)
    {
        for (uint32_t x = 0u; x < kSlices; ++x)
        {
            const uint32_t a = y * (kSlices + 1u) + x;
            const uint32_t b = a + 1u;
            const uint32_t c = a + (kSlices + 1u);
            const uint32_t d = c + 1u;
            sub.indices.push_back(a);
            sub.indices.push_back(b);
            sub.indices.push_back(c);
            sub.indices.push_back(b);
            sub.indices.push_back(d);
            sub.indices.push_back(c);
        }
    }

    assets::EnsureTangents(sub);
    mesh->submeshes.push_back(std::move(sub));
    return mesh;
}

constexpr uint32_t kAnimationPreviewSize = 1024u;
constexpr float kMaterialPreviewGroundHalfSize = 5.0f;
constexpr char kMaterialPreviewGroundPath[] = "__editor/material_preview_ground";

std::unique_ptr<assets::MeshAsset> CreateMaterialPreviewGroundMesh()
{
    constexpr float h = kMaterialPreviewGroundHalfSize;
    auto mesh = std::make_unique<assets::MeshAsset>();
    mesh->path = kMaterialPreviewGroundPath;
    mesh->debugName = "Material Preview Ground";
    mesh->state = assets::AssetState::Loaded;
    mesh->gpuStatus.dirty = true;

    assets::SubMeshData sub{};
    sub.positions = {-h, 0.f, -h, -h, 0.f, h, h, 0.f, h, h, 0.f, -h};
    sub.normals = {0.f, 1.f, 0.f, 0.f, 1.f, 0.f, 0.f, 1.f, 0.f, 0.f, 1.f, 0.f};
    sub.uvs = {0.f, 0.f, 0.f, 1.f, 1.f, 1.f, 1.f, 0.f};
    sub.indices = {0u, 1u, 2u, 0u, 2u, 3u}; // Front face points up.
    assets::EnsureTangents(sub);
    mesh->submeshes.push_back(std::move(sub));
    return mesh;
}

std::string SanitizeProjectFolderName(std::string name)
{
    for (char& ch : name)
    {
        const bool alphaNum = (ch >= 'a' && ch <= 'z') ||
                              (ch >= 'A' && ch <= 'Z') ||
                              (ch >= '0' && ch <= '9');
        if (!(alphaNum || ch == '_' || ch == '-'))
            ch = '_';
    }
    return name;
}

void ResetEditorLayerNames(renderer::addons::editor::EditorState& state)
{
    state.layerNames = {{
        "Default", "EditorGizmo", "Transparent", "UI",
        "User0", "User1", "User2", "User3",
        "","","","","","","","","","","","","","","","","","","","","","","",""
    }};
}

uint32_t NormalizeEditorObjectLayerMask(uint32_t mask) noexcept
{
    constexpr uint32_t kUserVisibleMask =
        renderer::LAYER_DEFAULT |
        renderer::LAYER_TRANSPARENT |
        renderer::LAYER_UI |
        renderer::LAYER_USER_0 |
        renderer::LAYER_USER_1 |
        renderer::LAYER_USER_2 |
        renderer::LAYER_USER_3;

    const uint32_t selected = mask & kUserVisibleMask;
    for (int b = 0; b < 32; ++b)
    {
        const uint32_t bit = (1u << b);
        if ((selected & bit) != 0u)
            return bit;
    }
    return renderer::LAYER_DEFAULT;
}

void NormalizeEditorObjectLayerMasks(ecs::World& world)
{
    world.ForEachAlive([&](EntityID id) {
        auto* mesh = world.Get<MeshComponent>(id);
        if (!mesh)
            return;
        if (world.Has<renderer::addons::editor::EditorRuntimeGizmoTag>(id))
            return;
        if (world.Has<renderer::addons::editor::EditorMaterialPreviewComponent>(id))
            return;
        if (world.Has<renderer::addons::editor::EditorAssetThumbnailComponent>(id))
            return;
        mesh->layerMask = NormalizeEditorObjectLayerMask(mesh->layerMask);
    });
}

std::string EscapeJsonString(std::string_view s)
{
    std::string out;
    out.reserve(s.size() + 4u);
    for (const char ch : s)
    {
        if (ch == '"')       { out += "\\\""; }
        else if (ch == '\\') { out += "\\\\"; }
        else if (ch == '\n') { out += "\\n";  }
        else if (ch == '\r') { out += "\\r";  }
        else if (ch == '\t') { out += "\\t";  }
        else                 { out += ch;      }
    }
    return out;
}

serialization::JsonValue ParseEditorScene(const std::string& json, std::string& error)
{
    serialization::JsonValue root;
    try { root = serialization::JsonParser::Parse(json, error); }
    catch (const std::exception& exception) { error = exception.what(); }
    if (error.empty())
    {
        const auto* entities = root.Get("entities");
        if (!entities || !entities->IsArray())
            error = Translate("scene.invalid_entities");
    }
    return root;
}


std::string FloatJson(float v)
{
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.9g", static_cast<double>(v));
    return buf;
}

constexpr float kOrthoLensDeg = 1.f;

math::Vec3 EditorCameraForward(const renderer::addons::editor::EditorCameraState& camera) noexcept
{
    return math::Quat::FromEulerDeg(camera.pitchDeg, camera.yawDeg, 0.f).Rotate({0.f, 0.f, -1.f}).Normalized();
}

// The orthographic view is a temporary long lens; only the normal camera is stored.
renderer::addons::editor::EditorCameraState PersistentEditorCamera(const renderer::addons::editor::EditorState& state) noexcept
{
    renderer::addons::editor::EditorCameraState camera = state.editorCamera;
    if (!state.viewportOrthographic)
        return camera;
    const float pivotDistance = state.viewportOrthoDistance * std::tan(kOrthoLensDeg * 0.5f * math::DEG_TO_RAD) /
                                std::tan(state.viewportPerspectiveFov * 0.5f * math::DEG_TO_RAD);
    camera.position = camera.position + EditorCameraForward(camera) * (state.viewportOrthoDistance - pivotDistance);
    camera.fovDeg = state.viewportPerspectiveFov;
    camera.nearPlane = state.viewportPerspectiveNear;
    camera.farPlane = state.viewportPerspectiveFar;
    return camera;
}

// Files saved while the long lens was active (lens below any valid editor setting).
[[nodiscard]] constexpr bool IsStoredOrthoLens(float fovDeg, float nearPlane) noexcept { return fovDeg < 5.f || nearPlane >= 50.f; }

std::string EditorSettingsJson(const renderer::addons::editor::EditorState& state,
                               const std::string& lastScene)
{
    const auto cam = PersistentEditorCamera(state);
    std::ostringstream f;
    f << "{\n"
      << "  \"lastScene\":\"" << EscapeJsonString(lastScene) << "\",\n"
      << "  \"camera\":{\n"
      << "    \"fovDeg\":"    << FloatJson(cam.fovDeg)    << ",\n"
      << "    \"nearPlane\":" << FloatJson(cam.nearPlane) << ",\n"
      << "    \"farPlane\":"  << FloatJson(cam.farPlane)  << ",\n"
      << "    \"moveSpeed\":" << FloatJson(cam.moveSpeed) << "\n"
      << "  },\n"
      << "  \"layers\":[";
    for (size_t i = 0; i < state.layerNames.size(); ++i)
    {
        if (i > 0u) f << ",";
        f << "\"" << EscapeJsonString(state.layerNames[i]) << "\"";
    }
    f << "],\n"
      << "  \"codeEditor\":{\n"
      << "    \"executable\":\"" << EscapeJsonString(state.codeEditorExecutableBuffer.data()) << "\",\n"
      << "    \"arguments\":\"" << EscapeJsonString(state.codeEditorArgumentsBuffer.data()) << "\"\n"
      << "  },\n"
      << "  \"snap\":{\n"
      << "    \"moveEnabled\":"   << (state.snap.moveEnabled ? "true" : "false") << ",\n"
      << "    \"moveStep\":"      << FloatJson(state.snap.moveStep) << ",\n"
      << "    \"rotateEnabled\":" << (state.snap.rotateEnabled ? "true" : "false") << ",\n"
      << "    \"rotateStepDeg\":" << FloatJson(state.snap.rotateStepDeg) << ",\n"
      << "    \"scaleEnabled\":"  << (state.snap.scaleEnabled ? "true" : "false") << ",\n"
      << "    \"scaleStep\":"     << FloatJson(state.snap.scaleStep) << ",\n"
      << "    \"showGrid\":"      << (state.snap.showGrid ? "true" : "false") << "\n"
      << "  },\n"
      << "  \"gizmo\":{\n"
      << "    \"mode\":"  << static_cast<int>(state.gizmoMode) << ",\n"
      << "    \"space\":" << static_cast<int>(state.transformSpace) << "\n"
      << "  },\n"
      << "  \"viewport\":{\n"
      << "    \"playAllParticles\":" << (state.playAllParticles ? "true" : "false") << ",\n"
      << "    \"showIcons\":" << (state.showEditorIcons ? "true" : "false") << ",\n"
      << "    \"showCameraPreview\":" << (state.showCameraPreview ? "true" : "false") << ",\n"
      << "    \"showPerfOverlay\":" << (state.showPerfOverlay ? "true" : "false") << ",\n"
      << "    \"showGpuPassTiming\":" << (state.showGpuPassTiming ? "true" : "false") << ",\n"
      << "    \"showSelectionOutline\":" << (state.showSelectionOutline ? "true" : "false") << "\n"
      << "  }\n"
      << "}\n";
    return f.str();
}

bool SaveEditorSettings(const std::filesystem::path& projectRoot,
                        const renderer::addons::editor::EditorState& state,
                        const std::string& lastScene)
{
    return !projectRoot.empty() && WriteTextFileAtomically(projectRoot / "editor" / "editor.settings",
                                                          EditorSettingsJson(state, lastScene));
}

// Scene that was open when the editor last closed this project (empty if unknown).
std::string ReadLastEditorScene(const std::filesystem::path& projectRoot)
{
    std::ifstream f(projectRoot / "editor" / "editor.settings", std::ios::binary);
    if (!f)
        return {};
    const std::string json((std::istreambuf_iterator<char>(f)),
                            std::istreambuf_iterator<char>());
    std::string parseError;
    const serialization::JsonValue root = serialization::JsonParser::Parse(json, parseError);
    if (!parseError.empty() || !root.IsObject())
        return {};
    const serialization::JsonValue* lastScene = root.Get("lastScene");
    return lastScene && lastScene->IsString() ? lastScene->AsString() : std::string{};
}

// Lädt Editor-Einstellungen aus {projectRoot}/editor/editor.settings
void LoadEditorSettings(const std::filesystem::path& projectRoot,
                        renderer::addons::editor::EditorState& state)
{
    ResetEditorLayerNames(state);

    if (projectRoot.empty())
        return;
    auto& cam = state.editorCamera;
    const std::filesystem::path path = projectRoot / "editor" / "editor.settings";
    if (!std::filesystem::exists(path))
        return;

    std::ifstream f(path, std::ios::binary);
    if (!f) return;
    const std::string json((std::istreambuf_iterator<char>(f)),
                            std::istreambuf_iterator<char>());

    std::string parseError;
    const serialization::JsonValue root = serialization::JsonParser::Parse(json, parseError);
    if (!parseError.empty() || !root.IsObject()) return;
    const serialization::JsonValue* c = root.Get("camera");
    if (c && c->IsObject())
    {
        if (const serialization::JsonValue* v = c->Get("fovDeg"))
            cam.fovDeg    = std::clamp(v->AsFloat(), 10.f, 170.f);
        if (const serialization::JsonValue* v = c->Get("nearPlane"))
            cam.nearPlane = std::max(0.001f, v->AsFloat());
        if (const serialization::JsonValue* v = c->Get("farPlane"))
            cam.farPlane  = std::max(cam.nearPlane + 1.f, v->AsFloat());
        if (const serialization::JsonValue* v = c->Get("fovDeg"); v && IsStoredOrthoLens(v->AsFloat(), cam.nearPlane))
        {
            cam.fovDeg = 60.f;
            cam.nearPlane = 0.05f;
            cam.farPlane = 2000.f;
        }
        if (const serialization::JsonValue* v = c->Get("moveSpeed"))
            cam.moveSpeed = std::max(0.1f, v->AsFloat());
    }

    if (const serialization::JsonValue* layers = root.Get("layers");
        layers && layers->IsArray())
    {
        const size_t count = std::min(state.layerNames.size(), layers->arrayVal.size());
        for (size_t i = 0; i < count; ++i)
        {
            const serialization::JsonValue& layer = layers->arrayVal[i];
            if (!layer.IsString())
                continue;
            if (i == 0u || i == 1u)
                continue;
            state.layerNames[i] = layer.AsString();
        }
    }

    if (const serialization::JsonValue* snap = root.Get("snap"); snap && snap->IsObject())
    {
        auto readBool = [&](const char* key, bool& out) {
            if (const auto* v = snap->Get(key); v && v->IsBool()) out = v->AsBool();
        };
        auto readStep = [&](const char* key, float& out) {
            if (const auto* v = snap->Get(key); v && v->IsNumber() && v->AsFloat() > 0.f) out = v->AsFloat();
        };
        readBool("moveEnabled",   state.snap.moveEnabled);
        readStep("moveStep",      state.snap.moveStep);
        readBool("rotateEnabled", state.snap.rotateEnabled);
        readStep("rotateStepDeg", state.snap.rotateStepDeg);
        readBool("scaleEnabled",  state.snap.scaleEnabled);
        readStep("scaleStep",     state.snap.scaleStep);
        readBool("showGrid",      state.snap.showGrid);
    }

    // Zuletzt gewaehlter Gizmo-Modus (Verschieben/Drehen/Skalieren) und Lokal/Welt.
    if (const serialization::JsonValue* gizmo = root.Get("gizmo"); gizmo && gizmo->IsObject())
    {
        using namespace renderer::addons::editor;
        if (const auto* v = gizmo->Get("mode"); v && v->IsNumber())
            state.gizmoMode = static_cast<GizmoMode>(std::clamp(v->AsInt(), 0, 3));
        if (const auto* v = gizmo->Get("space"); v && v->IsNumber())
            state.transformSpace = static_cast<EditorTransformSpace>(std::clamp(v->AsInt(), 0, 1));
    }

    if (const serialization::JsonValue* viewport = root.Get("viewport"); viewport && viewport->IsObject())
    {
        if (const auto* v = viewport->Get("playAllParticles"); v && v->IsBool())
            state.playAllParticles = v->AsBool();
        if (const auto* v = viewport->Get("showIcons"); v && v->IsBool())
            state.showEditorIcons = v->AsBool();
        if (const auto* v = viewport->Get("showCameraPreview"); v && v->IsBool())
            state.showCameraPreview = v->AsBool();
        if (const auto* v = viewport->Get("showPerfOverlay"); v && v->IsBool())
            state.showPerfOverlay = v->AsBool();
        if (const auto* v = viewport->Get("showGpuPassTiming"); v && v->IsBool())
            state.showGpuPassTiming = v->AsBool();
        if (const auto* outline = viewport->Get("showSelectionOutline"); outline && outline->IsBool())
            state.showSelectionOutline = outline->AsBool();
    }

    if (const serialization::JsonValue* codeEditor = root.Get("codeEditor");
        codeEditor && codeEditor->IsObject())
    {
        if (const serialization::JsonValue* executable = codeEditor->Get("executable");
            executable && executable->IsString())
        {
            std::snprintf(state.codeEditorExecutableBuffer.data(),
                          state.codeEditorExecutableBuffer.size(),
                          "%s",
                          executable->AsString().c_str());
        }
        if (const serialization::JsonValue* arguments = codeEditor->Get("arguments");
            arguments && arguments->IsString())
        {
            const std::string args = arguments->AsString() == "-g \"{file}:{line}\""
                ? std::string("\"{workspace}\" -g \"{file}:{line}\"")
                : arguments->AsString();
            std::snprintf(state.codeEditorArgumentsBuffer.data(),
                          state.codeEditorArgumentsBuffer.size(),
                          "%s",
                          args.c_str());
        }
    }
}

std::string BuildEditorCameraJson(const renderer::addons::editor::EditorCameraState& cam)
{
    std::string out;
    out += "  \"editorCamera\":{\n";
    out += "    \"position\":[" + FloatJson(cam.position.x) + ", " +
           FloatJson(cam.position.y) + ", " + FloatJson(cam.position.z) + "],\n";
    out += "    \"pitchDeg\":"  + FloatJson(cam.pitchDeg)   + ",\n";
    out += "    \"yawDeg\":"    + FloatJson(cam.yawDeg)     + ",\n";
    out += "    \"moveSpeed\":" + FloatJson(cam.moveSpeed)  + ",\n";
    out += "    \"fovDeg\":"    + FloatJson(cam.fovDeg)     + ",\n";
    out += "    \"nearPlane\":" + FloatJson(cam.nearPlane)  + ",\n";
    out += "    \"farPlane\":"  + FloatJson(cam.farPlane)   + "\n";
    out += "  }";
    return out;
}

std::string BuildEditorEnvironmentJson(const renderer::addons::editor::EditorState& state)
{
    std::string out;
    out += "  \"environment\":{\n";
    out += "    \"texturePath\":\"" + EscapeJsonString(state.environmentTexturePath) + "\",\n";
    out += "    \"enableIBL\":" + std::string(state.environmentEnableIBL ? "true" : "false") + ",\n";
    out += "    \"showSkybox\":" + std::string(state.showSkyboxBackground ? "true" : "false") + ",\n";
    out += "    \"iblIntensity\":" + FloatJson(state.environmentIntensity) + ",\n";
    out += "    \"backgroundColor\":[" +
           FloatJson(state.backgroundColor[0]) + ", " +
           FloatJson(state.backgroundColor[1]) + ", " +
           FloatJson(state.backgroundColor[2]) + ", " +
           FloatJson(state.backgroundColor[3]) + "],\n";
    out += "    \"ambientColor\":[" +
           FloatJson(state.ambientColor.x) + ", " +
           FloatJson(state.ambientColor.y) + ", " +
           FloatJson(state.ambientColor.z) + "],\n";
    out += "    \"ambientIntensity\":" + FloatJson(state.ambientIntensity) + ",\n";
    out += "    \"tonemapOperator\":\"" + std::string(renderer::addons::forward::TonemapOperatorName(state.tonemap.op)) + "\",\n";
    out += "    \"exposureEv\":" + FloatJson(state.tonemap.exposureEv) + ",\n";
    out += "    \"msaaSamples\":" + std::to_string(state.msaaSamples) + ",\n";
    out += "    \"specularOcclusion\":" + std::string(state.specularOcclusion ? "true" : "false") + ",\n";
    out += "    \"probeBakePasses\":" + std::to_string(state.probeBakePasses) + "\n";
    out += "  }";
    return out;
}

void ApplyEditorCameraFromSceneJson(renderer::addons::editor::EditorFrameContext* ctx,
                                    const serialization::JsonValue& root)
{
    if (!ctx)
        return;

    const serialization::JsonValue* cam = root.Get("editorCamera");
    if (!cam || !cam->IsObject())
        return;

    auto& dst = ctx->state.editorCamera;
    if (const serialization::JsonValue* pos = cam->Get("position"))
        dst.position = pos->AsVec3();
    if (const serialization::JsonValue* pitch = cam->Get("pitchDeg"))
        dst.pitchDeg = pitch->AsFloat();
    if (const serialization::JsonValue* yaw = cam->Get("yawDeg"))
        dst.yawDeg = yaw->AsFloat();
    if (const serialization::JsonValue* speed = cam->Get("moveSpeed"))
        dst.moveSpeed = speed->AsFloat();
    if (const serialization::JsonValue* fov = cam->Get("fovDeg"))
        dst.fovDeg    = std::clamp(fov->AsFloat(), 10.f, 170.f);
    if (const serialization::JsonValue* np = cam->Get("nearPlane"))
        dst.nearPlane = std::max(0.001f, np->AsFloat());
    if (const serialization::JsonValue* fp = cam->Get("farPlane"))
        dst.farPlane  = std::max(dst.nearPlane + 1.f, fp->AsFloat());
    if (const serialization::JsonValue* fov = cam->Get("fovDeg"); fov && IsStoredOrthoLens(fov->AsFloat(), dst.nearPlane))
    {
        // The long lens sat far behind the point it looked at: bring the camera back.
        const float longDistance = dst.nearPlane / 0.2f;
        const float pivotDistance = longDistance * std::tan(kOrthoLensDeg * 0.5f * math::DEG_TO_RAD) /
                                    std::tan(30.f * math::DEG_TO_RAD);
        dst.position = dst.position + EditorCameraForward(dst) * (longDistance - pivotDistance);
        dst.fovDeg = 60.f;
        dst.nearPlane = 0.05f;
        dst.farPlane = 2000.f;
    }
}

void ApplyEditorEnvironmentFromSceneJson(renderer::addons::editor::EditorFrameContext* ctx,
                                         const serialization::JsonValue& root)
{
    if (!ctx)
        return;

    auto clearEnvironment = [&]() {
        auto& state = ctx->state;
        state.environmentTexturePath.clear();
        state.environmentEnableIBL = false;
        state.showSkyboxBackground = false;
        state.backgroundColor = {0.f, 0.f, 0.f, 1.f};
        using EditorState = renderer::addons::editor::EditorState;
        state.ambientSource = EditorState::AmbientSource::Color;
        if (ctx->setEditorBackgroundColor)
            ctx->setEditorBackgroundColor(state.backgroundColor);
        if (ctx->clearEditorEnvironment)
            ctx->clearEditorEnvironment();
    };

    const serialization::JsonValue* env = root.Get("environment");
    // Scenes saved before tonemap settings existed keep their former look.
    ctx->state.tonemap = renderer::addons::forward::TonemapSettings{};
    ctx->state.tonemap.op = renderer::addons::forward::kLegacyTonemapOperator;
    ctx->state.msaaSamples = 1u;   // scenes without the key: MSAA off
    ctx->state.specularOcclusion = false;   // scenes without the key: probe specularOcclusion off (old look)
    ctx->state.probeBakePasses = 2u;
    if (!env || !env->IsObject())
    {
        clearEnvironment();
        return;
    }

    auto& state = ctx->state;
    if (const serialization::JsonValue* op = env->Get("tonemapOperator"); op && op->IsString())
    {
        if (!renderer::addons::forward::ParseTonemapOperator(op->AsString().c_str(), state.tonemap.op))
            Debug::LogWarning("KromEditorApp.cpp: unknown tonemap operator '%s', using Reinhard",
                              op->AsString().c_str());
    }
    if (const serialization::JsonValue* ev = env->Get("exposureEv"); ev && ev->IsNumber())
        state.tonemap.exposureEv = std::clamp(ev->AsFloat(), renderer::addons::forward::kMinExposureEv,
                                              renderer::addons::forward::kMaxExposureEv);
    if (const serialization::JsonValue* msaa = env->Get("msaaSamples"); msaa && msaa->IsNumber())
        state.msaaSamples = renderer::addons::forward::NormalizeMsaaSamples(
            static_cast<uint32_t>(std::max(0, msaa->AsInt())));
    if (const serialization::JsonValue* so = env->Get("specularOcclusion"); so && so->IsBool())
        state.specularOcclusion = so->AsBool();
    if (const serialization::JsonValue* passes = env->Get("probeBakePasses"); passes && passes->IsNumber())
        state.probeBakePasses = passes->AsInt() >= 2 ? 2u : 1u;
    if (const serialization::JsonValue* path = env->Get("texturePath");
        path && path->IsString())
        state.environmentTexturePath = path->AsString();
    if (const serialization::JsonValue* enable = env->Get("enableIBL");
        enable && enable->IsBool())
    {
        state.environmentEnableIBL = enable->AsBool();
        using EditorState = renderer::addons::editor::EditorState;
        state.ambientSource = state.environmentEnableIBL
            ? EditorState::AmbientSource::Skybox
            : EditorState::AmbientSource::Color;
    }
    if (const serialization::JsonValue* sky = env->Get("showSkybox");
        sky && sky->IsBool())
        state.showSkyboxBackground = sky->AsBool();

    if (const serialization::JsonValue* intensity = env->Get("iblIntensity");
        intensity && intensity->IsNumber())
        state.environmentIntensity = std::max(0.0f, intensity->AsFloat());
    if (const serialization::JsonValue* background = env->Get("backgroundColor");
        background && background->IsArray() && background->arrayVal.size() >= 3u)
    {
        state.backgroundColor[0] = std::clamp(background->At(0).AsFloat(), 0.0f, 1.0f);
        state.backgroundColor[1] = std::clamp(background->At(1).AsFloat(), 0.0f, 1.0f);
        state.backgroundColor[2] = std::clamp(background->At(2).AsFloat(), 0.0f, 1.0f);
        state.backgroundColor[3] = background->arrayVal.size() >= 4u
            ? std::clamp(background->At(3).AsFloat(), 0.0f, 1.0f)
            : 1.0f;
    }
    if (const serialization::JsonValue* ambient = env->Get("ambientColor");
        ambient && ambient->IsArray())
        state.ambientColor = ambient->AsVec3();
    if (const serialization::JsonValue* ambientIntensity = env->Get("ambientIntensity");
        ambientIntensity && ambientIntensity->IsNumber())
        state.ambientIntensity = std::max(0.0f, ambientIntensity->AsFloat());

    if (ctx->setEditorAmbientLight)
        ctx->setEditorAmbientLight(state.ambientColor, state.ambientIntensity);
    if (ctx->setEditorBackgroundColor)
        ctx->setEditorBackgroundColor(state.backgroundColor);
    if (!state.environmentTexturePath.empty() && ctx->setEditorEnvironment)
    {
        (void)ctx->setEditorEnvironment(state.environmentTexturePath,
                                        state.environmentIntensity,
                                        state.environmentEnableIBL);
    }
    else if (ctx->clearEditorEnvironment)
    {
        ctx->clearEditorEnvironment();
    }
}

std::string InjectEditorSceneMetadataJson(std::string json,
                                          const renderer::addons::editor::EditorState& state)
{
    const size_t end = json.find_last_of('}');
    if (end == std::string::npos)
        return json;

    std::string metadata;
    metadata += ",\n";
    metadata += BuildEditorCameraJson(PersistentEditorCamera(state));
    metadata += ",\n";
    metadata += BuildEditorEnvironmentJson(state);
    metadata += "\n";
    json.insert(end, metadata);
    return json;
}

// Scene JSON without runtime ids, to tell whether the content changed: entity
// ids, generations and the entity count go, parents are named by GUID, and the
// entities are sorted, because undo and reloads recreate them in another order.
void AppendCanonicalJson(std::string& out, const serialization::JsonValue& value,
                         const std::unordered_map<uint32_t, std::string>* guidById = nullptr,
                         const std::string* orderRank = nullptr)
{
    using serialization::JsonType;
    switch (value.type)
    {
    case JsonType::Null:   out += "null"; break;
    case JsonType::Bool:   out += value.boolVal ? "true" : "false"; break;
    case JsonType::Number:
    {
        char number[32];
        std::snprintf(number, sizeof(number), "%.9g", value.numVal);
        out += number;
        break;
    }
    case JsonType::String: out += '"'; out += value.strVal; out += '"'; break;
    case JsonType::Array:
        out += '[';
        for (const auto& item : value.arrayVal)
        {
            AppendCanonicalJson(out, item, guidById, orderRank);
            out += ',';
        }
        out += ']';
        break;
    case JsonType::Object:
        out += '{';
        for (const auto& [key, item] : value.objectVal)
        {
            out += key;
            out += ':';
            if (key == "parentId" && guidById && item.IsNumber())
            {
                const auto it = guidById->find(item.AsUint());
                out += it != guidById->end() ? it->second : std::string("?");
            }
            else if (key == "order" && orderRank)
                out += *orderRank; // hierarchy order: only the place among siblings counts
            else
                AppendCanonicalJson(out, item, guidById, orderRank);
            out += ',';
        }
        out += '}';
        break;
    }
}

std::string CanonicalSceneContent(const std::string& json)
{
    std::string error;
    const serialization::JsonValue root = serialization::JsonParser::Parse(json, error);
    const auto* entities = root.Get("entities");
    if (!error.empty() || !entities || !entities->IsArray())
        return json;
    std::unordered_map<uint32_t, std::string> guidById;
    const auto guidOf = [](const serialization::JsonValue& entity) {
        if (const auto* components = entity.Get("components"); components && components->IsArray())
            for (const auto& component : components->arrayVal)
                if (const auto* type = component.Get("type"); type && type->IsString() && type->AsString() == "GuidComponent")
                    if (const auto* guid = component.Get("guid"); guid && guid->IsString())
                        return guid->AsString();
        return std::string{};
    };
    for (const auto& entity : entities->arrayVal)
        if (const auto* id = entity.Get("entityId"); id && id->IsNumber())
            guidById[id->AsUint()] = guidOf(entity);
    // Undo renumbers the hierarchy order; the rank among siblings is what matters.
    const auto componentValue = [](const serialization::JsonValue& entity, const char* type, const char* key) -> const serialization::JsonValue* {
        if (const auto* components = entity.Get("components"); components && components->IsArray())
            for (const auto& component : components->arrayVal)
                if (const auto* name = component.Get("type"); name && name->IsString() && name->AsString() == type)
                    return component.Get(key);
        return nullptr;
    };
    std::vector<std::tuple<uint32_t, double, size_t>> siblings; // parent id, order, entity
    for (size_t i = 0; i < entities->arrayVal.size(); ++i)
    {
        const auto& entity = entities->arrayVal[i];
        const auto* parent = componentValue(entity, "ParentComponent", "parentId");
        const auto* order = componentValue(entity, "EditorHierarchyComponent", "order");
        if (order && order->IsNumber())
            siblings.emplace_back(parent && parent->IsNumber() ? parent->AsUint() : UINT32_MAX, order->numVal, i);
    }
    std::sort(siblings.begin(), siblings.end());
    std::vector<std::string> rank(entities->arrayVal.size());
    size_t counter = 0;
    for (size_t i = 0; i < siblings.size(); ++i)
    {
        const bool firstOfParent = i == 0 || std::get<0>(siblings[i - 1]) != std::get<0>(siblings[i]);
        counter = firstOfParent ? 0 : counter + 1;
        rank[std::get<2>(siblings[i])] = std::to_string(counter);
    }
    std::vector<std::string> parts;
    for (size_t i = 0; i < entities->arrayVal.size(); ++i)
    {
        const auto& entity = entities->arrayVal[i];
        std::string part = guidOf(entity) + "=";
        if (const auto* components = entity.Get("components"))
            AppendCanonicalJson(part, *components, &guidById, &rank[i]);
        parts.push_back(std::move(part));
    }
    std::sort(parts.begin(), parts.end());
    std::string out;
    for (const auto& [key, item] : root.objectVal)
        if (key != "entities" && key != "entityCount")
        {
            out += key;
            AppendCanonicalJson(out, item);
        }
    for (const std::string& part : parts)
        out += part + "\n";
    return out;
}

std::filesystem::path ResolveLegacyEditorScenePath()
{
    return ResolveAssetRoot() / "editor_scene.json";
}

#endif // KROM_EDITOR_HAS_IMGUI

} // namespace

KromEditorApp::~KromEditorApp()
{
    Shutdown();
}

bool KromEditorApp::Initialize(const KromEditorAppConfig& config)
{
    if (m_initialized)
        return true;

#ifdef KROM_EDITOR_HAS_IMGUI
    DrainConsoleLogs(nullptr);
    Debug::MinLevel = std::min(Debug::MinLevel, LogLevel::Info);
    Debug::SetLogObserver(CaptureConsoleLog);
#endif
    m_config = config;
    m_forwardPlusActive = config.enableForwardPlus &&
        config.backend != renderer::DeviceFactory::BackendType::OpenGL;
    if (config.enableForwardPlus && !m_forwardPlusActive)
        Debug::LogWarning("KromEditorApp: Forward+ requested, but OpenGL falls back to classic forward because buffer shader resources are not available there yet");
    m_cameraOptions.ambientColor = config.ambientColor;
    m_cameraOptions.ambientIntensity = config.ambientIntensity;
#ifdef KROM_EDITOR_HAS_IMGUI
    m_editorState.ambientColor = config.ambientColor;
    m_editorState.ambientIntensity = config.ambientIntensity;
    m_editorState.backgroundColor = config.clearColor;
    // Environment/IBL wird erst durch geladene Projekt- oder Scene-Metadaten gesetzt.
#endif

    if (!m_renderFeaturesRegistered)
    {
        renderer::addons::forward::ForwardFeatureConfig forwardConfig{};
        forwardConfig.clearColorValue = m_config.clearColor;
        forwardConfig.enableEnvironmentBackground = true;
        forwardConfig.enableBloom = true;
        forwardConfig.bloomThreshold = 1.0f;
        forwardConfig.bloomIntensity = 0.12f;
        forwardConfig.bloomBlurRadius = 1.0f;
        forwardConfig.tonemapSettings = m_tonemapSettings;
        forwardConfig.msaaSettings = m_msaaSettings;
        forwardConfig.mode = m_forwardPlusActive
            ? renderer::addons::forward::ForwardRendererMode::ForwardPlus
            : renderer::addons::forward::ForwardRendererMode::Forward;

        renderer::RenderSystem& renderSystem = m_renderLoop.GetRenderSystem();
        addons::shadow::ShadowFeatureConfig shadowConfig{};
        shadowConfig.maxShadowLights = config.maxShadowLights;
        shadowConfig.maxShadowViews = config.maxShadowViews;

        if (!renderSystem.RegisterFeature(addons::mesh_renderer::CreateMeshRendererFeature()) ||
            !renderSystem.RegisterFeature(addons::lighting::CreateLightingFeature()) ||
            !renderSystem.RegisterFeature(addons::shadow::CreateShadowFeature(shadowConfig)) ||
            !renderSystem.RegisterFeature(renderer::addons::forward::CreateForwardFeature(forwardConfig)))
        {
            Debug::LogError("KromEditorApp: failed to register engine features");
            return false;
        }
        if (config.enableGtao &&
            config.backend != renderer::DeviceFactory::BackendType::OpenGL)
        {
            if (!renderSystem.RegisterFeature(renderer::addons::gtao::CreateGtaoFeature(&m_gtaoResources)))
            {
                Debug::LogError("KromEditorApp: failed to register GTAO feature");
                return false;
            }
        }
        if (!renderSystem.RegisterFeature(engine::addons::probes::CreateProbeFeature(&m_probeRuntime)))
        {
            Debug::LogError("KromEditorApp: failed to register probe feature");
            return false;
        }
        if (m_probeRuntime)
            m_probeRuntime->SetAssetRoot(PathToUtf8(GetCurrentAssetRoot()));

#ifdef KROM_EDITOR_HAS_IMGUI
        {
            // getSelected: liefert irgendeine gültige selektierte Entity damit
            // BuildPass nicht vorzeitig abbricht. Die echte Entscheidung trifft
            // shouldOutlineEntity.
            auto getSelected = [this]() -> EntityID {
                if (!m_editorFrameCtx || !m_editorFrameCtx->state.showSelectionOutline) return NULL_ENTITY;
                // selectedEntity ist immer in multiSelection — erste gültige Entity genügt als Guard
                const auto& st = m_editorFrameCtx->state;
                if (st.selectedEntity.IsValid() && m_world && m_world->IsAlive(st.selectedEntity))
                    return st.selectedEntity;
                for (EntityID e : st.multiSelection)
                    if (e.IsValid() && m_world && m_world->IsAlive(e)) return e;
                return NULL_ENTITY;
            };

            // shouldOutlineEntity: Regeln
            // 1. Kamera-Gizmo  → outline wenn seine Kamera selektiert ist
            // 2. Direkt selektiert (multiSel ODER selectedEntity) → outline
            // Kein automatisches Kinder-Traversal — verhindert unerwartete
            // Massen-Outlines (z.B. großes Drachen-Mesh als Kind eines Parents).
            auto shouldOutlineEntity = [this](EntityID entity) -> bool {
                if (!m_editorFrameCtx || !m_world ||
                    !entity.IsValid() || !m_world->IsAlive(entity))
                    return false;

                using namespace renderer::addons::editor;
                const auto& st = m_editorFrameCtx->state;

                auto isSelected = [&](EntityID e) -> bool {
                    if (!e.IsValid()) return false;
                    if (e == st.selectedEntity) return true;
                    for (EntityID sel : st.multiSelection)
                        if (e == sel) return true;
                    return false;
                };

                // Regel 1: Kamera-Gizmo
                if (const auto* g = m_world->Get<EditorCameraGizmoComponent>(entity))
                    return isSelected(g->cameraEntity);

                // Regel 2: direkt selektiert
                if (isSelected(entity)) return true;

                // Regel 3: Mesh-Nachfahre eines selektierten Parents → Outline (alle Ebenen)
                if (!m_world->Get<MeshComponent>(entity))
                    return false;
                EntityID cursor = entity;
                while (cursor.IsValid() && m_world->IsAlive(cursor))
                {
                    const auto* pc = m_world->Get<ParentComponent>(cursor);
                    if (!pc) break;
                    if (isSelected(pc->parent)) return true;
                    cursor = pc->parent;
                }
                return false;

            };

            auto outlineFeature = renderer::addons::outline::CreateOutlineFeature(
                std::move(getSelected),
                std::move(shouldOutlineEntity));
            if (!renderSystem.RegisterFeature(std::move(outlineFeature)))
            {
                Debug::LogError("KromEditorApp: failed to register Outline feature");
                return false;
            }
        }
#endif // KROM_EDITOR_HAS_IMGUI (outline)

#ifdef KROM_EDITOR_HAS_IMGUI
        {
            renderer::addons::editor::EditorBackendCallbacks editorCbs;
            bool editorBackendAvailable = false;

            editorCbs.getFrameContext = [this]() -> renderer::addons::editor::EditorFrameContext* {
                return m_editorFrameCtx.get();
            };

#if defined(KROM_APP_BACKEND_DX11) && defined(KROM_IMGUI_HAS_DX11)
            editorCbs.init = [this]() -> bool {
                auto* window = m_renderLoop.GetWindow();
                auto* device = static_cast<renderer::dx11::DX11Device*>(
                    m_renderLoop.GetRenderSystem().GetDevice());
                if (!window || !device)
                    return false;
                ImGui::CreateContext();
                ImGuiIO& io = ImGui::GetIO();
                io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_DockingEnable;
                EnableEditorMultiViewports(io);
                io.ConfigDragClickToInputText = true;
                io.ConfigWindowsMoveFromTitleBarOnly = true; // Klicks ins Viewport verschieben kein Fenster
                ConfigureEditorUiFont(io);
                renderer::addons::editor::theme::ApplyEditorTheme();
                ImGui_ImplWin32_Init(window->GetNativeWindowHandle().native);
                ImGui_ImplDX11_Init(device->GetD3DDevice(), device->GetD3DContext());
                if (const renderer::ISwapchain* sc = m_renderLoop.GetRenderSystem().GetSwapchain())
                    ConfigureDx11ViewportSwapchains(sc->GetBackbufferFormat());
                InstallImGuiWin32Hook(window->GetNativeWindowHandle(), [this]() {
                    if (m_editorFrameCtx)
                        m_editorFrameCtx->state.closeConfirmOpen = true;
                });
                return true;
            };
            editorCbs.shutdown = [this]() {
                if (auto* w = m_renderLoop.GetWindow())
                    RemoveImGuiWin32Hook(w->GetNativeWindowHandle());
                ImGui_ImplDX11_Shutdown();
                ImGui_ImplWin32_Shutdown();
                ImGui::DestroyContext();
            };
            editorCbs.newFrame = [this]() {
                ImGui_ImplDX11_NewFrame();
                ImGui_ImplWin32_NewFrame();
                renderer::addons::editor::theme::RefreshEditorTheme();
                if (m_editorFrameCtx && !m_editorFrameCtx->state.pendingLayoutIni.empty())
                {
                    auto& ini = m_editorFrameCtx->state.pendingLayoutIni;
                    ImGui::LoadIniSettingsFromMemory(ini.data(), ini.size());
                    ini.clear();
                }
                ImGui::NewFrame();
            };
            editorCbs.renderDrawData = [this](renderer::ICommandList&) {
                ImGui::Render();
                ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
                m_imguiFrameRendered = true;
            };
            editorBackendAvailable = true;

#elif defined(KROM_APP_BACKEND_OPENGL) && defined(KROM_IMGUI_HAS_OPENGL3)
            editorCbs.init = [this]() -> bool {
                auto* window = m_renderLoop.GetWindow();
                if (!window)
                    return false;
                ImGui::CreateContext();
                ImGuiIO& io = ImGui::GetIO();
                io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_DockingEnable;
                io.ConfigDragClickToInputText = true;
                io.ConfigWindowsMoveFromTitleBarOnly = true; // Klicks ins Viewport verschieben kein Fenster
                ConfigureEditorUiFont(io);
                renderer::addons::editor::theme::ApplyEditorTheme();
#if defined(KROM_APP_USE_WIN32_PLATFORM) && defined(KROM_IMGUI_HAS_WIN32)
                ImGui_ImplWin32_Init(window->GetNativeWindowHandle().native);
                InstallImGuiWin32Hook(window->GetNativeWindowHandle(), [this]() {
                    if (m_editorFrameCtx)
                        m_editorFrameCtx->state.closeConfirmOpen = true;
                });
#elif defined(KROM_APP_USE_GLFW_PLATFORM) && defined(KROM_IMGUI_HAS_GLFW)
                ImGui_ImplGlfw_InitForOpenGL(
                    static_cast<GLFWwindow*>(window->GetNativeWindowHandle().toolkit), true);
#endif
                ImGui_ImplOpenGL3_Init("#version 410");
                return true;
            };
            editorCbs.shutdown = [this]() {
                ImGui_ImplOpenGL3_Shutdown();
#if defined(KROM_APP_USE_WIN32_PLATFORM) && defined(KROM_IMGUI_HAS_WIN32)
                if (auto* w = m_renderLoop.GetWindow())
                    RemoveImGuiWin32Hook(w->GetNativeWindowHandle());
                ImGui_ImplWin32_Shutdown();
#elif defined(KROM_APP_USE_GLFW_PLATFORM) && defined(KROM_IMGUI_HAS_GLFW)
                ImGui_ImplGlfw_Shutdown();
#endif
                ImGui::DestroyContext();
            };
            editorCbs.newFrame = [this]() {
#if defined(KROM_APP_USE_WIN32_PLATFORM) && defined(KROM_IMGUI_HAS_WIN32)
                ImGui_ImplWin32_NewFrame();
#elif defined(KROM_APP_USE_GLFW_PLATFORM) && defined(KROM_IMGUI_HAS_GLFW)
                ImGui_ImplGlfw_NewFrame();
#endif
                ImGui_ImplOpenGL3_NewFrame();
                renderer::addons::editor::theme::RefreshEditorTheme();
                if (m_editorFrameCtx && !m_editorFrameCtx->state.pendingLayoutIni.empty())
                {
                    auto& ini = m_editorFrameCtx->state.pendingLayoutIni;
                    ImGui::LoadIniSettingsFromMemory(ini.data(), ini.size());
                    ini.clear();
                }
                ImGui::NewFrame();
            };
            editorCbs.renderDrawData = [this](renderer::ICommandList&) {
                ImGui::Render();
                ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
                m_imguiFrameRendered = true;
            };
            editorBackendAvailable = true;

#elif defined(KROM_APP_BACKEND_VULKAN) && defined(KROM_IMGUI_HAS_VULKAN)
            editorCbs.init = [this]() -> bool {
                auto* window = m_renderLoop.GetWindow();
                auto* device = static_cast<renderer::vulkan::VulkanDevice*>(
                    m_renderLoop.GetRenderSystem().GetDevice());
                if (!window || !device || !device->GetActiveSwapchain())
                    return false;

                ImGui::CreateContext();
                ImGuiIO& io = ImGui::GetIO();
                io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_DockingEnable;
                io.ConfigDragClickToInputText = true;
                io.ConfigWindowsMoveFromTitleBarOnly = true; // Klicks ins Viewport verschieben kein Fenster
                ConfigureEditorUiFont(io);
                renderer::addons::editor::theme::ApplyEditorTheme();
#if defined(KROM_APP_USE_WIN32_PLATFORM) && defined(KROM_IMGUI_HAS_WIN32)
                ImGui_ImplWin32_Init(window->GetNativeWindowHandle().native);
                // The Win32 backend has no Platform_CreateVkSurface handler; the Vulkan
                // backend needs one for secondary viewports (asserted in Init).
                ImGui::GetPlatformIO().Platform_CreateVkSurface = CreateWin32ViewportVkSurface;
                EnableEditorMultiViewports(io);
                InstallImGuiWin32Hook(window->GetNativeWindowHandle(), [this]() {
                    if (m_editorFrameCtx)
                        m_editorFrameCtx->state.closeConfirmOpen = true;
                });
#elif defined(KROM_APP_USE_GLFW_PLATFORM) && defined(KROM_IMGUI_HAS_GLFW)
                ImGui_ImplGlfw_InitForVulkan(
                    static_cast<GLFWwindow*>(window->GetNativeWindowHandle().toolkit), true);
                EnableEditorMultiViewports(io); // GLFW backend brings its own surface hook
#endif

                VkFormat colorFormat = device->GetActiveSwapchain()->GetColorFormat();
                VkPipelineRenderingCreateInfoKHR renderingInfo{
                    VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO_KHR
                };
                renderingInfo.colorAttachmentCount = 1u;
                renderingInfo.pColorAttachmentFormats = &colorFormat;

                ImGui_ImplVulkan_InitInfo initInfo{};
                initInfo.ApiVersion = VK_API_VERSION_1_3;
                initInfo.Instance = device->GetInstance();
                initInfo.PhysicalDevice = device->GetPhysicalDevice();
                initInfo.Device = device->GetVkDevice();
                initInfo.QueueFamily = device->GetGraphicsQueueFamily();
                initInfo.Queue = device->GetGraphicsQueue();
                initInfo.DescriptorPoolSize = 1024u;
                initInfo.MinImageCount = 2u;
                initInfo.ImageCount = std::max(2u, device->GetActiveSwapchain()->GetBufferCount());
                initInfo.PipelineInfoMain.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
                initInfo.PipelineInfoMain.PipelineRenderingCreateInfo = renderingInfo;
                initInfo.UseDynamicRendering = true;

                // Secondary viewports (OS windows) prefer the editor swapchain format so
                // the linear theme colors look the same there (sRGB). The backend reads
                // the format list when it creates the first such window, so it must
                // outlive this lambda.
                static VkFormat s_viewportColorFormat = VK_FORMAT_UNDEFINED;
                s_viewportColorFormat = colorFormat;
                VkPipelineRenderingCreateInfoKHR viewportRenderingInfo{
                    VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO_KHR
                };
                viewportRenderingInfo.colorAttachmentCount = 1u;
                viewportRenderingInfo.pColorAttachmentFormats = &s_viewportColorFormat;
                initInfo.PipelineInfoForViewports.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
                initInfo.PipelineInfoForViewports.PipelineRenderingCreateInfo = viewportRenderingInfo;

                return ImGui_ImplVulkan_Init(&initInfo);
            };
            editorCbs.shutdown = [this]() {
                if (auto* device = m_renderLoop.GetRenderSystem().GetDevice())
                    device->WaitIdle();
                ImGui_ImplVulkan_Shutdown();
#if defined(KROM_APP_USE_WIN32_PLATFORM) && defined(KROM_IMGUI_HAS_WIN32)
                if (auto* w = m_renderLoop.GetWindow())
                    RemoveImGuiWin32Hook(w->GetNativeWindowHandle());
                ImGui_ImplWin32_Shutdown();
#elif defined(KROM_APP_USE_GLFW_PLATFORM) && defined(KROM_IMGUI_HAS_GLFW)
                ImGui_ImplGlfw_Shutdown();
#endif
                ImGui::DestroyContext();
            };
            editorCbs.newFrame = [this]() {
#if defined(KROM_APP_USE_WIN32_PLATFORM) && defined(KROM_IMGUI_HAS_WIN32)
                ImGui_ImplWin32_NewFrame();
#elif defined(KROM_APP_USE_GLFW_PLATFORM) && defined(KROM_IMGUI_HAS_GLFW)
                ImGui_ImplGlfw_NewFrame();
#endif
                ImGui_ImplVulkan_NewFrame();
                renderer::addons::editor::theme::RefreshEditorTheme();
                if (m_editorFrameCtx && !m_editorFrameCtx->state.pendingLayoutIni.empty())
                {
                    auto& ini = m_editorFrameCtx->state.pendingLayoutIni;
                    ImGui::LoadIniSettingsFromMemory(ini.data(), ini.size());
                    ini.clear();
                }
                ImGui::NewFrame();
            };
            editorCbs.renderDrawData = [this](renderer::ICommandList& cmd) {
                ImGui::Render();
                if (void* nativeCmd = cmd.GetNativeCommandBuffer())
                {
                    ImGui_ImplVulkan_RenderDrawData(
                        ImGui::GetDrawData(),
                        static_cast<VkCommandBuffer>(nativeCmd));
                }
                m_imguiFrameRendered = true;
            };
            editorBackendAvailable = true;

#elif defined(KROM_APP_BACKEND_VULKAN)
            Debug::LogWarning("KromEditorApp: Vulkan ImGui-Backend nicht eingebunden, Editor-UI deaktiviert");
#endif // backend

            editorCbs.getBackbufferRT = [this]() -> RenderTargetHandle {
                const renderer::ISwapchain* sc = m_renderLoop.GetRenderSystem().GetSwapchain();
                if (!sc) return RenderTargetHandle::Invalid();
                return sc->GetBackbufferRenderTarget(sc->GetCurrentBackbufferIndex());
            };

            if (editorBackendAvailable &&
                !renderSystem.RegisterFeature(renderer::addons::editor::CreateEditorFeature(std::move(editorCbs))))
            {
                Debug::LogError("KromEditorApp: Editor-Feature konnte nicht registriert werden");
                return false;
            }
        }
#endif // KROM_EDITOR_HAS_IMGUI

        m_renderFeaturesRegistered = true;
    }

    bool renderLoopInitialized = false;

    auto rollback = [&]() noexcept
    {
#ifdef KROM_EDITOR_HAS_IMGUI
        if (m_editorEnvironmentHandle.IsValid())
        {
            renderer::RenderSystem& renderSystem = m_renderLoop.GetRenderSystem();
            renderSystem.DestroyEnvironment(m_editorEnvironmentHandle);
            m_editorEnvironmentHandle = renderer::EnvironmentHandle::Invalid();
        }
#endif
        addons::prefab::SetPrefabAssetPipeline(nullptr);
        m_assetPipeline.reset();
#ifdef KROM_EDITOR_HAS_IMGUI
        // ImGui ist hier ggf. schon beendet: nur das Ziel freigeben, keine ImGui-Textur.
        DestroyPanelTargetNoImGui(m_sceneView);
        DestroyPanelTargetNoImGui(m_gameView);
        if (m_previewRT.IsValid())
        {
            if (renderer::IDevice* device = m_renderLoop.GetRenderSystem().GetDevice())
                device->DestroyRenderTarget(m_previewRT);
            m_previewRT = RenderTargetHandle::Invalid();
        }
        if (m_materialPreviewRT.IsValid())
        {
            if (renderer::IDevice* device = m_renderLoop.GetRenderSystem().GetDevice())
                device->DestroyRenderTarget(m_materialPreviewRT);
            m_materialPreviewRT = RenderTargetHandle::Invalid();
        }
        if (m_animationPreviewRT.IsValid())
        {
            if (renderer::IDevice* device = m_renderLoop.GetRenderSystem().GetDevice())
                device->DestroyRenderTarget(m_animationPreviewRT);
            m_animationPreviewRT = RenderTargetHandle::Invalid();
        }
        if (renderer::IDevice* device = m_renderLoop.GetRenderSystem().GetDevice())
        {
            for (auto& [_, thumb] : m_assetBrowserState.assetPreviewThumbnails)
            {
                if (thumb.renderTarget.IsValid())
                    device->DestroyRenderTarget(thumb.renderTarget);
                thumb.renderTarget = RenderTargetHandle::Invalid();
                thumb.gpuTexture = TextureHandle::Invalid();
                thumb.imguiId = nullptr;
            }
        }
#endif
        m_debugDraw.OnDeviceShutdown();
        m_animationPreviewDebugDraw.OnDeviceShutdown();
        m_particleView.OnDeviceShutdown();
        m_gameParticleView.OnDeviceShutdown();
    m_animationParticleView.OnDeviceShutdown();
        if (renderLoopInitialized)
            m_renderLoop.Shutdown();
        if (m_platform)
        {
            m_platform->Shutdown();
            m_platform.reset();
        }
#ifdef KROM_EDITOR_HAS_IMGUI
        m_editorFrameCtx.reset();
#endif
        m_scriptModule.Unload(m_world.get(), m_scriptRegistry);
        m_world.reset();
#ifdef KROM_EDITOR_HAS_IMGUI
        Debug::SetLogObserver(nullptr);
        DrainConsoleLogs(nullptr);
#endif
    };

    if (!m_engineSchemaRegistered)
    {
        RegisterCoreComponents(m_componentRegistry);
        RegisterAnimationComponents(m_componentRegistry);
        RegisterCameraComponents(m_componentRegistry);
        RegisterMeshRendererComponents(m_componentRegistry);
        RegisterLightingComponents(m_componentRegistry);
        ecs::RegisterComponent<engine::script::ScriptList>(m_componentRegistry, "ScriptList");
        RegisterPhysicsComponents(m_componentRegistry);
        RegisterAudioComponents(m_componentRegistry);
        RegisterParticleComponents(m_componentRegistry);
        RegisterProbeComponents(m_componentRegistry);
#ifdef KROM_EDITOR_HAS_IMGUI
        renderer::addons::editor::RegisterEditorComponents(m_componentRegistry);
        RegisterPrefabInstanceComponent(m_componentRegistry);
#endif
        m_engineSchemaRegistered = true;
    }
    m_world = std::make_unique<ecs::World>(m_componentRegistry);
    g_editorScriptWorld = m_world.get();
    g_editorScriptRegistry = &m_scriptRegistry;

#ifdef KROM_EDITOR_HAS_IMGUI
    m_editorState.assetBrowser = &m_assetBrowserState;
    const std::string defaultProjectsRoot = PathToUtf8(ResolveProjectsRoot());
    std::snprintf(m_editorState.projectParentDirBuffer.data(),
                  m_editorState.projectParentDirBuffer.size(),
                  "%s",
                  defaultProjectsRoot.c_str());
    std::snprintf(m_editorState.projectOpenPathBuffer.data(),
                  m_editorState.projectOpenPathBuffer.size(),
                  "%s",
                  defaultProjectsRoot.c_str());
    m_editorFrameCtx = std::make_unique<renderer::addons::editor::EditorFrameContext>(
        *m_world, m_materialSystem, m_assetRegistry, m_editorState);
    m_editorFrameCtx->requestClose = [this]() {
        if (m_renderLoop.GetWindow())
            m_renderLoop.GetWindow()->RequestClose();
    };
    m_editorFrameCtx->saveScene = [this]() {
        return SaveEditorScene();
    };
    m_editorFrameCtx->loadScene = [this]() {
        if (!LeavePrefabMode())
            return false;
        return LoadEditorScene();
    };
    m_editorFrameCtx->saveProject = [this]() {
        return !m_projectRoot.empty() && SaveProjectFile(false);
    };
    m_editorFrameCtx->saveEditorSettings = [this]() {
        return m_editorFrameCtx && SaveEditorSettings(m_projectRoot, m_editorFrameCtx->state, m_currentSceneName);
    };
    m_editorFrameCtx->loadProject = [this](const std::string& path) {
        if (!LeavePrefabMode())
            return false;
        return LoadProjectFile(path);
    };
    m_editorFrameCtx->createProject = [this](const std::string& projectName,
                                             const std::string& parentDirectory,
                                             renderer::DeviceFactory::BackendType backend) {
        if (!LeavePrefabMode())
            return false;
        return CreateEditorProject(projectName, parentDirectory, backend);
    };
    m_editorFrameCtx->setProjectBackend = [this](renderer::DeviceFactory::BackendType backend) {
        m_projectBackend = backend;
        if (m_editorFrameCtx)
        {
            m_editorFrameCtx->projectBackend = backend;
            m_editorFrameCtx->lastFileMessage =
                TranslateWithValue("project.backend_selected", ProjectBackendId(backend));
        }
        (void)SaveProjectFile(false);
    };
    m_editorFrameCtx->saveProjectSettings = [this](const ProjectSettings& settings) {
        auto previous = m_projectSettings;
        m_projectSettings = settings;
        if (SaveProjectFile(false))
            return true;
        m_projectSettings = std::move(previous);
        return false;
    };
    m_editorFrameCtx->resetProjectSettingsDraft = [this]() {
        m_editorFrameCtx->projectSettings = m_projectSettings;
    };
    m_editorFrameCtx->syncSceneState = [this]() {
        SyncEditorSceneState();
    };
    m_editorFrameCtx->isEditorSceneModified = [this]() { return IsEditorSceneModified(); };
    m_editorFrameCtx->switchEditorScene = [this](const std::string& name, bool additive) {
        if (name.empty() || !m_pendingSceneSwitch.empty() || m_editorFrameCtx->sceneLoadActive)
        {
            m_editorFrameCtx->discardSceneChangesOnce = false; // a refused switch must not skip a later save
            return false;
        }
        // Szenenwechsel im Editor beendet Play (die Szene wird vorher wiederhergestellt).
        StopEditorPlay();
        // Nur vormerken: kein verschachtelter Render-/ImGui-Frame aus einem UI-Callback.
        m_pendingSceneSwitch = name;
        m_pendingSceneAdditive = additive;
        return true;
    };
    m_editorFrameCtx->newEditorScene = [this](const std::string& name) {
        const bool created = LeavePrefabMode() && NewEditorScene(name);
        m_editorFrameCtx->discardSceneChangesOnce = false;
        return created;
    };
    m_editorFrameCtx->newEditorSceneInDir = [this](const std::string& name, const std::string& dir) {
        const bool created = LeavePrefabMode() && NewEditorSceneInDir(name, dir);
        m_editorFrameCtx->discardSceneChangesOnce = false;
        return created;
    };
    m_editorFrameCtx->unloadEditorScene = [this](const std::string& name) {
        if (!LeavePrefabMode())
            return false;
        return UnloadEditorScene(name);
    };
    m_editorFrameCtx->renameEditorScene = [this](const std::string& oldName, const std::string& newName) {
        if (!LeavePrefabMode())
            return false;
        return RenameEditorScene(oldName, newName);
    };
    m_editorFrameCtx->deleteEditorScene = [this](const std::string& name) {
        if (!LeavePrefabMode())
            return false;
        return DeleteEditorScene(name);
    };
    m_editorFrameCtx->deleteEditorSceneByPath = [this](const std::string& absPath) {
        std::error_code ec;
        std::filesystem::remove(PathFromUtf8(absPath), ec);
        if (ec)
        {
            if (m_editorFrameCtx)
                m_editorFrameCtx->lastFileMessage = Translate("common.delete_failed");
            return false;
        }
        if (m_editorFrameCtx)
            m_editorFrameCtx->lastFileMessage = Translate("scene.deleted");
        return true;
    };
    m_editorFrameCtx->setSceneOrder = [this](const std::vector<std::string>& order) {
        SetSceneOrder(order);
    };
    m_editorFrameCtx->toggleBuildExclude = [this](const std::string& sceneName, bool excluded) {
        if (excluded)
            m_buildExcludedScenes.insert(sceneName);
        else
            m_buildExcludedScenes.erase(sceneName);
        RefreshBuildExcludeState();
        SaveProjectFile();  // sofort in krom-project.json persistieren
    };
    m_editorFrameCtx->buildAllScenes = [this]() {
        if (!LeavePrefabMode())
            return false;
        return BuildAllScenes();
    };
    m_editorFrameCtx->exportProject = [this]() {
        if (!LeavePrefabMode())
            return false;
        return ExportProjectBuild();
    };
    m_editorFrameCtx->playGame = [this]() {
        if (!LeavePrefabMode())
            return false;
        if (m_projectRoot.empty())
        {
            if (m_editorFrameCtx)
                m_editorFrameCtx->lastFileMessage = Translate("play.no_project");
            return false;
        }
        if (!SaveEditorScene())
            return false;
        if (m_editorFrameCtx)
        {
            const auto bindings = renderer::addons::editor::RefreshCppScriptProject(*m_editorFrameCtx);
            if (!bindings.ok)
            {
                m_editorFrameCtx->lastFileMessage = bindings.message.empty()
                    ? Translate("script.bindings_failed")
                    : bindings.message;
                AddEditorConsoleLog(&m_editorFrameCtx->state, engine::LogLevel::Error,
                    "[Build] " + m_editorFrameCtx->lastFileMessage);
                return false;
            }
        }
        if (!BuildAllScenes())
            return false;

        constexpr const char* profiles[] = {nullptr, "Debug", "RelWithDebInfo", "Release"};
        const char* profile = profiles[m_projectSettings.testBuild];
        const std::filesystem::path buildDir = profile ? RuntimeProfileDir(profile) : PathFromUtf8(KROM_BUILD_DIR);
        return StartRuntimeBuild(buildDir, [this, buildDir](bool ok) {
            if (!ok)
            {
                if (m_editorFrameCtx)
                    m_editorFrameCtx->lastFileMessage = Translate("play.build_failed");
                return;
            }
            (void)LaunchPlayRuntime(buildDir);
        }, {}, profile ? profile : "Release");
    };
    m_editorFrameCtx->startPlay = [this]() {
        DrainConsoleLogs(nullptr);
        m_editorState.consoleLogs.clear();
        m_editorState.consoleScrollToBottom = true;
        return StartEditorPlay();
    };
    m_editorFrameCtx->togglePlayPause = [this]() { SetEditorPlayPaused(!m_playPaused); };
    m_editorFrameCtx->stopPlay = [this]() { StopEditorPlay(); };
    m_editorFrameCtx->setEditorEnvironment = [this](const std::string& path,
                                                    float intensity,
                                                    bool enableIBL) {
        if (!m_assetPipeline)
            return false;

        const TextureHandle sourceTexture = m_assetPipeline->LoadTexture(path);
        if (!sourceTexture.IsValid())
            return false;

        m_assetPipeline->UploadPendingGpuAssets();

        renderer::EnvironmentDesc env{};
        env.mode = renderer::EnvironmentMode::Texture;
        env.sourceTexture = sourceTexture;
        env.intensity = intensity;
        env.enableIBL = enableIBL;

        renderer::RenderSystem& renderSystem = m_renderLoop.GetRenderSystem();
        const renderer::EnvironmentHandle nextEnvironment = renderSystem.CreateEnvironment(env);
        if (!nextEnvironment.IsValid())
            return false;

        const renderer::EnvironmentHandle previousEnvironment = m_editorEnvironmentHandle;
        m_editorEnvironmentHandle = nextEnvironment;
        renderSystem.SetActiveEnvironment(nextEnvironment);
        if (previousEnvironment.IsValid())
            renderSystem.DestroyEnvironment(previousEnvironment);
        return true;
    };
    m_editorFrameCtx->clearEditorEnvironment = [this]() {
        renderer::RenderSystem& renderSystem = m_renderLoop.GetRenderSystem();
        if (m_editorEnvironmentHandle.IsValid())
        {
            const renderer::EnvironmentHandle oldEnvironment = m_editorEnvironmentHandle;
            m_editorEnvironmentHandle = renderer::EnvironmentHandle::Invalid();
            renderSystem.DestroyEnvironment(oldEnvironment);
        }
        renderSystem.SetActiveEnvironment(renderer::EnvironmentHandle::Invalid());
    };
    m_editorFrameCtx->setEditorAmbientLight = [this](const math::Vec3& color, float intensity) {
        m_cameraOptions.ambientColor = color;
        m_cameraOptions.ambientIntensity = intensity;
    };
    m_editorFrameCtx->setGtaoSettings = [this](const renderer::addons::gtao::GtaoSettings& s) {
        if (m_gtaoResources)
            m_gtaoResources->settings = s;
    };
    m_editorFrameCtx->reflectionProbeDataPath = [this](uint64_t probeId) {
        return engine::addons::probes::MakeProbeDataPath(m_currentSceneName, probeId);
    };
    m_editorFrameCtx->bakeReflectionProbe = [this](EntityID entity) {
        m_probeBakeQueue.emplace_back(entity, 1);
        if (m_editorState.probeBakePasses >= 2u)
            m_probeBakeQueue.emplace_back(entity, 2);
    };
    m_editorFrameCtx->bakeAllReflectionProbes = [this]() {
        if (!m_world)
            return;
        std::vector<EntityID> probes;
        m_world->View<ReflectionProbeComponent>([&](EntityID entity, ReflectionProbeComponent& probe) {
            if (probe.probeId == 0u)
                probe.probeId = engine::addons::probes::GenerateProbeId();
            const std::string path = engine::addons::probes::MakeProbeDataPath(m_currentSceneName, probe.probeId);
            if (probe.bakedDataPath != path)
                probe.bakedDataPath = path;
            probes.push_back(entity);
        });
        const int passes = m_editorState.probeBakePasses >= 2u ? 2 : 1;
        for (int pass = 1; pass <= passes; ++pass)
            for (const EntityID entity : probes)
                m_probeBakeQueue.emplace_back(entity, pass);
    };
    m_editorFrameCtx->reflectionProbeStatus = [this](EntityID entity) -> int {
        if (!m_world || !m_probeRuntime || !m_world->IsAlive(entity))
            return 0;
        for (const auto& queued : m_probeBakeQueue)
            if (queued.first == entity)
                return 3;
        if (m_probeBakeInFlight == entity)
            return 3;
        const auto* probe = m_world->Get<ReflectionProbeComponent>(entity);
        uint64_t baked = 0u;
        if (!probe || probe->bakedDataPath.empty() || !m_probeRuntime->GetBakedInputHash(probe->bakedDataPath, baked))
            return 0;
        return baked == engine::addons::probes::ComputeProbeInputHash(*m_world, entity) ? 1 : 2;
    };
    m_editorFrameCtx->restartParticleEmitter = [this](EntityID entity) {
        m_particles.Restart(entity);
        // Pausierte Emitter (z. B. Vorlagen fuer Scripts) zeigen ihren Burst als Vorschau.
        if (const auto* emitter = m_world ? m_world->Get<ParticleEmitterComponent>(entity) : nullptr;
            emitter && !emitter->playing)
        {
            m_particles.Burst(entity, std::max(emitter->burstCount, 20u));
        }
    };
    m_editorFrameCtx->restartAnimationParticles = [this](EntityID entity) { m_animationParticles.Restart(entity); };
    m_editorFrameCtx->animationParticleCount = [this](EntityID entity) { return m_animationParticles.ParticleCount(entity); };
    m_editorFrameCtx->particleCount = [this](EntityID entity) {
        return m_particles.ParticleCount(entity);
    };
    // Partikel-Texturen relativ zum Asset-Root, ueber die Asset-Pipeline.
    m_particleView.UseAssetTextures(m_assetPipeline, m_assetRegistry);
    m_gameParticleView.UseAssetTextures(m_assetPipeline, m_assetRegistry);
    m_animationParticleView.UseAssetTextures(m_assetPipeline, m_assetRegistry);
    m_editorFrameCtx->setEditorDebugFlags = [this](uint32_t flags) {
        m_editorState.editorDebugFlags = flags;
    };
    m_editorFrameCtx->setEditorBackgroundColor = [this](const std::array<float, 4>& color) {
        m_config.clearColor = color;
    };
    RefreshSceneList();
    // Native Datei-Dialoge: Callbacks nur setzen wenn die Plattform sie unterstuetzt.
    // Auf Windows: IFileOpenDialog (Win32 COM).
    // Auf Linux/macOS: IsAvailable() == false → Callbacks bleiben nullptr → kein "..."-Button.
    if (platform::dialog::IsAvailable())
    {
        // Parent-Handle wird beim Callback-Aufruf geholt (Fenster existiert dann garantiert).
        m_editorFrameCtx->browseForProjectFile = [this]() -> std::string {
            platform::NativeWindowHandle handle{};
            if (auto* win = m_renderLoop.GetWindow())
                handle = win->GetNativeWindowHandle();
            const platform::dialog::FileFilter filters[] = {
                { Translate("project.file_filter"), "*.json" },
                { Translate("project.all_files"), "*.*" }
            };
            const std::filesystem::path initialFolder = !m_projectRoot.empty()
                ? m_projectRoot
                : (std::filesystem::current_path() / "projects");
            return platform::dialog::BrowseForFile(
                Translate("project.open"), filters, 2, "krom-project.json", handle,
                PathToUtf8(initialFolder).c_str());
        };
        m_editorFrameCtx->projectsRootDirectory =
            PathToUtf8((PathFromUtf8(KROM_SOURCE_DIR) / "projects").make_preferred());
        m_editorFrameCtx->browseForProjectFolder = [this]() -> std::string {
            platform::NativeWindowHandle handle{};
            if (auto* win = m_renderLoop.GetWindow())
                handle = win->GetNativeWindowHandle();
            return platform::dialog::BrowseForFolder(
                Translate("project.choose_folder"), handle);
        };
        m_editorFrameCtx->browseForToolPath = [this](bool folder) -> std::string {
            platform::NativeWindowHandle handle{};
            if (auto* win = m_renderLoop.GetWindow())
                handle = win->GetNativeWindowHandle();
            if (folder)
                return platform::dialog::BrowseForFolder(Translate("settings.tool_visual_studio"), handle);
            const platform::dialog::FileFilter filters[] = {
                { Translate("settings.tool_programs"), "*.exe" },
                { Translate("project.all_files"), "*.*" }
            };
            return platform::dialog::BrowseForFile(Translate("settings.tool_browse"), filters, 2, nullptr, handle);
        };
    }
    {
        // Tool check from the editor settings: discovery takes seconds (VsDevCmd), so it
        // runs on a detached worker that owns the shared state, like runtime builds.
        struct ToolCheck
        {
            std::mutex mutex;
            bool running = false;
            bool finished = false;
            build::ToolDiscovery discovery;
            renderer::ShaderToolLocation dxc;
            renderer::ShaderToolLocation glslang;
            std::string dxcOverride;
            std::string glslangOverride;
        };
        auto check = std::make_shared<ToolCheck>();
        m_editorFrameCtx->startBuildToolsCheck =
            [check](const renderer::addons::editor::EditorPreferences& preferences) {
                {
                    std::lock_guard lock(check->mutex);
                    if (check->running) return;
                    check->running = true;
                    check->finished = false;
                }
                const auto overrides = BuildToolOverridesFrom(preferences);
                const auto dxcPath = PathFromUtf8(preferences.toolDxc);
                const auto glslangPath = PathFromUtf8(preferences.toolGlslang);
                std::thread([check, overrides, dxcPath, glslangPath] {
#ifdef _MSC_FULL_VER
                    auto discovery = build::DiscoverBuildTools(_MSC_FULL_VER, overrides);
#else
                    auto discovery = build::DiscoverBuildTools(0, overrides);
#endif
                    auto dxc = renderer::FindShaderTool(renderer::ShaderTool::Dxc, dxcPath);
                    auto glslang = renderer::FindShaderTool(renderer::ShaderTool::Glslang, glslangPath);
                    std::lock_guard lock(check->mutex);
                    check->discovery = std::move(discovery);
                    check->dxc = std::move(dxc);
                    check->glslang = std::move(glslang);
                    check->dxcOverride = PathToUtf8(dxcPath);
                    check->glslangOverride = PathToUtf8(glslangPath);
                    check->running = false;
                    check->finished = true;
                }).detach();
            };
        m_editorFrameCtx->buildToolsCheckResult = [check](std::string& report, bool& ok) {
            std::lock_guard lock(check->mutex);
            if (check->running) return true;
            report.clear();
            ok = false;
            if (!check->finished) return false;
            ok = true;
            auto line = [&](const std::string& name, const std::string& value) {
                report += name + ": " + value + '\n';
            };
            const auto& discovery = check->discovery;
            if (discovery.Succeeded())
            {
                const auto& tools = discovery.tools;
                line("Visual Studio", PathToUtf8(tools.installation) + " (" +
                    (discovery.installationOverridden ? Translate("settings.tool_source_override") : "vswhere") + ")");
                line("MSVC", (tools.toolset.empty() ? std::string("default") : tools.toolset) +
                    ", _MSC_FULL_VER " + std::to_string(tools.compilerVersion));
#ifdef _MSC_FULL_VER
                if (!build::CompilerCompatible(_MSC_FULL_VER, tools.compilerVersion, true))
                {
                    ok = false;
                    report += Translate("build.script_compiler_mismatch") + std::string(" Editor: ") +
                        std::to_string(_MSC_FULL_VER) + '\n';
                }
#endif
                line("CMake", PathToUtf8(tools.cmake));
                line("Ninja", PathToUtf8(tools.ninja));
            }
            else
            {
                ok = false;
                report += Translate(discovery.errorKey.c_str());
                if (!discovery.detail.empty()) report += " " + discovery.detail;
                report += '\n';
            }
            auto shaderLine = [&](const char* name, const renderer::ShaderToolLocation& location,
                                  const std::string& overridePath) {
                if (location.path.empty())
                {
                    ok = false;
                    line(name, std::string(Translate("settings.tool_not_found")) +
                        (overridePath.empty() ? std::string{} : " (" + overridePath + ")"));
                    return;
                }
                const std::string source = location.source == "override"
                    ? std::string(Translate("settings.tool_source_override")) : location.source;
                line(name, PathToUtf8(location.path) + " (" + source + ")");
            };
            shaderLine("DXC", check->dxc, check->dxcOverride);
            shaderLine("glslang", check->glslang, check->glslangOverride);
            if (!report.empty() && report.back() == '\n') report.pop_back();
            return false;
        };
    }
    m_editorFrameCtx->debugDraw = &m_debugDraw;

    m_editorFrameCtx->currentProjectName = m_projectName;
    m_editorFrameCtx->currentProjectRoot = PathToUtf8(m_projectRoot);
    m_editorFrameCtx->engineAssetRoot  = PathToUtf8(ResolveAssetRoot());
    m_editorFrameCtx->engineEditorDir  =
        PathToUtf8(PathFromUtf8(__FILE__).parent_path().parent_path() / "editor");
    m_editorFrameCtx->scriptRegistry   = &m_scriptRegistry;
#ifdef KROM_HAS_GAME_SCRIPTS
    KromRegisterGameScripts(m_scriptRegistry);
    Debug::Log("KromEditorApp: Projekt-Scripts registriert.");
#endif
    RefreshSceneList();
#endif

    if (!InitializePlatform())
    {
        Debug::LogError("KromEditorApp: platform initialization failed");
        rollback();
        return false;
    }

    if (!InitializeRenderLoop())
    {
        Debug::LogError("KromEditorApp: render loop initialization failed");
        rollback();
        return false;
    }
    renderLoopInitialized = true;

#ifdef KROM_EDITOR_HAS_IMGUI
    if (m_editorFrameCtx)
    {
        m_editorFrameCtx->jobSystem = &m_renderLoop.GetRenderSystem().GetJobSystem();
        if (renderer::IDevice* device = m_renderLoop.GetRenderSystem().GetDevice())
        {
            constexpr uint32_t kPreviewW = 320u;
            constexpr uint32_t kPreviewH = 180u;
            renderer::RenderTargetDesc previewDesc{};
            previewDesc.width       = kPreviewW;
            previewDesc.height      = kPreviewH;
            previewDesc.colorFormat = m_config.backend == renderer::DeviceFactory::BackendType::OpenGL
                ? renderer::Format::BGRA8_UNORM
                : renderer::Format::BGRA8_UNORM_SRGB;
            previewDesc.hasDepth    = true;
            previewDesc.depthFormat = renderer::Format::D24_UNORM_S8_UINT;
            m_previewRT = device->CreateRenderTarget(previewDesc);

            if (m_previewRT.IsValid())
            {
                m_editorFrameCtx->previewRT     = m_previewRT;
                m_editorFrameCtx->previewWidth  = kPreviewW;
                m_editorFrameCtx->previewHeight = kPreviewH;
#if defined(KROM_APP_BACKEND_OPENGL)
                m_editorFrameCtx->previewFlipY  = true;
#else
                m_editorFrameCtx->previewFlipY  = false;
#endif

                const TextureHandle previewColorTex =
                    device->GetRenderTargetColorTexture(m_previewRT);
#if defined(KROM_APP_BACKEND_VULKAN) && defined(KROM_IMGUI_HAS_VULKAN)
                if (auto* vkDevice = static_cast<renderer::vulkan::VulkanDevice*>(device))
                {
                    renderer::SamplerDesc samplerDesc{};
                    samplerDesc.minFilter = renderer::FilterMode::Linear;
                    samplerDesc.magFilter = renderer::FilterMode::Linear;
                    samplerDesc.mipFilter = renderer::FilterMode::Linear;
                    samplerDesc.addressU = renderer::WrapMode::Clamp;
                    samplerDesc.addressV = renderer::WrapMode::Clamp;
                    samplerDesc.addressW = renderer::WrapMode::Clamp;
                    m_previewSampler = vkDevice->CreateSampler(samplerDesc);

                    const auto* textureEntry = vkDevice->GetResources().textures.Get(previewColorTex);
                    const VkSampler sampler =
                        (m_previewSampler < vkDevice->GetResources().samplers.size())
                            ? vkDevice->GetResources().samplers[m_previewSampler].sampler
                            : VK_NULL_HANDLE;
                    if (textureEntry && textureEntry->sampleView && sampler)
                    {
                        m_previewTextureId = ImGui_ImplVulkan_AddTexture(
                            sampler,
                            textureEntry->sampleView,
                            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
                    }
                }
#else
                m_previewTextureId = device->GetNativeTextureHandle(previewColorTex);
#endif
                m_editorFrameCtx->cameraPreviewTexture =
                    [this](EntityID) -> void* {
                        return m_previewTextureId;
                    };
            }

            constexpr uint32_t kMaterialPreviewW = 512u;
            constexpr uint32_t kMaterialPreviewH = 512u;
            renderer::RenderTargetDesc materialPreviewDesc{};
            materialPreviewDesc.width       = kMaterialPreviewW;
            materialPreviewDesc.height      = kMaterialPreviewH;
            materialPreviewDesc.colorFormat = m_config.backend == renderer::DeviceFactory::BackendType::OpenGL
                ? renderer::Format::BGRA8_UNORM
                : renderer::Format::BGRA8_UNORM_SRGB;
            materialPreviewDesc.hasDepth    = true;
            materialPreviewDesc.depthFormat = renderer::Format::D24_UNORM_S8_UINT;
            m_materialPreviewRT = device->CreateRenderTarget(materialPreviewDesc);

            if (m_materialPreviewRT.IsValid())
            {
                m_editorFrameCtx->materialPreviewRT     = m_materialPreviewRT;
                m_editorFrameCtx->materialPreviewWidth  = kMaterialPreviewW;
                m_editorFrameCtx->materialPreviewHeight = kMaterialPreviewH;
#if defined(KROM_APP_BACKEND_OPENGL)
                m_editorFrameCtx->materialPreviewFlipY  = true;
#else
                m_editorFrameCtx->materialPreviewFlipY  = false;
#endif

                const TextureHandle previewColorTex =
                    device->GetRenderTargetColorTexture(m_materialPreviewRT);
#if defined(KROM_APP_BACKEND_VULKAN) && defined(KROM_IMGUI_HAS_VULKAN)
                if (auto* vkDevice = static_cast<renderer::vulkan::VulkanDevice*>(device))
                {
                    if (m_previewSampler == UINT32_MAX)
                    {
                        renderer::SamplerDesc samplerDesc{};
                        samplerDesc.minFilter = renderer::FilterMode::Linear;
                        samplerDesc.magFilter = renderer::FilterMode::Linear;
                        samplerDesc.mipFilter = renderer::FilterMode::Linear;
                        samplerDesc.addressU = renderer::WrapMode::Clamp;
                        samplerDesc.addressV = renderer::WrapMode::Clamp;
                        samplerDesc.addressW = renderer::WrapMode::Clamp;
                        m_previewSampler = vkDevice->CreateSampler(samplerDesc);
                    }

                    const auto* textureEntry = vkDevice->GetResources().textures.Get(previewColorTex);
                    const VkSampler sampler =
                        (m_previewSampler < vkDevice->GetResources().samplers.size())
                            ? vkDevice->GetResources().samplers[m_previewSampler].sampler
                            : VK_NULL_HANDLE;
                    if (textureEntry && textureEntry->sampleView && sampler)
                    {
                        m_materialPreviewTextureId = ImGui_ImplVulkan_AddTexture(
                            sampler,
                            textureEntry->sampleView,
                            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
                    }
                }
#else
                m_materialPreviewTextureId = device->GetNativeTextureHandle(previewColorTex);
#endif
                m_editorFrameCtx->materialPreviewTexture =
                    [this]() -> void* {
                        return m_materialPreviewTextureId;
                    };
            }
        }

        // ── Animation-Preview-RT ─────────────────────────────────────────────
        if (renderer::IDevice* device = m_renderLoop.GetRenderSystem().GetDevice())
        {
            renderer::RenderTargetDesc animationPreviewDesc{};
            animationPreviewDesc.width       = kAnimationPreviewSize;
            animationPreviewDesc.height      = kAnimationPreviewSize;
            animationPreviewDesc.colorFormat = m_config.backend == renderer::DeviceFactory::BackendType::OpenGL
                ? renderer::Format::RGBA8_UNORM_SRGB
                : renderer::Format::BGRA8_UNORM_SRGB;
            animationPreviewDesc.hasDepth    = true;
            animationPreviewDesc.depthFormat = renderer::Format::D24_UNORM_S8_UINT;
            m_animationPreviewRT = device->CreateRenderTarget(animationPreviewDesc);
            if (m_animationPreviewRT.IsValid())
            {
#if defined(KROM_APP_BACKEND_OPENGL)
                m_editorFrameCtx->animationPreviewFlipY = true;
#endif
                const TextureHandle previewColorTex = device->GetRenderTargetColorTexture(m_animationPreviewRT);
#if defined(KROM_APP_BACKEND_VULKAN) && defined(KROM_IMGUI_HAS_VULKAN)
                if (auto* vkDevice = static_cast<renderer::vulkan::VulkanDevice*>(device))
                {
                    const auto* textureEntry = vkDevice->GetResources().textures.Get(previewColorTex);
                    const VkSampler sampler =
                        (m_previewSampler < vkDevice->GetResources().samplers.size())
                            ? vkDevice->GetResources().samplers[m_previewSampler].sampler
                            : VK_NULL_HANDLE;
                    if (textureEntry && textureEntry->sampleView && sampler)
                        m_animationPreviewTextureId = ImGui_ImplVulkan_AddTexture(
                            sampler, textureEntry->sampleView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
                }
#else
                m_animationPreviewTextureId = device->GetNativeTextureHandle(previewColorTex);
#endif
                m_editorFrameCtx->animationPreviewTexture = [this]() -> void* { return m_animationPreviewTextureId; };
            }
        }

        // ── Prefab-Preview-RT ────────────────────────────────────────────────
        if (renderer::IDevice* device = m_renderLoop.GetRenderSystem().GetDevice())
        {
            constexpr uint32_t kPrefabPreviewW = 512u;
            constexpr uint32_t kPrefabPreviewH = 512u;
            renderer::RenderTargetDesc prefabPreviewDesc{};
            prefabPreviewDesc.width       = kPrefabPreviewW;
            prefabPreviewDesc.height      = kPrefabPreviewH;
            prefabPreviewDesc.colorFormat = m_config.backend == renderer::DeviceFactory::BackendType::OpenGL
                ? renderer::Format::RGBA8_UNORM_SRGB
                : renderer::Format::BGRA8_UNORM_SRGB;
            prefabPreviewDesc.hasDepth    = true;
            prefabPreviewDesc.depthFormat = renderer::Format::D24_UNORM_S8_UINT;
            m_prefabPreviewRT = device->CreateRenderTarget(prefabPreviewDesc);
            if (m_prefabPreviewRT.IsValid())
            {
                m_editorFrameCtx->prefabPreviewRT     = m_prefabPreviewRT;
                m_editorFrameCtx->prefabPreviewWidth  = kPrefabPreviewW;
                m_editorFrameCtx->prefabPreviewHeight = kPrefabPreviewH;
#if defined(KROM_APP_BACKEND_OPENGL)
                m_editorFrameCtx->prefabPreviewFlipY  = true;
#else
                m_editorFrameCtx->prefabPreviewFlipY  = false;
#endif

                const TextureHandle prefabColorTex =
                    device->GetRenderTargetColorTexture(m_prefabPreviewRT);
#if defined(KROM_APP_BACKEND_VULKAN) && defined(KROM_IMGUI_HAS_VULKAN)
                if (auto* vkDev = static_cast<renderer::vulkan::VulkanDevice*>(device))
                {
                    if (m_previewSampler == UINT32_MAX)
                    {
                        renderer::SamplerDesc sd{};
                        sd.minFilter = renderer::FilterMode::Linear;
                        sd.magFilter = renderer::FilterMode::Linear;
                        sd.mipFilter = renderer::FilterMode::Linear;
                        sd.addressU  = renderer::WrapMode::Clamp;
                        sd.addressV  = renderer::WrapMode::Clamp;
                        sd.addressW  = renderer::WrapMode::Clamp;
                        m_previewSampler = vkDev->CreateSampler(sd);
                    }
                    const auto* texEntry = vkDev->GetResources().textures.Get(prefabColorTex);
                    const VkSampler sampler =
                        (m_previewSampler < vkDev->GetResources().samplers.size())
                            ? vkDev->GetResources().samplers[m_previewSampler].sampler
                            : VK_NULL_HANDLE;
                    if (texEntry && texEntry->sampleView && sampler)
                        m_prefabPreviewTextureId = ImGui_ImplVulkan_AddTexture(
                            sampler, texEntry->sampleView,
                            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
                }
#else
                m_prefabPreviewTextureId = device->GetNativeTextureHandle(prefabColorTex);
#endif
                m_editorFrameCtx->prefabPreviewTexture =
                    [this]() -> void* { return m_prefabPreviewTextureId; };
            }
        }

        m_editorFrameCtx->editorTextureId = [this](TextureHandle texture) -> void* {
            if (!texture.IsValid())
                return nullptr;

            renderer::IDevice* baseDevice = m_renderLoop.GetRenderSystem().GetDevice();
            if (!baseDevice)
                return nullptr;

#if defined(KROM_APP_BACKEND_VULKAN) && defined(KROM_IMGUI_HAS_VULKAN)
            auto* vkDevice = static_cast<renderer::vulkan::VulkanDevice*>(baseDevice);
            if (!vkDevice)
                return nullptr;

            const uint32_t key = texture.value;
            if (auto it = m_editorTextureIdCache.find(key); it != m_editorTextureIdCache.end())
                return it->second;

            if (m_previewSampler == UINT32_MAX)
            {
                renderer::SamplerDesc samplerDesc{};
                samplerDesc.minFilter = renderer::FilterMode::Linear;
                samplerDesc.magFilter = renderer::FilterMode::Linear;
                samplerDesc.mipFilter = renderer::FilterMode::Linear;
                samplerDesc.addressU = renderer::WrapMode::Clamp;
                samplerDesc.addressV = renderer::WrapMode::Clamp;
                samplerDesc.addressW = renderer::WrapMode::Clamp;
                m_previewSampler = vkDevice->CreateSampler(samplerDesc);
            }

            const auto* textureEntry = vkDevice->GetResources().textures.Get(texture);
            const VkSampler sampler =
                (m_previewSampler < vkDevice->GetResources().samplers.size())
                    ? vkDevice->GetResources().samplers[m_previewSampler].sampler
                    : VK_NULL_HANDLE;
            if (!textureEntry || !textureEntry->sampleView || !sampler)
                return nullptr;

            void* id = ImGui_ImplVulkan_AddTexture(
                sampler,
                textureEntry->sampleView,
                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
            if (id)
                m_editorTextureIdCache.emplace(key, id);
            return id;
#else
            return baseDevice->GetNativeTextureHandle(texture);
#endif
        };
    }
#endif

    if (!InitializeAssetPipeline())
    {
        Debug::LogError("KromEditorApp: asset pipeline initialization failed");
        rollback();
        return false;
    }
#ifdef KROM_EDITOR_HAS_IMGUI
    if (m_editorFrameCtx)
    {
        m_editorFrameCtx->assetPipeline = m_assetPipeline.get();
        if (const renderer::IDevice* device = m_renderLoop.GetRenderSystem().GetDevice())
            m_editorFrameCtx->shaderTarget = device->GetShaderTargetProfile();
        m_editorFrameCtx->forwardPlusActive = m_forwardPlusActive;
    }
#endif

    if (!InitializeTonemapMaterial())
    {
        Debug::LogError("KromEditorApp: tonemap material initialization failed");
        rollback();
        return false;
    }

    m_initialized = true;
    return true;
}

int KromEditorApp::Run(IAppScene& scene)
{
    if (!m_initialized)
        return -1;

    AppSceneContext context{ m_assetRegistry,
                                 *m_assetPipeline,
                                 m_renderLoop,
                                 m_materialSystem,
                                 *m_world,
                                 m_transformSystem,
                                 m_forwardPlusActive };
    if (!scene.Build(context))
        return -2;

#ifdef KROM_EDITOR_HAS_IMGUI
    const auto& preferences = renderer::addons::editor::GetEditorPreferences();
    if (m_config.visible && m_editorFrameCtx && m_projectRoot.empty() && preferences.reopenProject && !preferences.lastProject.empty())
    {
        std::error_code error;
        const auto project = PathFromUtf8(preferences.lastProject);
        if (std::filesystem::is_regular_file(project, error))
            m_editorState.pendingStartProjectFile = preferences.lastProject;
    }
#endif

    m_transformSystem.Update(*m_world);
    m_boundsSystem.Update(*m_world);

    while (!m_renderLoop.ShouldExit())
    {
#ifdef KROM_EDITOR_HAS_IMGUI
        auto appCpuMark = std::chrono::steady_clock::now();
        auto measureAppCpu = [&](size_t index) {
            const auto now = std::chrono::steady_clock::now();
            if (m_editorFrameCtx) m_editorFrameCtx->appCpuTimings[index] =
                std::chrono::duration<double, std::milli>(now - appCpuMark).count();
            appCpuMark = now;
        };
#endif
        if (m_maximizeAfterProjectLoad)
        {
            m_maximizeAfterProjectLoad = false;
            if (auto* window = m_renderLoop.GetWindow())
            {
                window->SetResizable(true);
                if (m_config.visible)
                    window->Maximize();
            }
        }
#ifdef KROM_EDITOR_HAS_IMGUI
        ProcessPendingSceneSwitch();
        if (m_renderLoop.ShouldExit())
            break;
#endif
        if (auto* input = m_renderLoop.GetInput();
            input && input->IsKeyPressed(platform::Key::Escape) && m_renderLoop.GetWindow())
        {
#ifdef KROM_EDITOR_HAS_IMGUI
            if (!m_editorFrameCtx)
#endif
            m_renderLoop.GetWindow()->RequestClose();
        }

        const float deltaSeconds = m_timing.GetDeltaSecondsF();
#ifdef KROM_EDITOR_HAS_IMGUI
        const bool sceneLoading = m_editorFrameCtx && m_editorFrameCtx->sceneLoadActive;
#else
        constexpr bool sceneLoading = false;
#endif
        if (!sceneLoading && !scene.Update(context, deltaSeconds))
            return -3;

#ifdef KROM_EDITOR_HAS_IMGUI
        if (m_editorFrameCtx && !sceneLoading)
        {
            m_editorFrameCtx->deltaSeconds = deltaSeconds;
            const int backupMinutes = renderer::addons::editor::GetEditorPreferences().backupMinutes;
            if (backupMinutes > 0 && !m_projectRoot.empty() && !m_playActive &&
                !renderer::addons::editor::IsInPrefabMode(m_editorFrameCtx->state))
            {
                m_sceneBackupElapsed += deltaSeconds;
                if (m_sceneBackupElapsed >= backupMinutes * 60.f)
                {
                    m_sceneBackupElapsed = 0.f;
                    EnsureEntityGuids(*m_world);
                    const auto backup = m_projectRoot / "editor" / "backups" / GetCurrentEditorScenePath().filename();
                    if (!WriteEditorScene(backup))
                        m_editorFrameCtx->lastFileMessage = Translate("settings.backup_failed");
                }
            }
            else if (backupMinutes == 0)
                m_sceneBackupElapsed = 0.f;
            PollRuntimeStateFile(m_editorFrameCtx->state, m_projectRoot, deltaSeconds);
            ProcessEditorFileDrops();

            auto setDebugView = [this](uint32_t flags) {
                m_editorState.editorDebugFlags = flags;
                if (m_editorFrameCtx && m_editorFrameCtx->setEditorDebugFlags)
                    m_editorFrameCtx->setEditorDebugFlags(flags);
            };

            if (auto* input = m_renderLoop.GetInput(); input && !m_editorFrameCtx->gameViewFocused)
            {
                if (input->KeyHit(platform::Key::F1))  setDebugView(0u);
                if (input->KeyHit(platform::Key::F2))  setDebugView(renderer::DBG_VIEW_LINEAR_DEPTH);
                if (input->KeyHit(platform::Key::F3))  setDebugView(renderer::DBG_VIEW_GTAO);
                if (input->KeyHit(platform::Key::F4))  setDebugView(renderer::DBG_DISABLE_GTAO);
                if (input->KeyHit(platform::Key::F5))  setDebugView(renderer::DBG_VIEW_NORMALS);
                if (input->KeyHit(platform::Key::F6))  setDebugView(renderer::DBG_VIEW_AO);
                if (input->KeyHit(platform::Key::F7))  setDebugView(renderer::DBG_VIEW_ROUGHNESS);
                if (input->KeyHit(platform::Key::F8))  setDebugView(renderer::DBG_VIEW_METALLIC);
                if (input->KeyHit(platform::Key::F9))  setDebugView(renderer::DBG_VIEW_SHADOW);
                if (input->KeyHit(platform::Key::F10)) setDebugView(renderer::DBG_VIEW_NOL);
            }
        }
#endif

#ifdef KROM_EDITOR_HAS_IMGUI
        measureAppCpu(0);
#endif
        if (!sceneLoading)
        {
#ifdef KROM_EDITOR_HAS_IMGUI
        // Ein fertiger Script-Build kann Play in diesem Frame starten.
        PollRuntimeBuild();
#endif
        // Editor work that must see the current local transforms; in play it runs
        // inside RunFrame right before the TransformSystem.
        const auto editorSync = [this]()
        {
            mesh_renderer::UpdateLocalBoundsFromMeshes(*m_world, m_assetRegistry);
#ifdef KROM_EDITOR_HAS_IMGUI
            EnsureCameraGizmos();       // neue Kameras bekommen sofort ihr Gizmo
            SyncCameraGizmoTransforms();
            EnsureMaterialPreviewEntity();
            SyncMaterialPreviewEntity();
            ProcessAssetThumbnailQueue();
            SyncPreviewLightRigs();
            DrainConsoleLogs(&m_editorState);
#endif
        };
#ifdef KROM_EDITOR_HAS_IMGUI
        if (m_playActive && !m_playPaused)
            TickEditorPlay(deltaSeconds, editorSync);
        else
#endif
        {
            editorSync();
            m_transformSystem.Update(*m_world);
            m_boundsSystem.Update(*m_world);
        }
#ifdef KROM_EDITOR_HAS_IMGUI
        UploadSkinning();
#endif
        }

#ifdef KROM_EDITOR_HAS_IMGUI
        measureAppCpu(1);
#endif
        const renderer::ISwapchain* swapchain = m_renderLoop.GetRenderSystem().GetSwapchain();
        const uint32_t viewportWidth = (swapchain && swapchain->GetWidth() > 0u) ? swapchain->GetWidth() : m_config.width;
        const uint32_t viewportHeight = (swapchain && swapchain->GetHeight() > 0u) ? swapchain->GetHeight() : m_config.height;
#ifdef KROM_EDITOR_HAS_IMGUI
        // Viewport-Panel vorhanden: Szene in eigenes Ziel in Panelgroesse, sonst wie bisher
        // bildschirmfuellend (Startbildschirm, Laden). Ist der Game-Reiter sichtbar, ist die
        // Spielkamera die Hauptansicht; ist zugleich der Viewport sichtbar, wird die
        // Editor-Kamera als Zusatzansicht in dessen Ziel gerendert.
        bool gameInPanel = false;
        if (m_editorFrameCtx)
        {
            renderer::addons::editor::EditorFrameContext& ui = *m_editorFrameCtx;
            const bool flipY = m_config.backend == renderer::DeviceFactory::BackendType::OpenGL;
            if (ui.sceneViewRequestWidth == 0u || ui.sceneViewRequestHeight == 0u)
                ReleasePanelTarget(m_sceneView);
            else
                UpdatePanelTarget(m_sceneView, ui.sceneViewRequestWidth, ui.sceneViewRequestHeight, "EditorSceneView");
            if (ui.gameViewRequestWidth == 0u || ui.gameViewRequestHeight == 0u)
                ReleasePanelTarget(m_gameView);
            else
                UpdatePanelTarget(m_gameView, ui.gameViewRequestWidth, ui.gameViewRequestHeight, "EditorGameView");
            ui.sceneViewTexture = m_sceneView.textureId;
            ui.sceneViewFlipY = flipY;
            ui.gameViewFlipY = flipY;
            gameInPanel = !sceneLoading && ui.gameViewVisible && m_gameView.rt.IsValid();
            ui.gameOverlayText.clear();
            if (m_playActive)
                for (const addons::text::TextCommand& command : m_game.Text().Commands())
                    ui.gameOverlayText.push_back({command.text, command.x, command.y, command.size, command.color});
        }
        renderer::RenderView gameView{};
        if (gameInPanel &&
            engine::addons::camera::BuildPrimaryRenderView(*m_world, m_gameView.width, m_gameView.height,
                                                           gameView, m_cameraOptions))
        {
            renderer::IDevice* device = m_renderLoop.GetRenderSystem().GetDevice();
            gameView.sceneOutputRT = m_gameView.rt;
            gameView.sceneOutputTex = device ? device->GetRenderTargetColorTexture(m_gameView.rt)
                                             : TextureHandle::Invalid();
            gameView.sceneOutputWidth = m_gameView.width;
            gameView.sceneOutputHeight = m_gameView.height;
            gameView.enableSelectionOutline = false;
            gameView.visibilityLayerMask &= ~(renderer::LAYER_EDITOR_ANIMATION_PREVIEW | renderer::LAYER_EDITOR_GIZMO | renderer::LAYER_EDITOR_GIZMO_DEPTH |
                                              renderer::LAYER_EDITOR_MATERIAL_PREVIEW |
                                              renderer::LAYER_EDITOR_ASSET_THUMBNAIL |
                                              renderer::LAYER_EDITOR_PREFAB_PREVIEW);
            m_editorFrameCtx->gameViewTexture = m_gameView.textureId;
        }
        else
        {
            // Ohne Hauptkamera zeigt der Game-Reiter einen Hinweis.
            gameInPanel = false;
            if (m_editorFrameCtx)
                m_editorFrameCtx->gameViewTexture = nullptr;
        }
        // Auch bei verdecktem Viewport-Reiter im Ziel bleiben (kein Umschalten = kein Flimmern).
        const bool sceneInPanel = m_editorFrameCtx && m_sceneView.rt.IsValid();
        const uint32_t editorRenderWidth = sceneInPanel ? m_sceneView.width : viewportWidth;
        const uint32_t editorRenderHeight = sceneInPanel ? m_sceneView.height : viewportHeight;
        if (m_editorFrameCtx)
        {
            // Without the panel the whole editor window; ImGui positions are desktop
            // coordinates with multi-viewports, so use the window origin.
            const ImVec2 windowOrigin = ImGui::GetCurrentContext()
                ? ImGui::GetMainViewport()->Pos : ImVec2(0.f, 0.f);
            m_editorFrameCtx->renderViewX = sceneInPanel ? m_editorFrameCtx->sceneViewX : windowOrigin.x;
            m_editorFrameCtx->renderViewY = sceneInPanel ? m_editorFrameCtx->sceneViewY : windowOrigin.y;
            m_editorFrameCtx->renderViewWidth = static_cast<float>(editorRenderWidth);
            m_editorFrameCtx->renderViewHeight = static_cast<float>(editorRenderHeight);
        }
#endif

        renderer::RenderView view{};
#ifdef KROM_EDITOR_HAS_IMGUI
        if (m_editorFrameCtx)
        {
            renderer::addons::editor::BuildEditorCameraRenderView(
                m_editorState,
                editorRenderWidth,
                editorRenderHeight,
                view,
                m_cameraOptions.ambientColor,
                m_cameraOptions.ambientIntensity);
            view.visibilityLayerMask &= ~renderer::LAYER_EDITOR_MATERIAL_PREVIEW;
            view.visibilityLayerMask &= ~renderer::LAYER_EDITOR_ASSET_THUMBNAIL;
            view.visibilityLayerMask &= ~renderer::LAYER_EDITOR_PREFAB_PREVIEW;
            view.visibilityLayerMask &= ~renderer::LAYER_EDITOR_ANIMATION_PREVIEW;
            // Prefab mode / model view: the scene lights are gone, light the object with the
            // prefab rig (see SyncPreviewLightRigs). Lights inside the prefab still apply.
            if (renderer::addons::editor::IsInPrefabMode(m_editorFrameCtx->state))
                view.lightLayerMask |= renderer::LAYER_EDITOR_PREFAB_PREVIEW;
            if (sceneInPanel)
            {
                renderer::IDevice* device = m_renderLoop.GetRenderSystem().GetDevice();
                view.sceneOutputRT = m_sceneView.rt;
                view.sceneOutputTex = device ? device->GetRenderTargetColorTexture(m_sceneView.rt)
                                             : TextureHandle::Invalid();
                view.sceneOutputWidth = m_sceneView.width;
                view.sceneOutputHeight = m_sceneView.height;
            }
        }
        else
#endif
        if (!engine::addons::camera::BuildPrimaryRenderView(
                *m_world,
                viewportWidth,
                viewportHeight,
                view,
                m_cameraOptions))
        {
            Debug::LogError("KromEditorApp: failed to build primary render view");
            return -4;
        }

        view.debugFlags = context.debugFlags | m_editorState.editorDebugFlags;
        if (m_editorState.editorDebugFlags != 0u)
            view.enableBloom = false;
#ifdef KROM_EDITOR_HAS_IMGUI
        auto applyEditorEnvironmentViewSettings = [this](renderer::RenderView& target) {
            target.backgroundMode = m_editorState.showSkyboxBackground
                ? BackgroundMode::Skybox
                : BackgroundMode::ClearColor;
            target.clearColor = m_editorState.backgroundColor;
        };
        if (m_editorFrameCtx)
            applyEditorEnvironmentViewSettings(view);
        else
#endif
            view.clearColor = m_config.clearColor;
#ifdef KROM_EDITOR_HAS_IMGUI
        // Game-Reiter sichtbar: Hauptansicht = Spielkamera (Hintergrund der Kamera, wie im Spiel).
        // Viewport zugleich sichtbar: Editor-Kamera als Zusatzansicht in dessen Ziel.
        const bool editorViewOffscreen = gameInPanel && m_editorFrameCtx && m_sceneView.rt.IsValid() &&
                                         m_editorFrameCtx->sceneViewVisible;
        renderer::RenderView editorOffscreenView{};
        if (gameInPanel)
        {
            if (editorViewOffscreen)
                editorOffscreenView = view;
            gameView.debugFlags = view.debugFlags;
            view = gameView;
        }
        // Ansicht der Editor-Kamera dieses Frames (Gizmo-Linien, Partikel-Vorschau).
        const bool drawEditorView = !gameInPanel || editorViewOffscreen;
        const renderer::RenderView& editorView = gameInPanel ? editorOffscreenView : view;
        renderer::FramePipelineCallbacks editorOffscreenCallbacks;
#endif

        renderer::FramePipelineCallbacks frameCallbacks;
        renderer::RenderView cameraPreviewView{};
        renderer::RenderView materialPreviewView{};
        renderer::RenderView assetThumbnailView{};
        // 6 editor views plus the 6 faces of a reflection probe capture.
        std::array<renderer::OffscreenRenderRequest, 12u> offscreenRequests{};
        std::span<const renderer::OffscreenRenderRequest> activeOffscreenRequests{};
        size_t offscreenRequestCount = 0u;

#ifdef KROM_EDITOR_HAS_IMGUI
        const bool navigatingEditorCamera =
            ImGui::GetCurrentContext() && ImGui::GetIO().MouseDown[1];
        const EntityID previewEntity = m_editorFrameCtx
            ? m_editorFrameCtx->state.selectedEntity
            : NULL_ENTITY;
        const bool hasCameraPreviewSelection = previewEntity.IsValid() &&
            m_world &&
            m_world->Has<engine::CameraComponent>(previewEntity);
        const bool previewRequested = !sceneLoading && m_editorFrameCtx &&
            m_editorFrameCtx->state.cameraPreviewWindowOpen &&
            hasCameraPreviewSelection;
        if (previewRequested && !navigatingEditorCamera && m_editorFrameCtx && m_previewRT.IsValid() &&
            m_editorFrameCtx->previewWidth > 0u && m_editorFrameCtx->previewHeight > 0u)
        {
            const bool builtView = engine::addons::camera::BuildRenderViewFromCamera(
                *m_world,
                previewEntity,
                m_editorFrameCtx->previewWidth,
                m_editorFrameCtx->previewHeight,
                cameraPreviewView,
                m_cameraOptions);

            if (builtView)
            {
                // DBG_DISABLE_IBL/GTAO/SHADOWS hier NICHT mehr erzwingen: DBG_DISABLE_IBL ersetzt
                // die echte IBL-Diffuse durch einen flachen ambientColor-Fallback (pbr_lit.ps.hlsl),
                // was die Vorschau ohne Schatten/AO/IBL deutlich blaustichig und flach wirken liess
                // im Vergleich zum Hauptviewport. Die Vorschau soll wie im Spiel aussehen.
                cameraPreviewView.debugFlags = context.debugFlags;
                // applyEditorEnvironmentViewSettings() NICHT aufrufen: die Kamera-Vorschau soll
                // das backgroundMode/clearColor der ausgewaehlten CameraComponent zeigen (so wie
                // es im Spiel aussieht), nicht den globalen Editor-Viewport-Hintergrund. Vorher
                // wurde hier z.B. "Solid" der Kamera durch den Editor-Skybox-Toggle ueberschrieben.
                cameraPreviewView.visibilityLayerMask &= ~renderer::LAYER_EDITOR_MATERIAL_PREVIEW;
                cameraPreviewView.visibilityLayerMask &= ~renderer::LAYER_EDITOR_ASSET_THUMBNAIL;
                cameraPreviewView.visibilityLayerMask &= ~renderer::LAYER_EDITOR_PREFAB_PREVIEW;
                cameraPreviewView.visibilityLayerMask &= ~renderer::LAYER_EDITOR_ANIMATION_PREVIEW;
                cameraPreviewView.enableShadows = false;
                cameraPreviewView.enableAmbientOcclusion = false;
                renderer::IDevice* device = m_renderLoop.GetRenderSystem().GetDevice();
                renderer::OffscreenRenderRequest& request = offscreenRequests[offscreenRequestCount++];
                request.view = &cameraPreviewView;
                request.outputRT = m_previewRT;
                request.outputTex = device
                    ? device->GetRenderTargetColorTexture(m_previewRT)
                    : TextureHandle::Invalid();
                request.viewportWidth = m_editorFrameCtx->previewWidth;
                request.viewportHeight = m_editorFrameCtx->previewHeight;
            }
        }

        const bool materialPreviewRequested = !sceneLoading && m_editorFrameCtx &&
            m_editorFrameCtx->state.materialWindowOpen &&
            m_editorFrameCtx->state.selectedMaterialAssetLoaded &&
            m_materialPreviewRT.IsValid() &&
            m_editorFrameCtx->materialPreviewWidth > 0u &&
            m_editorFrameCtx->materialPreviewHeight > 0u;
        if (materialPreviewRequested && BuildMaterialPreviewRenderView(materialPreviewView))
        {
            renderer::IDevice* device = m_renderLoop.GetRenderSystem().GetDevice();
            renderer::OffscreenRenderRequest& request = offscreenRequests[offscreenRequestCount++];
            request.view = &materialPreviewView;
            request.outputRT = m_materialPreviewRT;
            request.outputTex = device
                ? device->GetRenderTargetColorTexture(m_materialPreviewRT)
                : TextureHandle::Invalid();
            request.viewportWidth = m_editorFrameCtx->materialPreviewWidth;
            request.viewportHeight = m_editorFrameCtx->materialPreviewHeight;
        }

        renderer::RenderView animationPreviewView{};
        renderer::FramePipelineCallbacks animationPreviewCallbacks;
        if (!sceneLoading && m_animationPreviewRT.IsValid() && BuildAnimationPreviewRenderView(animationPreviewView))
        {
            renderer::IDevice* device = m_renderLoop.GetRenderSystem().GetDevice();
            renderer::OffscreenRenderRequest& request = offscreenRequests[offscreenRequestCount++];
            request.view = &animationPreviewView;
            request.callbacks = &animationPreviewCallbacks;
            request.outputRT = m_animationPreviewRT;
            request.outputTex = device
                ? device->GetRenderTargetColorTexture(m_animationPreviewRT)
                : TextureHandle::Invalid();
            request.viewportWidth = kAnimationPreviewSize;
            request.viewportHeight = kAnimationPreviewSize;
        }

        // Prefab-Preview-RT
        renderer::RenderView prefabPreviewView{};
        const bool prefabPreviewRequested = !sceneLoading && m_editorFrameCtx &&
            m_editorFrameCtx->state.prefabEditorOpen &&
            m_editorFrameCtx->state.prefabPreviewRootEntity.IsValid() &&
            m_world && m_world->IsAlive(m_editorFrameCtx->state.prefabPreviewRootEntity) &&
            m_prefabPreviewRT.IsValid() &&
            m_editorFrameCtx->prefabPreviewWidth > 0u &&
            m_editorFrameCtx->prefabPreviewHeight > 0u;
        if (prefabPreviewRequested && BuildPrefabPreviewRenderView(prefabPreviewView))
        {
            renderer::IDevice* device = m_renderLoop.GetRenderSystem().GetDevice();
            renderer::OffscreenRenderRequest& request = offscreenRequests[offscreenRequestCount++];
            request.view          = &prefabPreviewView;
            request.outputRT      = m_prefabPreviewRT;
            request.outputTex     = device
                ? device->GetRenderTargetColorTexture(m_prefabPreviewRT)
                : TextureHandle::Invalid();
            request.viewportWidth  = m_editorFrameCtx->prefabPreviewWidth;
            request.viewportHeight = m_editorFrameCtx->prefabPreviewHeight;
        }

        if (!sceneLoading && m_activeAssetThumbnailRT.IsValid() && BuildAssetThumbnailRenderView(assetThumbnailView))
        {
            renderer::IDevice* device = m_renderLoop.GetRenderSystem().GetDevice();
            renderer::OffscreenRenderRequest& request = offscreenRequests[offscreenRequestCount++];
            request.view = &assetThumbnailView;
            request.outputRT = m_activeAssetThumbnailRT;
            request.outputTex = device
                ? device->GetRenderTargetColorTexture(m_activeAssetThumbnailRT)
                : TextureHandle::Invalid();
            request.viewportWidth = 160u;
            request.viewportHeight = 160u;

            if (m_editorFrameCtx && m_editorFrameCtx->state.assetBrowser && !m_activeAssetThumbnailPath.empty())
            {
                const std::string key = PathToGenericUtf8(m_activeAssetThumbnailPath.lexically_normal());
                auto& thumbnails = m_editorFrameCtx->state.assetBrowser->assetPreviewThumbnails;
                if (auto it = thumbnails.find(key); it != thumbnails.end() &&
                    it->second.status == renderer::addons::editor::AssetBrowserState::PreviewStatus::Rendering)
                {
                    it->second.status = renderer::addons::editor::AssetBrowserState::PreviewStatus::Ready;
                    m_pendingThumbnailSaves.push_back(key);
                }
            }
            m_activeAssetThumbnailRT = RenderTargetHandle::Invalid();
            m_activeAssetThumbnailPath.clear();
        }

        measureAppCpu(2);
        if (m_editorFrameCtx)
        {
            const renderer::IDevice* device = m_renderLoop.GetRenderSystem().GetDevice();
            const math::Mat4 clipAdjustment = device
                ? device->GetClipSpaceAdjustment()
                : math::Mat4::Identity();
            // Editor-Hilfen gehoeren zur Editor-Kamera: Hauptansicht oder Zusatzansicht.
            renderer::FramePipelineCallbacks& editorCallbacks =
                editorViewOffscreen ? editorOffscreenCallbacks : frameCallbacks;
            m_debugDraw.Clear();
            m_debugDraw.SetViewProjection(clipAdjustment * editorView.projection * editorView.view);
            if (renderer::addons::editor::AnimationPreviewOwnsGizmos() && !sceneLoading)
                renderer::addons::editor::DrawAnimationPreviewGizmos(*m_editorFrameCtx);
            if (m_editorFrameCtx->state.animationPreviewActive && !sceneLoading)
            {
                m_animationPreviewDebugDraw.Clear();
                m_animationPreviewDebugDraw.SetViewProjection(clipAdjustment * animationPreviewView.projection * animationPreviewView.view);
                renderer::addons::editor::DrawAnimationPreviewGrid(*m_editorFrameCtx, m_animationPreviewDebugDraw);
                addons::debug_draw::AddDebugDrawCallback(animationPreviewCallbacks, m_animationPreviewDebugDraw);
            }
            if (!sceneLoading && drawEditorView)
                renderer::addons::editor::DrawLightDebugLines(*m_editorFrameCtx);
            measureAppCpu(3);
            if (drawEditorView)
                addons::debug_draw::AddDebugDrawCallback(editorCallbacks, m_debugDraw);

            // Partikel-Vorschau: nach dem Transform-Update simulieren, Pass nur bei Bedarf.
            // Standard wie Unity: nur Auswahl samt Kindern; Viewport-Schalter spielt alle ab.
            const auto& editorState = m_editorFrameCtx->state;
            if (sceneLoading)
                m_particles.Clear();
            else if (m_playActive)
            {
                // Play: Partikel kommen aus der Spielschleife (Update in GameSimulation::RunFrame).
            }
            else if (editorState.playAllParticles)
            {
                // Every scene emitter, but not the hidden copy of the animation window.
                m_particleSelection.clear();
                m_world->View<engine::ParticleEmitterComponent>([&](EntityID id, engine::ParticleEmitterComponent&) {
                    if (!m_world->Has<renderer::addons::editor::EditorRuntimeGizmoTag>(id))
                        m_particleSelection.insert(id);
                });
                m_particles.Update(*m_world, m_timing.GetDeltaSecondsF(), &m_particleSelection);
            }
            else
            {
                m_particleSelection.clear();
                std::vector<EntityID> pending(editorState.multiSelection.begin(), editorState.multiSelection.end());
                pending.push_back(editorState.selectedEntity);
                while (!pending.empty())
                {
                    const EntityID id = pending.back();
                    pending.pop_back();
                    if (!id.IsValid() || !m_world->IsAlive(id) || !m_particleSelection.insert(id).second)
                        continue;
                    if (const auto* children = m_world->Get<ChildrenComponent>(id))
                        pending.insert(pending.end(), children->children.begin(), children->children.end());
                }
                m_particles.Update(*m_world, m_timing.GetDeltaSecondsF(), &m_particleSelection);
            }
            // Waehrend Play kommen die Partikel aus der Spielschleife, sonst aus der Vorschau.
            particles::ParticleSystem& particleSource = m_playActive ? m_game.Particles() : m_particles;
            renderer::IDevice* particleDevice = m_renderLoop.GetRenderSystem().GetDevice();
            if (drawEditorView)
                m_particleView.Submit(particleSource, editorView, particleDevice, editorCallbacks);
            if (gameInPanel)
                m_gameParticleView.Submit(particleSource, view, particleDevice, frameCallbacks);

            // Emitters of the object shown in the animation window.
            if (editorState.animationPreviewActive && !sceneLoading)
            {
                m_animationParticleSelection.clear();
                std::vector<EntityID> pendingCopies{renderer::addons::editor::GetAnimationPreviewRoot()};
                while (!pendingCopies.empty())
                {
                    const EntityID id = pendingCopies.back();
                    pendingCopies.pop_back();
                    if (!id.IsValid() || !m_world->IsAlive(id) || !m_animationParticleSelection.insert(id).second)
                        continue;
                    if (const auto* children = m_world->Get<ChildrenComponent>(id))
                        pendingCopies.insert(pendingCopies.end(), children->children.begin(), children->children.end());
                }
                m_animationParticles.Update(*m_world, m_timing.GetDeltaSecondsF(), &m_animationParticleSelection);
                m_animationParticleView.Submit(m_animationParticles, animationPreviewView, particleDevice, animationPreviewCallbacks);
            }

            if (editorViewOffscreen)
            {
                renderer::OffscreenRenderRequest& request = offscreenRequests[offscreenRequestCount++];
                request.view = &editorOffscreenView;
                request.outputRT = m_sceneView.rt;
                request.outputTex = particleDevice
                    ? particleDevice->GetRenderTargetColorTexture(m_sceneView.rt)
                    : TextureHandle::Invalid();
                request.viewportWidth = m_sceneView.width;
                request.viewportHeight = m_sceneView.height;
                request.callbacks = &editorOffscreenCallbacks;
            }
        }

        // Reflection probe bake requested in the inspector: its six faces go
        // into this frame; ProbeCapture::Finish runs right after the frame.
        if (!m_probeBakeQueue.empty() && !m_probeCapture && m_probeRuntime && !sceneLoading)
        {
            const EntityID probeEntity = m_probeBakeQueue.front().first;
            const bool secondPass = m_probeBakeQueue.front().second == 2;
            m_probeBakeQueue.erase(m_probeBakeQueue.begin());
            m_probeBakeInFlight = probeEntity;
            const auto* probe = m_world->IsAlive(probeEntity) ? m_world->Get<ReflectionProbeComponent>(probeEntity) : nullptr;
            const auto* probeXf = probe ? m_world->Get<WorldTransformComponent>(probeEntity) : nullptr;
            renderer::IDevice* probeDevice = m_renderLoop.GetRenderSystem().GetDevice();
            if (probe && probeXf && probeDevice && !probe->bakedDataPath.empty() &&
                offscreenRequestCount + 6u <= offscreenRequests.size())
            {
                engine::addons::probes::ProbeBakeRequest bake{};
                bake.position = probeXf->position;
                bake.extents = probe->extents;
                bake.resolution = probe->resolution;
                bake.relativePath = probe->bakedDataPath;
                bake.lightWithProbes = secondPass;
                bake.inputHash = engine::addons::probes::ComputeProbeInputHash(*m_world, probeEntity);
                m_probeCapture = std::make_unique<engine::addons::probes::ProbeCapture>();
                std::string error;
                if (m_probeCapture->Begin(*probeDevice, *m_probeRuntime, editorView, bake, &error))
                {
                    for (const renderer::OffscreenRenderRequest& face : m_probeCapture->GetRequests())
                        offscreenRequests[offscreenRequestCount++] = face;
                }
                else
                {
                    Debug::LogError("KromEditorApp.cpp: probe capture failed: %s", error.c_str());
                    if (m_editorFrameCtx)
                        m_editorFrameCtx->lastFileMessage = Translate("probe.bake_failed") + error;
                    m_probeCapture.reset();
                }
            }
        }

        measureAppCpu(4);
        if (offscreenRequestCount > 0u)
            activeOffscreenRequests =
                std::span<const renderer::OffscreenRenderRequest>(offscreenRequests.data(), offscreenRequestCount);
#endif

        if (!m_renderLoop.Tick(*m_world, m_materialSystem, view, m_timing, frameCallbacks, activeOffscreenRequests))
        {
            if (m_renderLoop.ShouldExit())
                break;
            return -5;
        }
#ifdef KROM_EDITOR_HAS_IMGUI
        if (m_probeCapture && m_probeCapture->IsActive())
        {
            renderer::RenderSystem& probeRenderSystem = m_renderLoop.GetRenderSystem();
            std::string error;
            const bool baked = probeRenderSystem.GetDevice() &&
                m_probeCapture->Finish(*probeRenderSystem.GetDevice(), *m_probeRuntime,
                                       &probeRenderSystem.GetJobSystem(), &error);
            if (!baked)
                Debug::LogError("KromEditorApp.cpp: probe bake failed: %s", error.c_str());
            if (m_editorFrameCtx)
                m_editorFrameCtx->lastFileMessage = baked
                    ? Translate("probe.bake_done")
                    : Translate("probe.bake_failed") + error;
            m_probeCapture.reset();
            m_probeBakeInFlight = NULL_ENTITY;
        }
        // Detached editor windows (multi-viewports): drawn after the engine frame
        // was submitted and presented, so textures rendered this frame (material
        // preview, scene view) are complete and the engine's command list is closed.
        if (m_imguiFrameRendered)
        {
            m_imguiFrameRendered = false;
            if (ImGui::GetCurrentContext() &&
                (ImGui::GetIO().ConfigFlags & ImGuiConfigFlags_ViewportsEnable))
            {
                ImGui::UpdatePlatformWindows();
                ImGui::RenderPlatformWindowsDefault();
            }
        }
        if (m_editorFrameCtx)
        {
            measureAppCpu(5);
            m_editorFrameCtx->renderStats = &m_renderLoop.GetRenderSystem().GetStats();
            m_renderLoop.GetRenderSystem().SetGpuPassTimingEnabled(m_editorState.showGpuPassTiming);
            *m_tonemapSettings = m_editorState.tonemap;
            m_msaaSettings->sampleCount = m_editorState.msaaSamples;
            if (m_probeRuntime)
            {
                engine::addons::probes::ProbeSettings probeSettings = m_probeRuntime->GetSettings();
                probeSettings.specularOcclusion = m_editorState.specularOcclusion;
                probeSettings.debugView = static_cast<engine::addons::probes::ProbeDebugView>(m_editorState.probeDebugView);
                probeSettings.bakePasses = m_editorState.probeBakePasses;
                m_probeRuntime->SetSettings(probeSettings);
            }
            if (const renderer::IDevice* msaaDevice = m_renderLoop.GetRenderSystem().GetDevice())
                m_editorFrameCtx->msaaAvailable =
                    msaaDevice->GetShaderTargetProfile() != assets::ShaderTargetProfile::OpenGL_GLSL450;
            m_editorFrameCtx->gpuPassTimings = m_renderLoop.GetRenderSystem().GetGpuPassTimings();
            m_editorFrameCtx->frameData =
                m_renderLoop.GetRenderSystem().GetRenderSnapshot().GetFrameDataView();

            // Present the launcher before starting to prepare the project.
            if (!m_sceneLoadJob && !m_editorState.pendingStartProjectFile.empty() &&
                !LoadProjectFile(m_editorState.pendingStartProjectFile, true))
            {
                m_editorState.pendingStartProjectFile.clear();
                if (m_editorFrameCtx->lastFileMessage.empty())
                    m_editorFrameCtx->lastFileMessage = Translate("project.load_failed");
            }

            // Remember the open scene as soon as it changes, not only on a clean
            // shutdown, so the editor reopens it even after a crash or kill.
            // Play mode may switch scenes; those are not editor scenes.
            if (!m_sceneLoadJob && m_editorState.pendingStartProjectFile.empty() &&
                !m_playActive && !m_projectRoot.empty() &&
                m_currentSceneName != m_rememberedSceneName)
            {
                std::error_code ec;
                if (std::filesystem::is_regular_file(GetCurrentEditorScenePath(), ec) &&
                    SaveEditorSettings(m_projectRoot, m_editorFrameCtx->state, m_currentSceneName))
                    m_rememberedSceneName = m_currentSceneName;
            }
        }
#endif
    }

    return 0;
}

#ifdef KROM_EDITOR_HAS_IMGUI
void KromEditorApp::DestroyPanelTargetNoImGui(PanelTarget& target)
{
    if (target.rt.IsValid())
        if (renderer::IDevice* device = m_renderLoop.GetRenderSystem().GetDevice())
            device->DestroyRenderTarget(target.rt);
    target = PanelTarget{};
}

void KromEditorApp::ReleasePanelTarget(PanelTarget& target)
{
    renderer::IDevice* device = m_renderLoop.GetRenderSystem().GetDevice();
    if (!target.rt.IsValid() || !device)
    {
        target.rt = RenderTargetHandle::Invalid();
        target.textureId = nullptr;
        target.width = target.height = 0u;
        return;
    }
    // Das Ziel kann noch in einem laufenden Frame stecken (Groessenwechsel ist selten).
    device->WaitIdle();
#if defined(KROM_APP_BACKEND_VULKAN) && defined(KROM_IMGUI_HAS_VULKAN)
    if (target.textureId)
        ImGui_ImplVulkan_RemoveTexture(static_cast<VkDescriptorSet>(target.textureId));
#endif
    device->DestroyRenderTarget(target.rt);
    target.rt = RenderTargetHandle::Invalid();
    target.textureId = nullptr;
    target.width = target.height = 0u;
}

void KromEditorApp::UpdatePanelTarget(PanelTarget& target, uint32_t width, uint32_t height,
                                      const char* debugName)
{
    renderer::IDevice* device = m_renderLoop.GetRenderSystem().GetDevice();
    if (!device)
        return;
    // Verdeckter Reiter (0): Ziel behalten, damit beim Zurueckschalten nichts flimmert.
    if (width == 0u || height == 0u)
        return;
    if (target.rt.IsValid() && width == target.width && height == target.height)
    {
        target.stableFrames = 0u;
        return;
    }
    // Waehrend das Panel gezogen oder in der Groesse veraendert wird, nicht jeden Frame
    // neu anlegen (WaitIdle, Flimmern): erst wenn die Groesse ruht oder die Maus los ist.
    // Bis dahin zeigt das Panel das bisherige Bild skaliert.
    if (target.rt.IsValid())
    {
        if (width != target.pendingWidth || height != target.pendingHeight)
        {
            target.pendingWidth = width;
            target.pendingHeight = height;
            target.stableFrames = 0u;
        }
        const bool mouseHeld = ImGui::GetCurrentContext() && ImGui::GetIO().MouseDown[0];
        if (mouseHeld && ++target.stableFrames < 30u)
            return;
    }
    target.stableFrames = 0u;

    ReleasePanelTarget(target);

    renderer::RenderTargetDesc desc{};
    desc.width       = width;
    desc.height      = height;
    // Format wie der Backbuffer, fuer den die Tonemap-Pipeline gebaut ist; nur Farbe,
    // Tonemap schreibt ohne Tiefe (sonst Vulkan-Formatabweichung, VUID 08910/08914).
    desc.colorFormat = m_config.backend == renderer::DeviceFactory::BackendType::OpenGL
        ? renderer::Format::BGRA8_UNORM
        : renderer::Format::RGBA8_UNORM_SRGB;
    desc.hasDepth    = false;
    desc.debugName   = debugName;
    target.rt = device->CreateRenderTarget(desc);
    if (!target.rt.IsValid())
    {
        Debug::LogError("KromEditorApp: Panel-Ziel '%s' %ux%u konnte nicht angelegt werden",
                        debugName, width, height);
        return;
    }
    target.width = width;
    target.height = height;

    const TextureHandle colorTex = device->GetRenderTargetColorTexture(target.rt);
#if defined(KROM_APP_BACKEND_VULKAN) && defined(KROM_IMGUI_HAS_VULKAN)
    auto* vkDevice = static_cast<renderer::vulkan::VulkanDevice*>(device);
    if (m_previewSampler == UINT32_MAX)
    {
        renderer::SamplerDesc samplerDesc{};
        samplerDesc.minFilter = renderer::FilterMode::Linear;
        samplerDesc.magFilter = renderer::FilterMode::Linear;
        samplerDesc.mipFilter = renderer::FilterMode::Linear;
        samplerDesc.addressU = renderer::WrapMode::Clamp;
        samplerDesc.addressV = renderer::WrapMode::Clamp;
        samplerDesc.addressW = renderer::WrapMode::Clamp;
        m_previewSampler = vkDevice->CreateSampler(samplerDesc);
    }
    const auto* textureEntry = vkDevice->GetResources().textures.Get(colorTex);
    const VkSampler sampler = (m_previewSampler < vkDevice->GetResources().samplers.size())
        ? vkDevice->GetResources().samplers[m_previewSampler].sampler
        : VK_NULL_HANDLE;
    if (textureEntry && textureEntry->sampleView && sampler)
        target.textureId = ImGui_ImplVulkan_AddTexture(
            sampler, textureEntry->sampleView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
#else
    target.textureId = device->GetNativeTextureHandle(colorTex);
#endif
}

std::filesystem::path KromEditorApp::GetCurrentEditorScenePath() const
{
    // m_currentSceneName ist z.B. "Level1" → "Assets/Scenes/Level1.json"
    // Default "editor_scene" bewahrt Rückwärtskompatibilität mit alten Projekten.
    const std::string filename = m_currentSceneName.empty() ? "editor_scene" : m_currentSceneName;
    return m_projectRoot.empty()
        ? ResolveLegacyEditorScenePath()
        : (m_projectRoot / "Assets" / "Scenes" / PathFromUtf8(filename + ".json"));
}

std::filesystem::path KromEditorApp::GetEditorScenePath(const std::string& sceneName) const
{
    if (m_projectRoot.empty())
        return ResolveLegacyEditorScenePath().parent_path() / PathFromUtf8(sceneName + ".json");

    // Wenn sceneName bereits einen Slash enthält ist es ein relativer Pfad vom Projektroot
    // z.B. "Assets/Levels/Village" → projectRoot/Assets/Levels/Village.json
    if (sceneName.find('/') != std::string::npos || sceneName.find('\\') != std::string::npos)
    {
        std::filesystem::path p = m_projectRoot / PathFromUtf8(sceneName);
        if (p.extension().empty())
            p.replace_extension(".json");
        return p;
    }

    return m_projectRoot / "Assets" / "Scenes" / PathFromUtf8(sceneName + ".json");
}

std::filesystem::path KromEditorApp::GetCurrentProjectFilePath() const
{
    return m_projectRoot.empty()
        ? (ResolveProjectsRoot() / "krom-project.json")
        : (m_projectRoot / "krom-project.json");
}

bool KromEditorApp::SaveProjectFile(bool saveScene)
{
    const std::filesystem::path projectFilePath = GetCurrentProjectFilePath();
    const std::filesystem::path projectRoot = projectFilePath.parent_path();
    std::error_code ec;
    std::filesystem::create_directories(projectRoot, ec);
    if (ec)
    {
        if (m_editorFrameCtx)
            m_editorFrameCtx->lastFileMessage = Translate("project.folder_failed");
        return false;
    }

    const std::string effectiveName = !m_projectName.empty()
        ? m_projectName
        : PathToUtf8(projectRoot.filename());
    const std::string effectiveRuntimeTarget = !m_projectRuntimeTarget.empty()
        ? SanitizeRuntimeTargetName(m_projectRuntimeTarget)
        : SanitizeRuntimeTargetName(effectiveName);
    serialization::JsonWriter out;
    out.BeginObject();
    out.WriteString("name", effectiveName);
    out.WriteString("runtimeTarget", effectiveRuntimeTarget);
    out.WriteString("backend", ProjectBackendId(m_projectBackend));
    out.WriteString("assetRoot", "Assets");
    out.WriteString("shaderCache", "shader-bin");
    out.WriteString("editorScene", "Assets/Scenes/" + m_currentSceneName + ".json");
    out.BeginArray("sceneOrder");
    for (const auto& scene : m_sceneOrder)
        out.WriteString("", scene);
    out.EndArray();
    out.BeginArray("buildExclude");
    std::vector<std::string> excluded(m_buildExcludedScenes.begin(), m_buildExcludedScenes.end());
    std::sort(excluded.begin(), excluded.end());
    for (const auto& scene : excluded)
        out.WriteString("", scene);
    out.EndArray();
    WriteProjectSettings(out, m_projectSettings);
    out.WriteInt("version", 1);
    out.EndObject();
    if (!WriteTextFileAtomically(projectFilePath, out.GetString() + "\n"))
    {
        if (m_editorFrameCtx)
            m_editorFrameCtx->lastFileMessage = Translate("project.write_failed");
        return false;
    }

    if (!saveScene)
        return true;

    if (m_editorFrameCtx)
        SaveEditorSettings(m_projectRoot, m_editorFrameCtx->state, m_currentSceneName);

    const bool sceneSaved = SaveEditorScene();
    if (m_editorFrameCtx)
        m_editorFrameCtx->lastFileMessage = sceneSaved
            ? Translate("project.saved")
            : Translate("project.scene_save_failed");
    return sceneSaved;
}

bool KromEditorApp::LoadProjectFile(const std::string& projectPath, bool stageInitialScene)
{
    if (projectPath.empty())
    {
        if (m_editorFrameCtx)
            m_editorFrameCtx->lastFileMessage = Translate("project.path_missing");
        return false;
    }

    std::filesystem::path inputPath = PathFromUtf8(projectPath);
    std::filesystem::path projectFilePath = inputPath;
    if (std::filesystem::is_directory(inputPath))
        projectFilePath = inputPath / "krom-project.json";
    if (projectFilePath.filename() != "krom-project.json")
        projectFilePath /= "krom-project.json";

    std::ifstream in(projectFilePath, std::ios::binary);
    if (!in)
    {
        if (m_editorFrameCtx)
            m_editorFrameCtx->lastFileMessage = Translate("project.file_missing");
        return false;
    }

    const std::string json((std::istreambuf_iterator<char>(in)),
                           std::istreambuf_iterator<char>());
    std::string err;
    const serialization::JsonValue root = serialization::JsonParser::Parse(json, err);
    if (!err.empty() || !root.IsObject())
    {
        if (m_editorFrameCtx)
            m_editorFrameCtx->lastFileMessage = Translate("project.file_invalid");
        return false;
    }

    const std::filesystem::path projectRoot = projectFilePath.parent_path();
    const std::string name = root.Get("name") ? root.Get("name")->AsString() : PathToUtf8(projectRoot.filename());
    const std::string runtimeTarget = root.Get("runtimeTarget")
        ? root.Get("runtimeTarget")->AsString()
        : name;
    const std::string backendId = root.Get("backend") ? root.Get("backend")->AsString() : "vulkan";
    const renderer::DeviceFactory::BackendType backend = BackendFromProjectId(backendId);

    // Einstellungen des bisher offenen Projekts sichern, bevor sie ueberschrieben werden.
    if (m_editorFrameCtx && !m_projectRoot.empty() && m_projectRoot != projectRoot)
        SaveEditorSettings(m_projectRoot, m_editorFrameCtx->state, m_currentSceneName);

    StopEditorPlay();
#ifdef _WIN32
    m_scriptModule.Unload(m_world.get(), m_scriptRegistry);
#endif
    m_projectRoot = projectRoot;
    m_projectName = name;
    m_projectRuntimeTarget = SanitizeRuntimeTargetName(runtimeTarget);
    m_projectBackend = backend;
    m_projectSettings = ReadProjectSettings(root);
    m_sceneBackupElapsed = 0.f;
    if (m_editorFrameCtx)
        m_editorFrameCtx->projectSettings = m_projectSettings;

    (void)ReloadProjectScriptModule();

    // Editor preferences are restored after the initial scene has been loaded.

    // sceneOrder laden
    m_sceneOrder.clear();
    if (const auto* order = root.Get("sceneOrder"); order && order->IsArray())
        for (const auto& v : order->arrayVal)
            if (v.IsString() && !v.AsString().empty())
                m_sceneOrder.push_back(v.AsString());

    // buildExclude laden
    m_buildExcludedScenes.clear();
    if (const auto* excl = root.Get("buildExclude"); excl && excl->IsArray())
        for (const auto& v : excl->arrayVal)
            if (v.IsString() && !v.AsString().empty())
                m_buildExcludedScenes.insert(v.AsString());
    RefreshBuildExcludeState();

    // Start scene: the scene that was open when the editor last closed this
    // project (editor/editor.settings "lastScene"). Fallback: first existing
    // scene from sceneOrder, then the first scene file on disk.
    m_currentSceneName = "editor_scene";
    {
        const auto scenePath = [&](const std::string& name) {
            return projectRoot / "Assets" / "Scenes" / PathFromUtf8(name + ".json");
        };

        bool found = false;
        const std::string lastScene = ReadLastEditorScene(projectRoot);
        m_rememberedSceneName = lastScene;
        if (!lastScene.empty() && std::filesystem::exists(scenePath(lastScene)))
        {
            m_currentSceneName = lastScene;
            found = true;
        }
        for (const std::string& s : m_sceneOrder)
        {
            if (found)
                break;
            if (!s.empty() && std::filesystem::exists(scenePath(s)))
            {
                m_currentSceneName = s;
                found = true;
                break;
            }
        }

        if (!found)
        {
            std::vector<std::string> diskScenes;
            std::error_code ec2;
            for (const auto& entry : std::filesystem::directory_iterator(
                     projectRoot / "Assets" / "Scenes", ec2))
            {
                if (!ec2 && entry.path().extension() == ".json")
                    diskScenes.push_back(PathToUtf8(entry.path().stem()));
            }
            std::sort(diskScenes.begin(), diskScenes.end());
            if (!diskScenes.empty())
            {
                m_currentSceneName = diskScenes.front();
                found = true;
            }
        }

        if (found)
            Debug::LogVerbose("KromEditorApp: project load uses first scene '%s'.",
                       m_currentSceneName.c_str());
    }

    m_loadedSceneNames = { m_currentSceneName };

    ApplyEditorProjectPaths(!stageInitialScene);

    if (m_editorFrameCtx)
    {
        renderer::addons::editor::ClearEditorHistory(m_editorFrameCtx->state);
        m_editorFrameCtx->state.selectedEntity = NULL_ENTITY;

        // Material-Editor-State vollständig zurücksetzen. Ohne diesen Reset würde
        // selectedMaterialAssetPath vom alten Projekt übrig bleiben und in
        // AssignTexture (Priority-1-Check) jede Inspector-Texturzuweisung abfangen
        // und auf den falschen/alten Pfad umleiten. Außerdem würde ApplyMaterial­
        // AssetToMeshEntity veraltete in-memory Daten (selectedMaterialAssetData)
        // statt der Datei auf der Disk verwenden.
        auto& s = m_editorFrameCtx->state;
        s.selectedMaterialAssetPath.clear();
        s.selectedMaterialAssetNameDraft.clear();
        s.pendingOpenMaterialAssetPath.clear();
        s.queuedMaterialAssetPath.clear();
        s.selectedMaterialAssetLoaded    = false;
        s.selectedMaterialAssetDirty     = false;
        s.materialWindowOpen             = false;
        s.materialWindowFocusRequest     = false;
        s.selectedMaterialAssetData      = {};
        s.materialAssetLoadInFlight      = false;
        s.materialAssetLoadFuture        = {};

        std::snprintf(s.projectOpenPathBuffer.data(),
                      s.projectOpenPathBuffer.size(),
                      "%s",
                      PathToUtf8(projectRoot).c_str());
    }

    const auto initialScene = GetCurrentEditorScenePath();
    const bool hasSceneFile = std::filesystem::exists(initialScene);
    if (stageInitialScene && m_editorFrameCtx)
    {
        auto job = std::make_shared<SceneLoadJob>();
        job->name = m_currentSceneName;
        job->openingProject = true;
        job->hasSceneFile = hasSceneFile;
        job->textures = renderer::addons::editor::ListProjectTextures(*m_editorFrameCtx);
        job->stage = SceneLoadJob::Stage::Textures;
        m_sceneLoadJob = std::move(job);
        m_editorFrameCtx->sceneLoadActive = true;
        return true;
    }

    return FinishProjectLoad(hasSceneFile ? LoadEditorScene() : (SyncEditorSceneState(), true));
}

bool KromEditorApp::FinishProjectLoad(bool sceneLoaded)
{
    // Scene camera data supplies defaults; saved editor preferences take precedence.
    if (m_editorFrameCtx)
        LoadEditorSettings(m_projectRoot, m_editorFrameCtx->state);

    if (m_editorFrameCtx)
        m_editorFrameCtx->lastFileMessage = sceneLoaded
            ? Translate("project.loaded")
            : Translate("project.scene_load_failed");

    if (sceneLoaded) m_maximizeAfterProjectLoad = true;
    if (sceneLoaded)
    {
        RememberSavedSceneContent();
        renderer::addons::editor::RecordProjectOpened(m_projectRoot);
        if (m_config.visible) RememberProject(m_projectRoot / "krom-project.json");
    }
    return sceneLoaded;
}

void KromEditorApp::ApplyEditorProjectPaths(bool preloadTextures)
{
    if (!m_assetPipeline)
        return;

    // Pfad-Caches leeren, damit relative Asset-Pfade des neuen Projekts nicht
    // fälschlicherweise auf Handles aus einer vorherigen Session zeigen.
    m_assetRegistry.ClearPathCaches();
    // Asset stores keep the previous project's assets (handles may still be in
    // use). Their project-relative paths ("Models/rig.glb#mesh/0") would match the
    // same relative path in the new project when prefabs search the stores by
    // path, so forget them. Absolute paths (editor/engine assets) cannot collide.
    {
        const auto forget = [](auto& asset) {
            if (!asset.path.empty() && IsPortableAssetPath(asset.path))
                asset.path.clear();
        };
        m_assetRegistry.meshes.ForEach([&](auto, auto& asset) { forget(asset); });
        m_assetRegistry.materials.ForEach([&](auto, auto& asset) { forget(asset); });
        m_assetRegistry.skeletons.ForEach([&](auto, auto& asset) { forget(asset); });
        m_assetRegistry.animations.ForEach([&](auto, auto& asset) { forget(asset); });
    }

    // SharedMaterialBindings-Cache leeren: veraltete (materialPath, meshHandle)-
    // Einträge aus dem alten Projekt würden sonst die Material-Zuweisung überspringen.
    renderer::addons::editor::ClearSharedMaterialBindings();

    const std::filesystem::path assetRoot = GetCurrentAssetRoot();
    m_assetPipeline->SetAssetRoot(assetRoot);
    if (m_probeRuntime)
        m_probeRuntime->SetAssetRoot(PathToUtf8(assetRoot));
    if (!m_projectRoot.empty())
        renderer::addons::editor::CleanupThumbnailCache(m_projectRoot);
    renderer::ShaderCompiler::SetCacheDirectory(GetCurrentShaderCacheDir());
    renderer::ShaderCompiler::SetEngineAssetDirectory(ResolveAssetRoot());

    if (m_editorFrameCtx)
    {
        m_editorFrameCtx->currentProjectName = m_projectName;
        m_editorFrameCtx->currentProjectRoot = PathToUtf8(m_projectRoot);
        m_editorFrameCtx->projectBackend = m_projectBackend;
        // m_currentSceneName und m_loadedSceneNames wurden bereits vom Aufrufer gesetzt
        // (LoadProjectFile oder CreateEditorProject) — hier nicht überschreiben.
        RefreshSceneList();
    }

    m_assetBrowserState.initialized = false;
    m_assetBrowserState.currentDir = assetRoot;

    // Alle Texturen im Asset-Root vorladen damit der erste Klick auf "Textures"
    // sofort reagiert (kein Warten auf On-Demand-Import).
    if (preloadTextures && m_editorFrameCtx)
        renderer::addons::editor::PreloadAllTextures(
            *m_editorFrameCtx, m_assetBrowserState);
}

void KromEditorApp::ProcessEditorFileDrops()
{
    if (!m_editorFrameCtx)
        return;

    platform::IWindow* window = m_renderLoop.GetWindow();
    if (!window)
        return;

    const std::vector<std::filesystem::path> droppedFiles = window->ConsumeDroppedFiles();
    if (droppedFiles.empty())
        return;

    renderer::addons::editor::ImportExternalFilesToAssetBrowser(
        *m_editorFrameCtx,
        m_assetBrowserState,
        droppedFiles);
}

bool KromEditorApp::CreateEditorProject(const std::string& projectName,
                                        const std::string& parentDirectory,
                                        renderer::DeviceFactory::BackendType backend)
{
    const std::string sanitizedName = SanitizeProjectFolderName(projectName);
    if (projectName.empty() || sanitizedName.empty())
    {
        if (m_editorFrameCtx)
            m_editorFrameCtx->lastFileMessage = Translate("project.name_missing");
        return false;
    }
    if (parentDirectory.empty())
    {
        if (m_editorFrameCtx)
            m_editorFrameCtx->lastFileMessage = Translate("project.folder_missing");
        return false;
    }

    const std::filesystem::path parentPath = PathFromUtf8(parentDirectory);
    const std::filesystem::path projectRoot = parentPath / PathFromUtf8(sanitizedName);
    const std::filesystem::path assetRoot = projectRoot / "Assets";
    const std::filesystem::path sceneDir = assetRoot / "Scenes";
    const std::filesystem::path projectFilePath = projectRoot / "krom-project.json";
    const std::string runtimeTarget = SanitizeRuntimeTargetName(sanitizedName);

    std::error_code ec;
    std::filesystem::create_directories(sceneDir, ec);
    if (!ec) std::filesystem::create_directories(assetRoot / "Models", ec);
    if (!ec) std::filesystem::create_directories(assetRoot / "Materials", ec);
    if (!ec) std::filesystem::create_directories(assetRoot / "Textures", ec);
    if (!ec) std::filesystem::create_directories(assetRoot / "Scripts", ec);
    if (!ec) std::filesystem::create_directories(projectRoot / "shader-bin", ec);
    if (ec)
    {
        Debug::LogError("KromEditorApp: failed to create project directories: %s", ec.message().c_str());
        if (m_editorFrameCtx)
            m_editorFrameCtx->lastFileMessage = Translate("project.folders_failed");
        return false;
    }

    {
        std::ofstream out(projectFilePath, std::ios::binary | std::ios::trunc);
        if (!out)
        {
            Debug::LogError("KromEditorApp: failed to write project file: %s", PathToUtf8(projectFilePath).c_str());
            if (m_editorFrameCtx)
                m_editorFrameCtx->lastFileMessage = Translate("project.write_failed");
            return false;
        }

        out << "{\n"
            << "  \"name\":\"" << EscapeJsonString(projectName) << "\",\n"
            << "  \"runtimeTarget\":\"" << EscapeJsonString(runtimeTarget) << "\",\n"
            << "  \"backend\":\"" << ProjectBackendId(backend) << "\",\n"
            << "  \"assetRoot\":\"Assets\",\n"
            << "  \"shaderCache\":\"shader-bin\",\n"
            << "  \"editorScene\":\"Assets/Scenes/editor_scene.json\",\n"
            << "  \"version\":1\n"
            << "}\n";
        if (!out.good())
        {
            if (m_editorFrameCtx)
                m_editorFrameCtx->lastFileMessage = Translate("project.write_failed");
            return false;
        }
    }

    // Like LoadProjectFile: end Play first. Otherwise the play stash of the old
    // project survives the switch and Stop restores the old scene into the new one.
    StopEditorPlay();

    std::vector<EntityID> entities;
    if (m_world)
    {
        m_world->ForEachAlive([&](EntityID id) {
            entities.push_back(id);
        });
        for (EntityID id : entities)
            m_world->DestroyEntity(id);
    }

    m_projectRoot = projectRoot;
#ifdef _WIN32
    m_scriptModule.Unload(m_world.get(), m_scriptRegistry);
#endif
    m_projectName = projectName;
    m_projectRuntimeTarget = runtimeTarget;
    m_projectBackend = backend;
    m_projectSettings = {};
    m_sceneBackupElapsed = 0.f;
    if (m_editorFrameCtx)
        m_editorFrameCtx->projectSettings = m_projectSettings;
    m_buildExcludedScenes.clear();
    m_currentSceneName = "editor_scene";
    m_loadedSceneNames = { m_currentSceneName };
    m_sceneOrder       = { m_currentSceneName };
    RefreshBuildExcludeState();
    ApplyEditorProjectPaths();

    if (m_editorFrameCtx)
    {
        renderer::addons::editor::ClearEditorHistory(m_editorFrameCtx->state);
        m_editorFrameCtx->state.selectedEntity = NULL_ENTITY;
        ResetEditorLayerNames(m_editorFrameCtx->state);

        // Material-Editor-State zurücksetzen (identisch zu LoadProjectFile).
        auto& s = m_editorFrameCtx->state;
        s.selectedMaterialAssetPath.clear();
        s.selectedMaterialAssetNameDraft.clear();
        s.pendingOpenMaterialAssetPath.clear();
        s.queuedMaterialAssetPath.clear();
        s.selectedMaterialAssetLoaded    = false;
        s.selectedMaterialAssetDirty     = false;
        s.materialWindowOpen             = false;
        s.materialWindowFocusRequest     = false;
        s.selectedMaterialAssetData      = {};
        s.materialAssetLoadInFlight      = false;
        s.materialAssetLoadFuture        = {};
    }

    // Standard-Entities in die erste Szene einfügen (Kamera + Licht)
    CreateDefaultSceneEntities();
    ApplyDefaultProjectEnvironment(assetRoot);

    SyncEditorSceneState();
    RefreshSceneList();

    if (m_editorFrameCtx)
        m_editorFrameCtx->lastFileMessage = TranslateWithValue("project.created", PathToUtf8(projectRoot));

    if (m_editorFrameCtx)
        SaveEditorSettings(m_projectRoot, m_editorFrameCtx->state, m_currentSceneName);

    const bool saved = SaveEditorScene();
    if (saved)
    {
        m_maximizeAfterProjectLoad = true;
        renderer::addons::editor::RecordProjectOpened(projectRoot);
        if (m_config.visible) RememberProject(projectRoot / "krom-project.json");
    }
    return saved;
}

void KromEditorApp::SyncEditorSceneState(SceneSyncStep step)
{
    if (!m_world)
        return;

    if (step == SceneSyncStep::All || step == SceneSyncStep::Hierarchy)
    {
        std::vector<EntityID> entities;
        std::vector<std::pair<EntityID, EntityID>> parentLinks;
        std::unordered_map<uint32_t, uint32_t> nextSiblingOrder;
        m_world->ForEachAlive([&](EntityID id) {
            entities.push_back(id);
            if (const auto* parent = m_world->Get<ParentComponent>(id))
                parentLinks.emplace_back(id, parent->parent);
        });

        NormalizeEditorObjectLayerMasks(*m_world);

        for (EntityID id : entities)
        {
            if (m_world->Has<TransformComponent>(id) && !m_world->Has<WorldTransformComponent>(id))
                m_world->Add<WorldTransformComponent>(id);
            if (m_world->Has<ChildrenComponent>(id))
                m_world->Remove<ChildrenComponent>(id);
            if (!m_world->Has<renderer::addons::editor::EditorHierarchyComponent>(id))
            {
                const EntityID parent = m_world->Has<ParentComponent>(id)
                    ? m_world->Get<ParentComponent>(id)->parent
                    : NULL_ENTITY;
                const uint32_t parentKey = parent.IsValid() ? parent.value : NULL_ENTITY.value;
                const uint32_t order = nextSiblingOrder[parentKey]++;
                m_world->Add<renderer::addons::editor::EditorHierarchyComponent>(
                    id,
                    renderer::addons::editor::EditorHierarchyComponent{order});
            }
        }

        for (const auto& [child, parent] : parentLinks)
        {
            if (!parent.IsValid() || !m_world->IsAlive(parent))
                continue;
            if (auto* children = m_world->Get<ChildrenComponent>(parent))
                children->Add(child);
            else
                m_world->Add<ChildrenComponent>(parent).Add(child);
        }

    }

    // Mesh-Handles zuerst wiederherstellen — ResolveMaterialAssetBindings benoetigt
    // gueltige Mesh-Handles um Runtime-Materialien zu erstellen.
    if (step == SceneSyncStep::All || step == SceneSyncStep::Meshes)
    {
        if (m_assetPipeline)
            addons::mesh_renderer::ResolveMeshAssetBindings(*m_assetPipeline, m_assetRegistry, *m_world);

    // Kamera-Gizmo-Entities VOR ResolveMaterialAssetBindings erstellen:
    // EnsureCameraGizmos setzt MaterialComponent::materialAssetPath,
    // ResolveMaterialAssetBindings liest diesen Pfad und erstellt das GPU-Handle.
#ifdef KROM_EDITOR_HAS_IMGUI
    EnsureCameraGizmos();
#endif
    }

    // Material-Bindings nach den Meshes aufloesen — erfasst jetzt auch die
    // soeben erstellten Gizmo-Entities.
    if (m_editorFrameCtx && (step == SceneSyncStep::All || step == SceneSyncStep::Materials))
    {
        renderer::addons::editor::ResolveMaterialAssetBindings(*m_editorFrameCtx);
    }
    if (m_editorFrameCtx && (step == SceneSyncStep::All || step == SceneSyncStep::Prefabs))
    {
        engine::addons::editor::ResolvePrefabBindings(*m_editorFrameCtx);
    }

    if (step == SceneSyncStep::All || step == SceneSyncStep::Finalize)
    {
        mesh_renderer::UpdateLocalBoundsFromMeshes(*m_world, m_assetRegistry);
        m_transformSystem.Update(*m_world);
        m_boundsSystem.Update(*m_world);
    }
}

#ifdef KROM_EDITOR_HAS_IMGUI
void KromEditorApp::EnsureCameraGizmos()
{
    if (!m_world || !m_assetPipeline || !m_editorFrameCtx)
        return;

    using namespace renderer::addons::editor;

    // Absoluter Pfad zum editor/-Ordner.
    // ResolveAssetRoot() basiert auf __FILE__ → zeigt immer auf den Source-Baum,
    // unabhaengig davon ob ein Projekt geladen ist (GetCurrentAssetRoot koennte
    // auf m_projectRoot/Assets zeigen, was ein anderer Ordner ist).
    const std::filesystem::path editorDir =
        ResolveAssetRoot().parent_path() / "editor";
    const std::string kGlbPath = PathToGenericUtf8(editorDir / "camera.glb");
    const std::string kMatPath = PathToGenericUtf8(editorDir / "matcam.mat");
    const std::string kMeshKey = kGlbPath + "#mesh/0";

    // Schritt 1a: Legacy-Gizmos bereinigen — Entities die camera.glb als Mesh haben
    // aber kein EditorCameraGizmoComponent (wurden vor Einführung des Tags gespeichert).
    // Ohne diesen Schritt blockieren sie den Raycast und verhindern den Kamera-Redirect.
    {
        std::vector<EntityID> legacyGizmos;
        m_world->ForEachAlive([&](EntityID id) {
            if (m_world->Has<EditorCameraGizmoComponent>(id)) return; // bereits getaggt
            const auto* mc = m_world->Get<MeshComponent>(id);
            if (!mc) return;
            if (mc->meshAssetPath == kMeshKey)
                legacyGizmos.push_back(id);
        });
        for (EntityID id : legacyGizmos)
        {
            Debug::LogVerbose("EnsureCameraGizmos: Legacy-Gizmo %u entfernt", id.value);
            m_world->DestroyEntity(id);
        }
    }

    // Schritt 1b: Verwaiste Gizmos entfernen (Kamera wurde gelöscht)
    {
        std::vector<EntityID> orphans;
        m_world->ForEachAlive([&](EntityID id) {
            const auto* g = m_world->Get<EditorCameraGizmoComponent>(id);
            if (!g) return;
            if (!g->cameraEntity.IsValid() || !m_world->IsAlive(g->cameraEntity))
                orphans.push_back(id);
        });
        for (EntityID id : orphans)
            m_world->DestroyEntity(id);
    }

    // Schritt 2: Welche Kamera-Entities haben bereits ein Gizmo?
    std::unordered_set<uint32_t> alreadyGizmoed;
    m_world->ForEachAlive([&](EntityID id) {
        if (const auto* g = m_world->Get<EditorCameraGizmoComponent>(id))
            if (g->cameraEntity.IsValid())
                alreadyGizmoed.insert(g->cameraEntity.value);
    });

    // Schritt 3: Für jede CameraComponent-Entity ohne Gizmo → Gizmo erstellen
    // Nur Editor-Entities (haben EditorHierarchyComponent via EnsureEntityHierarchyOrder).
    // System-Kameras wie EditorScene::Build-Camera werden übersprungen.
    std::vector<EntityID> camerasNeedingGizmo;
    m_world->ForEachAlive([&](EntityID id) {
        if (!m_world->Has<CameraComponent>(id)) return;
        if (alreadyGizmoed.count(id.value)) return;
        if (!m_world->Has<renderer::addons::editor::EditorHierarchyComponent>(id)) return;
        camerasNeedingGizmo.push_back(id);
    });

    // Debug: zeige alle gefundenen Kamera-Entities
    for (EntityID id : camerasNeedingGizmo)
    {
        const auto* nc = m_world->Get<NameComponent>(id);
        Debug::LogVerbose("EnsureCameraGizmos: erstelle Gizmo fuer Kamera %u ('%s')",
                   id.value, nc ? nc->name.c_str() : "?");
    }

    if (camerasNeedingGizmo.empty())
        return;

    // Mesh aus Cache holen oder einmalig importieren
    if (!m_cameraGizmoMeshHandle.IsValid())
    {
        Debug::LogVerbose("KromEditorApp::EnsureCameraGizmos: Lade aus '%s'", PathToUtf8(editorDir).c_str());
        assets::ImportedAssetBundle bundle = m_assetPipeline->ImportBundle(kGlbPath);
        if (!bundle.Ok() || bundle.meshes.empty())
        {
            Debug::LogWarning("KromEditorApp::EnsureCameraGizmos: camera.glb nicht gefunden (%s) — Fehler: %s",
                              kGlbPath.c_str(), bundle.error.c_str());
            return;
        }
        Debug::LogVerbose("KromEditorApp::EnsureCameraGizmos: camera.glb geladen (%zu Submeshes)",
                   bundle.meshes[0].submeshes.size());
        // Tangenten generieren damit jedes Material-Template funktioniert
        // (PBR braucht sie, Unlit schadet es nicht — matcam.mat bleibt unberührt)
        for (assets::SubMeshData& sub : bundle.meshes[0].submeshes)
            assets::EnsureTangents(sub);
        auto meshAsset = std::make_unique<assets::MeshAsset>(std::move(bundle.meshes[0]));
        meshAsset->path               = kMeshKey;
        meshAsset->state              = assets::AssetState::Loaded;
        meshAsset->gpuStatus.dirty    = true;
        meshAsset->gpuStatus.uploaded = false;
        meshAsset->materialHandles.clear();
        m_cameraGizmoMeshHandle = m_assetRegistry.GetOrAddMesh(kMeshKey, std::move(meshAsset));
    }
    const MeshHandle meshHandle = m_cameraGizmoMeshHandle;

    for (EntityID cameraId : camerasNeedingGizmo)
    {
        EntityID gizmoId = m_world->CreateEntity();

        // Weltpose aus Kamera kopieren (Gizmo ist Top-Level, hat keinen Parent)
        auto& gt = m_world->Add<TransformComponent>(gizmoId);
        if (const auto* camW = m_world->Get<WorldTransformComponent>(cameraId))
        {
            gt.localPosition = camW->position;
            gt.localRotation = camW->rotation;
        }
        gt.localScale = {1.0f, 1.0f, 1.0f};
        gt.dirty = true;
        m_world->Add<WorldTransformComponent>(gizmoId);
        // BoundsComponent nötig damit RaycastTriangles den Gizmo trifft (Maus-Picking)
        m_world->Add<BoundsComponent>(gizmoId);

        // Gizmo-Tag ZUERST setzen — Entity ist ab sofort als reines Editor-Hilfsobjekt
        // markiert, bevor MeshComponent es sichtbar macht.
        m_world->Add<EditorRuntimeGizmoTag>(gizmoId);
        m_world->Add<EditorCameraGizmoComponent>(gizmoId,
            EditorCameraGizmoComponent{cameraId});

        // Mesh — LAYER_EDITOR_GIZMO_DEPTH: wird von Szenengeometrie verdeckt (normaler Z-Test)
        MeshComponent mc;
        mc.mesh           = meshHandle;
        mc.meshAssetPath  = kMeshKey;
        mc.castShadows    = false;
        mc.receiveShadows = false;
        mc.layerMask      = renderer::LAYER_EDITOR_GIZMO_DEPTH;
        m_world->Add<MeshComponent>(gizmoId, mc);

        // Material-Pfad setzen — ResolveMaterialAssetBindings (laeuft direkt nach
        // EnsureCameraGizmos) liest diesen Pfad und erstellt das GPU-Handle.
        MaterialComponent matc;
        matc.materialAssetPath = kMatPath;
        m_world->Add<MaterialComponent>(gizmoId, matc);

        Debug::LogVerbose("KromEditorApp: Kamera-Gizmo erstellt (camera=%u, gizmo=%u)",
                   cameraId.value, gizmoId.value);
    }

    m_assetPipeline->UploadPendingGpuAssets();

    // Material sofort auflösen — ResolveMaterialAssetBindings läuft nicht jeden Frame
    if (m_editorFrameCtx)
        renderer::addons::editor::ResolveMaterialAssetBindings(*m_editorFrameCtx);
}

void KromEditorApp::SyncCameraGizmoTransforms()
{
    if (!m_world) return;

    using namespace renderer::addons::editor;
    m_world->ForEachAlive([&](EntityID id) {
        const auto* gizmo = m_world->Get<EditorCameraGizmoComponent>(id);
        if (!gizmo || !m_world->IsAlive(gizmo->cameraEntity)) return;
        const auto* camW = m_world->Get<WorldTransformComponent>(gizmo->cameraEntity);
        if (!camW) return;
        auto* gizT = m_world->Get<TransformComponent>(id);
        if (!gizT) return;
        gizT->localPosition = camW->position;
        gizT->localRotation = camW->rotation;
        gizT->localScale = {1.0f, 1.0f, 1.0f};
        gizT->dirty = true;
    });
}

void KromEditorApp::EnsureMaterialPreviewEntity()
{
    if (!m_world || !m_editorFrameCtx)
        return;

    const bool previewOpen =
        m_editorFrameCtx->state.materialWindowOpen &&
        m_editorFrameCtx->state.selectedMaterialAssetLoaded &&
        !m_editorFrameCtx->state.selectedMaterialAssetPath.empty();
    if (!previewOpen)
        return;

    using namespace renderer::addons::editor;

    if (!m_world->IsAlive(m_materialPreviewGround))
    {
        const MeshHandle groundMesh = m_assetRegistry.GetOrAddMesh(kMaterialPreviewGroundPath,
                                                                  CreateMaterialPreviewGroundMesh());
        if (m_assetPipeline)
            m_assetPipeline->UploadPendingGpuAssets();

        const EntityID ground = m_world->CreateEntity();
        m_materialPreviewGround = ground;
        m_world->Add<NameComponent>(ground, NameComponent{"Editor Material Preview Ground"});
        m_world->Add<TransformComponent>(ground).localPosition.y = -1.02f;
        m_world->Add<WorldTransformComponent>(ground);
        m_world->Add<BoundsComponent>(ground).extentsLocal =
            {kMaterialPreviewGroundHalfSize, 0.f, kMaterialPreviewGroundHalfSize};
        m_world->Add<EditorMaterialPreviewComponent>(ground);
        auto& mesh = m_world->Add<MeshComponent>(ground, MeshComponent{groundMesh});
        mesh.meshAssetPath = kMaterialPreviewGroundPath;
        mesh.layerMask = renderer::LAYER_EDITOR_MATERIAL_PREVIEW;
        mesh.castShadows = false;
        ResolveMaterialAssetBindings(*m_editorFrameCtx, std::span{&ground, 1u});
    }

    if (!m_materialPreviewSphereMeshHandle.IsValid())
    {
        m_materialPreviewSphereMeshHandle =
            m_assetRegistry.GetOrAddMesh("__editor/material_preview_sphere",
                                         CreateMaterialPreviewSphereMesh());
        if (m_assetPipeline)
            m_assetPipeline->UploadPendingGpuAssets();
    }

    if (m_materialPreviewEntity.IsValid() && m_world->IsAlive(m_materialPreviewEntity))
        return;

    const EntityID id = m_world->CreateEntity();
    m_materialPreviewEntity = id;

    m_world->Add<NameComponent>(id, NameComponent{"Editor Material Preview"});

    m_world->Add<TransformComponent>(id);
    m_world->Add<WorldTransformComponent>(id);
    m_world->Add<BoundsComponent>(id);
    m_world->Add<EditorMaterialPreviewComponent>(id);

    MeshComponent mesh{};
    mesh.mesh = m_materialPreviewSphereMeshHandle;
    mesh.meshAssetPath = "__editor/material_preview_sphere";
    mesh.visible = true;
    mesh.castShadows = true;
    mesh.receiveShadows = false;
    mesh.layerMask = renderer::LAYER_EDITOR_MATERIAL_PREVIEW;
    m_world->Add<MeshComponent>(id, mesh);

    MaterialComponent material{};
    material.materialAssetPath = m_editorFrameCtx->state.selectedMaterialAssetPath;
    m_world->Add<MaterialComponent>(id, material);
}

void KromEditorApp::SyncMaterialPreviewEntity()
{
    if (!m_world || !m_editorFrameCtx)
        return;

    if (!m_materialPreviewEntity.IsValid() || !m_world->IsAlive(m_materialPreviewEntity))
        return;

    const bool previewOpen =
        m_editorFrameCtx->state.materialWindowOpen &&
        m_editorFrameCtx->state.selectedMaterialAssetLoaded &&
        !m_editorFrameCtx->state.selectedMaterialAssetPath.empty();

    if (auto* ground = m_world->Get<MeshComponent>(m_materialPreviewGround))
        ground->visible = previewOpen;

    if (auto* mesh = m_world->Get<MeshComponent>(m_materialPreviewEntity))
    {
        mesh->visible = previewOpen;
        mesh->layerMask = renderer::LAYER_EDITOR_MATERIAL_PREVIEW;
    }

    if (!previewOpen)
        return;

    if (auto* material = m_world->Get<MaterialComponent>(m_materialPreviewEntity))
        material->materialAssetPath = m_editorFrameCtx->state.selectedMaterialAssetPath;

    renderer::addons::editor::ApplyMaterialAssetToEntity(
        *m_editorFrameCtx,
        m_materialPreviewEntity,
        PathFromUtf8(m_editorFrameCtx->state.selectedMaterialAssetPath));
}

namespace {

math::Vec3 MaterialPreviewEyeOffset(const renderer::addons::editor::EditorState& state) noexcept
{
    const float distance = std::clamp(state.materialPreviewDistance, 2.0f, 8.0f);
    return math::Quat::FromEulerDeg(-state.materialPreviewPitchDeg, state.materialPreviewYawDeg, 0.f)
        .Rotate({0.f, 0.f, distance});
}

struct PreviewRigLight
{
    // Light position relative to the preview camera: x = camera right, y = camera up,
    // z = towards the camera (negative = behind the object). Only the direction matters.
    math::Vec3 cameraSpacePosition;
    math::Vec3 color;
    float      intensity;
};

// Classic three-point setup: warm key from upper left, cool fill from the right,
// neutral rim from behind to separate the silhouette from the background.
const std::array<PreviewRigLight, 3> kPreviewLightRig{{
    {{-0.6f, 0.8f,  0.7f}, {1.00f, 0.96f, 0.90f}, 2.6f},
    {{ 0.9f, 0.1f,  0.5f}, {0.80f, 0.88f, 1.00f}, 0.8f},
    {{ 0.2f, 0.7f, -0.9f}, {1.00f, 1.00f, 1.00f}, 1.6f},
}};

// One white light from upper left, behind the material sphere.
const std::array<PreviewRigLight, 1> kMaterialPreviewLightRig{{
    {{-0.8f, 1.0f, -0.55f}, {1.f, 1.f, 1.f}, 3.3f},
}};

// Rotation that turns the light's local forward (0,0,-1) into `direction`.
math::Quat RotationFromLightForward(const math::Vec3& direction) noexcept
{
    const math::Vec3 from{0.f, 0.f, -1.f};
    const math::Vec3 to = direction.Normalized();
    const float d = math::Vec3::Dot(from, to);
    if (d < -0.9999f)
        return math::Quat::FromAxisAngleDeg(math::Vec3::Up(), 180.f);
    const math::Vec3 c = math::Vec3::Cross(from, to);
    return math::Quat{c.x, c.y, c.z, 1.f + d}.Normalized();
}

void SyncPreviewLightRig(ecs::World& world,
                         std::span<EntityID> rig,
                         uint32_t lightLayerBit,
                         const char* namePrefix,
                         const math::Vec3& cameraForward,
                         std::span<const PreviewRigLight> profile = kPreviewLightRig)
{
    using namespace renderer::addons::editor;
    const math::Vec3 forward = cameraForward.Normalized();
    math::Vec3 right = math::Vec3::Cross(forward, math::Vec3::Up());
    if (right.LengthSq() < 1e-6f)
        right = math::Vec3::Right();
    right = right.Normalized();
    const math::Vec3 up = math::Vec3::Cross(right, forward).Normalized();
    const math::Vec3 towardsCamera = -forward;

    for (size_t i = 0; i < rig.size(); ++i)
    {
        const bool enabled = i < profile.size();
        EntityID& id = rig[i];
        if (!id.IsValid() || !world.IsAlive(id))
        {
            if (!enabled)
                continue;

            id = world.CreateEntity();
            world.Add<NameComponent>(id, NameComponent{std::string(namePrefix) + " Light " + std::to_string(i)});
            world.Add<TransformComponent>(id);
            world.Add<WorldTransformComponent>(id);
            world.Add<EditorPreviewLightComponent>(id);
            world.Add<ActiveComponent>(id);

            LightComponent light{};
            light.type = LightType::Directional;
            light.range = 100.f;
            if (lightLayerBit == renderer::LAYER_EDITOR_MATERIAL_PREVIEW)
            {
                light.castShadows = true;
                light.shadowSettings.enabled = true;
                light.shadowSettings.resolution = 512u;
                light.shadowSettings.maxDistance = 12.f;
            }
            // The matching preview selects this layer and scene views exclude it.
            light.layerMask = lightLayerBit;
            world.Add<LightComponent>(id, light);
        }

        // Asset thumbnails share the rig between materials and geometry.
        world.Get<ActiveComponent>(id)->active = enabled;
        if (!enabled)
            continue;
        auto* transform = world.Get<TransformComponent>(id);
        auto* light = world.Get<LightComponent>(id);
        if (!transform || !light)
            continue;
        const math::Vec3& p = profile[i].cameraSpacePosition;
        const math::Vec3 lightPos = right * p.x + up * p.y + towardsCamera * p.z;
        transform->localRotation = RotationFromLightForward(-lightPos);
        transform->dirty = true;
        light->color = profile[i].color;
        light->intensity = profile[i].intensity;
    }
}

} // namespace

void KromEditorApp::SyncPreviewLightRigs()
{
    if (!m_world || !m_editorFrameCtx)
        return;

    const auto& state = m_editorFrameCtx->state;

    const bool materialPreviewOpen = state.materialWindowOpen && state.selectedMaterialAssetLoaded;
    if (materialPreviewOpen)
    {
        // Anchor the light and its shadow to the stage at the initial viewing angle.
        static const math::Vec3 stageForward =
            math::Quat::FromEulerDeg(-2.5f, 25.f, 0.f).Rotate({0.f, 0.f, -1.f});
        SyncPreviewLightRig(*m_world, m_materialPreviewLights,
                            renderer::LAYER_EDITOR_MATERIAL_PREVIEW, "Editor Material Preview",
                            stageForward, kMaterialPreviewLightRig);
    }

    // Prefab mode and model view clear the scene (and with it every scene light), so the
    // editor viewport gets the prefab rig there. It follows the editor camera, so the side
    // facing the viewer is always lit.
    const bool isolatedMode = renderer::addons::editor::IsInPrefabMode(state);
    const bool prefabPreviewOpen = state.prefabEditorOpen && state.prefabPreviewRootEntity.IsValid();
    if (isolatedMode)
    {
        const math::Vec3 forward =
            math::Quat::FromEulerDeg(state.editorCamera.pitchDeg, state.editorCamera.yawDeg, 0.f)
                .Rotate({0.f, 0.f, -1.f});
        SyncPreviewLightRig(*m_world, m_prefabPreviewLights,
                            renderer::LAYER_EDITOR_PREFAB_PREVIEW, "Editor Prefab Preview", forward);
    }
    else if (prefabPreviewOpen)
    {
        // The prefab camera orbits, so the rig follows it and the lit side always faces the viewer.
        const float pitch = state.prefabPreviewPitchDeg * math::DEG_TO_RAD;
        const float yaw   = state.prefabPreviewYawDeg   * math::DEG_TO_RAD;
        const math::Vec3 eyeOffset{std::cos(pitch) * std::sin(yaw),
                                   std::sin(pitch),
                                   std::cos(pitch) * std::cos(yaw)};
        SyncPreviewLightRig(*m_world, m_prefabPreviewLights,
                            renderer::LAYER_EDITOR_PREFAB_PREVIEW, "Editor Prefab Preview", -eyeOffset);
    }

    if (state.animationPreviewActive)
        SyncPreviewLightRig(*m_world, m_animationPreviewLights,
                            renderer::LAYER_EDITOR_ANIMATION_PREVIEW, "Editor Animation Preview",
                            state.animationPreviewTarget - state.animationPreviewEye);

    if (m_activeAssetThumbnailRT.IsValid())
    {
        const std::span<const PreviewRigLight> profile = m_activeAssetThumbnailIsMaterial
            ? std::span<const PreviewRigLight>{kMaterialPreviewLightRig}
            : std::span<const PreviewRigLight>{kPreviewLightRig};
        SyncPreviewLightRig(*m_world, m_assetThumbnailLights,
                            renderer::LAYER_EDITOR_ASSET_THUMBNAIL, "Editor Asset Thumbnail",
                            -m_activeAssetThumbnailEyeOffset, profile);
    }
}

bool KromEditorApp::BuildMaterialPreviewRenderView(renderer::RenderView& outView) const noexcept
{
    if (!m_editorFrameCtx ||
        !m_editorFrameCtx->state.materialWindowOpen ||
        !m_editorFrameCtx->state.selectedMaterialAssetLoaded ||
        !m_materialPreviewEntity.IsValid() ||
        !m_world ||
        !m_world->IsAlive(m_materialPreviewEntity))
        return false;

    const math::Vec3 eye = MaterialPreviewEyeOffset(m_editorFrameCtx->state);
    const math::Vec3 target{0.f, 0.f, 0.f};
    outView.view = math::Mat4::LookAtRH(eye, target, math::Vec3::Up());
    outView.projection = math::Mat4::PerspectiveFovRH(35.0f * math::DEG_TO_RAD, 1.0f, 0.05f, 20.0f);
    outView.cameraPosition = eye;
    outView.cameraForward = (target - eye).Normalized();
    outView.nearPlane = 0.05f;
    outView.farPlane = 20.0f;
    outView.ambientColor = {0.22f, 0.22f, 0.24f};
    outView.ambientIntensity = 1.8f;
    outView.debugFlags = 0u;
    outView.visibilityLayerMask = renderer::LAYER_EDITOR_MATERIAL_PREVIEW;
    // Only the material preview light rig (see SyncPreviewLightRigs); scene lights carry
    // other bits and are rejected by the exclude mask.
    outView.lightLayerMask = renderer::LAYER_EDITOR_MATERIAL_PREVIEW;
    outView.lightLayerExcludeMask = ~renderer::LAYER_EDITOR_MATERIAL_PREVIEW;
    outView.backgroundMode = BackgroundMode::Skybox;
    outView.enableBloom = false;
    outView.enableShadows = true;
    outView.enableAmbientOcclusion = false;
    outView.clearColor = {0.f, 0.f, 0.f, 1.f};
    return true;
}

bool KromEditorApp::BuildAnimationPreviewRenderView(renderer::RenderView& outView) const noexcept
{
    if (!m_editorFrameCtx || !m_editorFrameCtx->state.animationPreviewActive)
        return false;
    const auto& state = m_editorFrameCtx->state;
    const math::Vec3 eye = state.animationPreviewEye;
    const math::Vec3 target = state.animationPreviewTarget;
    outView.view = math::Mat4::LookAtRH(eye, target, math::Vec3::Up());
    outView.projection = math::Mat4::PerspectiveFovRH(state.animationPreviewFovDeg * math::DEG_TO_RAD,
                                                      state.animationPreviewAspect, state.animationPreviewNear,
                                                      state.animationPreviewFar);
    outView.cameraPosition = eye;
    outView.cameraForward = (target - eye).Normalized();
    outView.nearPlane = state.animationPreviewNear;
    outView.farPlane = state.animationPreviewFar;
    outView.ambientColor = {0.22f, 0.22f, 0.24f};
    outView.ambientIntensity = 1.8f;
    outView.debugFlags = 0u;
    // Gizmo meshes live in world space: while the viewport owns them, those of the
    // selected scene object would show up in the preview too (the copy stands at
    // the same place), sized for the viewport camera.
    outView.visibilityLayerMask = renderer::LAYER_EDITOR_ANIMATION_PREVIEW |
        (renderer::addons::editor::AnimationPreviewOwnsGizmos() ? renderer::LAYER_EDITOR_GIZMO : 0u);
    outView.lightLayerMask = renderer::LAYER_EDITOR_ANIMATION_PREVIEW;
    outView.lightLayerExcludeMask = ~renderer::LAYER_EDITOR_ANIMATION_PREVIEW;
    outView.backgroundMode = BackgroundMode::ClearColor;
    outView.enableBloom = false;
    outView.enableShadows = false;
    outView.enableAmbientOcclusion = false;
    outView.enableSelectionOutline = false;
    outView.clearColor = {0.10f, 0.11f, 0.13f, 1.f};
    return true;
}

bool KromEditorApp::BuildPrefabPreviewRenderView(renderer::RenderView& outView) const noexcept
{
    if (!m_editorFrameCtx ||
        !m_editorFrameCtx->state.prefabEditorOpen ||
        !m_editorFrameCtx->state.prefabPreviewRootEntity.IsValid() ||
        !m_world ||
        !m_world->IsAlive(m_editorFrameCtx->state.prefabPreviewRootEntity))
        return false;

    const float pitch    = m_editorFrameCtx->state.prefabPreviewPitchDeg * math::DEG_TO_RAD;
    const float yaw      = m_editorFrameCtx->state.prefabPreviewYawDeg   * math::DEG_TO_RAD;
    const float radius   = m_editorFrameCtx->state.prefabPreviewBoundsRadius;
    const float distance = std::clamp(m_editorFrameCtx->state.prefabPreviewDistance,
                                       radius * 0.3f, radius * 20.f);

    // Orbit-Kamera um das berechnete Bounding-Box-Zentrum
    const math::Vec3 target = m_editorFrameCtx->state.prefabPreviewCenter;
    const float cosP = std::cos(pitch);
    const math::Vec3 offset{
        distance * cosP * std::sin(yaw),
        distance * std::sin(pitch),
        distance * cosP * std::cos(yaw)
    };
    const math::Vec3 eye = target + offset;

    const float nearPlane = std::max(distance * 0.01f, 0.01f);
    const float farPlane  = std::max(distance * 100.f,  10.f);

    outView.view             = math::Mat4::LookAtRH(eye, target, math::Vec3::Up());
    outView.projection       = math::Mat4::PerspectiveFovRH(45.f * math::DEG_TO_RAD, 1.f, nearPlane, farPlane);
    outView.cameraPosition   = eye;
    outView.cameraForward    = (target - eye).Normalized();
    outView.nearPlane        = nearPlane;
    outView.farPlane         = farPlane;
    outView.ambientColor     = {0.22f, 0.22f, 0.24f};
    outView.ambientIntensity = 1.8f;
    outView.debugFlags       = 0u;
    outView.visibilityLayerMask = renderer::LAYER_EDITOR_PREFAB_PREVIEW;
    // Only the prefab preview light rig (see SyncPreviewLightRigs).
    outView.lightLayerMask = renderer::LAYER_EDITOR_PREFAB_PREVIEW;
    outView.lightLayerExcludeMask = ~renderer::LAYER_EDITOR_PREFAB_PREVIEW;
    outView.backgroundMode = BackgroundMode::Skybox;
    outView.enableBloom    = false;
    outView.enableShadows  = false;
    outView.enableAmbientOcclusion = false;
    outView.clearColor     = {0.08f, 0.08f, 0.1f, 1.f};
    return true;
}

void KromEditorApp::ProcessAssetThumbnailQueue()
{
    if (!m_world || !m_editorFrameCtx || !m_editorFrameCtx->state.assetBrowser)
        return;

    using namespace renderer::addons::editor;
    AssetBrowserState& browser = *m_editorFrameCtx->state.assetBrowser;
    renderer::IDevice* cacheDevice = m_renderLoop.GetRenderSystem().GetDevice();

    // Pfad relativ zu <Projekt>/Assets - so heissen die Bilder im Platten-Cache.
    const auto assetRelPath = [this](const std::string& key) -> std::string {
        std::error_code ec;
        const std::filesystem::path rel =
            std::filesystem::relative(PathFromUtf8(key), m_projectRoot / "Assets", ec);
        return (ec || rel.empty()) ? std::string{} : PathToGenericUtf8(rel);
    };

    // Im letzten Frame gerenderte Vorschaubilder zuruecklesen und speichern
    // (verkleinert auf die Haelfte, angezeigt werden 72 px).
    if (cacheDevice && !m_projectRoot.empty())
    {
        for (const std::string& key : m_pendingThumbnailSaves)
        {
            const auto it = browser.assetPreviewThumbnails.find(key);
            if (it == browser.assetPreviewThumbnails.end() ||
                it->second.status != AssetBrowserState::PreviewStatus::Ready || !it->second.gpuTexture.IsValid())
            {
                continue;
            }
            std::vector<uint8_t> pixels;
            uint32_t width = 0u;
            uint32_t height = 0u;
            const std::string rel = assetRelPath(key);
            if (!rel.empty() && cacheDevice->ReadTexturePixels(it->second.gpuTexture, pixels, width, height))
            {
                const std::vector<uint8_t> small = DownscaleRgba(pixels, width, height, 2u);
                SaveCachedThumbnail(m_projectRoot, rel, it->second.fileStamp, width / 2u, height / 2u, small);
            }
        }
    }
    m_pendingThumbnailSaves.clear();

    // Wartende Vorschaubilder zuerst im Platten-Cache suchen (schnell, mehrere pro Frame).
    if (cacheDevice && !m_projectRoot.empty() && m_editorFrameCtx->editorTextureId)
    {
        const renderer::Format cacheFormat = m_config.backend == renderer::DeviceFactory::BackendType::OpenGL
            ? renderer::Format::RGBA8_UNORM
            : renderer::Format::RGBA8_UNORM_SRGB;
        uint32_t loadedThisFrame = 0u;
        for (auto& [key, thumb] : browser.assetPreviewThumbnails)
        {
            if (thumb.status != AssetBrowserState::PreviewStatus::Queued || loadedThisFrame >= 16u)
                continue;
            std::vector<uint8_t> pixels;
            uint32_t width = 0u;
            uint32_t height = 0u;
            const std::string rel = assetRelPath(key);
            if (rel.empty() || !LoadCachedThumbnail(m_projectRoot, rel, thumb.fileStamp, width, height, pixels))
                continue;
            if (!thumb.diskTexture.IsValid())
            {
                renderer::TextureDesc desc{};
                desc.width = width;
                desc.height = height;
                desc.mipLevels = 1u;
                desc.format = cacheFormat;
                desc.usage = renderer::ResourceUsage::ShaderResource | renderer::ResourceUsage::CopyDest;
                thumb.diskTexture = cacheDevice->CreateTexture(desc);
            }
            if (!thumb.diskTexture.IsValid())
                continue;
            cacheDevice->UploadTextureData(thumb.diskTexture, pixels.data(), pixels.size());
            thumb.imguiId = m_editorFrameCtx->editorTextureId(thumb.diskTexture);
            thumb.status = thumb.imguiId ? AssetBrowserState::PreviewStatus::Ready
                                         : AssetBrowserState::PreviewStatus::Queued;
            ++loadedThisFrame;
        }
    }

    auto queuedIt = browser.assetPreviewThumbnails.end();
    for (auto it = browser.assetPreviewThumbnails.begin(); it != browser.assetPreviewThumbnails.end(); ++it)
    {
        if (it->second.status == AssetBrowserState::PreviewStatus::Queued)
        {
            queuedIt = it;
            break;
        }
    }
    if (queuedIt == browser.assetPreviewThumbnails.end())
        return;

    for (EntityID entity : m_assetThumbnailEntities)
        if (entity.IsValid() && m_world->IsAlive(entity))
            m_world->DestroyEntity(entity);
    m_assetThumbnailEntities.clear();
    m_activeAssetThumbnailRT = RenderTargetHandle::Invalid();
    m_activeAssetThumbnailPath.clear();

    AssetBrowserState::AssetPreviewThumbnail& thumb = queuedIt->second;
    const std::filesystem::path path = PathFromUtf8(queuedIt->first);
    if (!std::filesystem::exists(path))
    {
        thumb.status = AssetBrowserState::PreviewStatus::Failed;
        return;
    }

    renderer::IDevice* device = m_renderLoop.GetRenderSystem().GetDevice();
    if (!device || !m_editorFrameCtx->editorTextureId)
    {
        thumb.status = AssetBrowserState::PreviewStatus::Failed;
        return;
    }

    if (!thumb.renderTarget.IsValid())
    {
        renderer::RenderTargetDesc desc{};
        desc.width = 160u;
        desc.height = 160u;
        desc.colorFormat = m_config.backend == renderer::DeviceFactory::BackendType::OpenGL
            ? renderer::Format::BGRA8_UNORM
            : renderer::Format::BGRA8_UNORM_SRGB;
        desc.hasDepth = true;
        desc.depthFormat = renderer::Format::D24_UNORM_S8_UINT;
        thumb.renderTarget = device->CreateRenderTarget(desc);
        thumb.gpuTexture = thumb.renderTarget.IsValid()
            ? device->GetRenderTargetColorTexture(thumb.renderTarget)
            : TextureHandle::Invalid();
    }
    if (!thumb.renderTarget.IsValid() || !thumb.gpuTexture.IsValid())
    {
        thumb.status = AssetBrowserState::PreviewStatus::Failed;
        return;
    }
    thumb.imguiId = m_editorFrameCtx->editorTextureId(thumb.gpuTexture);
    if (!thumb.imguiId)
    {
        thumb.status = AssetBrowserState::PreviewStatus::Failed;
        return;
    }

    bool createdPreview = false;
    EntityID previewRoot = NULL_ENTITY;
    const bool isMaterial = thumb.kind == AssetBrowserState::PreviewKind::Material;
    if (isMaterial)
    {
        if (!m_materialPreviewSphereMeshHandle.IsValid())
        {
            m_materialPreviewSphereMeshHandle =
                m_assetRegistry.GetOrAddMesh("__editor/material_preview_sphere",
                                             CreateMaterialPreviewSphereMesh());
            if (m_assetPipeline)
                m_assetPipeline->UploadPendingGpuAssets();
        }

        EntityID entity = m_world->CreateEntity();
        m_assetThumbnailEntities.push_back(entity);
        m_world->Add<NameComponent>(entity, NameComponent{"Editor Asset Material Thumbnail"});
        auto& transform = m_world->Add<TransformComponent>(entity);
        transform.localPosition = {0.f, 0.f, 0.f};
        transform.localRotation = math::Quat::FromEulerDeg(0.f, -25.f, 0.f);
        transform.localScale = {1.f, 1.f, 1.f};
        transform.dirty = true;
        m_world->Add<WorldTransformComponent>(entity);
        m_world->Add<BoundsComponent>(entity);
        m_world->Add<EditorAssetThumbnailComponent>(entity);

        MeshComponent mesh{};
        mesh.mesh = m_materialPreviewSphereMeshHandle;
        mesh.meshAssetPath = "__editor/material_preview_sphere";
        mesh.visible = true;
        mesh.castShadows = false;
        mesh.receiveShadows = false;
        mesh.layerMask = renderer::LAYER_EDITOR_ASSET_THUMBNAIL;
        m_world->Add<MeshComponent>(entity, mesh);

        MaterialComponent material{};
        material.materialAssetPath = PathToUtf8(path);
        m_world->Add<MaterialComponent>(entity, material);
        ApplyMaterialAssetToEntity(*m_editorFrameCtx, entity, path);
        m_activeAssetThumbnailCenter = {0.f, 0.f, 0.f};
        m_activeAssetThumbnailEyeOffset = {0.f, 0.12f, 1.35f / std::tan(kAssetThumbnailFov * 0.5f)};
        createdPreview = true;
    }
    else if (thumb.kind == AssetBrowserState::PreviewKind::Model)
    {
        if (!m_assetPipeline)
        {
            thumb.status = AssetBrowserState::PreviewStatus::Failed;
            return;
        }

        assets::ImportedAssetBundle bundle = m_assetPipeline->ImportBundle(PathToUtf8(path));
        if (!bundle.Ok() || bundle.meshes.empty())
        {
            thumb.status = AssetBrowserState::PreviewStatus::Failed;
            return;
        }

        for (assets::MaterialAsset& material : bundle.materials)
        {
            material.baseColorFactor = {1.f, 1.f, 1.f, 1.f};
            material.emissiveFactor = {0.f, 0.f, 0.f};
            material.metallicFactor = 0.f;
            material.roughnessFactor = 1.f;
            material.normalScale = 0.f;
            material.occlusionStrength = 1.f;
            material.alphaMode = assets::MaterialAlphaMode::Opaque;
            material.transparent = false;
            material.doubleSided = true;
            material.castShadows = false;
            material.baseColorTexture.path.clear();
            material.normalTexture.path.clear();
            material.metallicRoughnessTexture.path.clear();
            material.emissiveTexture.path.clear();
            material.occlusionTexture.path.clear();
        }

        auto prefab = engine::addons::prefab::BuildPrefabFromImportedBundle(
            std::move(bundle),
            m_assetRegistry,
            engine::addons::prefab::PrefabBuildOptions{PathToUtf8(path.stem()), true, false, false});
        engine::addons::prefab::PrefabInstantiateOptions options{};
        const engine::addons::prefab::PrefabInstance instance =
            engine::addons::prefab::InstantiatePrefab(*m_world, prefab, options);
        if (!instance.IsValid())
        {
            thumb.status = AssetBrowserState::PreviewStatus::Failed;
            return;
        }
        for (EntityID entity : instance.entities)
        {
            m_assetThumbnailEntities.push_back(entity);
            if (!m_world->Has<EditorAssetThumbnailComponent>(entity))
                m_world->Add<EditorAssetThumbnailComponent>(entity);
            if (auto* mesh = m_world->Get<MeshComponent>(entity))
            {
                mesh->visible = true;
                mesh->castShadows = false;
                mesh->receiveShadows = false;
                mesh->layerMask = renderer::LAYER_EDITOR_ASSET_THUMBNAIL;
                AssignThumbnailWhiteMaterialToEntity(*m_editorFrameCtx, entity);
            }
        }
        if (m_assetPipeline)
            m_assetPipeline->UploadPendingGpuAssets();
        previewRoot = instance.root;
        createdPreview = true;
    }
    else if (thumb.kind == AssetBrowserState::PreviewKind::Prefab)
    {
        engine::addons::prefab::PrefabAsset prefab;
        std::string error;
        if (!engine::addons::prefab::LoadPrefabFromFile(path, m_assetRegistry, prefab, &error) ||
            prefab.Empty())
        {
            thumb.status = AssetBrowserState::PreviewStatus::Failed;
            return;
        }

        // Static thumbnails share the scene world and survive in-game scene changes.
        for (auto& record : prefab.records)
            record.particleEmitter.reset();

        engine::addons::prefab::PrefabInstantiateOptions options{};
        const engine::addons::prefab::PrefabInstance instance =
            engine::addons::prefab::InstantiatePrefab(*m_world, prefab, options);
        if (!instance.IsValid())
        {
            thumb.status = AssetBrowserState::PreviewStatus::Failed;
            return;
        }

        for (EntityID entity : instance.entities)
        {
            m_assetThumbnailEntities.push_back(entity);
            if (!m_world->Has<EditorAssetThumbnailComponent>(entity))
                m_world->Add<EditorAssetThumbnailComponent>(entity);
            if (auto* mesh = m_world->Get<MeshComponent>(entity))
            {
                mesh->visible = true;
                mesh->castShadows = false;
                mesh->receiveShadows = false;
                mesh->layerMask = renderer::LAYER_EDITOR_ASSET_THUMBNAIL;
            }
        }

        if (m_assetPipeline)
            engine::addons::mesh_renderer::ResolveMeshAssetBindings(
                *m_assetPipeline, m_assetRegistry, *m_world);
        if (m_editorFrameCtx)
            renderer::addons::editor::ResolveMaterialAssetBindings(*m_editorFrameCtx);
        if (m_assetPipeline)
            m_assetPipeline->UploadPendingGpuAssets();

        previewRoot = instance.root;
        createdPreview = true;
    }

    if (!createdPreview)
    {
        thumb.status = AssetBrowserState::PreviewStatus::Failed;
        return;
    }

    if (previewRoot.IsValid())
    {
        m_transformSystem.Update(*m_world);
        const auto* root = m_world->Get<WorldTransformComponent>(previewRoot);
        FitAssetThumbnailCamera(*m_world, m_assetRegistry, m_assetThumbnailEntities,
                                root ? root->position : math::Vec3{},
                                m_activeAssetThumbnailCenter, m_activeAssetThumbnailEyeOffset);
    }

    m_activeAssetThumbnailRT = thumb.renderTarget;
    m_activeAssetThumbnailPath = path;
    m_activeAssetThumbnailIsMaterial = isMaterial;
    thumb.status = AssetBrowserState::PreviewStatus::Rendering;
}

bool KromEditorApp::BuildAssetThumbnailRenderView(renderer::RenderView& outView) const noexcept
{
    if (!m_activeAssetThumbnailRT.IsValid())
        return false;

    const float distance = m_activeAssetThumbnailEyeOffset.Length();
    const float nearPlane = std::min(0.03f, distance * 0.01f);
    const float farPlane = std::max(20.f, distance * 4.f);
    const math::Vec3 target = m_activeAssetThumbnailCenter;
    const math::Vec3 eye = target + m_activeAssetThumbnailEyeOffset;
    outView.view = math::Mat4::LookAtRH(eye, target, math::Vec3::Up());
    outView.projection = math::Mat4::PerspectiveFovRH(
        renderer::addons::editor::kAssetThumbnailFov, 1.f, nearPlane, farPlane);
    outView.cameraPosition = eye;
    outView.cameraForward = -m_activeAssetThumbnailEyeOffset.Normalized();
    outView.nearPlane = nearPlane;
    outView.farPlane = farPlane;
    outView.ambientColor = {0.28f, 0.28f, 0.28f};
    outView.ambientIntensity = m_activeAssetThumbnailIsMaterial ? 0.2f : 0.7f;
    outView.debugFlags = 0u;
    outView.visibilityLayerMask = renderer::LAYER_EDITOR_ASSET_THUMBNAIL;
    // Only the thumbnail light rig (see SyncPreviewLightRigs).
    outView.lightLayerMask = renderer::LAYER_EDITOR_ASSET_THUMBNAIL;
    outView.lightLayerExcludeMask = ~renderer::LAYER_EDITOR_ASSET_THUMBNAIL;
    outView.backgroundMode = BackgroundMode::ClearColor;
    outView.enableBloom = false;
    outView.enableShadows = false;
    outView.enableAmbientOcclusion = false;
    outView.clearColor = {0.f, 0.f, 0.f, 1.f};
    return true;
}
#else
void KromEditorApp::EnsureCameraGizmos() {}
void KromEditorApp::SyncCameraGizmoTransforms() {}
void KromEditorApp::EnsureMaterialPreviewEntity() {}
void KromEditorApp::SyncMaterialPreviewEntity() {}
void KromEditorApp::SyncPreviewLightRigs() {}
bool KromEditorApp::BuildAnimationPreviewRenderView(renderer::RenderView&) const noexcept { return false; }
bool KromEditorApp::BuildMaterialPreviewRenderView(renderer::RenderView&) const noexcept { return false; }
void KromEditorApp::ProcessAssetThumbnailQueue() {}
bool KromEditorApp::BuildAssetThumbnailRenderView(renderer::RenderView&) const noexcept { return false; }
#endif // KROM_EDITOR_HAS_IMGUI

namespace {

// Gehoert die Entity in die Datei der Szene 'sceneName'? Gilt fuer die Editor-.json
// und den Live-.kscene-Export gleichermassen.
bool IsEntityPartOfScene(const ecs::World& world, EntityID id, const std::string& sceneName)
{
    using namespace renderer::addons::editor;
    if (world.Has<EditorRuntimeGizmoTag>(id))
        return false;  // Laufzeit-Gizmo-Entity nie speichern
    if (world.Has<EditorMaterialPreviewComponent>(id))
        return false;  // Materialpreview nie speichern
    if (world.Has<EditorAssetThumbnailComponent>(id))
        return false;  // Asset-Thumbnails nie speichern
    if (world.Has<EditorPreviewLightComponent>(id))
        return false;  // never save the preview light rig
    // Ungetaggte Kameras ohne EditorHierarchyComponent sind System-Kameras
    // (z.B. alte EditorScene::Build-Kamera) — nie speichern
    if (world.Has<CameraComponent>(id) && !world.Has<EditorHierarchyComponent>(id))
        return false;
    const auto* tag = world.Get<EditorSceneTagComponent>(id);
    if (!tag) return true;                 // ungetaggt → aktuelle Szene
    return tag->sceneName == sceneName;    // nur eigene Entities
}

} // namespace

bool KromEditorApp::LeavePrefabMode()
{
    return !m_editorFrameCtx || renderer::addons::editor::ExitPrefabMode(*m_editorFrameCtx);
}

bool KromEditorApp::SaveEditorScene()
{
#ifdef KROM_EDITOR_HAS_IMGUI
    // Nie den Spielstand speichern: Play aendert die Welt, Stop stellt sie wieder her.
    if (m_playActive)
    {
        if (m_editorFrameCtx)
            m_editorFrameCtx->lastFileMessage = Translate("play.save_disabled");
        return false;
    }
#endif
    // Nie das Prefab als Szene schreiben: vorher zurueck zur Szene.
    if (!LeavePrefabMode())
        return false;
    if (!m_world)
        return false;

#ifdef KROM_EDITOR_HAS_IMGUI
    // Saving the scene (also on scene switch) saves the open material and animation,
    // whether or not the editors save automatically. The animation first, because
    // saving it assigns the clip to its object in this scene.
    if (m_editorFrameCtx)
    {
        renderer::addons::editor::SaveOpenAnimationIfChanged(*m_editorFrameCtx);
        renderer::addons::editor::SaveOpenControllerIfChanged(*m_editorFrameCtx);
        renderer::addons::editor::SaveSelectedMaterialAssetIfChanged(*m_editorFrameCtx);
    }
#endif

    NormalizeEditorObjectLayerMasks(*m_world);
    EnsureEntityGuids(*m_world);

#ifdef KROM_EDITOR_HAS_IMGUI
    if (m_editorFrameCtx && !m_projectRoot.empty())
    {
        const auto bindings = renderer::addons::editor::RefreshCppScriptProject(*m_editorFrameCtx);
        if (!bindings.ok)
        {
            m_editorFrameCtx->lastFileMessage = bindings.message.empty()
                ? Translate("script.bindings_failed")
                : bindings.message;
            AddEditorConsoleLog(&m_editorFrameCtx->state, engine::LogLevel::Error,
                "[Build] " + m_editorFrameCtx->lastFileMessage);
        }

        m_world->ForEachAlive([&](EntityID entity)
        {
            auto* scripts = m_world->Get<engine::script::ScriptList>(entity);
            if (!scripts)
                return;
            for (auto& inst : scripts->Instances_Mutable())
            {
                const auto markerFields =
                    renderer::addons::editor::FindCppScriptFields(*m_editorFrameCtx, inst.className);
                if (markerFields.empty())
                    continue;

                std::unordered_set<std::string> validFields;
                for (const auto& field : markerFields)
                    validFields.insert(field.name);

                for (auto it = inst.fieldValues.begin(); it != inst.fieldValues.end();)
                {
                    if (validFields.count(it->first) == 0u)
                        it = inst.fieldValues.erase(it);
                    else
                        ++it;
                }
            }
        });
    }
#endif

    if (!WriteEditorScene(GetCurrentEditorScenePath()))
        return false;
    RememberSavedSceneContent();
    DeleteUnusedProbeFiles();
    return true;
}

// Baked probe files of this scene that no probe uses any more (probe removed,
// object deleted) are deleted on save, not on removal, so undo keeps the file
// until the scene is saved.
void KromEditorApp::DeleteUnusedProbeFiles()
{
    if (!m_world || m_projectRoot.empty())
        return;
    std::vector<uint64_t> usedIds;
    m_world->View<ReflectionProbeComponent>([&](EntityID, const ReflectionProbeComponent& probe) {
        if (probe.probeId != 0u)
            usedIds.push_back(probe.probeId);
    });
    const auto deleted = engine::addons::probes::DeleteUnusedProbeFiles(
        PathToUtf8(m_projectRoot / "Assets"), m_currentSceneName, usedIds);
#ifdef KROM_EDITOR_HAS_IMGUI
    if (m_editorFrameCtx)
        for (const std::string& path : deleted)
            AddEditorConsoleLog(&m_editorFrameCtx->state, engine::LogLevel::Info,
                Translate("probe.unused_deleted") + path);
#else
    (void)deleted;
#endif
}

bool KromEditorApp::SaveSceneBeforeLeaving()
{
    if (!m_editorFrameCtx || !m_editorFrameCtx->discardSceneChangesOnce)
        return SaveEditorScene();
    m_editorFrameCtx->discardSceneChangesOnce = false;
    // Only the scene is discarded: the open material and animation are files of
    // their own and are kept as on every switch. The prefab mode is left so the
    // scene, not the prefab, is replaced.
    if (!LeavePrefabMode())
        return false;
#ifdef KROM_EDITOR_HAS_IMGUI
    renderer::addons::editor::SaveOpenAnimationIfChanged(*m_editorFrameCtx);
        renderer::addons::editor::SaveOpenControllerIfChanged(*m_editorFrameCtx);
    renderer::addons::editor::SaveSelectedMaterialAssetIfChanged(*m_editorFrameCtx);
#endif
    return true;
}

std::string KromEditorApp::EditorSceneContentSignature()
{
    if (!m_world)
        return {};
#ifdef KROM_EDITOR_HAS_IMGUI
    std::string signature = CanonicalSceneContent(SerializeEditorSceneEntities());
    if (m_editorFrameCtx)
        signature += BuildEditorEnvironmentJson(m_editorFrameCtx->state);
#else
    std::string signature = SerializeEditorSceneEntities();
#endif
    return signature;
}

void KromEditorApp::RememberSavedSceneContent()
{
    m_savedSceneContent = EditorSceneContentSignature();
}

bool KromEditorApp::IsEditorSceneModified()
{
#ifdef KROM_EDITOR_HAS_IMGUI
    // Play and the prefab mode show other content; leaving them restores the scene.
    if (m_playActive || !m_world || (m_editorFrameCtx && renderer::addons::editor::IsInPrefabMode(m_editorFrameCtx->state)))
        return false;
#endif
    return m_savedSceneContent.empty() || EditorSceneContentSignature() != m_savedSceneContent;
}

std::string KromEditorApp::SerializeEditorSceneEntities()
{
    serialization::SceneSerializer serializer(*m_world);
    serializer.RegisterDefaultHandlers();
    addons::camera::RegisterCameraSerializationHandlers(serializer);
    addons::lighting::RegisterLightingSerializationHandlers(serializer);
    addons::mesh_renderer::RegisterMeshRendererSerializationHandlers(serializer);
    addons::prefab::RegisterPrefabSerializationHandlers(serializer);
    engine::script::RegisterScriptSerializationHandlers(serializer);
    engine::physics::RegisterPhysicsSerializationHandlers(serializer);
    engine::audio::RegisterAudioSerializationHandlers(serializer);
    engine::particles::RegisterParticleSerializationHandlers(serializer);
    engine::addons::probes::RegisterProbeSerializationHandlers(serializer);
    renderer::addons::editor::RegisterEditorSerializationHandlers(serializer);

    // Fix: Nur Entities der aktuellen Szene speichern.
    // Persistente Entities anderer Szenen (EditorSceneTagComponent.sceneName != m_currentSceneName)
    // werden übersprungen — sie gehören in ihre eigene Szenendatei, nicht in diese.
    // Kamera-Gizmo-Entities (EditorCameraGizmoComponent) sind reine Laufzeit-Hilfsobjekte
    // und werden ebenfalls nicht gespeichert.
    {
        const std::string saveSceneName = m_currentSceneName;
        serializer.SetEntityFilter([this, saveSceneName](EntityID id) -> bool {
            return IsEntityPartOfScene(*m_world, id, saveSceneName);
        });
    }

    return serializer.SerializeToJson("EditorScene");
}

bool KromEditorApp::WriteEditorScene(const std::filesystem::path& path)
{
    std::string sceneJson = SerializeEditorSceneEntities();
    if (m_editorFrameCtx)
        sceneJson = InjectEditorSceneMetadataJson(std::move(sceneJson), m_editorFrameCtx->state);
    if (!WriteTextFileAtomically(path, sceneJson))
    {
        if (m_editorFrameCtx)
            m_editorFrameCtx->lastFileMessage = Translate("scene.save_failed");
        Debug::LogError("KromEditorApp: failed to write editor scene: %s",
                        PathToUtf8(path).c_str());
        return false;
    }

    Debug::Log("KromEditorApp: editor scene saved to %s", PathToUtf8(path).c_str());
    return true;
}

bool KromEditorApp::LoadEditorScene(bool clearFirst, const std::string& sceneTag,
                                   const serialization::JsonValue* preparedRoot, bool deferSync)
{
    if (!m_world)
        return false;

    // sceneTag != "" → additives Laden einer anderen Szene;
    // sceneTag == "" → primäre Szene (m_currentSceneName) laden.
    const std::filesystem::path path = sceneTag.empty()
        ? GetCurrentEditorScenePath()
        : GetEditorScenePath(sceneTag);

    serialization::JsonValue parsedRoot;
    if (!preparedRoot)
    {
        std::ifstream in(path, std::ios::binary);
        if (!in)
        {
            if (clearFirst)
            {
                // Normaler Reload einer Datei die nicht existiert → Fehler
                Debug::LogError("KromEditorApp: failed to open editor scene for reading: %s",
                                PathToUtf8(path).c_str());
                return false;
            }
            // Szenenwechsel / additives Laden in eine noch nicht existierende Datei
            // ist kein Fehler — leere Szene.
            Debug::Log("KromEditorApp: neue Szene '%s' (noch keine Datei vorhanden)",
                       PathToUtf8(path).c_str());
            if (!deferSync)
                SyncEditorSceneState();
            return true;
        }

        const std::string json((std::istreambuf_iterator<char>(in)),
                               std::istreambuf_iterator<char>());

        std::string parseError;
        parsedRoot = serialization::JsonParser::Parse(json, parseError);
        if (!parseError.empty())
        {
            Debug::LogError("KromEditorApp: Szenendatei ungueltig: %s", parseError.c_str());
            return false;
        }
        preparedRoot = &parsedRoot;
    }

    // Snapshot bestehender Entity-IDs — neu erstellte Entities werden danach getaggt.
    std::unordered_set<uint32_t> existingIds;
    m_world->ForEachAlive([&](EntityID id) { existingIds.insert(id.value); });

    if (clearFirst)
    {
        // Alle Entities löschen (normaler Reload)
        std::vector<EntityID> entities;
        m_world->ForEachAlive([&](EntityID id) { entities.push_back(id); });
        for (EntityID id : entities)
            m_world->DestroyEntity(id);
        existingIds.clear(); // nach dem Clear sind alle Entities neu
    }

    serialization::SceneDeserializer deserializer(*m_world);
    deserializer.RegisterDefaultHandlers();
    addons::camera::RegisterCameraDeserializationHandlers(deserializer);
    addons::lighting::RegisterLightingDeserializationHandlers(deserializer);
    addons::mesh_renderer::RegisterMeshRendererDeserializationHandlers(deserializer);
    addons::prefab::RegisterPrefabDeserializationHandlers(deserializer);
    engine::script::RegisterScriptDeserializationHandlers(deserializer, &m_scriptRegistry);
    engine::physics::RegisterPhysicsDeserializationHandlers(deserializer);
    engine::audio::RegisterAudioDeserializationHandlers(deserializer);
    engine::particles::RegisterParticleDeserializationHandlers(deserializer);
    engine::addons::probes::RegisterProbeDeserializationHandlers(deserializer);
    renderer::addons::editor::RegisterEditorDeserializationHandlers(deserializer);

    const serialization::DeserializeResult result = deserializer.DeserializeFromValue(*preparedRoot);
    if (!result.success)
    {
        Debug::LogError("KromEditorApp: failed to load editor scene: %s", result.error.c_str());
        return false;
    }
    engine::script::ResolveScriptEntityReferences(*m_world, m_scriptRegistry);

    if (sceneTag.empty())
    {
        ApplyEditorCameraFromSceneJson(m_editorFrameCtx.get(), *preparedRoot);
        ApplyEditorEnvironmentFromSceneJson(m_editorFrameCtx.get(), *preparedRoot);
    }

    // Neu geladene Entities mit Szenen-Tag versehen (nur wenn kein Tag bereits vorhanden).
    const std::string tagName = sceneTag.empty() ? m_currentSceneName : sceneTag;
    if (!tagName.empty())
    {
        using namespace renderer::addons::editor;
        m_world->ForEachAlive([&](EntityID id) {
            if (existingIds.count(id.value) == 0 &&
                !m_world->Has<EditorSceneTagComponent>(id))
            {
                m_world->Add<EditorSceneTagComponent>(id, EditorSceneTagComponent{tagName});
            }
        });
    }

    if (sceneTag.empty())
        m_editorState.selectedEntity = NULL_ENTITY;

    EnsureEntityGuids(*m_world);

    // Migration: Kameras die VOR dem Laden existierten (existingIds) sind
    // System-Kameras — entfernen wenn die geladene Szene eigene Kameras hat.
    {
        bool loadedSceneHasCamera = false;
        m_world->ForEachAlive([&](EntityID id) {
            if (existingIds.count(id.value) == 0 && m_world->Has<CameraComponent>(id))
                loadedSceneHasCamera = true;
        });
        if (loadedSceneHasCamera)
        {
            std::vector<EntityID> preLaodCams;
            m_world->ForEachAlive([&](EntityID id) {
                if (existingIds.count(id.value) > 0 && m_world->Has<CameraComponent>(id))
                    preLaodCams.push_back(id);
            });
            for (EntityID id : preLaodCams)
                m_world->DestroyEntity(id);
        }
    }

    if (!deferSync)
        SyncEditorSceneState();

    Debug::Log("%s%s", Translate("scene.loaded_prefix"),
               (sceneTag.empty() ? m_currentSceneName : sceneTag).c_str());
    Debug::LogVerbose("KromEditorApp: editor scene loaded from %s (%u entities, %u components)",
               PathToUtf8(path).c_str(),
               result.entitiesRead,
               result.componentsRead);
    return true;
}
#endif

// =============================================================================
// Scene-Management
// =============================================================================

std::vector<std::string> KromEditorApp::ListEditorScenes() const
{
    std::vector<std::string> result;
    if (m_projectRoot.empty())
        return result;

    const std::filesystem::path scenesDir = m_projectRoot / "Assets" / "Scenes";
    std::error_code ec;
    if (!std::filesystem::exists(scenesDir, ec))
        return result;

    for (const auto& entry : std::filesystem::directory_iterator(scenesDir, ec))
    {
        if (ec) break;
        if (entry.is_regular_file() && entry.path().extension() == ".json")
            result.push_back(PathToUtf8(entry.path().stem()));
    }
    std::sort(result.begin(), result.end());
    return result;
}

void KromEditorApp::RefreshLoadedScenesState()
{
    if (!m_editorFrameCtx)
        return;
    m_editorFrameCtx->loadedEditorScenes = m_loadedSceneNames;
}

void KromEditorApp::RefreshSceneOrderState()
{
    if (!m_editorFrameCtx)
        return;

    // m_sceneOrder enthält die persistierte Reihenfolge.
    // Szenen die auf Disk existieren aber nicht in m_sceneOrder stehen, werden hinten angehängt.
    const std::vector<std::string> available = ListEditorScenes();
    std::vector<std::string> synced = m_sceneOrder;
    for (const std::string& s : available)
    {
        if (std::find(synced.begin(), synced.end(), s) == synced.end())
            synced.push_back(s);
    }
    // Einträge entfernen die nicht mehr existieren
    synced.erase(std::remove_if(synced.begin(), synced.end(), [&](const std::string& s) {
        return std::find(available.begin(), available.end(), s) == available.end();
    }), synced.end());

    m_editorFrameCtx->sceneOrder = synced;
}

void KromEditorApp::SetSceneOrder(const std::vector<std::string>& order)
{
    m_sceneOrder = order;
    RefreshSceneOrderState();
    SaveProjectFile();
}

namespace {

// Wie die Runtime: Objekt samt Kindern zerstoeren und beim Parent austragen.
void DestroyEntityTree(ecs::World& world, EntityID entity)
{
    if (!world.IsAlive(entity))
        return;
    if (const auto* children = world.Get<ChildrenComponent>(entity))
    {
        const std::vector<EntityID> copy = children->children;
        for (const EntityID child : copy)
            DestroyEntityTree(world, child);
    }
    if (const auto* parent = world.Get<ParentComponent>(entity))
        if (auto* siblings = world.Get<ChildrenComponent>(parent->parent))
            siblings->Remove(entity);
    world.DestroyEntity(entity);
}

// Editor-Hilfsobjekte (Gizmos, Vorschauen) bleiben bei Szenenwechseln im Spiel stehen.
bool IsEditorHelperEntity(const ecs::World& world, EntityID id)
{
    using namespace renderer::addons::editor;
    return world.Has<EditorRuntimeGizmoTag>(id) || world.Has<EditorMaterialPreviewComponent>(id) ||
           world.Has<EditorAssetThumbnailComponent>(id) || world.Has<EditorPrefabPreviewComponent>(id) ||
           world.Has<EditorPreviewLightComponent>(id);
}

} // namespace

void KromEditorApp::ReportScriptBuildError(const std::string& error)
{
    if (!m_editorFrameCtx) return;
    m_editorFrameCtx->lastFileMessage = error;
    AddEditorConsoleLog(&m_editorFrameCtx->state, LogLevel::Error, "[Build] " + error);
    m_editorFrameCtx->consolePanelVisible = true;
    m_editorFrameCtx->state.consoleFocusRequest = true;
}

bool KromEditorApp::CheckScriptEngineLibraries(std::string& error) const
{
    return ScriptModuleHost::CheckEngineLibraries(PathFromUtf8(KROM_BUILD_DIR), CurrentExecutablePath(), error);
}

bool KromEditorApp::ReloadProjectScriptModule(bool reportMissing)
{
#ifdef _WIN32
    if (!m_world || m_projectRoot.empty()) return true;
    const auto library = ScriptModuleHost::LibraryPath(PathFromUtf8(KROM_BUILD_DIR), m_projectRuntimeTarget);
    if (!std::filesystem::exists(library))
    {
        if (reportMissing) ReportScriptBuildError(Translate("script.library_missing") + PathToUtf8(library));
        return !reportMissing;
    }
    std::string error;
    // Auch reine Header-Aenderungen der Engine erkennen: gemeinsam genutzte Header,
    // die neuer als die laufende EXE sind, wuerden abweichende Layouts bedeuten.
    // Ninja aus dem CMakeCache des Editor-Builds: keine Werkzeugsuche im Hauptthread.
    if (!CheckScriptEngineLibraries(error) ||
        !ScriptModuleHost::CheckSharedEngineHeaders(build::CachedMakeProgram(PathFromUtf8(KROM_BUILD_DIR)),
            PathFromUtf8(KROM_BUILD_DIR), PathFromUtf8(KROM_SOURCE_DIR), CurrentExecutablePath(),
            "krom_scripts_" + m_projectRuntimeTarget, error) ||
        !m_scriptModule.Swap(library, *m_world, m_scriptRegistry, error, m_projectRoot))
    {
        ReportScriptBuildError(error);
        return false;
    }
    engine::script::ResolveScriptEntityReferences(*m_world, m_scriptRegistry);
    return true;
#else
    (void)reportMissing;
    return true;
#endif
}

bool KromEditorApp::StartEditorPlay()
{
#ifdef _WIN32
    if (m_playActive) return true;
    if (!m_editorFrameCtx || !m_world || m_editorFrameCtx->sceneLoadActive || m_runtimeBuildJob)
        return false;
    if (m_projectRoot.empty()) { ReportScriptBuildError(Translate("project.no_project")); return false; }
    if (!LeavePrefabMode()) return false;
    std::string error;
    if (!CheckScriptEngineLibraries(error)) { ReportScriptBuildError(error); return false; }
    const auto bindings = renderer::addons::editor::GenerateCppScriptBindings(*m_editorFrameCtx);
    if (!bindings.ok) { ReportScriptBuildError(bindings.message); return false; }
    try
    {
        const auto buildDir = PathFromUtf8(KROM_BUILD_DIR);
        const auto library = ScriptModuleHost::LibraryPath(buildDir, m_projectRuntimeTarget);
        if (!m_scriptModule.IsLoaded() || m_scriptModule.NeedsBuild(m_projectRoot, library))
        {
            // Vor dem Build erfassen: waehrend des Builds gespeicherte Quellen bleiben neuer.
            const auto buildStart = std::filesystem::file_time_type::clock::now();
            return StartRuntimeBuild(buildDir, [this, library, buildStart](bool ok) {
                if (!ok)
                {
                    ReportScriptBuildError(Translate("play.script_build_failed"));
                    return;
                }
                // Ohne Neu-Link (nichts zu tun fuer Ninja) die geladene DLL behalten.
                if (m_scriptModule.SourceLibraryChanged(library) && !ReloadProjectScriptModule(true)) return;
                m_scriptModule.MarkBuilt(m_projectRoot, library, buildStart);
                (void)BeginEditorPlay();
            }, "krom_scripts_" + m_projectRuntimeTarget);
        }
        if (m_scriptModule.SourceLibraryChanged(library) && !ReloadProjectScriptModule(true)) return false;
    }
    catch (const std::exception& exception) { ReportScriptBuildError(exception.what()); return false; }
#endif
    return BeginEditorPlay();
}

bool KromEditorApp::BeginEditorPlay()
{
    if (m_playActive)
        return true;
    if (!m_editorFrameCtx || !m_world || m_editorFrameCtx->sceneLoadActive)
        return false;
    // Szenenwechsel und Neustarts im Spiel lesen die Szenendatei erneut.
    if (!SaveEditorScene())
        return false;
    renderer::addons::editor::EditorFrameContext& ctx = *m_editorFrameCtx;
    if (!renderer::addons::editor::BeginPlaySession(ctx))
        return false;

    m_game.Shutdown();   // frischer Zustand: Spielzeit, Physik, Audio, Partikel
    m_game.SetAssetRoot(GetCurrentAssetRoot());
    m_game.SetProjectSettings(m_projectSettings);
    m_game.SetAudioDevice(true);
    // Schrift fuer MeasureScreenText wie in der Runtime (Projekt, Engine, System).
    {
        std::vector<std::filesystem::path> fonts{GetCurrentAssetRoot() / "fonts" / "default.ttf",
                                                 ResolveAssetRoot() / "fonts" / "default.ttf"};
#if defined(_WIN32)
        const wchar_t* windir = _wgetenv(L"WINDIR");
        const std::filesystem::path systemFonts =
            std::filesystem::path(windir ? windir : L"C:/Windows") / "Fonts";
        fonts.push_back(systemFonts / "segoeui.ttf");
        fonts.push_back(systemFonts / "arial.ttf");
#endif
        m_game.Text().SetFontCandidates(std::move(fonts));
    }
    m_playInstantiatePrefab = [this](std::string_view path, const math::Vec3& position,
                                     const math::Quat& rotation, const math::Vec3& scale) {
        return InstantiatePrefabForPlay(path, position, rotation, scale);
    };
    m_particles.Clear();

    m_playActive = true;
    m_playPaused = false;
    ctx.playActive = true;
    ctx.playPaused = false;
    ctx.focusScenePanel = false;
    ctx.focusGamePanel = true;
    ctx.lastFileMessage = Translate("play.started");

    return true;
}

void KromEditorApp::StopEditorPlay()
{
    if (!m_playActive)
        return;
    m_game.Shutdown();
    m_playActive = false;
    m_playPaused = false;
    m_particles.Clear();
    if (!m_editorFrameCtx)
        return;
    renderer::addons::editor::EditorFrameContext& ctx = *m_editorFrameCtx;
    ctx.playActive = false;
    ctx.playPaused = false;
    ctx.gameOverlayText.clear();
    ctx.focusGamePanel = false;
    ctx.focusScenePanel = true;
    ctx.gameViewFocused = false;
    const bool restored = renderer::addons::editor::EndPlaySession(ctx);
    ctx.lastFileMessage = restored ? Translate("play.restored")
                                   : Translate("play.restore_failed");
}

void KromEditorApp::SetEditorPlayPaused(bool paused)
{
    if (!m_playActive)
        return;
    m_playPaused = paused;
    if (m_editorFrameCtx)
    {
        m_editorFrameCtx->playPaused = paused;
        m_editorFrameCtx->focusScenePanel = paused;
        m_editorFrameCtx->focusGamePanel = !paused;
        if (paused) m_editorFrameCtx->gameViewFocused = false;
    }
}

engine::gameplay::GameSimulationHooks KromEditorApp::MakeEditorPlayHooks()
{
    engine::gameplay::GameSimulationHooks hooks{};
    hooks.instantiatePrefab = &m_playInstantiatePrefab;
    hooks.materials = &m_materialSystem;
    hooks.destroyEntity = [this](EntityID id) { DestroyEntityTree(*m_world, id); };
    hooks.loadScene = [this](const engine::script::SceneLoadRequest& request) { LoadSceneForPlay(request); };
    return hooks;
}

void KromEditorApp::TickEditorPlay(float unscaledDeltaSeconds, const std::function<void()>& editorSync)
{
    // Bildschirmtext in Pixeln des Spielbilds (GetScreenSize der Scripts).
    m_game.Text().SetViewport(m_gameView.width > 0u ? m_gameView.width : 1280u,
                              m_gameView.height > 0u ? m_gameView.height : 720u);
    // Tastatur und Maus nur, solange der Game-Reiter den Fokus hat.
    const platform::IInput* input =
        (m_editorFrameCtx && m_editorFrameCtx->gameViewFocused) ? m_renderLoop.GetInput() : nullptr;

    if (!m_playAnimation)
        m_playAnimation = std::make_unique<engine::addons::animation::AnimationSystem>(m_assetRegistry);
    // Controllers come from the open project; clips they name are imported on demand.
    if (m_assetPipeline && m_playAnimation->Controllers().AssetRoot() != m_assetPipeline->GetAssetRoot())
        m_playAnimation->SetAssetSources(m_assetPipeline->GetAssetRoot(), [this](const std::string& clipPath) {
            return addons::mesh_renderer::ImportModelAnimationAssets(*m_assetPipeline, m_assetRegistry, clipPath);
        });

    engine::gameplay::GameFrameSteps steps{};
    steps.transforms       = &m_transformSystem;
    steps.bounds           = &m_boundsSystem;
    steps.animation        = m_playAnimation.get();
    steps.beforeTransforms = editorSync;
    m_game.RunFrame(*m_world, unscaledDeltaSeconds, input, MakeEditorPlayHooks(), steps);
}

void KromEditorApp::UploadSkinning()
{
    // As in the runtime: the renderer reads SkinComponent::gpuBuffer. Runs in edit
    // mode too (bind pose), because skinned materials need a bone buffer in every
    // frame. GarbageCollect drops buffers of removed entities (Destroy, LoadScene,
    // Stop restoring the edit scene).
    if (!m_skinning)
    {
        if (renderer::IDevice* device = m_renderLoop.GetRenderSystem().GetDevice())
            m_skinning = std::make_unique<engine::addons::animation::SkinningSystem>(*device, &m_assetRegistry);
    }
    if (m_skinning)
    {
        m_skinning->Upload(*m_world);
        m_skinning->GarbageCollect(*m_world);
    }
}

EntityID KromEditorApp::InstantiatePrefabForPlay(std::string_view prefabAssetPath, const math::Vec3& position,
                                                 const math::Quat& rotation, const math::Vec3& scale)
{
    if (!m_world || prefabAssetPath.empty())
        return NULL_ENTITY;
    std::filesystem::path path = PathFromUtf8(prefabAssetPath);
    if (path.is_relative())
        path = GetCurrentAssetRoot() / path;

    addons::prefab::PrefabAsset prefab;
    std::string error;
    if (!addons::prefab::LoadPrefabFromFile(path, m_assetRegistry, prefab, &error))
    {
        Debug::LogError("Play: Prefab konnte nicht geladen werden '%s': %s",
                        PathToUtf8(path).c_str(), error.c_str());
        return NULL_ENTITY;
    }
    addons::prefab::PrefabInstantiateOptions options{};
    options.position = position;
    options.rotation = rotation;
    options.scale = scale;
    options.prefabAssetPath = std::string(prefabAssetPath);
    options.scriptRegistry = &m_scriptRegistry;
    const addons::prefab::PrefabInstance instance = addons::prefab::InstantiatePrefab(*m_world, prefab, options);
    if (!instance.IsValid())
    {
        Debug::LogError("Play: Prefab konnte nicht instanziiert werden '%s'", PathToUtf8(path).c_str());
        return NULL_ENTITY;
    }
    if (m_assetPipeline)
        addons::mesh_renderer::ResolveMeshAssetBindings(*m_assetPipeline, m_assetRegistry, *m_world);
    if (m_editorFrameCtx)
        renderer::addons::editor::ResolveMaterialAssetBindings(*m_editorFrameCtx);
    for (const EntityID entity : instance.entities)
    {
        if (!entity.IsValid() || !m_world->IsAlive(entity))
            continue;
        if (!m_world->Has<WorldTransformComponent>(entity))
            m_world->Add<WorldTransformComponent>(entity);
        if (auto* transform = m_world->Get<TransformComponent>(entity))
        {
            transform->dirty = true;
            ++transform->localVersion;
        }
    }
    return instance.root;
}

void KromEditorApp::LoadSceneForPlay(const engine::script::SceneLoadRequest& request)
{
    // Wie die Runtime: Index = Position unter den gebauten Szenen (ohne ausgeschlossene).
    std::string sceneName;
    if (!request.path.empty())
    {
        sceneName = PathToUtf8(PathFromUtf8(request.path).stem());
    }
    else if (request.index > 0)
    {
        int index = 0;
        for (const std::string& name : m_sceneOrder)
        {
            if (m_buildExcludedScenes.count(name) != 0u)
                continue;
            if (++index == request.index)
            {
                sceneName = name;
                break;
            }
        }
    }
    if (sceneName.empty())
    {
        Debug::LogWarning("Play: LoadScene - Szene nicht gefunden (index=%d, path='%s')",
                          request.index, request.path.c_str());
        return;
    }

    const auto path = GetEditorScenePath(sceneName);
    std::ifstream input(path, std::ios::binary);
    if (!input)
    {
        Debug::LogError("Play: Szenendatei nicht lesbar: %s", PathToUtf8(path).c_str());
        return;
    }
    const std::string json((std::istreambuf_iterator<char>(input)), {});
    std::string error;
    const auto root = ParseEditorScene(json, error);
    if (!error.empty())
    {
        Debug::LogError("Play: Szene '%s' konnte nicht geladen werden: %s", sceneName.c_str(), error.c_str());
        return;
    }

    if (!request.additive)
    {
        m_game.ResetForSceneChange();
        std::vector<EntityID> sceneEntities;
        m_world->ForEachAlive([&](EntityID id) {
            if (!IsEditorHelperEntity(*m_world, id))
                sceneEntities.push_back(id);
        });
        for (const EntityID id : sceneEntities)
            if (m_world->IsAlive(id))
                m_world->DestroyEntity(id);
        if (m_editorFrameCtx)
        {
            m_editorFrameCtx->state.selectedEntity = NULL_ENTITY;
            m_editorFrameCtx->state.multiSelection.clear();
        }
    }
    // Szene aus ihrer Editor-Datei laden (gleicher Inhalt wie die gebaute .kscene).
    if (!LoadEditorScene(/*clearFirst=*/false, sceneName, &root))
        Debug::LogError("Play: Szene '%s' konnte nicht geladen werden", sceneName.c_str());
}

void KromEditorApp::CreateDefaultSceneEntities()
{
    if (!m_world || !m_editorFrameCtx)
        return;

    // New scenes start with the filmic default operator.
    m_editorFrameCtx->state.tonemap = renderer::addons::forward::TonemapSettings{};

    using namespace renderer::addons::editor;

    // Kamera
    const EntityID cam = CreateCameraEntity(*m_editorFrameCtx);
    if (cam.IsValid())
    {
        if (auto* t = m_world->Get<TransformComponent>(cam))
        {
            t->localPosition = {0.f, 2.f, 8.f};
            t->dirty         = true;
        }
    }

    // Directional Light
    const EntityID light = CreateLightEntity(*m_editorFrameCtx, LightType::Directional);
    if (light.IsValid())
    {
        if (auto* t = m_world->Get<TransformComponent>(light))
        {
            t->localRotation = math::Quat::FromEulerDeg(-45.f, 30.f, 0.f);
            t->dirty         = true;
        }
    }

    // Undo-History löschen — Default-Entities sollen nicht rückgängig gemacht werden
    ClearEditorHistory(m_editorFrameCtx->state);
}

void KromEditorApp::ApplyDefaultProjectEnvironment(const std::filesystem::path& assetRoot)
{
    if (!m_editorFrameCtx)
        return;

    using EditorState = renderer::addons::editor::EditorState;
    auto& state = m_editorFrameCtx->state;

    // Environment des vorherigen Projekts nicht in das neue übernehmen.
    state.environmentTexturePath.clear();
    state.environmentEnableIBL = false;
    state.showSkyboxBackground = false;
    state.ambientSource        = EditorState::AmbientSource::Color;

    // editor/sky.hdr ins Projekt kopieren, damit Play und Export die Datei
    // unter Assets/ finden (siehe EnsureCameraGizmos zum editor/-Pfad).
    const std::filesystem::path source =
        ResolveAssetRoot().parent_path() / "editor" / "sky.hdr";
    const std::filesystem::path target = assetRoot / "Textures" / "sky.hdr";
    std::error_code ec;
    if (!std::filesystem::exists(target, ec))
        std::filesystem::copy_file(source, target, ec);
    if (ec || !std::filesystem::exists(target, ec))
    {
        Debug::LogWarning("KromEditorApp: default sky could not be copied from %s",
                          PathToUtf8(source).c_str());
        if (m_editorFrameCtx->clearEditorEnvironment)
            m_editorFrameCtx->clearEditorEnvironment();
        return;
    }

    state.environmentTexturePath = "Textures/sky.hdr";
    state.environmentEnableIBL   = true;
    state.showSkyboxBackground   = true;
    state.ambientSource          = EditorState::AmbientSource::Skybox;
    if (m_editorFrameCtx->setEditorEnvironment)
        (void)m_editorFrameCtx->setEditorEnvironment(state.environmentTexturePath,
                                                     state.environmentIntensity,
                                                     state.environmentEnableIBL);
}

void KromEditorApp::RefreshSceneList()
{
    if (!m_editorFrameCtx)
        return;
    m_editorFrameCtx->currentEditorSceneName = m_currentSceneName;
    m_editorFrameCtx->availableEditorScenes  = ListEditorScenes();
    RefreshLoadedScenesState();
    RefreshSceneOrderState();
}

void KromEditorApp::ShowSceneLoadProgress(float progress, const char* phase)
{
    if (!m_editorFrameCtx || !m_editorFrameCtx->sceneLoadActive)
        return;
    m_editorFrameCtx->sceneLoadProgress = progress;
    m_editorFrameCtx->sceneLoadPhase = phase;
    m_editorFrameCtx->sceneLoadIndeterminate = false;
}

static void StartSceneRead(const std::shared_ptr<SceneLoadJob>& job, const std::filesystem::path& path)
{
    std::thread([job, path]() {
        try
        {
            std::ifstream input(path, std::ios::binary);
            if (!input)
            {
                std::error_code ec;
                if (!std::filesystem::exists(path, ec) && !ec)
                    job->root = serialization::JsonParser::Parse("{\"entities\":[]}", job->error);
                else
                    job->error = Translate("scene.file_unreadable");
            }
            else
            {
                const std::string json((std::istreambuf_iterator<char>(input)), {});
                if (input.bad())
                    job->error = Translate("scene.read_error");
                else
                    job->root = serialization::JsonParser::Parse(json, job->error);
            }
            if (job->error.empty())
            {
                const auto* entities = job->root.Get("entities");
                if (!job->root.IsObject() || !entities || !entities->IsArray())
                    job->error = Translate("scene.invalid_entities");
            }
        }
        catch (const std::exception& error) { job->error = error.what(); }
        catch (...) { job->error = Translate("scene.unknown_read_error"); }
        job->readFinished.store(true, std::memory_order_release);
    }).detach();
}

void KromEditorApp::ProcessPendingSceneSwitch()
{
    if (!m_editorFrameCtx)
        return;
    if (!m_sceneLoadJob)
    {
        if (m_pendingSceneSwitch.empty())
            return;
        auto job = std::make_shared<SceneLoadJob>();
        job->name = std::move(m_pendingSceneSwitch);
        m_pendingSceneSwitch.clear();
        job->additive = m_pendingSceneAdditive;
        if (job->additive &&
            (job->name == m_currentSceneName ||
             std::find(m_loadedSceneNames.begin(), m_loadedSceneNames.end(), job->name) != m_loadedSceneNames.end()))
        {
            m_editorFrameCtx->lastFileMessage = Translate("scene.already_loaded") + job->name;
            return;
        }
        m_sceneLoadJob = std::move(job);
        m_editorFrameCtx->sceneLoadActive = true;
        m_editorFrameCtx->sceneLoadName = m_sceneLoadJob->name;
        ShowSceneLoadProgress(0.02f, Translate("scene.phase.saving"));
        return; // Erst den normalen UI-Frame zeichnen, dann arbeiten.
    }

    const auto job = m_sceneLoadJob;
    const auto fail = [&](const std::string& error) {
        if (job->openingProject)
        {
            m_editorState.pendingStartProjectFile.clear();
            FinishProjectLoad(false);
            Debug::LogError("KromEditorApp: %s", error.c_str());
        }
        else
            m_editorFrameCtx->lastFileMessage = Translate("scene.switch_failed") + error;
        Debug::LogError("KromEditorApp: %s", m_editorFrameCtx->lastFileMessage.c_str());
        m_editorFrameCtx->sceneLoadActive = false;
        m_sceneLoadJob.reset();
    };
    // GPU resources stay on the editor thread. Yield after a short batch so
    // the launcher can draw and present between resource uploads.
    const auto processAssets = [&](size_t count, auto&& load) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(6);
        while (job->nextAsset < count)
        {
            load(job->nextAsset++);
            if (std::chrono::steady_clock::now() >= deadline)
                break;
        }
        if (job->nextAsset < count)
            return false;
        job->nextAsset = 0;
        return true;
    };
    const auto collectAssetEntities = [&] {
        job->assetEntities.clear();
        m_world->ForEachAlive([&](EntityID id) {
            if (m_world->Has<MeshComponent>(id))
                job->assetEntities.push_back(id);
        });
    };
    using Stage = SceneLoadJob::Stage;
    switch (job->stage)
    {
    case Stage::Save:
    {
        if (!job->additive ? !SaveSceneBeforeLeaving() : !SaveEditorScene())
        {
            fail(m_editorFrameCtx->lastFileMessage);
            return;
        }
        const auto path = GetEditorScenePath(job->name);
        job->stage = Stage::Read;
        ShowSceneLoadProgress(0.15f, Translate("scene.phase.reading"));
        m_editorFrameCtx->sceneLoadIndeterminate = true;
        StartSceneRead(job, path);
        return;
    }
    case Stage::Textures:
        if (!processAssets(job->textures.size(), [&](size_t index) {
                renderer::addons::editor::PreloadEditorTexture(*m_editorFrameCtx, m_assetBrowserState, job->textures[index]);
            }))
            return;
        if (job->hasSceneFile)
        {
            StartSceneRead(job, GetCurrentEditorScenePath());
            job->stage = Stage::Read;
        }
        else
            job->stage = Stage::Hierarchy;
        return;
    case Stage::Read:
        if (!job->readFinished.load(std::memory_order_acquire))
            return;
        if (!job->error.empty())
        {
            fail(job->error);
            return;
        }
        job->stage = Stage::Apply;
        ShowSceneLoadProgress(0.35f, Translate("scene.phase.objects"));
        return;
    case Stage::Apply:
        if (job->openingProject
            ? !LoadEditorScene(true, "", &job->root, true)
            : (!SwitchEditorScene(job->name, job->additive, true) ||
               !LoadEditorScene(false, job->additive ? job->name : "", &job->root, true)))
        {
            fail(job->name);
            return;
        }
        job->stage = Stage::Hierarchy;
        ShowSceneLoadProgress(0.50f, Translate("scene.phase.hierarchy"));
        return;
    case Stage::Hierarchy:
        SyncEditorSceneState(SceneSyncStep::Hierarchy);
        if (job->openingProject)
            collectAssetEntities();
        job->stage = Stage::Meshes;
        ShowSceneLoadProgress(0.62f, Translate("scene.phase.meshes"));
        return;
    case Stage::Meshes:
        if (job->openingProject)
        {
            if (m_assetPipeline && !processAssets(job->assetEntities.size(), [&](size_t index) {
                    addons::mesh_renderer::ResolveMeshAssetBindings(
                        *m_assetPipeline, m_assetRegistry, *m_world, {&job->assetEntities[index], 1});
                }))
                return;
            EnsureCameraGizmos();
            collectAssetEntities();
        }
        else
            SyncEditorSceneState(SceneSyncStep::Meshes);
        job->stage = Stage::Materials;
        ShowSceneLoadProgress(0.75f, Translate("scene.phase.materials"));
        return;
    case Stage::Materials:
        if (job->openingProject)
        {
            if (!processAssets(job->assetEntities.size(), [&](size_t index) {
                    renderer::addons::editor::ResolveMaterialAssetBindings(
                        *m_editorFrameCtx, {&job->assetEntities[index], 1});
                }))
                return;
        }
        else
            SyncEditorSceneState(SceneSyncStep::Materials);
        job->stage = Stage::Prefabs;
        ShowSceneLoadProgress(0.85f, Translate("scene.phase.prefabs"));
        return;
    case Stage::Prefabs:
        SyncEditorSceneState(SceneSyncStep::Prefabs);
        job->stage = Stage::Finalize;
        ShowSceneLoadProgress(0.95f, Translate("scene.phase.finishing"));
        return;
    case Stage::Finalize:
        SyncEditorSceneState(SceneSyncStep::Finalize);
        if (job->openingProject)
        {
            job->stage = Stage::Done;
            return;
        }
        if (!job->additive)
            renderer::addons::editor::ClearEditorHistory(m_editorFrameCtx->state);
        m_editorFrameCtx->lastFileMessage =
            std::string(job->additive ? Translate("scene.additive_loaded") : Translate("scene.loaded_prefix")) + job->name;
        job->stage = Stage::Done;
        ShowSceneLoadProgress(1.f, Translate("scene.phase.ready"));
        return;
    case Stage::Done:
        if (job->openingProject)
        {
            FinishProjectLoad(true);
            m_editorState.pendingStartProjectFile.clear();
        }
        if (!job->additive)
            RememberSavedSceneContent();
        m_editorFrameCtx->sceneLoadActive = false;
        m_sceneLoadJob.reset();
        m_timing.BeginFrame();
        return;
    }
}
bool KromEditorApp::SwitchEditorScene(const std::string& sceneName, bool additive, bool prepareOnly)
{
    if (!m_world || sceneName.empty())
        return false;

    // 1. Aktuelle Szene speichern bevor wir wechseln (ausser der Nutzer hat "Nicht speichern" gewaehlt).
    if (!prepareOnly && !(additive ? SaveEditorScene() : SaveSceneBeforeLeaving()))
        return false;

    if (!additive)
    {
        // ── Exklusives Laden: bestehende nicht-persistente Entities entfernen ──
        using namespace renderer::addons::editor;

        // Fix 2a: Ungetaggte persistente Entities vor dem Wechsel mit dem aktuellen
        // Szenenname taggen. Beim Zurückwechseln werden sie korrekt erkannt und
        // zuerst zerstört, bevor die Szene aus der Datei neu geladen wird.
        m_world->ForEachAlive([&](EntityID id) {
            if (!m_world->Has<EditorPersistentComponent>(id)) return;
            if (!m_world->Has<EditorSceneTagComponent>(id))
                m_world->Add<EditorSceneTagComponent>(id, EditorSceneTagComponent{m_currentSceneName});
        });

        // Fix 2b: Alle Entities der ZIEL-Szene (tagged mit sceneName) vorab zerstören,
        // damit beim Laden keine Duplikate entstehen. Persistente Entities von SceneA
        // die nach einem früheren Besuch überlebt haben, werden hier bereinigt.
        // Ihr Zustand wurde oben per auto-save gesichert.
        {
            std::function<void(EntityID)> destroyTaggedRecursive = [&](EntityID e) {
                if (!m_world->IsAlive(e)) return;
                if (const auto* ch = m_world->Get<ChildrenComponent>(e))
                {
                    const auto copy = ch->children;
                    for (EntityID c : copy) destroyTaggedRecursive(c);
                }
                m_world->DestroyEntity(e);
            };

            // Nur Root-Entities der Ziel-Szene sammeln (Kinder werden rekursiv mitentfernt).
            std::vector<EntityID> targetRoots;
            m_world->ForEachAlive([&](EntityID id) {
                const auto* tag = m_world->Get<EditorSceneTagComponent>(id);
                if (!tag || tag->sceneName != sceneName) return;
                if (!m_world->Has<ParentComponent>(id))
                    targetRoots.push_back(id);
            });
            for (EntityID e : targetRoots)
                if (m_world->IsAlive(e))
                    destroyTaggedRecursive(e);
        }

        // 2. Persistente Entities die Kinder von nicht-persistenten Entities sind,
        //    zuerst von ihren Eltern abtrennen — sonst würden sie bei der
        //    rekursiven Zerstörung der nicht-persistenten Subtrees mitgelöscht.
        {
            std::vector<std::pair<EntityID, EntityID>> toDetach;  // {entity, parent}
            m_world->ForEachAlive([&](EntityID id) {
                if (!m_world->Has<EditorPersistentComponent>(id)) return;
                if (!m_world->Has<ParentComponent>(id))           return; // schon Root
                const EntityID parent = m_world->Get<ParentComponent>(id)->parent;
                if (m_world->IsAlive(parent) &&
                    !m_world->Has<EditorPersistentComponent>(parent))
                    toDetach.emplace_back(id, parent);
            });
            for (const auto& [child, parent] : toDetach)
            {
                if (!m_world->IsAlive(child) || !m_world->IsAlive(parent)) continue;

                // Weltkoordinaten in lokale Transform einbacken, damit die Entity
                // nach dem Abtrennen an ihrer Weltposition bleibt.
                // Ohne diesen Schritt würde TransformSystem im nächsten Frame die
                // World-Matrix aus dem (jetzt falschen) lokalen Offset neu berechnen
                // und die Entity auf die alte Child-Position springen lassen.
                if (auto* childTc = m_world->Get<TransformComponent>(child))
                {
                    if (const auto* childWtc = m_world->Get<WorldTransformComponent>(child))
                    {
                        childTc->localPosition = childWtc->position;
                        childTc->localRotation = childWtc->rotation;
                        if (childTc->inheritParentScale)
                            childTc->localScale = childWtc->scale;
                        childTc->dirty = true;
                    }
                }

                if (m_world->Has<ChildrenComponent>(parent))
                    m_world->Get<ChildrenComponent>(parent)->Remove(child);
                m_world->Remove<ParentComponent>(child);
            }
        }

        // 3. Alle nicht-persistenten Root-Entities rekursiv löschen.
        std::vector<EntityID> roots;
        m_world->ForEachAlive([&](EntityID id) {
            if (!m_world->Has<ParentComponent>(id))
                roots.push_back(id);
        });

        std::function<void(EntityID)> destroyRecursive = [&](EntityID e) {
            if (!m_world->IsAlive(e)) return;
            if (const auto* ch = m_world->Get<ChildrenComponent>(e))
            {
                const auto copy = ch->children;
                for (EntityID c : copy) destroyRecursive(c);
            }
            m_world->DestroyEntity(e);
        };

        for (EntityID root : roots)
        {
            if (!m_world->IsAlive(root))                        continue;
            if (m_world->Has<EditorPersistentComponent>(root))  continue; // bewahren
            destroyRecursive(root);
        }

        // 4. Selektion zurücksetzen wenn die selektierte Entity gelöscht wurde.
        if (m_editorState.selectedEntity.IsValid() &&
            !m_world->IsAlive(m_editorState.selectedEntity))
            m_editorState.selectedEntity = NULL_ENTITY;

        // Rotations-Edit-State zurücksetzen: veraltete Euler-Winkel aus der
        // vorherigen Szene würden sonst beim ersten Zugriff fälschlich übernommen.
        m_editorState.rotationEditInitialized = false;
        m_editorState.rotationEditEntity      = NULL_ENTITY;
        m_editorState.snapLastPos             = {};

        // 5. Auf neue (primäre) Szene wechseln und laden.
        m_currentSceneName = sceneName;
        m_loadedSceneNames = { sceneName };  // nur primäre Szene geladen
        RefreshSceneList();

        if (prepareOnly)
            return true;
        if (!LoadEditorScene(/*clearFirst=*/false, /*sceneTag=*/""))
        {
            if (m_editorFrameCtx)
                m_editorFrameCtx->lastFileMessage = Translate("scene.switch_failed") + sceneName;
            return false;
        }
        m_transformSystem.Update(*m_world);
        m_boundsSystem.Update(*m_world);

        if (m_editorFrameCtx)
        {
            ClearEditorHistory(m_editorFrameCtx->state);
            m_editorFrameCtx->lastFileMessage = Translate("scene.loaded_prefix") + sceneName;
        }

        Debug::LogVerbose("KromEditorApp: Szenenwechsel → '%s'", sceneName.c_str());
        return true;
    }
    else
    {
        // ── Additiver Ladevorgang ────────────────────────────────────────────
        // Primäre Szene (m_currentSceneName) bleibt unberührt.
        // sceneName wird on top der aktuellen World geladen.

        // Doppelt-Laden verhindern
        if (sceneName == m_currentSceneName ||
            std::find(m_loadedSceneNames.begin(), m_loadedSceneNames.end(), sceneName)
                != m_loadedSceneNames.end())
        {
            if (m_editorFrameCtx)
                m_editorFrameCtx->lastFileMessage = Translate("scene.already_loaded") + sceneName;
            return false;
        }

        m_loadedSceneNames.push_back(sceneName);
        RefreshLoadedScenesState();

        // sceneTag != "" → LoadEditorScene lädt aus GetEditorScenePath(sceneTag)
        // und taggt neue Entities mit diesem Namen (nicht m_currentSceneName).
        if (prepareOnly)
            return true;
        if (!LoadEditorScene(/*clearFirst=*/false, /*sceneTag=*/sceneName))
        {
            // Rückgängig machen wenn Laden fehlschlug
            m_loadedSceneNames.erase(
                std::remove(m_loadedSceneNames.begin(), m_loadedSceneNames.end(), sceneName),
                m_loadedSceneNames.end());
            RefreshLoadedScenesState();
            if (m_editorFrameCtx)
                m_editorFrameCtx->lastFileMessage = Translate("scene.additive_failed") + sceneName;
            return false;
        }

        m_transformSystem.Update(*m_world);
        m_boundsSystem.Update(*m_world);

        if (m_editorFrameCtx)
            m_editorFrameCtx->lastFileMessage = Translate("scene.additive_loaded") + sceneName;

        Debug::Log("KromEditorApp: Szene additiv geladen '%s' (Primär: '%s')",
                   sceneName.c_str(), m_currentSceneName.c_str());
        return true;
    }
}

bool KromEditorApp::UnloadEditorScene(const std::string& sceneName)
{
    if (!m_world || sceneName.empty())
        return false;

    // Die primäre Szene kann nicht per Unload entladen werden — dafür SwitchEditorScene nutzen.
    if (sceneName == m_currentSceneName)
    {
        Debug::LogWarning("KromEditorApp::UnloadEditorScene: primäre Szene '%s' kann nicht entladen werden.",
                          sceneName.c_str());
        return false;
    }

    using namespace renderer::addons::editor;

    // Alle Entities dieser Szene sammeln (via EditorSceneTagComponent)
    std::unordered_set<uint32_t> taggedIds;
    m_world->View<EditorSceneTagComponent>([&](EntityID id, const EditorSceneTagComponent& tag) {
        if (tag.sceneName == sceneName)
            taggedIds.insert(id.value);
    });

    auto isTagged = [&](EntityID id) { return taggedIds.count(id.value) > 0; };

    // Getaggte Entities von Eltern abtrennen die NICHT getaggt sind,
    // damit keine dangling ChildrenComponent-Einträge entstehen.
    m_world->View<EditorSceneTagComponent>([&](EntityID id, const EditorSceneTagComponent& tag) {
        if (tag.sceneName != sceneName || !m_world->IsAlive(id)) return;
        const auto* pc = m_world->Get<ParentComponent>(id);
        if (!pc || !pc->parent.IsValid()) return;
        if (!isTagged(pc->parent))
        {
            if (m_world->IsAlive(pc->parent))
                if (auto* cc = m_world->Get<ChildrenComponent>(pc->parent))
                    cc->Remove(id);
            m_world->Remove<ParentComponent>(id);
        }
    });

    // Rekursives Löschen: getaggte Entities mit ihren Kindern entfernen.
    // Nicht-getaggte Kinder (z.B. aus einer anderen Szene) bleiben erhalten —
    // sie wurden durch die Abtrennung oben schon zu Root-Entities gemacht.
    std::function<void(EntityID)> destroyTagged = [&](EntityID e) {
        if (!m_world->IsAlive(e)) return;
        if (const auto* ch = m_world->Get<ChildrenComponent>(e))
        {
            const auto copy = ch->children;
            for (EntityID c : copy)
                if (isTagged(c))
                    destroyTagged(c);
        }
        m_world->DestroyEntity(e);
    };

    // Root-Entities der Szene (kein getaggter Parent)
    std::vector<EntityID> roots;
    m_world->View<EditorSceneTagComponent>([&](EntityID id, const EditorSceneTagComponent& tag) {
        if (tag.sceneName != sceneName || !m_world->IsAlive(id)) return;
        const auto* pc = m_world->Get<ParentComponent>(id);
        if (!pc || !pc->parent.IsValid() || !isTagged(pc->parent))
            roots.push_back(id);
    });

    for (EntityID root : roots)
        if (m_world->IsAlive(root))
            destroyTagged(root);

    // Selektion zurücksetzen wenn die Entity gelöscht wurde
    if (m_editorState.selectedEntity.IsValid() &&
        !m_world->IsAlive(m_editorState.selectedEntity))
        m_editorState.selectedEntity = NULL_ENTITY;

    // Aus der Ladeliste entfernen
    m_loadedSceneNames.erase(
        std::remove(m_loadedSceneNames.begin(), m_loadedSceneNames.end(), sceneName),
        m_loadedSceneNames.end());
    RefreshLoadedScenesState();

    if (m_editorFrameCtx)
        m_editorFrameCtx->lastFileMessage = Translate("scene.unloaded") + sceneName;

    Debug::Log("KromEditorApp: Szene '%s' entladen (%u Entities entfernt).",
               sceneName.c_str(), static_cast<uint32_t>(taggedIds.size()));
    return true;
}

// =============================================================================
// Kscene-Export (Build-System)
// =============================================================================

namespace {

// .kscene = SceneSerializer-Format der Editor-.json, ohne Editor-Komponenten,
// plus Runtime-Metadaten (version, persistent, environment). Neue Komponenten
// brauchen damit nur ihre Serialisierungs-Handler — kein eigenes Export-Schema.
constexpr int32_t kKsceneVersion = 2;

// Gleiche Schluessel wie der "environment"-Block der Editor-.json.
struct KsceneEnvironment
{
    std::string          texturePath;
    bool                 enableIBL = false;
    float                iblIntensity = 1.f;
    std::array<float, 4> backgroundColor{0.f, 0.f, 0.f, 1.f};
    math::Vec3           ambientColor{0.06f, 0.06f, 0.08f};
    float                ambientIntensity = 1.f;
    renderer::addons::forward::TonemapSettings tonemap{};
    uint32_t             msaaSamples = 1u;
    bool                 specularOcclusion = false;   // probe add-on; scenes without the key: off
    uint32_t             probeBakePasses = 2u;        // probe add-on
};

static KsceneEnvironment EnvironmentFromEditorState(const renderer::addons::editor::EditorState& state)
{
    KsceneEnvironment env;
    env.texturePath      = state.environmentTexturePath;
    env.enableIBL        = state.environmentEnableIBL;
    env.iblIntensity     = state.environmentIntensity;
    for (size_t i = 0; i < 4; ++i) env.backgroundColor[i] = state.backgroundColor[i];
    env.ambientColor     = state.ambientColor;
    env.ambientIntensity = state.ambientIntensity;
    env.tonemap          = state.tonemap;
    env.msaaSamples      = state.msaaSamples;
    env.specularOcclusion = state.specularOcclusion;   // probe add-on
    env.probeBakePasses = state.probeBakePasses;       // probe add-on
    return env;
}

// Liest den Environment-Block einer Editor-.json; false wenn keiner vorhanden ist.
static bool EnvironmentFromSceneJson(const serialization::JsonValue& root, KsceneEnvironment& out)
{
    const serialization::JsonValue* env = root.Get("environment");
    if (!env || !env->IsObject())
        return false;

    if (const auto* path = env->Get("texturePath"); path && path->IsString())
        out.texturePath = path->AsString();
    if (const auto* enable = env->Get("enableIBL"); enable && enable->IsBool())
        out.enableIBL = enable->AsBool();
    if (const auto* intensity = env->Get("iblIntensity"); intensity && intensity->IsNumber())
        out.iblIntensity = intensity->AsFloat();
    if (const auto* bg = env->Get("backgroundColor"); bg && bg->IsArray() && bg->arrayVal.size() >= 3u)
        for (size_t i = 0; i < 4; ++i)
            out.backgroundColor[i] = i < bg->arrayVal.size() ? bg->At(i).AsFloat() : 1.f;
    if (const auto* ambient = env->Get("ambientColor"); ambient && ambient->IsArray())
        out.ambientColor = ambient->AsVec3();
    if (const auto* ambientIntensity = env->Get("ambientIntensity"); ambientIntensity && ambientIntensity->IsNumber())
        out.ambientIntensity = ambientIntensity->AsFloat();
    out.tonemap.op = renderer::addons::forward::kLegacyTonemapOperator;
    if (const auto* op = env->Get("tonemapOperator"); op && op->IsString())
        (void)renderer::addons::forward::ParseTonemapOperator(op->AsString().c_str(), out.tonemap.op);
    if (const auto* ev = env->Get("exposureEv"); ev && ev->IsNumber())
        out.tonemap.exposureEv = ev->AsFloat();
    out.msaaSamples = 1u;
    if (const auto* msaa = env->Get("msaaSamples"); msaa && msaa->IsNumber())
        out.msaaSamples = renderer::addons::forward::NormalizeMsaaSamples(static_cast<uint32_t>(std::max(0, msaa->AsInt())));
    if (const auto* so = env->Get("specularOcclusion"); so && so->IsBool())
        out.specularOcclusion = so->AsBool();   // probe add-on
    if (const auto* passes = env->Get("probeBakePasses"); passes && passes->IsNumber())
        out.probeBakePasses = passes->AsInt() >= 2 ? 2u : 1u;   // probe add-on
    return true;
}

static void WriteKsceneEnvironment(serialization::JsonWriter& w, const KsceneEnvironment& env)
{
    w.BeginObject("environment");
    w.WriteString("texturePath",  env.texturePath);
    w.WriteBool  ("enableIBL",    env.enableIBL);
    w.WriteFloat ("iblIntensity", env.iblIntensity);
    w.BeginArray ("backgroundColor");
    for (float v : env.backgroundColor) w.WriteFloat("", v);
    w.EndArray();
    w.BeginArray ("ambientColor");
    w.WriteFloat("", env.ambientColor.x);
    w.WriteFloat("", env.ambientColor.y);
    w.WriteFloat("", env.ambientColor.z);
    w.EndArray();
    w.WriteFloat ("ambientIntensity", env.ambientIntensity);
    w.WriteString("tonemapOperator", renderer::addons::forward::TonemapOperatorName(env.tonemap.op));
    w.WriteFloat ("exposureEv", env.tonemap.exposureEv);
    w.WriteInt   ("msaaSamples", static_cast<int32_t>(env.msaaSamples));
    w.WriteBool  ("specularOcclusion", env.specularOcclusion);   // probe add-on
    w.WriteInt   ("probeBakePasses", static_cast<int32_t>(env.probeBakePasses));   // probe add-on
    w.EndObject();
}

// Konvertiert eine World in das .kscene-JSON (Runtime-Format).
// Wird sowohl vom Live-Export (aktuelle World) als auch vom Batch-Build
// (temporäre World pro Datei) genutzt.
static std::string WorldToKsceneJson(const ecs::World& world,
                                     const std::string& sceneName,
                                     const KsceneEnvironment* env,
                                     std::function<bool(EntityID)> entityFilter = nullptr)
{
    serialization::SceneSerializer serializer(world);
    serializer.RegisterDefaultHandlers();
    addons::camera::RegisterCameraSerializationHandlers(serializer);
    addons::lighting::RegisterLightingSerializationHandlers(serializer);
    addons::mesh_renderer::RegisterMeshRendererSerializationHandlers(serializer);
    addons::prefab::RegisterPrefabSerializationHandlers(serializer);
    engine::script::RegisterScriptSerializationHandlers(serializer);
    engine::physics::RegisterPhysicsSerializationHandlers(serializer);
    engine::audio::RegisterAudioSerializationHandlers(serializer);
    engine::particles::RegisterParticleSerializationHandlers(serializer);
    engine::addons::probes::RegisterProbeSerializationHandlers(serializer);
    // Editor-Komponenten bewusst nicht registriert — die Runtime kennt sie nicht.

    if (entityFilter)
        serializer.SetEntityFilter(std::move(entityFilter));
    serializer.SetRootMetadataWriter([env](serialization::JsonWriter& w) {
        w.WriteInt("version", kKsceneVersion);
        w.WriteBool("persistent", false);
        if (env)
            WriteKsceneEnvironment(w, *env);
    });
    return serializer.SerializeToJson(sceneName);
}

static bool WriteKsceneFile(const std::string& json,
                             const std::filesystem::path& outPath,
                             const char* debugLabel)
{
    if (!WriteTextFileAtomically(outPath, json))
    {
        Debug::LogError("KromEditorApp: %s: Schreibfehler: %s",
            debugLabel, PathToUtf8(outPath).c_str());
        return false;
    }
    Debug::Log("KromEditorApp: %s → %s", debugLabel, PathToUtf8(outPath).c_str());
    return true;
}

} // namespace

// Exportiert die aktuell geladene World (schneller Pfad für "aktuelle Szene").
bool KromEditorApp::ExportAsKscene(const std::filesystem::path& outPath)
{
    if (!m_world)
        return false;
    EnsureEntityGuids(*m_world);

    const std::string sceneName = m_currentSceneName.empty() ? "Scene" : m_currentSceneName;
    std::optional<KsceneEnvironment> env;
    if (m_editorFrameCtx)
        env = EnvironmentFromEditorState(m_editorFrameCtx->state);

    const std::string filterSceneName = m_currentSceneName;
    const std::string json = WorldToKsceneJson(*m_world, sceneName, env ? &*env : nullptr,
        [this, filterSceneName](EntityID id) {
            return IsEntityPartOfScene(*m_world, id, filterSceneName);
        });
    const bool ok = WriteKsceneFile(json, outPath, "ExportAsKscene");
    if (ok && m_editorFrameCtx)
        m_editorFrameCtx->lastFileMessage = Translate("export.success") + PathToUtf8(outPath.filename());
    return ok;
}

// Liest eine editor-.json-Datei, deserialisiert in eine temporäre World und schreibt .kscene.
// Berührt weder m_world noch die Asset-Pipeline — safe für den Batch-Build.
bool KromEditorApp::ExportSceneFileAsKscene(const std::filesystem::path& editorJsonPath,
                                             const std::filesystem::path& kscenePath,
                                             const std::string& sceneName)
{
    std::ifstream in(editorJsonPath, std::ios::binary);
    if (!in)
    {
        Debug::LogError("KromEditorApp: Build: Editor-JSON nicht lesbar: %s",
            PathToUtf8(editorJsonPath).c_str());
        return false;
    }
    const std::string editorJson(std::istreambuf_iterator<char>(in), {});
    const auto json = MakeKsceneJson(editorJson, sceneName);
    return json && WriteKsceneFile(*json, kscenePath, "Build");
}

std::optional<std::string> KromEditorApp::MakeKsceneJson(const std::string& json,
                                                         const std::string& sceneName)
{
    std::string error;
    const auto root = ParseEditorScene(json, error);
    if (!error.empty())
    {
        Debug::LogError("KromEditorApp: Build: Szene '%s' konnte nicht gelesen werden: %s",
                        sceneName.c_str(), error.c_str());
        return std::nullopt;
    }

    // Temporäre World — keine GPU-Ressourcen, nur Komponenten-Daten
    ecs::World tempWorld(m_componentRegistry);

    serialization::SceneDeserializer deser(tempWorld);
    deser.RegisterDefaultHandlers();
    addons::camera::RegisterCameraDeserializationHandlers(deser);
    addons::lighting::RegisterLightingDeserializationHandlers(deser);
    addons::mesh_renderer::RegisterMeshRendererDeserializationHandlers(deser);
    addons::prefab::RegisterPrefabDeserializationHandlers(deser);
    engine::script::RegisterScriptDeserializationHandlers(deser, &m_scriptRegistry);
    engine::physics::RegisterPhysicsDeserializationHandlers(deser);
    engine::audio::RegisterAudioDeserializationHandlers(deser);
    engine::particles::RegisterParticleDeserializationHandlers(deser);
    engine::addons::probes::RegisterProbeDeserializationHandlers(deser);
    // Editor-Komponenten bewusst nicht registriert — wir brauchen sie nicht

    const auto result = deser.DeserializeFromValue(root);
    if (!result.success)
    {
        Debug::LogError("KromEditorApp: Build: Deserialisierung fehlgeschlagen '%s': %s",
            sceneName.c_str(), result.error.c_str());
        return std::nullopt;
    }
    engine::script::ResolveScriptEntityReferences(tempWorld, m_scriptRegistry);
    EnsureEntityGuids(tempWorld);

    // Environment kommt aus der jeweiligen Szenendatei, nicht aus der gerade offenen Szene.
    KsceneEnvironment env;
    const bool hasEnv = EnvironmentFromSceneJson(root, env);

    return WorldToKsceneJson(tempWorld, sceneName, hasEnv ? &env : nullptr);
}

// Exportiert alle nicht-ausgeschlossenen Szenen des Projekts — der eigentliche Build.
bool KromEditorApp::BuildAllScenes()
{
    if (m_projectRoot.empty())
    {
        Debug::LogError("KromEditorApp: Build: Kein Projekt geöffnet");
        if (m_editorFrameCtx)
            m_editorFrameCtx->lastFileMessage = Translate("build.no_project");
        return false;
    }

    // Aktuelle Szene sichern bevor wir iterieren
    if (!SaveEditorScene())
        return false;

    const auto scenes = ListEditorScenes();
    int exported = 0, skipped = 0, failed = 0;

    for (const std::string& name : scenes)
    {
        if (m_buildExcludedScenes.count(name))
        {
            Debug::Log("KromEditorApp: Build: übersprungen (excluded): %s", name.c_str());
            ++skipped;
            continue;
        }

        const std::filesystem::path jsonPath  = m_projectRoot / "Assets" / "Scenes" / PathFromUtf8(name + ".json");
        const std::filesystem::path kscenePath = m_projectRoot / "Assets" / "Scenes" / PathFromUtf8(name + ".kscene");

        if (ExportSceneFileAsKscene(jsonPath, kscenePath, name))
            ++exported;
        else
            ++failed;
    }

    const std::string msg = TranslateWithValue("build.exported_count", std::to_string(exported))
        + (skipped > 0 ? TranslateWithValue("build.skipped_count", std::to_string(skipped)) : "")
        + (failed > 0 ? TranslateWithValue("build.failed_count", std::to_string(failed)) : "")
        + ".";

    Debug::Log("KromEditorApp: %s", msg.c_str());
    if (m_editorFrameCtx)
        m_editorFrameCtx->lastFileMessage = msg;
    return failed == 0 && exported > 0;
}

bool KromEditorApp::ExportProjectBuild()
{
    if (m_projectRoot.empty())
    {
        if (m_editorFrameCtx)
            m_editorFrameCtx->lastFileMessage = Translate("export.no_project");
        return false;
    }

    if (m_editorFrameCtx)
    {
        const auto bindings = renderer::addons::editor::RefreshCppScriptProject(*m_editorFrameCtx);
        if (!bindings.ok)
        {
            m_editorFrameCtx->lastFileMessage = bindings.message.empty()
                ? Translate("export.bindings_failed")
                : bindings.message;
            return false;
        }
    }

    if (!BuildAllScenes())
        return false;

    const char* profile = m_projectSettings.exportBuild == 1 ? "RelWithDebInfo" : "Release";
    const std::filesystem::path buildDir = RuntimeProfileDir(profile);
    return StartRuntimeBuild(buildDir, [this, buildDir](bool ok) {
        if (!ok)
        {
            if (m_editorFrameCtx)
                m_editorFrameCtx->lastFileMessage = Translate("export.build_failed");
            return;
        }
        (void)FinishProjectExport(buildDir);
    }, {}, profile);
}

// Zweiter Teil des Exports, nach erfolgreichem Runtime-Build (Main-Thread).
bool KromEditorApp::FinishProjectExport(const std::filesystem::path& buildDir)
{
    const std::filesystem::path exe = RuntimeExecutablePath(buildDir, m_projectRuntimeTarget, m_projectBackend);
    if (!std::filesystem::exists(exe))
    {
        if (m_editorFrameCtx)
            m_editorFrameCtx->lastFileMessage = Translate("export.exe_missing");
        return false;
    }

    const std::string configName = PathToUtf8(buildDir.filename());
    const std::filesystem::path exportDir =
        m_projectRoot / "Build" / PathFromUtf8(configName + "-" + RuntimeBackendId(m_projectBackend));

    std::error_code ec;
    std::filesystem::create_directories(exportDir, ec);
    if (ec)
    {
        if (m_editorFrameCtx)
            m_editorFrameCtx->lastFileMessage = Translate("export.folder_failed");
        return false;
    }

    std::filesystem::copy_file(exe, exportDir / exe.filename(),
        std::filesystem::copy_options::overwrite_existing, ec);
    if (ec)
    {
        if (m_editorFrameCtx)
            m_editorFrameCtx->lastFileMessage = Translate("export.exe_copy_failed");
        return false;
    }

    // Layout:  <Spiel>.exe | krom-project.json | assets/ (Engine) | Content/ (Projekt-Assets)
    // Projekt-Assets liegen bewusst nicht in "Assets": unter Windows waere das derselbe
    // Ordner wie die Engine-"assets", die die Runtime neben der EXE sucht.
    const std::string exportAssetRoot = "Content";
    {
        std::ifstream in(m_projectRoot / "krom-project.json", std::ios::binary);
        std::string projectJson((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        if (!in && projectJson.empty())
        {
            if (m_editorFrameCtx)
                m_editorFrameCtx->lastFileMessage = Translate("export.project_read_failed");
            return false;
        }
        projectJson = std::regex_replace(projectJson,
            std::regex(R"("assetRoot"\s*:\s*"[^"]*")"),
            "\"assetRoot\":\"" + exportAssetRoot + "\"");
        std::ofstream out(exportDir / "krom-project.json", std::ios::binary | std::ios::trunc);
        out << projectJson;
        if (!out.good())
        {
            if (m_editorFrameCtx)
                m_editorFrameCtx->lastFileMessage = Translate("export.project_write_failed");
            return false;
        }
    }

    // Alte Exporte aufraeumen, damit geloeschte Assets nicht liegen bleiben.
    std::filesystem::remove_all(exportDir / PathFromUtf8(exportAssetRoot), ec);
    std::filesystem::remove_all(exportDir / "assets", ec);
    ec.clear();

    // Projekt-Assets ohne Script-Quellcode (der steckt bereits in der EXE).
    const std::filesystem::path sourceAssets = m_projectRoot / "Assets";
    for (const auto& entry : std::filesystem::directory_iterator(sourceAssets, ec))
    {
        if (entry.is_directory() && entry.path().filename() == "Scripts")
            continue;
        const std::filesystem::path target = exportDir / PathFromUtf8(exportAssetRoot) / entry.path().filename();
        constexpr auto options = std::filesystem::copy_options::recursive | std::filesystem::copy_options::overwrite_existing;
        if (entry.is_directory() && entry.path().filename() == "Scenes")
        {
            std::filesystem::create_directories(target, ec);
            if (ec) break;
            for (const auto& scene : std::filesystem::directory_iterator(entry.path(), ec))
            {
                const auto extension = scene.path().extension();
                if ((extension == ".json" || extension == ".kscene") &&
                    m_buildExcludedScenes.count(PathToUtf8(scene.path().stem())))
                    continue;
                std::filesystem::copy(scene.path(), target / scene.path().filename(), options, ec);
                if (ec) break;
            }
        }
        else
        {
            std::filesystem::create_directories(target.parent_path(), ec);
            if (!ec) std::filesystem::copy(entry.path(), target, options, ec);
        }
        if (ec)
            break;
    }
    if (ec)
    {
        if (m_editorFrameCtx)
            m_editorFrameCtx->lastFileMessage = Translate("export.assets_failed");
        return false;
    }

    // Engine-Assets (Shader usw.), die der Build neben die Runtime-EXE kopiert.
    std::filesystem::copy(exe.parent_path() / "assets", exportDir / "assets",
        std::filesystem::copy_options::recursive |
        std::filesystem::copy_options::overwrite_existing, ec);
    if (ec)
    {
        if (m_editorFrameCtx)
            m_editorFrameCtx->lastFileMessage = Translate("export.engine_assets_failed");
        return false;
    }

    // DLLs neben der Runtime: d3dcompiler_47 (DX11) und die MSVC-Laufzeit, die der
    // Build app-lokal mitkopiert - so startet das Spiel ohne installiertes VC-Redist.
    for (const auto& entry : std::filesystem::directory_iterator(exe.parent_path(), ec))
    {
        if (!entry.is_regular_file() || ToLowerAscii(PathToUtf8(entry.path().extension())) != ".dll")
            continue;
        std::error_code copyEc;
        std::filesystem::copy_file(entry.path(), exportDir / entry.path().filename(),
            std::filesystem::copy_options::overwrite_existing, copyEc);
    }
    ec.clear();

    // Shader vorkompilieren: die exportierte Runtime laedt unsichtbar jede Szene und
    // fuellt dabei shader-bin/ (und den IBL-Cache). Auf dem Ziel-PC wird dann nur
    // noch geladen — dort gibt es in der Regel kein dxc.exe aus dem Vulkan-SDK.
    std::filesystem::remove_all(exportDir / "shader-bin", ec);
    std::filesystem::remove_all(exportDir / ".krom", ec);
    std::filesystem::remove(exportDir / "krom.log", ec);
    ec.clear();
    if (m_editorFrameCtx)
        AddEditorConsoleLog(&m_editorFrameCtx->state, engine::LogLevel::Info,
            Translate("export.warmup"));
    if (!RunShaderWarmup(exportDir / exe.filename(), exportDir / "krom-project.json",
                         m_editorFrameCtx ? &m_editorFrameCtx->state : nullptr))
    {
        if (m_editorFrameCtx)
            m_editorFrameCtx->lastFileMessage = Translate("export.warmup_failed");
        return false;
    }

    if (m_editorFrameCtx)
    {
        m_editorFrameCtx->lastFileMessage = Translate("export.success") + PathToUtf8(exportDir);
        AddEditorConsoleLog(&m_editorFrameCtx->state, engine::LogLevel::Info,
            "[Export] " + PathToUtf8(exportDir));
    }
    return true;
}

bool KromEditorApp::StartRuntimeBuild(const std::filesystem::path& buildDir,
                                      std::function<void(bool ok)> onFinished,
                                      const std::string& scriptTarget,
                                      const char* configuration)
{
    if (m_runtimeBuildJob)
    {
        if (m_editorFrameCtx)
            m_editorFrameCtx->lastFileMessage = Translate("build.already_running");
        return false;
    }

    RuntimeBuildRequest request;
    if (!PrepareRuntimeBuild(m_projectRuntimeTarget, m_projectBackend, buildDir,
                             m_editorFrameCtx ? &m_editorFrameCtx->state : nullptr, request,
                             scriptTarget, m_projectRoot, configuration))
    {
        if (m_editorFrameCtx)
            m_editorFrameCtx->lastFileMessage = Translate("build.start_failed");
        return false;
    }

    auto job = std::make_shared<RuntimeBuildJob>();
    job->scriptBuild = !scriptTarget.empty();
    job->target      = request.target;
    job->projectRoot = m_projectRoot;
    job->startTime   = std::chrono::steady_clock::now();
    job->lastTerminalReport = job->startTime;
    job->onFinished  = std::move(onFinished);
    job->worker = std::thread([job, request = std::move(request)]() {
        const std::function<void(LogLevel, std::string)> log = [&job](LogLevel level, std::string line) {
            std::lock_guard<std::mutex> lock(job->mutex);
            job->logs.emplace_back(level, std::move(line));
        };
        std::vector<engine::app::build::ProcessCommand> commands;
        const bool ok = ResolveRuntimeBuildCommands(request, log, commands) &&
            RunRuntimeBuildCommands(commands, log,
            [&job](std::string line) {
                std::lock_guard<std::mutex> lock(job->mutex);
                job->progress = std::move(line);
            }, job->cancel);
        job->ok = ok;
        job->finished.store(true, std::memory_order_release);
    });
    m_runtimeBuildJob = std::move(job);

    Debug::LogVerbose("KromEditorApp: Runtime-Build gestartet (%s) - Fortschritt im Editor.",
               m_runtimeBuildJob->target.c_str());
    if (m_editorFrameCtx)
    {
        m_editorFrameCtx->runtimeBuildActive = true;
        m_editorFrameCtx->scriptBuildActive = m_runtimeBuildJob->scriptBuild;
        m_editorFrameCtx->cancelBuild = [job = m_runtimeBuildJob] { job->cancel.store(true, std::memory_order_release); };
        m_editorFrameCtx->runtimeBuildSuccessSeconds = 0.f;
        m_editorFrameCtx->runtimeBuildStatus = Translate("build.starting");
        m_editorFrameCtx->lastFileMessage    = Translate("build.running");
    }
    return true;
}

void KromEditorApp::PollRuntimeBuild()
{
    if (!m_runtimeBuildJob)
        return;
    RuntimeBuildJob& job = *m_runtimeBuildJob;

    // finished zuerst lesen: alles, was der Worker davor geloggt hat, ist dann sichtbar.
    const bool finished = job.finished.load(std::memory_order_acquire);
    std::vector<std::pair<LogLevel, std::string>> logs;
    std::string progress;
    {
        std::lock_guard<std::mutex> lock(job.mutex);
        logs.swap(job.logs);
        progress = job.progress;
    }
    for (auto& [level, line] : logs)
        AddEditorConsoleLog(m_editorFrameCtx ? &m_editorFrameCtx->state : nullptr,
                            level, "[Build] " + line);

    const auto now = std::chrono::steady_clock::now();
    const long long seconds =
        std::chrono::duration_cast<std::chrono::seconds>(now - job.startTime).count();

    if (!finished)
    {
        if (m_editorFrameCtx)
        {
            std::string status = TranslateWithValue("build.progress", job.target + ", " + std::to_string(seconds) + " s");
            if (!progress.empty())
                status += "\n" + progress;
            m_editorFrameCtx->runtimeBuildActive = true;
            m_editorFrameCtx->runtimeBuildStatus = std::move(status);
        }
        // Auch im Terminal sichtbar, aber gedrosselt.
        if (now - job.lastTerminalReport >= std::chrono::seconds(10))
        {
            job.lastTerminalReport = now;
            Debug::LogVerbose("KromEditorApp: Runtime-Build laeuft (%lld s) %s",
                       seconds, progress.c_str());
        }
        return;
    }

    if (job.worker.joinable())
        job.worker.join();
    const bool ok = job.ok;
    const bool cancelled = job.cancel.load(std::memory_order_acquire);
    const bool sameProject = job.projectRoot == m_projectRoot;
    std::function<void(bool)> onFinished = std::move(job.onFinished);
    m_runtimeBuildJob.reset();

    if (m_editorFrameCtx)
    {
        m_editorFrameCtx->runtimeBuildActive = false;
        m_editorFrameCtx->scriptBuildActive = false;
        m_editorFrameCtx->cancelBuild = {};
        m_editorFrameCtx->runtimeBuildStatus.clear();
        if (sameProject && ok)
        {
            m_editorFrameCtx->runtimeBuildStatus = TranslateWithValue("build.success_duration", std::to_string(seconds));
            m_editorFrameCtx->runtimeBuildSuccessSeconds = 2.5f;
        }
        else if (sameProject && !cancelled)
        {
            m_editorFrameCtx->consolePanelVisible = true;
            m_editorFrameCtx->state.consoleFocusRequest = true;
        }
    }
    Debug::LogVerbose("KromEditorApp: Runtime-Build %s nach %lld s.",
                      ok ? "erfolgreich" : (cancelled ? "abgebrochen" : "fehlgeschlagen"), seconds);

    if (!sameProject)
    {
        if (m_editorFrameCtx)
            m_editorFrameCtx->lastFileMessage =
                Translate("build.project_changed");
        return;
    }
    if (onFinished)
        onFinished(ok);
}

bool KromEditorApp::LaunchPlayRuntime(const std::filesystem::path& buildDir)
{
    const std::filesystem::path exe = RuntimeExecutablePath(buildDir, m_projectRuntimeTarget, m_projectBackend);
    const std::filesystem::path projectFile = m_projectRoot / "krom-project.json";
    if (m_editorFrameCtx)
    {
        m_editorFrameCtx->state.runtimeScriptFieldValues.clear();
        m_editorFrameCtx->state.runtimeStateLastError.clear();
        m_editorFrameCtx->state.runtimeStatePollTimer = 0.0;
        m_editorFrameCtx->state.runtimeStateFilePath =
            PathToUtf8(m_projectRoot / "editor" / "runtime-state.json");
        std::error_code ec;
        std::filesystem::remove(m_projectRoot / "editor" / "runtime-state.json", ec);
    }
    const bool launched = LaunchRuntimeProcess(exe, projectFile, true);
    if (m_editorFrameCtx)
        m_editorFrameCtx->lastFileMessage = launched
            ? Translate("play.window_started")
            : Translate("play.window_failed");
    return launched;
}

void KromEditorApp::RefreshBuildExcludeState()
{
    if (!m_editorFrameCtx) return;
    m_editorFrameCtx->buildExcludedScenes.assign(
        m_buildExcludedScenes.begin(), m_buildExcludedScenes.end());
}

bool KromEditorApp::NewEditorScene(const std::string& sceneName)
{
    if (sceneName.empty())
        return false;
    // Switching scenes ends Play (as switchEditorScene does); without it the
    // save guard rejects the new scene while this function reported success.
    StopEditorPlay();

    std::string finalSceneName = sceneName;
    if (!m_projectRoot.empty())
    {
        const std::filesystem::path scenesDir = m_projectRoot / "Assets" / "Scenes";
        const std::filesystem::path scenePath =
            renderer::addons::editor::MakeUniqueFilesystemPath(scenesDir, sceneName, ".json");
        finalSceneName = PathToUtf8(scenePath.stem());
    }

    SwitchEditorScene(finalSceneName, /*additive=*/false);

    if (std::find(m_sceneOrder.begin(), m_sceneOrder.end(), finalSceneName) == m_sceneOrder.end())
        m_sceneOrder.push_back(finalSceneName);

    SaveEditorScene();
    RefreshSceneList();

    if (m_editorFrameCtx)
        m_editorFrameCtx->lastFileMessage = Translate("scene.created_prefix") + finalSceneName;

    Debug::Log("KromEditorApp: neue Szene erstellt: '%s'", finalSceneName.c_str());
    return true;
}

bool KromEditorApp::NewEditorSceneInDir(const std::string& sceneName,
                                        const std::string& targetDirStr)
{
    if (sceneName.empty())
        return false;

    const std::filesystem::path targetDir = PathFromUtf8(targetDirStr);
    std::error_code ec;
    std::filesystem::create_directories(targetDir, ec);
    if (ec)
        return false;

    const std::filesystem::path sceneFile =
        renderer::addons::editor::MakeUniqueFilesystemPath(targetDir, sceneName, ".json");
    const std::string finalSceneName = PathToUtf8(sceneFile.stem());

    // Nur die leere Datei schreiben — kein Szenen-Switching, kein SaveProjectFile.
    // Der User kann die neue Szene per Doppelklick im Asset-Browser laden.
    std::ofstream f(sceneFile, std::ios::binary | std::ios::trunc);
    if (!f)
        return false;
    f << "{\"entities\":[]}\n";

    if (m_editorFrameCtx)
        m_editorFrameCtx->lastFileMessage =
            Translate("scene.created_prefix") + PathToUtf8(sceneFile.filename());

    Debug::Log("KromEditorApp: Szene '%s' erstellt in '%s'",
               finalSceneName.c_str(), targetDirStr.c_str());
    return true;
}

namespace {

struct SceneReferenceRename
{
    std::string oldName;
    std::string newName;

    std::string Replacement(std::string value) const
    {
        if (!value.ends_with(".json") && !value.ends_with(".kscene"))
            return {};
        std::replace(value.begin(), value.end(), '\\', '/');
        auto path = PathFromUtf8(value).lexically_normal();
        const auto parent = path.parent_path();
        if ((parent != "Scenes" && parent != "Assets/Scenes") || path.stem() != PathFromUtf8(oldName))
            return {};
        path.replace_filename(PathFromUtf8(newName + PathToUtf8(path.extension())));
        return PathToGenericUtf8(path);
    }

    bool UpdateScriptFields(std::string& json, const serialization::JsonValue& entities) const
    {
        std::vector<std::pair<const serialization::JsonValue*, std::string>> edits;
        for (const auto& entity : entities.arrayVal)
        {
            const auto* components = entity.Get("components");
            if (!components || !components->IsArray())
                continue;
            for (const auto& component : components->arrayVal)
            {
                const auto* type = component.Get("type");
                const auto* scripts = component.Get("scripts");
                if (!type || type->AsString() != "ScriptList" || !scripts || !scripts->IsArray())
                    continue;
                for (const auto& script : scripts->arrayVal)
                {
                    const auto* fields = script.Get("fields");
                    if (!fields || !fields->IsObject())
                        continue;
                    for (const auto& [name, value] : fields->objectVal)
                    {
                        if (!value.IsString())
                            continue;
                        auto replacement = Replacement(value.AsString());
                        if (!replacement.empty())
                            edits.emplace_back(&value, "\"" + EscapeJsonString(replacement) + "\"");
                    }
                }
            }
        }
        // Keep unrelated JSON, including unknown components, byte for byte.
        std::sort(edits.begin(), edits.end(), [](const auto& a, const auto& b) {
            return a.first->stringOffset > b.first->stringOffset;
        });
        for (const auto& [value, text] : edits)
            json.replace(value->stringOffset, value->stringLength, text);
        return !edits.empty();
    }
};

bool ReadOptionalSceneFile(const std::filesystem::path& path, std::optional<std::string>& text)
{
    std::error_code ec;
    if (!std::filesystem::exists(path, ec))
    {
        text.reset();
        return !ec;
    }
    if (!std::filesystem::is_regular_file(path, ec) || ec)
        return false;
    std::ifstream input(path, std::ios::binary);
    if (!input)
        return false;
    try { text.emplace(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()); }
    catch (const std::ios_base::failure&) { return false; }
    return !input.bad();
}

struct SceneFileChange
{
    std::filesystem::path path;
    std::optional<std::string> before;
    std::optional<std::string> after;

    bool Replace(const std::optional<std::string>& expected,
                 const std::optional<std::string>& contents) const
    {
        std::optional<std::string> current;
        if (!ReadOptionalSceneFile(path, current) || current != expected)
            return false;
        if (contents)
            return WriteTextFileAtomically(path, *contents);
        std::error_code ec;
        std::filesystem::remove(path, ec);
        return !ec;
    }
};

} // namespace

bool KromEditorApp::RenameEditorScene(const std::string& oldName, const std::string& newName)
{
    if (oldName.empty() || newName.empty() || oldName == newName)
        return false;
    if (m_projectRoot.empty() || m_sceneLoadJob || !m_pendingSceneSwitch.empty() || m_runtimeBuildJob)
        return false;

    const std::filesystem::path oldPath = GetEditorScenePath(oldName);
    const std::filesystem::path newPath = GetEditorScenePath(newName);
    auto newExport = newPath;
    newExport.replace_extension(".kscene");
    const auto fail = [this](const std::string& detail) {
        Debug::LogError("KromEditorApp::RenameEditorScene: %s", detail.c_str());
        if (m_editorFrameCtx)
            m_editorFrameCtx->lastFileMessage = Translate("common.rename_failed_prefix") + detail;
        return false;
    };
    if (newName.find_first_of("/\\:*?\"<>|") != std::string::npos ||
        newName == "." || newName == "..")
        return fail(newName);

    std::error_code ec;
    for (const auto& path : {newPath, newExport})
    {
        if (std::filesystem::exists(path, ec))
        {
            if (m_editorFrameCtx)
                m_editorFrameCtx->lastFileMessage = TranslateWithValue("common.rename_exists", PathToUtf8(path.filename()));
            return false;
        }
        if (ec)
            return fail(PathToUtf8(path) + " - " + ec.message());
    }

    // Capture unsaved changes before rewriting references on disk.
    if (!SaveEditorScene())
        return false;

    const SceneReferenceRename rename{oldName, newName};
    std::vector<SceneFileChange> changes;
    std::vector<SceneFileChange> obsolete;
    auto sceneNames = ListEditorScenes();
    if (std::find(sceneNames.begin(), sceneNames.end(), oldName) == sceneNames.end())
        sceneNames.push_back(oldName);
    for (const auto& scene : sceneNames)
    {
        const bool renamed = scene == oldName;
        const auto scenePath = GetEditorScenePath(scene);
        std::optional<std::string> original;
        if (!ReadOptionalSceneFile(scenePath, original) || !original)
            return fail(PathToUtf8(scenePath));

        std::string parseError;
        const auto root = ParseEditorScene(*original, parseError);
        if (!parseError.empty())
            return fail(PathToUtf8(scenePath) + " - " + parseError);

        std::string updated = *original;
        const bool referencesChanged = rename.UpdateScriptFields(updated, *root.Get("entities"));
        if (!renamed && !referencesChanged)
            continue;

        auto exportPath = scenePath;
        exportPath.replace_extension(".kscene");
        std::optional<std::string> originalExport;
        if (!ReadOptionalSceneFile(exportPath, originalExport))
            return fail(PathToUtf8(exportPath));
        if (originalExport)
        {
            auto exported = MakeKsceneJson(updated, renamed ? newName : scene);
            if (!exported)
                return fail(PathToUtf8(exportPath));
            if (renamed)
            {
                changes.push_back({newExport, std::nullopt, std::move(exported)});
                obsolete.push_back({exportPath, std::move(originalExport), std::nullopt});
            }
            else
                changes.push_back({exportPath, std::move(originalExport), std::move(exported)});
        }

        if (renamed)
        {
            changes.push_back({newPath, std::nullopt, std::move(updated)});
            obsolete.push_back({oldPath, std::move(original), std::nullopt});
        }
        else
            changes.push_back({scenePath, std::move(original), std::move(updated)});
    }

    std::string current = m_currentSceneName == oldName ? newName : m_currentSceneName;
    auto order = m_sceneOrder;
    std::replace(order.begin(), order.end(), oldName, newName);
    auto excluded = m_buildExcludedScenes;
    if (excluded.erase(oldName))
        excluded.insert(newName);

    if (m_editorFrameCtx)
    {
        const auto settingsPath = m_projectRoot / "editor" / "editor.settings";
        std::optional<std::string> originalSettings;
        if (!ReadOptionalSceneFile(settingsPath, originalSettings))
            return fail(PathToUtf8(settingsPath));
        changes.push_back({settingsPath, std::move(originalSettings),
                           EditorSettingsJson(m_editorFrameCtx->state, current)});
    }
    for (auto& file : obsolete)
        changes.push_back(std::move(file));

    size_t applied = 0;
    const auto rollback = [&] {
        while (applied > 0)
        {
            const auto& file = changes[--applied];
            if (!file.Replace(file.after, file.before))
                Debug::LogError("KromEditorApp::RenameEditorScene: rollback failed: %s",
                                PathToUtf8(file.path).c_str());
        }
    };
    for (const auto& file : changes)
    {
        if (!file.Replace(file.before, file.after))
        {
            rollback();
            return fail(PathToUtf8(file.path));
        }
        ++applied;
    }

    m_currentSceneName.swap(current);
    m_sceneOrder.swap(order);
    m_buildExcludedScenes.swap(excluded);
    if (!SaveProjectFile(false))
    {
        m_currentSceneName.swap(current);
        m_sceneOrder.swap(order);
        m_buildExcludedScenes.swap(excluded);
        rollback();
        return fail(PathToUtf8(GetCurrentProjectFilePath()));
    }

    for (auto& s : m_loadedSceneNames)
        if (s == oldName) s = newName;

    if (m_world)
    {
        using namespace renderer::addons::editor;
        m_world->ForEachAlive([&](EntityID id) {
            auto* tag = m_world->Get<EditorSceneTagComponent>(id);
            if (tag && tag->sceneName == oldName)
                tag->sceneName = newName;
            auto* scripts = m_world->Get<engine::script::ScriptList>(id);
            if (!scripts)
                return;
            for (auto& instance : scripts->Instances_Mutable())
            {
                for (auto& [name, value] : instance.fieldValues)
                {
                    if (value.type != engine::script::ScriptFieldType::String)
                        continue;
                    auto replacement = rename.Replacement(value.stringValue);
                    if (replacement.empty())
                        continue;
                    value.stringValue = std::move(replacement);
                    if (instance.script)
                        (void)m_scriptRegistry.WriteField(*instance.script, instance.className, name, value);
                }
            }
        });
    }

    // Undo snapshots can contain the old scene tags and script paths.
    if (m_editorFrameCtx)
        renderer::addons::editor::ClearEditorHistory(m_editorFrameCtx->state);
    RefreshSceneList();

    if (m_editorFrameCtx)
        m_editorFrameCtx->lastFileMessage = Translate("scene.renamed_prefix") + oldName + " \xE2\x86\x92 " + newName;

    Debug::Log("KromEditorApp: Szene umbenannt '%s' → '%s'", oldName.c_str(), newName.c_str());
    return true;
}

bool KromEditorApp::DeleteEditorScene(const std::string& sceneName)
{
    if (sceneName.empty())
        return false;

    const std::filesystem::path path = GetEditorScenePath(sceneName);

    // Prüfen ob es mindestens eine weitere Szene gibt, falls wir die aktive löschen
    const bool isDeletingCurrent = (sceneName == m_currentSceneName);
    if (isDeletingCurrent)
    {
        // Andere Szene zum Wechseln suchen
        const auto scenes = ListEditorScenes();
        std::string fallback;
        for (const std::string& s : scenes)
            if (s != sceneName) { fallback = s; break; }

        if (fallback.empty())
        {
            if (m_editorFrameCtx)
                m_editorFrameCtx->lastFileMessage =
                    TranslateWithValue("scene.cannot_delete_only", sceneName);
            return false;
        }

        // Zur Fallback-Szene wechseln (speichert automatisch)
        SwitchEditorScene(fallback, /*additive=*/false);
    }
    else
    {
        // Additiv geladene Szene zuerst entladen
        if (std::find(m_loadedSceneNames.begin(), m_loadedSceneNames.end(), sceneName)
            != m_loadedSceneNames.end())
            UnloadEditorScene(sceneName);
    }

    // Datei löschen
    std::error_code ec;
    std::filesystem::remove(path, ec);
    if (ec)
    {
        Debug::LogError("KromEditorApp::DeleteEditorScene: remove failed: %s", ec.message().c_str());
        if (m_editorFrameCtx)
            m_editorFrameCtx->lastFileMessage = Translate("common.delete_failed_prefix") + ec.message();
        return false;
    }

    // Interne Referenzen entfernen
    m_sceneOrder.erase(
        std::remove(m_sceneOrder.begin(), m_sceneOrder.end(), sceneName), m_sceneOrder.end());
    m_buildExcludedScenes.erase(sceneName);
    m_loadedSceneNames.erase(
        std::remove(m_loadedSceneNames.begin(), m_loadedSceneNames.end(), sceneName),
        m_loadedSceneNames.end());

    SaveProjectFile();
    RefreshSceneList();
    RefreshBuildExcludeState();

    if (m_editorFrameCtx)
        m_editorFrameCtx->lastFileMessage = Translate("scene.deleted_prefix") + sceneName;

    Debug::Log("KromEditorApp: Szene gelöscht '%s'", sceneName.c_str());
    return true;
}

// =============================================================================

void KromEditorApp::Shutdown()
{
#ifdef KROM_EDITOR_HAS_IMGUI
    // Laufendes Play beenden (Physik/Audio), ohne die Szene noch wiederherzustellen.
    m_game.Shutdown();
    m_skinning.reset();   // before the device goes away
    m_playActive = false;
#endif
    m_scriptModule.Unload(m_world.get(), m_scriptRegistry);

    // Laufenden Runtime-Build abkoppeln statt auf ihn zu warten: Der Worker haelt
    // seinen Job per shared_ptr selbst am Leben, der Build-Prozess laeuft weiter.
    if (m_runtimeBuildJob)
    {
        m_runtimeBuildJob->cancel.store(true, std::memory_order_release);
        if (m_runtimeBuildJob->worker.joinable())
            m_runtimeBuildJob->worker.detach();
        m_runtimeBuildJob.reset();
    }

#ifdef KROM_EDITOR_HAS_IMGUI
    // Leaving the editor keeps open material and animation changes.
    if (m_editorFrameCtx && m_editorState.pendingStartProjectFile.empty())
    {
        renderer::addons::editor::SaveOpenAnimationIfChanged(*m_editorFrameCtx);
        renderer::addons::editor::SaveOpenControllerIfChanged(*m_editorFrameCtx);
        renderer::addons::editor::SaveSelectedMaterialAssetIfChanged(*m_editorFrameCtx);
    }
    // Keep saved settings if the editor closes before the initial scene is ready.
    if (m_editorFrameCtx && m_editorState.pendingStartProjectFile.empty())
        SaveEditorSettings(m_projectRoot, m_editorFrameCtx->state, m_currentSceneName);
    m_sceneLoadJob.reset();

    if (m_editorEnvironmentHandle.IsValid())
    {
        renderer::RenderSystem& renderSystem = m_renderLoop.GetRenderSystem();
        renderSystem.DestroyEnvironment(m_editorEnvironmentHandle);
        m_editorEnvironmentHandle = renderer::EnvironmentHandle::Invalid();
    }
#endif

    addons::prefab::SetPrefabAssetPipeline(nullptr);
    m_assetPipeline.reset();

#ifdef KROM_EDITOR_HAS_IMGUI
    // ImGui ist hier ggf. schon beendet: nur das Ziel freigeben, keine ImGui-Textur.
    DestroyPanelTargetNoImGui(m_sceneView);
    DestroyPanelTargetNoImGui(m_gameView);
    if (m_previewRT.IsValid())
    {
        if (renderer::IDevice* device = m_renderLoop.GetRenderSystem().GetDevice())
            device->DestroyRenderTarget(m_previewRT);
        m_previewRT = RenderTargetHandle::Invalid();
    }
    if (m_materialPreviewRT.IsValid())
    {
        if (renderer::IDevice* device = m_renderLoop.GetRenderSystem().GetDevice())
            device->DestroyRenderTarget(m_materialPreviewRT);
        m_materialPreviewRT = RenderTargetHandle::Invalid();
    }
    if (m_animationPreviewRT.IsValid())
    {
        if (renderer::IDevice* device = m_renderLoop.GetRenderSystem().GetDevice())
            device->DestroyRenderTarget(m_animationPreviewRT);
        m_animationPreviewRT = RenderTargetHandle::Invalid();
    }
    if (renderer::IDevice* device = m_renderLoop.GetRenderSystem().GetDevice())
    {
        for (auto& [_, thumb] : m_assetBrowserState.assetPreviewThumbnails)
        {
            if (thumb.renderTarget.IsValid())
                device->DestroyRenderTarget(thumb.renderTarget);
            thumb.renderTarget = RenderTargetHandle::Invalid();
            thumb.gpuTexture = TextureHandle::Invalid();
            thumb.imguiId = nullptr;
        }
    }
#endif

    m_debugDraw.OnDeviceShutdown();
        m_animationPreviewDebugDraw.OnDeviceShutdown();
    m_particleView.OnDeviceShutdown();
    m_gameParticleView.OnDeviceShutdown();
    m_animationParticleView.OnDeviceShutdown();

    if (m_initialized)
        m_renderLoop.Shutdown();

    if (m_platform)
        m_platform->Shutdown();

#ifdef KROM_EDITOR_HAS_IMGUI
    Debug::SetLogObserver(nullptr);
    DrainConsoleLogs(nullptr);
    m_editorFrameCtx.reset();
#endif

    m_world.reset();
    m_platform.reset();
    m_initialized = false;
}

bool KromEditorApp::InitializePlatform()
{
#if defined(KROM_APP_USE_WIN32_PLATFORM)
    m_platform = std::make_unique<platform::win32::Win32Platform>();
#elif defined(KROM_APP_USE_GLFW_PLATFORM)
    m_platform = std::make_unique<platform::GLFWPlatform>();
#endif

    return m_platform && m_platform->Initialize();
}

bool KromEditorApp::InitializeRenderLoop()
{
    if (!m_deviceFactoryRegistry.IsRegistered(m_config.backend))
    {
        Debug::LogError("KromEditorApp: backend '%s' is not registered", BackendDisplayName(m_config.backend));
        return false;
    }

    const auto adapters = m_deviceFactoryRegistry.EnumerateAdapters(m_config.backend);
    if (adapters.empty())
    {
        Debug::LogError("KromEditorApp: backend '%s' reported no adapters", BackendDisplayName(m_config.backend));
        return false;
    }

    // Monitor-Auflösung abfragen wenn width/height == 0 (Autodetect)
    if (m_config.width == 0u || m_config.height == 0u)
    {
        uint32_t monW = 0u, monH = 0u;
        m_platform->GetPrimaryMonitorSize(monW, monH);
        if (monW > 0u && monH > 0u)
        {
            m_config.width  = monW;
            m_config.height = monH;
        }
        else
        {
            m_config.width  = 1280u;  // Fallback
            m_config.height = 720u;
        }
        Debug::Log("KromEditorApp: Monitor-Aufloesung %ux%u", m_config.width, m_config.height);
    }

    platform::WindowDesc windowDesc{};
    windowDesc.title      = m_config.windowTitle;
    windowDesc.width      = m_config.width;
    windowDesc.height     = m_config.height;
    windowDesc.windowMode = m_config.windowMode;
    windowDesc.visible    = m_config.visible;
    // The project launcher has a fixed size; opening a project unlocks the editor window.
    windowDesc.resizable  = false;
    if (m_config.backend == renderer::DeviceFactory::BackendType::OpenGL)
    {
        windowDesc.openglContext = true;
        windowDesc.openglMajor = 4;
        windowDesc.openglMinor = 1;
        windowDesc.openglDebugContext = m_config.enableDebugLayer;
    }

    renderer::IDevice::DeviceDesc deviceDesc{};
    deviceDesc.enableDebugLayer = m_config.enableDebugLayer;
    deviceDesc.adapterIndex = renderer::DeviceFactory::FindBestAdapter(adapters);
    deviceDesc.appName = m_config.windowTitle;

    if (!m_renderLoop.Initialize(m_config.backend, *m_platform, windowDesc, &m_eventBus, deviceDesc))
    {
        Debug::LogError("KromEditorApp: render loop initialization failed for backend '%s'", BackendDisplayName(m_config.backend));
        return false;
    }

    return true;
}

bool KromEditorApp::InitializeAssetPipeline()
{
    m_renderLoop.GetRenderSystem().SetAssetRegistry(&m_assetRegistry);
    m_assetPipeline = std::make_unique<assets::AssetPipeline>(m_assetRegistry, m_renderLoop.GetRenderSystem().GetDevice());
    mesh_renderer::ConfigureAssetPipeline(*m_assetPipeline);
    // GLB/GLTF-Importer fuer ResolveMeshAssetBindings und LoadMesh registrieren.
    m_assetPipeline->RegisterMeshImporter(
        std::make_unique<addons::gltf::GltfImporter>());
    m_projectBackend = m_config.backend;
    const std::filesystem::path assetRoot = GetCurrentAssetRoot();
    m_assetPipeline->SetAssetRoot(assetRoot);
    if (m_probeRuntime)
        m_probeRuntime->SetAssetRoot(PathToUtf8(assetRoot));
    // Prefab loading resolves model skeletons/animations and relative paths with it.
    addons::prefab::SetPrefabAssetPipeline(m_assetPipeline.get());
    if (!m_projectRoot.empty())
        renderer::addons::editor::CleanupThumbnailCache(m_projectRoot);
    renderer::ShaderCompiler::SetCacheDirectory(GetCurrentShaderCacheDir());
    // Engine-Asset-Root setzen damit #include "per_object_binding.hlsl" immer
    // gefunden wird — unabhaengig vom Projekt-Asset-Root.
    renderer::ShaderCompiler::SetEngineAssetDirectory(ResolveAssetRoot());
    return true;
}

std::filesystem::path KromEditorApp::GetCurrentAssetRoot() const
{
    return m_projectRoot.empty() ? ResolveAssetRoot() : (m_projectRoot / "Assets");
}

std::filesystem::path KromEditorApp::GetCurrentShaderCacheDir() const
{
    return m_projectRoot.empty() ? (ResolveAssetRoot().parent_path() / "shader_artifacts")
                                 : (m_projectRoot / "shader-bin");
}

bool KromEditorApp::InitializeTonemapMaterial()
{
    const char* tonemapVsPath = "fullscreen.vs.hlsl";
    const char* tonemapPsPath = "passthrough.ps.hlsl";
    if (m_config.backend == renderer::DeviceFactory::BackendType::OpenGL)
    {
        tonemapVsPath = "fullscreen.opengl.vs.glsl";
        tonemapPsPath = "passthrough.opengl.fs.glsl";
        // Experiment (Auftrag 13): GLSL generated from the HLSL via SPIR-V and
        // SPIRV-Cross. Off unless KROM_GLSL_EXPERIMENT_GENERATED_TONEMAP=1.
        const char* generated = std::getenv("KROM_GLSL_EXPERIMENT_GENERATED_TONEMAP");
        if (generated && generated[0] == '1')
        {
            tonemapVsPath = "generated/fullscreen.opengl.vs.glsl";
            tonemapPsPath = "generated/passthrough.opengl.fs.glsl";
            Debug::LogWarning("KromEditorApp.cpp: tonemap pass uses generated GLSL (experiment)");
        }
    }

    const ShaderHandle tonemapVs = m_assetPipeline->LoadShader(PathToUtf8(ResolveAssetRoot() / PathFromUtf8(tonemapVsPath)), assets::ShaderStage::Vertex);
    const ShaderHandle tonemapPs = m_assetPipeline->LoadShader(PathToUtf8(ResolveAssetRoot() / PathFromUtf8(tonemapPsPath)), assets::ShaderStage::Fragment);
    if (!tonemapVs.IsValid() || !tonemapPs.IsValid())
    {
        Debug::LogError("KromEditorApp: failed to load tonemap shaders");
        return false;
    }

    renderer::MaterialParam tonemapSampler{};
    tonemapSampler.name = "linearclamp";
    tonemapSampler.type = renderer::MaterialParam::Type::Sampler;
    tonemapSampler.samplerIdx = 0u;

    const renderer::ISwapchain* swapchain = m_renderLoop.GetRenderSystem().GetSwapchain();
    const renderer::Format backbufferFormat = swapchain ? swapchain->GetBackbufferFormat()
                                                        : renderer::Format::BGRA8_UNORM_SRGB;

    renderer::MaterialDesc tonemapDesc{};
    renderer::MaterialRuntimeDesc tonemapDescRuntime{};
    tonemapDesc.name   = "ExampleTonemap";
    tonemapDesc.domain = renderer::MaterialDomain::Postprocess;
    tonemapDesc.renderPolicy.depth.test     = false;
    tonemapDesc.renderPolicy.depth.write    = false;
    tonemapDesc.renderPolicy.cull.mode      = renderer::MaterialCullMode::None;
    tonemapDesc.renderPolicy.castShadows    = false;
    tonemapDesc.renderPolicy.receiveShadows = false;
    tonemapDescRuntime.renderPass     = renderer::StandardRenderPasses::Postprocess();
    tonemapDescRuntime.vertexShader   = tonemapVs;
    tonemapDescRuntime.fragmentShader = tonemapPs;
    tonemapDescRuntime.colorFormat    = backbufferFormat;
    tonemapDescRuntime.depthFormat    = renderer::Format::Unknown;
    tonemapDesc.parameters.push_back(tonemapSampler);

    const MaterialHandle tonemapMaterial = engine::renderer::MaterialRuntimeBridge::RegisterMaterial(m_materialSystem, std::move(tonemapDesc), tonemapDescRuntime);
    m_renderLoop.GetRenderSystem().SetDefaultTonemapMaterial(tonemapMaterial, m_materialSystem);
    return tonemapMaterial.IsValid();
}

renderer::DeviceFactory::BackendType SelectAppBackend()
{
#if defined(KROM_APP_BACKEND_DX11)
    return renderer::DeviceFactory::BackendType::DirectX11;
#elif defined(KROM_APP_BACKEND_OPENGL)
    return renderer::DeviceFactory::BackendType::OpenGL;
#elif defined(KROM_APP_BACKEND_VULKAN)
    return renderer::DeviceFactory::BackendType::Vulkan;
#else
#error Example backend not configured.
#endif
}

const char* BackendDisplayName(renderer::DeviceFactory::BackendType backend) noexcept
{
    switch (backend)
    {
    case renderer::DeviceFactory::BackendType::DirectX11: return "DirectX11";
    case renderer::DeviceFactory::BackendType::OpenGL: return "OpenGL";
    case renderer::DeviceFactory::BackendType::Vulkan: return "Vulkan";
    default: return "Unknown";
    }
}

} // namespace engine::app
