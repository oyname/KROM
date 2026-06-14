#include "renderer/MaterialDomain.hpp"
#include "renderer/RenderFrameConstants.hpp"
#include "renderer/RenderLayers.hpp"
#include "renderer/ShaderCompiler.hpp"
#include "renderer/runtime/MaterialRuntimeBridge.hpp"
#include "renderer/runtime/MaterialRuntimeDesc.hpp"
#include "app/KromEditorApp.hpp"

#include "addons/camera/CameraComponents.hpp"
#include "addons/animation/AnimationComponents.hpp"
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
#include "addons/prefab/Prefab.hpp"
#include "addons/prefab/PrefabInstanceComponent.hpp"
#include "addons/prefab/PrefabSerialization.hpp"
#include "addons/script/ScriptList.hpp"
#include "addons/script/ScriptSerialization.hpp"
#include "addons/editor/EditorFileNaming.hpp"
#include "addons/editor/EditorPrefabWindow.hpp"
#include "addons/editor/EditorScriptAssets.hpp"
#include "addons/shadow/ShadowFeature.hpp"
#include "addons/gtao/GtaoFeature.hpp"
#include "addons/outline/OutlineFeature.hpp"
#include "core/Debug.hpp"
#include "serialization/SceneSerializer.hpp"
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <filesystem>
#include <random>
#include <unordered_map>
#include <unordered_set>

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

std::filesystem::path RuntimeExecutablePath(const std::string& runtimeTarget,
                                            engine::renderer::DeviceFactory::BackendType backend)
{
    return std::filesystem::path(KROM_BUILD_DIR) / "bin" /
        RuntimeExecutableName(runtimeTarget, backend);
}

void AddEditorConsoleLog(engine::renderer::addons::editor::EditorState* state,
                         engine::LogLevel level,
                         std::string message)
{
    if (!state)
        return;

    constexpr size_t kMaxLogs = 512u;
    if (state->consoleLogs.size() >= kMaxLogs)
        state->consoleLogs.erase(state->consoleLogs.begin());
    state->consoleLogs.push_back({ level, std::move(message) });
    state->consoleScrollToBottom = true;
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
    state.runtimeStateFilePath = path.string();

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

bool BuildRuntimeTarget(const std::string& runtimeTarget,
                        engine::renderer::DeviceFactory::BackendType backend,
                        engine::renderer::addons::editor::EditorState* editorState)
{
#if defined(_WIN32)
    const std::filesystem::path sourceDir(KROM_SOURCE_DIR);
    const std::filesystem::path buildDir(KROM_BUILD_DIR);
    const std::string target = RuntimeTargetName(runtimeTarget, backend);
    const std::filesystem::path vsDev =
        "C:/Program Files/Microsoft Visual Studio/2022/Community/Common7/Tools/VsDevCmd.bat";
    const std::filesystem::path ninja =
        "C:/Program Files/Microsoft Visual Studio/2022/Community/Common7/IDE/CommonExtensions/Microsoft/CMake/Ninja/ninja.exe";
    if (!std::filesystem::exists(vsDev) || !std::filesystem::exists(ninja))
    {
        AddEditorConsoleLog(editorState, engine::LogLevel::Error,
            "[Build] Visual Studio/Ninja wurde nicht gefunden.");
        return false;
    }

    const std::filesystem::path buildScript = buildDir / "_krom_play_build.cmd";
    {
        std::ofstream script(buildScript, std::ios::binary | std::ios::trunc);
        if (!script)
        {
            AddEditorConsoleLog(editorState, engine::LogLevel::Error,
                "[Build] Temporaeres Build-Script konnte nicht geschrieben werden.");
            return false;
        }

        script << "@echo off\n"
               << "cd /d \"" << sourceDir.string() << "\"\n"
               << "if errorlevel 1 exit /b %errorlevel%\n"
               << "call \"" << vsDev.string() << "\" -arch=x64 -host_arch=x64 >nul\n"
               << "if errorlevel 1 exit /b %errorlevel%\n"
               << "\"" << ninja.string() << "\" -C \"" << buildDir.string() << "\" " << target << "\n"
               << "exit /b %errorlevel%\n";
    }

    std::string command = "cmd /d /s /c \"\"";
    command += buildScript.string();
    command += "\" 2>&1\"";

    AddEditorConsoleLog(editorState, engine::LogLevel::Info,
        std::string("[Build] Starte Runtime-Build: ") + target);

    FILE* pipe = _popen(command.c_str(), "r");
    if (!pipe)
    {
        AddEditorConsoleLog(editorState, engine::LogLevel::Error,
            "[Build] Build-Prozess konnte nicht gestartet werden.");
        return false;
    }

    bool showedDiagnostic = false;
    char buffer[2048];
    while (std::fgets(buffer, sizeof(buffer), pipe))
    {
        std::string line(buffer);
        while (!line.empty() && (line.back() == '\n' || line.back() == '\r'))
            line.pop_back();
        if (line.empty() || !ShouldShowBuildOutputLine(line))
            continue;

        showedDiagnostic = true;
        AddEditorConsoleLog(editorState, BuildOutputLevel(line), "[Build] " + line);
    }

    const int exitCode = _pclose(pipe);
    if (exitCode == 0)
    {
        AddEditorConsoleLog(editorState, engine::LogLevel::Info, "[Build] Runtime-Build erfolgreich.");
        return true;
    }

    if (!showedDiagnostic)
        AddEditorConsoleLog(editorState, engine::LogLevel::Error,
            "[Build] Runtime-Build fehlgeschlagen. Keine Compilerdiagnose erfasst.");
    AddEditorConsoleLog(editorState, engine::LogLevel::Error, "[Build] Runtime-Build fehlgeschlagen.");
    return false;
#else
    (void)runtimeTarget;
    (void)backend;
    (void)editorState;
    return false;
#endif
}

bool LaunchRuntimeProcess(const std::filesystem::path& exePath,
                          const std::filesystem::path& projectFile,
                          bool editorLiveState)
{
#if defined(_WIN32)
    if (!std::filesystem::exists(exePath) || !std::filesystem::exists(projectFile))
        return false;
    std::string command = "cmd /c start \"KROM Play\" /D \"";
    command += exePath.parent_path().string();
    command += "\" \"";
    command += exePath.string();
    command += "\" \"";
    command += projectFile.string();
    command += "\" 1";
    if (editorLiveState)
        command += " --editor-live-state";
    return std::system(command.c_str()) == 0;
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
#include "addons/editor/EditorFeature.hpp"
#include "addons/editor/EditorSerialization.hpp"
#include "addons/editor/EditorMaterialLibrary.hpp"
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
#elif defined(KROM_APP_BACKEND_OPENGL) && defined(KROM_IMGUI_HAS_OPENGL3)
#include "imgui_impl_opengl3.h"
#elif defined(KROM_APP_BACKEND_VULKAN) && defined(KROM_IMGUI_HAS_VULKAN)
#include "imgui_impl_vulkan.h"
#include "addons/vulkan/VulkanDevice.hpp"
#endif

#endif // KROM_EDITOR_HAS_IMGUI

#include "platform/NativeFileDialog.hpp"

#include <filesystem>
#include <algorithm>
#include <cmath>
#include <fstream>
#include <memory>
#include <cstdint>
#include <cstdio>
#include <cfloat>
#include <unordered_set>

namespace engine::app {

namespace {

std::filesystem::path ResolveAssetRoot()
{
    return std::filesystem::path(__FILE__).parent_path().parent_path() / "assets";
}

#ifdef KROM_EDITOR_HAS_IMGUI
std::filesystem::path ResolveProjectsRoot()
{
    return std::filesystem::path(__FILE__).parent_path().parent_path() / "projects";
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


std::string FloatJson(float v)
{
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.9g", static_cast<double>(v));
    return buf;
}

// Speichert Editor-Einstellungen in {projectRoot}/editor/editor.settings
void SaveEditorSettings(const std::filesystem::path& projectRoot,
                        const renderer::addons::editor::EditorState& state)
{
    if (projectRoot.empty())
        return;
    const auto& cam = state.editorCamera;
    std::error_code ec;
    std::filesystem::create_directories(projectRoot / "editor", ec);
    std::ofstream f(projectRoot / "editor" / "editor.settings",
                    std::ios::binary | std::ios::trunc);
    if (!f) return;
    f << "{\n"
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
      << "  }\n"
      << "}\n";
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
    out += "    \"ambientIntensity\":" + FloatJson(state.ambientIntensity) + "\n";
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
}

void ApplyEditorEnvironmentFromSceneJson(renderer::addons::editor::EditorFrameContext* ctx,
                                         const serialization::JsonValue& root)
{
    if (!ctx)
        return;

    const serialization::JsonValue* env = root.Get("environment");
    if (!env || !env->IsObject())
        return;

    auto& state = ctx->state;
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
    metadata += BuildEditorCameraJson(state.editorCamera);
    metadata += ",\n";
    metadata += BuildEditorEnvironmentJson(state);
    metadata += "\n";
    json.insert(end, metadata);
    return json;
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
    if (m_editorState.environmentTexturePath.empty())
        m_editorState.environmentTexturePath = "autumn_field_puresky_2k.hdr";
    // environmentIntensity bleibt beim Default aus EditorFeature.hpp (0.2f)
    // oder wird aus editor.settings geladen – kein Hardcode hier.
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
        forwardConfig.mode = m_forwardPlusActive
            ? renderer::addons::forward::ForwardRendererMode::ForwardPlus
            : renderer::addons::forward::ForwardRendererMode::Forward;

        renderer::RenderSystem& renderSystem = m_renderLoop.GetRenderSystem();
        if (!renderSystem.RegisterFeature(addons::mesh_renderer::CreateMeshRendererFeature()) ||
            !renderSystem.RegisterFeature(addons::lighting::CreateLightingFeature()) ||
            !renderSystem.RegisterFeature(addons::shadow::CreateShadowFeature()) ||
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

#ifdef KROM_EDITOR_HAS_IMGUI
        {
            // getSelected: liefert irgendeine gültige selektierte Entity damit
            // BuildPass nicht vorzeitig abbricht. Die echte Entscheidung trifft
            // shouldOutlineEntity.
            auto getSelected = [this]() -> EntityID {
                if (!m_editorFrameCtx) return NULL_ENTITY;
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
                io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
                io.ConfigDragClickToInputText = true;
                ImGui_ImplWin32_Init(window->GetNativeHandle());
                ImGui_ImplDX11_Init(device->GetD3DDevice(), device->GetD3DContext());
                InstallImGuiWin32Hook(window->GetNativeHandle());
                return true;
            };
            editorCbs.shutdown = [this]() {
                if (auto* w = m_renderLoop.GetWindow())
                    RemoveImGuiWin32Hook(w->GetNativeHandle());
                ImGui_ImplDX11_Shutdown();
                ImGui_ImplWin32_Shutdown();
                ImGui::DestroyContext();
            };
            editorCbs.newFrame = []() {
                ImGui_ImplDX11_NewFrame();
                ImGui_ImplWin32_NewFrame();
                ImGui::NewFrame();
            };
            editorCbs.renderDrawData = [](renderer::ICommandList&) {
                ImGui::Render();
                ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
            };
            editorBackendAvailable = true;

#elif defined(KROM_APP_BACKEND_OPENGL) && defined(KROM_IMGUI_HAS_OPENGL3)
            editorCbs.init = [this]() -> bool {
                auto* window = m_renderLoop.GetWindow();
                if (!window)
                    return false;
                ImGui::CreateContext();
                ImGuiIO& io = ImGui::GetIO();
                io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
                io.ConfigDragClickToInputText = true;
#if defined(KROM_APP_USE_WIN32_PLATFORM) && defined(KROM_IMGUI_HAS_WIN32)
                ImGui_ImplWin32_Init(window->GetNativeHandle());
                InstallImGuiWin32Hook(window->GetNativeHandle());
#elif defined(KROM_APP_USE_GLFW_PLATFORM) && defined(KROM_IMGUI_HAS_GLFW)
                ImGui_ImplGlfw_InitForOpenGL(
                    static_cast<GLFWwindow*>(window->GetNativeHandle()), true);
#endif
                ImGui_ImplOpenGL3_Init("#version 410");
                return true;
            };
            editorCbs.shutdown = [this]() {
                ImGui_ImplOpenGL3_Shutdown();
#if defined(KROM_APP_USE_WIN32_PLATFORM) && defined(KROM_IMGUI_HAS_WIN32)
                if (auto* w = m_renderLoop.GetWindow())
                    RemoveImGuiWin32Hook(w->GetNativeHandle());
                ImGui_ImplWin32_Shutdown();
#elif defined(KROM_APP_USE_GLFW_PLATFORM) && defined(KROM_IMGUI_HAS_GLFW)
                ImGui_ImplGlfw_Shutdown();
#endif
                ImGui::DestroyContext();
            };
            editorCbs.newFrame = []() {
#if defined(KROM_APP_USE_WIN32_PLATFORM) && defined(KROM_IMGUI_HAS_WIN32)
                ImGui_ImplWin32_NewFrame();
#elif defined(KROM_APP_USE_GLFW_PLATFORM) && defined(KROM_IMGUI_HAS_GLFW)
                ImGui_ImplGlfw_NewFrame();
#endif
                ImGui_ImplOpenGL3_NewFrame();
                ImGui::NewFrame();
            };
            editorCbs.renderDrawData = [](renderer::ICommandList&) {
                ImGui::Render();
                ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
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
                io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
                io.ConfigDragClickToInputText = true;
#if defined(KROM_APP_USE_WIN32_PLATFORM) && defined(KROM_IMGUI_HAS_WIN32)
                ImGui_ImplWin32_Init(window->GetNativeHandle());
                InstallImGuiWin32Hook(window->GetNativeHandle());
#elif defined(KROM_APP_USE_GLFW_PLATFORM) && defined(KROM_IMGUI_HAS_GLFW)
                ImGui_ImplGlfw_InitForVulkan(
                    static_cast<GLFWwindow*>(window->GetNativeHandle()), true);
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

                return ImGui_ImplVulkan_Init(&initInfo);
            };
            editorCbs.shutdown = [this]() {
                if (auto* device = m_renderLoop.GetRenderSystem().GetDevice())
                    device->WaitIdle();
                ImGui_ImplVulkan_Shutdown();
#if defined(KROM_APP_USE_WIN32_PLATFORM) && defined(KROM_IMGUI_HAS_WIN32)
                if (auto* w = m_renderLoop.GetWindow())
                    RemoveImGuiWin32Hook(w->GetNativeHandle());
                ImGui_ImplWin32_Shutdown();
#elif defined(KROM_APP_USE_GLFW_PLATFORM) && defined(KROM_IMGUI_HAS_GLFW)
                ImGui_ImplGlfw_Shutdown();
#endif
                ImGui::DestroyContext();
            };
            editorCbs.newFrame = []() {
#if defined(KROM_APP_USE_WIN32_PLATFORM) && defined(KROM_IMGUI_HAS_WIN32)
                ImGui_ImplWin32_NewFrame();
#elif defined(KROM_APP_USE_GLFW_PLATFORM) && defined(KROM_IMGUI_HAS_GLFW)
                ImGui_ImplGlfw_NewFrame();
#endif
                ImGui_ImplVulkan_NewFrame();
                ImGui::NewFrame();
            };
            editorCbs.renderDrawData = [](renderer::ICommandList& cmd) {
                ImGui::Render();
                if (void* nativeCmd = cmd.GetNativeCommandBuffer())
                {
                    ImGui_ImplVulkan_RenderDrawData(
                        ImGui::GetDrawData(),
                        static_cast<VkCommandBuffer>(nativeCmd));
                }
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
        m_assetPipeline.reset();
#ifdef KROM_EDITOR_HAS_IMGUI
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
        m_world.reset();
    };

    if (!m_engineSchemaRegistered)
    {
        RegisterCoreComponents(m_componentRegistry);
        RegisterAnimationComponents(m_componentRegistry);
        RegisterCameraComponents(m_componentRegistry);
        RegisterMeshRendererComponents(m_componentRegistry);
        RegisterLightingComponents(m_componentRegistry);
        ecs::RegisterComponent<engine::script::ScriptList>(m_componentRegistry, "ScriptList");
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
    const std::string defaultProjectsRoot = ResolveProjectsRoot().string();
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
        return LoadEditorScene();
    };
    m_editorFrameCtx->saveProject = [this]() {
        return SaveProjectFile();
    };
    m_editorFrameCtx->loadProject = [this](const std::string& path) {
        return LoadProjectFile(path);
    };
    m_editorFrameCtx->createProject = [this](const std::string& projectName,
                                             const std::string& parentDirectory,
                                             renderer::DeviceFactory::BackendType backend) {
        return CreateEditorProject(projectName, parentDirectory, backend);
    };
    m_editorFrameCtx->setProjectBackend = [this](renderer::DeviceFactory::BackendType backend) {
        m_projectBackend = backend;
        if (m_editorFrameCtx)
        {
            m_editorFrameCtx->projectBackend = backend;
            m_editorFrameCtx->lastFileMessage =
                std::string("Projekt-Backend: ") + ProjectBackendId(backend);
        }
        SaveProjectFile();
    };
    m_editorFrameCtx->syncSceneState = [this]() {
        SyncEditorSceneState();
    };
    m_editorFrameCtx->switchEditorScene = [this](const std::string& name, bool additive) {
        return SwitchEditorScene(name, additive);
    };
    m_editorFrameCtx->newEditorScene = [this](const std::string& name) {
        return NewEditorScene(name);
    };
    m_editorFrameCtx->newEditorSceneInDir = [this](const std::string& name, const std::string& dir) {
        return NewEditorSceneInDir(name, dir);
    };
    m_editorFrameCtx->unloadEditorScene = [this](const std::string& name) {
        return UnloadEditorScene(name);
    };
    m_editorFrameCtx->renameEditorScene = [this](const std::string& oldName, const std::string& newName) {
        return RenameEditorScene(oldName, newName);
    };
    m_editorFrameCtx->deleteEditorScene = [this](const std::string& name) {
        return DeleteEditorScene(name);
    };
    m_editorFrameCtx->deleteEditorSceneByPath = [this](const std::string& absPath) {
        std::error_code ec;
        std::filesystem::remove(std::filesystem::path(absPath), ec);
        if (ec)
        {
            if (m_editorFrameCtx)
                m_editorFrameCtx->lastFileMessage = "Loeschen fehlgeschlagen.";
            return false;
        }
        if (m_editorFrameCtx)
            m_editorFrameCtx->lastFileMessage = "Szene geloescht.";
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
        return BuildAllScenes();
    };
    m_editorFrameCtx->exportProject = [this]() {
        return ExportProjectBuild();
    };
    m_editorFrameCtx->playGame = [this]() {
        if (m_projectRoot.empty())
        {
            if (m_editorFrameCtx)
                m_editorFrameCtx->lastFileMessage = "Play fehlgeschlagen: Kein Projekt geoeffnet.";
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
                    ? "Script-Bindings konnten nicht generiert werden."
                    : bindings.message;
                AddEditorConsoleLog(&m_editorFrameCtx->state, engine::LogLevel::Error,
                    "[Build] " + m_editorFrameCtx->lastFileMessage);
                return false;
            }
        }
        if (!BuildAllScenes())
            return false;

        const auto backend = m_projectBackend;
        if (!BuildRuntimeTarget(m_projectRuntimeTarget, backend,
                                m_editorFrameCtx ? &m_editorFrameCtx->state : nullptr))
        {
            if (m_editorFrameCtx)
                m_editorFrameCtx->lastFileMessage = "Play fehlgeschlagen: Runtime-Build fehlgeschlagen.";
            return false;
        }

        const std::filesystem::path exe = RuntimeExecutablePath(m_projectRuntimeTarget, backend);
        const std::filesystem::path projectFile = m_projectRoot / "krom-project.json";
        if (m_editorFrameCtx)
        {
            m_editorFrameCtx->state.runtimeScriptFieldValues.clear();
            m_editorFrameCtx->state.runtimeStateLastError.clear();
            m_editorFrameCtx->state.runtimeStatePollTimer = 0.0;
            m_editorFrameCtx->state.runtimeStateFilePath =
                (m_projectRoot / "editor" / "runtime-state.json").string();
            std::error_code ec;
            std::filesystem::remove(m_projectRoot / "editor" / "runtime-state.json", ec);
        }
        const bool launched = LaunchRuntimeProcess(exe, projectFile, true);
        if (m_editorFrameCtx)
            m_editorFrameCtx->lastFileMessage = launched
                ? "Play gestartet."
                : "Play fehlgeschlagen: Runtime konnte nicht gestartet werden.";
        return launched;
    };
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
            void* handle = nullptr;
            if (auto* win = m_renderLoop.GetWindow())
                handle = win->GetNativeHandle();
            const platform::dialog::FileFilter filters[] = {
                { "KROM Projektdatei (krom-project.json)", "*.json" },
                { "Alle Dateien",                          "*.*"    }
            };
            const std::filesystem::path initialFolder = !m_projectRoot.empty()
                ? m_projectRoot
                : (std::filesystem::current_path() / "projects");
            return platform::dialog::BrowseForFile(
                "KROM Projekt oeffnen", filters, 2, "krom-project.json", handle,
                initialFolder.string().c_str());
        };
        m_editorFrameCtx->browseForProjectFolder = [this]() -> std::string {
            void* handle = nullptr;
            if (auto* win = m_renderLoop.GetWindow())
                handle = win->GetNativeHandle();
            return platform::dialog::BrowseForFolder(
                "Projektordner auswaehlen", handle);
        };
    }
    m_editorFrameCtx->debugDraw = &m_debugDraw;

    m_editorFrameCtx->currentProjectName = m_projectName;
    m_editorFrameCtx->currentProjectRoot = m_projectRoot.string();
    m_editorFrameCtx->engineAssetRoot  = ResolveAssetRoot().string();
    m_editorFrameCtx->engineEditorDir  =
        (std::filesystem::path(__FILE__).parent_path().parent_path() / "editor").string();
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

    m_transformSystem.Update(*m_world);
    m_boundsSystem.Update(*m_world);

    while (!m_renderLoop.ShouldExit())
    {
        if (auto* input = m_renderLoop.GetInput();
            input && input->IsKeyPressed(platform::Key::Escape) && m_renderLoop.GetWindow())
        {
#ifdef KROM_EDITOR_HAS_IMGUI
            if (!m_editorFrameCtx)
#endif
            m_renderLoop.GetWindow()->RequestClose();
        }

        const float deltaSeconds = m_timing.GetDeltaSecondsF();
        if (!scene.Update(context, deltaSeconds))
            return -3;

#ifdef KROM_EDITOR_HAS_IMGUI
        if (m_editorFrameCtx)
        {
            m_editorFrameCtx->deltaSeconds = deltaSeconds;
            PollRuntimeStateFile(m_editorFrameCtx->state, m_projectRoot, deltaSeconds);
            ProcessEditorFileDrops();

            auto setDebugView = [this](uint32_t flags) {
                m_editorState.editorDebugFlags = flags;
                if (m_editorFrameCtx && m_editorFrameCtx->setEditorDebugFlags)
                    m_editorFrameCtx->setEditorDebugFlags(flags);
            };

            if (auto* input = m_renderLoop.GetInput())
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

        mesh_renderer::UpdateLocalBoundsFromMeshes(*m_world, m_assetRegistry);
#ifdef KROM_EDITOR_HAS_IMGUI
        EnsureCameraGizmos();       // neue Kameras bekommen sofort ihr Gizmo
        SyncCameraGizmoTransforms();
        EnsureMaterialPreviewEntity();
        SyncMaterialPreviewEntity();
        ProcessAssetThumbnailQueue();
#endif
        m_transformSystem.Update(*m_world);
        m_boundsSystem.Update(*m_world);

        const renderer::ISwapchain* swapchain = m_renderLoop.GetRenderSystem().GetSwapchain();
        const uint32_t viewportWidth = (swapchain && swapchain->GetWidth() > 0u) ? swapchain->GetWidth() : m_config.width;
        const uint32_t viewportHeight = (swapchain && swapchain->GetHeight() > 0u) ? swapchain->GetHeight() : m_config.height;

        renderer::RenderView view{};
#ifdef KROM_EDITOR_HAS_IMGUI
        if (m_editorFrameCtx)
        {
            renderer::addons::editor::BuildEditorCameraRenderView(
                m_editorState,
                viewportWidth,
                viewportHeight,
                view,
                m_cameraOptions.ambientColor,
                m_cameraOptions.ambientIntensity);
            view.visibilityLayerMask &= ~renderer::LAYER_EDITOR_MATERIAL_PREVIEW;
            view.visibilityLayerMask &= ~renderer::LAYER_EDITOR_ASSET_THUMBNAIL;
            view.visibilityLayerMask &= ~renderer::LAYER_EDITOR_PREFAB_PREVIEW;
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

        renderer::FramePipelineCallbacks frameCallbacks;
        renderer::RenderView cameraPreviewView{};
        renderer::RenderView materialPreviewView{};
        renderer::RenderView assetThumbnailView{};
        std::array<renderer::OffscreenRenderRequest, 4u> offscreenRequests{};
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
        const bool previewRequested = m_editorFrameCtx &&
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
                cameraPreviewView.debugFlags = context.debugFlags |
                    static_cast<uint32_t>(renderer::DBG_DISABLE_IBL) |
                    static_cast<uint32_t>(renderer::DBG_DISABLE_GTAO) |
                    static_cast<uint32_t>(renderer::DBG_DISABLE_SHADOWS);
                applyEditorEnvironmentViewSettings(cameraPreviewView);
                cameraPreviewView.visibilityLayerMask &= ~renderer::LAYER_EDITOR_MATERIAL_PREVIEW;
                cameraPreviewView.visibilityLayerMask &= ~renderer::LAYER_EDITOR_ASSET_THUMBNAIL;
                cameraPreviewView.visibilityLayerMask &= ~renderer::LAYER_EDITOR_PREFAB_PREVIEW;
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

        const bool materialPreviewRequested = m_editorFrameCtx &&
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

        // Prefab-Preview-RT
        renderer::RenderView prefabPreviewView{};
        const bool prefabPreviewRequested = m_editorFrameCtx &&
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

        if (m_activeAssetThumbnailRT.IsValid() && BuildAssetThumbnailRenderView(assetThumbnailView))
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
                const std::string key = m_activeAssetThumbnailPath.lexically_normal().generic_string();
                auto& thumbnails = m_editorFrameCtx->state.assetBrowser->assetPreviewThumbnails;
                if (auto it = thumbnails.find(key); it != thumbnails.end() &&
                    it->second.status == renderer::addons::editor::AssetBrowserState::PreviewStatus::Rendering)
                {
                    it->second.status = renderer::addons::editor::AssetBrowserState::PreviewStatus::Ready;
                }
            }
            m_activeAssetThumbnailRT = RenderTargetHandle::Invalid();
            m_activeAssetThumbnailPath.clear();
        }

        if (offscreenRequestCount > 0u)
            activeOffscreenRequests =
                std::span<const renderer::OffscreenRenderRequest>(offscreenRequests.data(), offscreenRequestCount);

        if (m_editorFrameCtx)
        {
            const renderer::IDevice* device = m_renderLoop.GetRenderSystem().GetDevice();
            const math::Mat4 clipAdjustment = device
                ? device->GetClipSpaceAdjustment()
                : math::Mat4::Identity();
            m_debugDraw.Clear();
            m_debugDraw.SetViewProjection(clipAdjustment * view.projection * view.view);
            renderer::addons::editor::DrawLightDebugLines(*m_editorFrameCtx);
            addons::debug_draw::AddDebugDrawCallback(frameCallbacks, m_debugDraw);
        }
#endif

        if (!m_renderLoop.Tick(*m_world, m_materialSystem, view, m_timing, frameCallbacks, activeOffscreenRequests))
        {
            if (m_renderLoop.ShouldExit())
                break;
            return -5;
        }
    }

    return 0;
}

#ifdef KROM_EDITOR_HAS_IMGUI
std::filesystem::path KromEditorApp::GetCurrentEditorScenePath() const
{
    // m_currentSceneName ist z.B. "Level1" → "Assets/Scenes/Level1.json"
    // Default "editor_scene" bewahrt Rückwärtskompatibilität mit alten Projekten.
    const std::string filename = m_currentSceneName.empty() ? "editor_scene" : m_currentSceneName;
    return m_projectRoot.empty()
        ? ResolveLegacyEditorScenePath()
        : (m_projectRoot / "Assets" / "Scenes" / (filename + ".json"));
}

std::filesystem::path KromEditorApp::GetEditorScenePath(const std::string& sceneName) const
{
    if (m_projectRoot.empty())
        return ResolveLegacyEditorScenePath().parent_path() / (sceneName + ".json");

    // Wenn sceneName bereits einen Slash enthält ist es ein relativer Pfad vom Projektroot
    // z.B. "Assets/Levels/Village" → projectRoot/Assets/Levels/Village.json
    if (sceneName.find('/') != std::string::npos || sceneName.find('\\') != std::string::npos)
    {
        std::filesystem::path p = m_projectRoot / sceneName;
        if (p.extension().empty())
            p.replace_extension(".json");
        return p;
    }

    return m_projectRoot / "Assets" / "Scenes" / (sceneName + ".json");
}

std::filesystem::path KromEditorApp::GetCurrentProjectFilePath() const
{
    return m_projectRoot.empty()
        ? (ResolveProjectsRoot() / "krom-project.json")
        : (m_projectRoot / "krom-project.json");
}

bool KromEditorApp::SaveProjectFile()
{
    const std::filesystem::path projectFilePath = GetCurrentProjectFilePath();
    const std::filesystem::path projectRoot = projectFilePath.parent_path();
    std::error_code ec;
    std::filesystem::create_directories(projectRoot, ec);
    if (ec)
    {
        if (m_editorFrameCtx)
            m_editorFrameCtx->lastFileMessage = "Projektordner konnte nicht erstellt werden.";
        return false;
    }

    const std::string effectiveName = !m_projectName.empty()
        ? m_projectName
        : projectRoot.filename().string();
    const std::string effectiveRuntimeTarget = !m_projectRuntimeTarget.empty()
        ? SanitizeRuntimeTargetName(m_projectRuntimeTarget)
        : SanitizeRuntimeTargetName(effectiveName);
    std::ofstream out(projectFilePath, std::ios::binary | std::ios::trunc);
    if (!out)
    {
        if (m_editorFrameCtx)
            m_editorFrameCtx->lastFileMessage = "Projektdatei konnte nicht geschrieben werden.";
        return false;
    }

    out << "{\n"
        << "  \"name\":\"" << EscapeJsonString(effectiveName) << "\",\n"
        << "  \"runtimeTarget\":\"" << EscapeJsonString(effectiveRuntimeTarget) << "\",\n"
        << "  \"backend\":\"" << ProjectBackendId(m_projectBackend) << "\",\n"
        << "  \"assetRoot\":\"Assets\",\n"
        << "  \"shaderCache\":\"shader-bin\",\n"
        << "  \"editorScene\":\"Assets/Scenes/" << EscapeJsonString(m_currentSceneName) << ".json\",\n";

    // sceneOrder — Szenenreihenfolge für Runtime-LoadScene(index)
    out << "  \"sceneOrder\":[";
    {
        bool first = true;
        for (const std::string& s : m_sceneOrder)
        {
            if (!first) out << ",";
            out << "\"" << EscapeJsonString(s) << "\"";
            first = false;
        }
    }
    out << "],\n";

    // buildExclude — Szenen die beim Build übersprungen werden
    out << "  \"buildExclude\":[";
    bool firstExcl = true;
    for (const std::string& excl : m_buildExcludedScenes)
    {
        if (!firstExcl) out << ",";
        out << "\"" << EscapeJsonString(excl) << "\"";
        firstExcl = false;
    }
    out << "],\n"
        << "  \"version\":1\n"
        << "}\n";
    if (!out.good())
    {
        if (m_editorFrameCtx)
            m_editorFrameCtx->lastFileMessage = "Projektdatei konnte nicht geschrieben werden.";
        return false;
    }

    if (m_editorFrameCtx)
        SaveEditorSettings(m_projectRoot, m_editorFrameCtx->state);

    const bool sceneSaved = SaveEditorScene();
    if (m_editorFrameCtx)
        m_editorFrameCtx->lastFileMessage = sceneSaved
            ? "Projekt gespeichert."
            : "Projektdatei gespeichert, Scene-Speichern fehlgeschlagen.";
    return sceneSaved;
}

bool KromEditorApp::LoadProjectFile(const std::string& projectPath)
{
    if (projectPath.empty())
    {
        if (m_editorFrameCtx)
            m_editorFrameCtx->lastFileMessage = "Projektpfad fehlt.";
        return false;
    }

    std::filesystem::path inputPath(projectPath);
    std::filesystem::path projectFilePath = inputPath;
    if (std::filesystem::is_directory(inputPath))
        projectFilePath = inputPath / "krom-project.json";
    if (projectFilePath.filename() != "krom-project.json")
        projectFilePath /= "krom-project.json";

    std::ifstream in(projectFilePath, std::ios::binary);
    if (!in)
    {
        if (m_editorFrameCtx)
            m_editorFrameCtx->lastFileMessage = "Projektdatei nicht gefunden.";
        return false;
    }

    const std::string json((std::istreambuf_iterator<char>(in)),
                           std::istreambuf_iterator<char>());
    std::string err;
    const serialization::JsonValue root = serialization::JsonParser::Parse(json, err);
    if (!err.empty() || !root.IsObject())
    {
        if (m_editorFrameCtx)
            m_editorFrameCtx->lastFileMessage = "Projektdatei ist ungueltig.";
        return false;
    }

    const std::filesystem::path projectRoot = projectFilePath.parent_path();
    const std::string name = root.Get("name") ? root.Get("name")->AsString() : projectRoot.filename().string();
    const std::string runtimeTarget = root.Get("runtimeTarget")
        ? root.Get("runtimeTarget")->AsString()
        : name;
    const std::string backendId = root.Get("backend") ? root.Get("backend")->AsString() : "vulkan";
    const renderer::DeviceFactory::BackendType backend = BackendFromProjectId(backendId);

    m_projectRoot = projectRoot;
    m_projectName = name;
    m_projectRuntimeTarget = SanitizeRuntimeTargetName(runtimeTarget);
    m_projectBackend = backend;

    // Editor-Einstellungen laden (Kamera FOV/Near/Far/Speed)
    if (m_editorFrameCtx)
        LoadEditorSettings(m_projectRoot, m_editorFrameCtx->state);

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

    // Projekt-Load ist deterministisch: immer die erste existierende Szene
    // aus sceneOrder laden. editorScene bleibt nur als gespeicherte Info erhalten,
    // darf aber beim Öffnen des Projekts nicht die Startszene bestimmen.
    m_currentSceneName = "editor_scene";
    {
        const auto scenePath = [&](const std::string& name) {
            return projectRoot / "Assets" / "Scenes" / (name + ".json");
        };

        bool found = false;
        for (const std::string& s : m_sceneOrder)
        {
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
                    diskScenes.push_back(entry.path().stem().string());
            }
            std::sort(diskScenes.begin(), diskScenes.end());
            if (!diskScenes.empty())
            {
                m_currentSceneName = diskScenes.front();
                found = true;
            }
        }

        if (found)
            Debug::Log("KromEditorApp: project load uses first scene '%s'.",
                       m_currentSceneName.c_str());
    }

    m_loadedSceneNames = { m_currentSceneName };

    ApplyEditorProjectPaths();

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
                      projectRoot.string().c_str());
    }

    const bool sceneLoaded = std::filesystem::exists(GetCurrentEditorScenePath())
        ? LoadEditorScene()
        : (SyncEditorSceneState(), true);

    if (m_editorFrameCtx)
        m_editorFrameCtx->lastFileMessage = sceneLoaded
            ? "Projekt geladen."
            : "Projekt geladen, Scene-Laden fehlgeschlagen.";

    return sceneLoaded;
}

void KromEditorApp::ApplyEditorProjectPaths()
{
    if (!m_assetPipeline)
        return;

    // Pfad-Caches leeren, damit relative Asset-Pfade des neuen Projekts nicht
    // fälschlicherweise auf Handles aus einer vorherigen Session zeigen.
    m_assetRegistry.ClearPathCaches();

    // SharedMaterialBindings-Cache leeren: veraltete (materialPath, meshHandle)-
    // Einträge aus dem alten Projekt würden sonst die Material-Zuweisung überspringen.
    renderer::addons::editor::ClearSharedMaterialBindings();

    const std::filesystem::path assetRoot = GetCurrentAssetRoot();
    m_assetPipeline->SetAssetRoot(assetRoot.string());
    renderer::ShaderCompiler::SetCacheDirectory(GetCurrentShaderCacheDir());
    renderer::ShaderCompiler::SetEngineAssetDirectory(ResolveAssetRoot());

    if (m_editorFrameCtx)
    {
        m_editorFrameCtx->currentProjectName = m_projectName;
        m_editorFrameCtx->currentProjectRoot = m_projectRoot.string();
        m_editorFrameCtx->projectBackend = m_projectBackend;
        // m_currentSceneName und m_loadedSceneNames wurden bereits vom Aufrufer gesetzt
        // (LoadProjectFile oder CreateEditorProject) — hier nicht überschreiben.
        RefreshSceneList();
    }

    m_assetBrowserState.initialized = false;
    m_assetBrowserState.currentDir = assetRoot;

    // Alle Texturen im Asset-Root vorladen damit der erste Klick auf "Textures"
    // sofort reagiert (kein Warten auf On-Demand-Import).
    if (m_editorFrameCtx)
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
            m_editorFrameCtx->lastFileMessage = "Projektname fehlt.";
        return false;
    }
    if (parentDirectory.empty())
    {
        if (m_editorFrameCtx)
            m_editorFrameCtx->lastFileMessage = "Projektordner fehlt.";
        return false;
    }

    const std::filesystem::path parentPath = std::filesystem::path(parentDirectory);
    const std::filesystem::path projectRoot = parentPath / sanitizedName;
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
            m_editorFrameCtx->lastFileMessage = "Projektordner konnten nicht erstellt werden.";
        return false;
    }

    {
        std::ofstream out(projectFilePath, std::ios::binary | std::ios::trunc);
        if (!out)
        {
            Debug::LogError("KromEditorApp: failed to write project file: %s", projectFilePath.string().c_str());
            if (m_editorFrameCtx)
                m_editorFrameCtx->lastFileMessage = "Projektdatei konnte nicht geschrieben werden.";
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
                m_editorFrameCtx->lastFileMessage = "Projektdatei konnte nicht geschrieben werden.";
            return false;
        }
    }

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
    m_projectName = projectName;
    m_projectRuntimeTarget = runtimeTarget;
    m_projectBackend = backend;
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

    SyncEditorSceneState();
    RefreshSceneList();

    if (m_editorFrameCtx)
        m_editorFrameCtx->lastFileMessage = "Projekt erstellt: " + projectRoot.string();

    if (m_editorFrameCtx)
        SaveEditorSettings(m_projectRoot, m_editorFrameCtx->state);

    return SaveEditorScene();
}

void KromEditorApp::SyncEditorSceneState()
{
    if (!m_world)
        return;

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

    // Mesh-Handles zuerst wiederherstellen — ResolveMaterialAssetBindings benoetigt
    // gueltige Mesh-Handles um Runtime-Materialien zu erstellen.
    if (m_assetPipeline)
        addons::mesh_renderer::ResolveMeshAssetBindings(*m_assetPipeline, m_assetRegistry, *m_world);

    // Kamera-Gizmo-Entities VOR ResolveMaterialAssetBindings erstellen:
    // EnsureCameraGizmos setzt MaterialComponent::materialAssetPath,
    // ResolveMaterialAssetBindings liest diesen Pfad und erstellt das GPU-Handle.
#ifdef KROM_EDITOR_HAS_IMGUI
    EnsureCameraGizmos();
#endif

    // Material-Bindings nach den Meshes aufloesen — erfasst jetzt auch die
    // soeben erstellten Gizmo-Entities.
    if (m_editorFrameCtx)
    {
        renderer::addons::editor::ResolveMaterialAssetBindings(*m_editorFrameCtx);
        // Prefab-Instanzen nach Szenen-Load synchronisieren
        engine::addons::editor::ResolvePrefabBindings(*m_editorFrameCtx);
    }

    mesh_renderer::UpdateLocalBoundsFromMeshes(*m_world, m_assetRegistry);
    m_transformSystem.Update(*m_world);
    m_boundsSystem.Update(*m_world);
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
    const std::string kGlbPath = (editorDir / "camera.glb").generic_string();
    const std::string kMatPath = (editorDir / "matcam.mat").generic_string();
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
            Debug::Log("EnsureCameraGizmos: Legacy-Gizmo %u entfernt", id.value);
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
        Debug::Log("EnsureCameraGizmos: erstelle Gizmo fuer Kamera %u ('%s')",
                   id.value, nc ? nc->name.c_str() : "?");
    }

    if (camerasNeedingGizmo.empty())
        return;

    // Mesh aus Cache holen oder einmalig importieren
    if (!m_cameraGizmoMeshHandle.IsValid())
    {
        Debug::Log("KromEditorApp::EnsureCameraGizmos: Lade aus '%s'", editorDir.string().c_str());
        assets::ImportedAssetBundle bundle = m_assetPipeline->ImportBundle(kGlbPath);
        if (!bundle.Ok() || bundle.meshes.empty())
        {
            Debug::LogWarning("KromEditorApp::EnsureCameraGizmos: camera.glb nicht gefunden (%s) — Fehler: %s",
                              kGlbPath.c_str(), bundle.error.c_str());
            return;
        }
        Debug::Log("KromEditorApp::EnsureCameraGizmos: camera.glb geladen (%zu Submeshes)",
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

        // Transform aus Kamera kopieren
        auto& gt = m_world->Add<TransformComponent>(gizmoId);
        if (const auto* camT = m_world->Get<TransformComponent>(cameraId))
        {
            gt.localPosition = camT->localPosition;
            gt.localRotation = camT->localRotation;
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

        Debug::Log("KromEditorApp: Kamera-Gizmo erstellt (camera=%u, gizmo=%u)",
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
        const auto* camT = m_world->Get<TransformComponent>(gizmo->cameraEntity);
        if (!camT) return;
        auto* gizT = m_world->Get<TransformComponent>(id);
        if (!gizT) return;
        gizT->localPosition = camT->localPosition;
        gizT->localRotation = camT->localRotation;
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

    auto& transform = m_world->Add<TransformComponent>(id);
    transform.localPosition = {0.f, 0.f, 0.f};
    transform.localRotation = math::Quat::FromEulerDeg(0.f, -25.f, 0.f);
    transform.localScale = {1.f, 1.f, 1.f};
    transform.dirty = true;
    m_world->Add<WorldTransformComponent>(id);
    m_world->Add<BoundsComponent>(id);
    m_world->Add<EditorMaterialPreviewComponent>(id);

    MeshComponent mesh{};
    mesh.mesh = m_materialPreviewSphereMeshHandle;
    mesh.meshAssetPath = "__editor/material_preview_sphere";
    mesh.visible = true;
    mesh.castShadows = false;
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

    if (auto* mesh = m_world->Get<MeshComponent>(m_materialPreviewEntity))
    {
        mesh->visible = previewOpen;
        mesh->layerMask = renderer::LAYER_EDITOR_MATERIAL_PREVIEW;
    }

    if (!previewOpen)
        return;

    if (auto* transform = m_world->Get<TransformComponent>(m_materialPreviewEntity))
    {
        transform->localRotation =
            math::Quat::FromEulerDeg(m_editorFrameCtx->state.materialPreviewPitchDeg,
                                     m_editorFrameCtx->state.materialPreviewYawDeg,
                                     0.f);
        transform->dirty = true;
    }

    if (auto* material = m_world->Get<MaterialComponent>(m_materialPreviewEntity))
        material->materialAssetPath = m_editorFrameCtx->state.selectedMaterialAssetPath;

    renderer::addons::editor::ApplyMaterialAssetToEntity(
        *m_editorFrameCtx,
        m_materialPreviewEntity,
        std::filesystem::path(m_editorFrameCtx->state.selectedMaterialAssetPath));
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

    const float distance = std::clamp(m_editorFrameCtx->state.materialPreviewDistance, 2.0f, 8.0f);
    const math::Vec3 eye{0.f, 0.18f, distance};
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
    outView.backgroundMode = BackgroundMode::Skybox;
    outView.enableBloom = false;
    outView.clearColor = {0.f, 0.f, 0.f, 1.f};
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
    outView.lightLayerMask = renderer::LAYER_NONE;
    outView.backgroundMode = BackgroundMode::Skybox;
    outView.enableBloom    = false;
    outView.clearColor     = {0.08f, 0.08f, 0.1f, 1.f};
    return true;
}

void KromEditorApp::ProcessAssetThumbnailQueue()
{
    if (!m_world || !m_editorFrameCtx || !m_editorFrameCtx->state.assetBrowser)
        return;

    using namespace renderer::addons::editor;
    AssetBrowserState& browser = *m_editorFrameCtx->state.assetBrowser;

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
    m_activeAssetThumbnailCenter = {0.f, 0.f, 0.f};
    m_activeAssetThumbnailRadius = 1.f;

    AssetBrowserState::AssetPreviewThumbnail& thumb = queuedIt->second;
    const std::filesystem::path path = std::filesystem::path(queuedIt->first);
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
    if (thumb.kind == AssetBrowserState::PreviewKind::Material)
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
        material.materialAssetPath = path.string();
        m_world->Add<MaterialComponent>(entity, material);
        ApplyMaterialAssetToEntity(*m_editorFrameCtx, entity, path);
        m_activeAssetThumbnailCenter = {0.f, 0.f, 0.f};
        m_activeAssetThumbnailRadius = 1.f;
        createdPreview = true;
    }
    else if (thumb.kind == AssetBrowserState::PreviewKind::Model)
    {
        if (!m_assetPipeline)
        {
            thumb.status = AssetBrowserState::PreviewStatus::Failed;
            return;
        }

        assets::ImportedAssetBundle bundle = m_assetPipeline->ImportBundle(path.string());
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

        math::Vec3 min{ FLT_MAX, FLT_MAX, FLT_MAX };
        math::Vec3 max{ -FLT_MAX, -FLT_MAX, -FLT_MAX };
        for (const assets::MeshAsset& mesh : bundle.meshes)
        {
            math::Vec3 meshMin;
            math::Vec3 meshMax;
            mesh.ComputeBounds(meshMin, meshMax);
            min.x = std::min(min.x, meshMin.x);
            min.y = std::min(min.y, meshMin.y);
            min.z = std::min(min.z, meshMin.z);
            max.x = std::max(max.x, meshMax.x);
            max.y = std::max(max.y, meshMax.y);
            max.z = std::max(max.z, meshMax.z);
        }
        const math::Vec3 center = (min + max) * 0.5f;
        const math::Vec3 extents = (max - min) * 0.5f;
        const float radius = std::max(0.1f, std::sqrt(extents.x * extents.x +
                                                      extents.y * extents.y +
                                                      extents.z * extents.z));

        auto prefab = engine::addons::prefab::BuildPrefabFromImportedBundle(
            std::move(bundle),
            m_assetRegistry,
            engine::addons::prefab::PrefabBuildOptions{path.stem().string(), true, false, false});
        engine::addons::prefab::PrefabInstantiateOptions options{};
        options.position = -center;
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
        m_activeAssetThumbnailCenter = {0.f, 0.f, 0.f};
        m_activeAssetThumbnailRadius = radius;
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

        math::Vec3 min{ FLT_MAX, FLT_MAX, FLT_MAX };
        math::Vec3 max{ -FLT_MAX, -FLT_MAX, -FLT_MAX };
        for (const engine::addons::prefab::PrefabEntityRecord& record : prefab.records)
        {
            const math::Vec3 halfExtents{
                std::max(0.05f, std::abs(record.localScale.x) * 0.5f),
                std::max(0.05f, std::abs(record.localScale.y) * 0.5f),
                std::max(0.05f, std::abs(record.localScale.z) * 0.5f)
            };
            const math::Vec3 pMin = record.localPosition - halfExtents;
            const math::Vec3 pMax = record.localPosition + halfExtents;
            min.x = std::min(min.x, pMin.x);
            min.y = std::min(min.y, pMin.y);
            min.z = std::min(min.z, pMin.z);
            max.x = std::max(max.x, pMax.x);
            max.y = std::max(max.y, pMax.y);
            max.z = std::max(max.z, pMax.z);
        }

        if (min.x == FLT_MAX)
        {
            min = {-0.5f, -0.5f, -0.5f};
            max = { 0.5f,  0.5f,  0.5f};
        }
        const math::Vec3 center = (min + max) * 0.5f;
        const math::Vec3 extents = (max - min) * 0.5f;
        const float radius = std::max(0.1f, std::sqrt(extents.x * extents.x +
                                                      extents.y * extents.y +
                                                      extents.z * extents.z));

        engine::addons::prefab::PrefabInstantiateOptions options{};
        options.position = -center;
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

        m_activeAssetThumbnailCenter = {0.f, 0.f, 0.f};
        m_activeAssetThumbnailRadius = radius;
        createdPreview = true;
    }

    if (!createdPreview)
    {
        thumb.status = AssetBrowserState::PreviewStatus::Failed;
        return;
    }

    m_activeAssetThumbnailRT = thumb.renderTarget;
    m_activeAssetThumbnailPath = path;
    thumb.status = AssetBrowserState::PreviewStatus::Rendering;
}

bool KromEditorApp::BuildAssetThumbnailRenderView(renderer::RenderView& outView) const noexcept
{
    if (!m_activeAssetThumbnailRT.IsValid())
        return false;

    const float fov = 35.0f * math::DEG_TO_RAD;
    const float distance = std::max(1.5f, (m_activeAssetThumbnailRadius / std::tan(fov * 0.5f)) * 1.35f);
    const math::Vec3 target = m_activeAssetThumbnailCenter;
    const math::Vec3 eye = target + math::Vec3{0.f, m_activeAssetThumbnailRadius * 0.12f, distance};
    outView.view = math::Mat4::LookAtRH(eye, target, math::Vec3::Up());
    outView.projection = math::Mat4::PerspectiveFovRH(fov, 1.0f, 0.03f, std::max(20.f, distance + m_activeAssetThumbnailRadius * 4.f));
    outView.cameraPosition = eye;
    outView.cameraForward = (target - eye).Normalized();
    outView.nearPlane = 0.03f;
    outView.farPlane = std::max(20.f, distance + m_activeAssetThumbnailRadius * 4.f);
    outView.ambientColor = {0.28f, 0.28f, 0.28f};
    outView.ambientIntensity = 0.7f;
    outView.debugFlags = 0u;
    outView.visibilityLayerMask = renderer::LAYER_EDITOR_ASSET_THUMBNAIL;
    outView.backgroundMode = BackgroundMode::ClearColor;
    outView.enableBloom = false;
    outView.clearColor = {0.f, 0.f, 0.f, 1.f};
    return true;
}
#else
void KromEditorApp::EnsureCameraGizmos() {}
void KromEditorApp::SyncCameraGizmoTransforms() {}
void KromEditorApp::EnsureMaterialPreviewEntity() {}
void KromEditorApp::SyncMaterialPreviewEntity() {}
bool KromEditorApp::BuildMaterialPreviewRenderView(renderer::RenderView&) const noexcept { return false; }
void KromEditorApp::ProcessAssetThumbnailQueue() {}
bool KromEditorApp::BuildAssetThumbnailRenderView(renderer::RenderView&) const noexcept { return false; }
#endif // KROM_EDITOR_HAS_IMGUI

bool KromEditorApp::SaveEditorScene()
{
    if (!m_world)
        return false;

    NormalizeEditorObjectLayerMasks(*m_world);
    EnsureEntityGuids(*m_world);

#ifdef KROM_EDITOR_HAS_IMGUI
    if (m_editorFrameCtx && !m_projectRoot.empty())
    {
        const auto bindings = renderer::addons::editor::RefreshCppScriptProject(*m_editorFrameCtx);
        if (!bindings.ok)
        {
            m_editorFrameCtx->lastFileMessage = bindings.message.empty()
                ? "Script-Bindings konnten nicht generiert werden."
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

    const std::filesystem::path path = GetCurrentEditorScenePath();
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    if (ec)
    {
        Debug::LogError("KromEditorApp: editor scene directory could not be created: %s",
                        ec.message().c_str());
        return false;
    }

    serialization::SceneSerializer serializer(*m_world);
    serializer.RegisterDefaultHandlers();
    addons::camera::RegisterCameraSerializationHandlers(serializer);
    addons::lighting::RegisterLightingSerializationHandlers(serializer);
    addons::mesh_renderer::RegisterMeshRendererSerializationHandlers(serializer);
    addons::prefab::RegisterPrefabSerializationHandlers(serializer);
    engine::script::RegisterScriptSerializationHandlers(serializer);
    renderer::addons::editor::RegisterEditorSerializationHandlers(serializer);

    // Fix: Nur Entities der aktuellen Szene speichern.
    // Persistente Entities anderer Szenen (EditorSceneTagComponent.sceneName != m_currentSceneName)
    // werden übersprungen — sie gehören in ihre eigene Szenendatei, nicht in diese.
    // Kamera-Gizmo-Entities (EditorCameraGizmoComponent) sind reine Laufzeit-Hilfsobjekte
    // und werden ebenfalls nicht gespeichert.
    {
        const std::string saveSceneName = m_currentSceneName;
        serializer.SetEntityFilter([this, saveSceneName](EntityID id) -> bool {
            using namespace renderer::addons::editor;
            if (m_world->Has<EditorRuntimeGizmoTag>(id))
                return false;  // Laufzeit-Gizmo-Entity nie speichern
            if (m_world->Has<EditorMaterialPreviewComponent>(id))
                return false;  // Materialpreview nie speichern
            if (m_world->Has<EditorAssetThumbnailComponent>(id))
                return false;  // Asset-Thumbnails nie speichern
            // Ungetaggte Kameras ohne EditorHierarchyComponent sind System-Kameras
            // (z.B. alte EditorScene::Build-Kamera) — nie speichern
            if (m_world->Has<CameraComponent>(id) &&
                !m_world->Has<EditorHierarchyComponent>(id))
                return false;
            const auto* tag = m_world->Get<EditorSceneTagComponent>(id);
            if (!tag) return true;                     // ungetaggt → aktuelle Szene
            return tag->sceneName == saveSceneName;    // nur eigene Entities
        });
    }

    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out)
    {
        Debug::LogError("KromEditorApp: failed to open editor scene for writing: %s",
                        path.string().c_str());
        return false;
    }

    std::string sceneJson = serializer.SerializeToJson("EditorScene");
    if (m_editorFrameCtx)
        sceneJson = InjectEditorSceneMetadataJson(std::move(sceneJson), m_editorFrameCtx->state);
    out << sceneJson;
    if (!out.good())
    {
        Debug::LogError("KromEditorApp: failed to write editor scene: %s",
                        path.string().c_str());
        return false;
    }

    Debug::Log("KromEditorApp: editor scene saved to %s", path.string().c_str());
    return true;
}

bool KromEditorApp::LoadEditorScene(bool clearFirst, const std::string& sceneTag)
{
    if (!m_world)
        return false;

    // sceneTag != "" → additives Laden einer anderen Szene;
    // sceneTag == "" → primäre Szene (m_currentSceneName) laden.
    const std::filesystem::path path = sceneTag.empty()
        ? GetCurrentEditorScenePath()
        : GetEditorScenePath(sceneTag);

    std::ifstream in(path, std::ios::binary);
    if (!in)
    {
        if (clearFirst)
        {
            // Normaler Reload einer Datei die nicht existiert → Fehler
            Debug::LogError("KromEditorApp: failed to open editor scene for reading: %s",
                            path.string().c_str());
            return false;
        }
        // Szenenwechsel / additives Laden in eine noch nicht existierende Datei
        // ist kein Fehler — leere Szene.
        Debug::Log("KromEditorApp: neue Szene '%s' (noch keine Datei vorhanden)",
                   path.string().c_str());
        SyncEditorSceneState();
        return true;
    }

    const std::string json((std::istreambuf_iterator<char>(in)),
                           std::istreambuf_iterator<char>());

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
    renderer::addons::editor::RegisterEditorDeserializationHandlers(deserializer);

    const serialization::DeserializeResult result = deserializer.DeserializeFromJson(json);
    if (!result.success)
    {
        Debug::LogError("KromEditorApp: failed to load editor scene: %s", result.error.c_str());
        return false;
    }
    engine::script::ResolveScriptEntityReferences(*m_world, m_scriptRegistry);

    if (sceneTag.empty())
    {
        std::string sceneMetaErr;
        const serialization::JsonValue sceneRoot = serialization::JsonParser::Parse(json, sceneMetaErr);
        if (sceneMetaErr.empty() && sceneRoot.IsObject())
        {
            ApplyEditorCameraFromSceneJson(m_editorFrameCtx.get(), sceneRoot);
            ApplyEditorEnvironmentFromSceneJson(m_editorFrameCtx.get(), sceneRoot);
        }
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

    SyncEditorSceneState();

    Debug::Log("KromEditorApp: editor scene loaded from %s (%u entities, %u components)",
               path.string().c_str(),
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
            result.push_back(entry.path().stem().string());
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

void KromEditorApp::CreateDefaultSceneEntities()
{
    if (!m_world || !m_editorFrameCtx)
        return;

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

void KromEditorApp::RefreshSceneList()
{
    if (!m_editorFrameCtx)
        return;
    m_editorFrameCtx->currentEditorSceneName = m_currentSceneName;
    m_editorFrameCtx->availableEditorScenes  = ListEditorScenes();
    RefreshLoadedScenesState();
    RefreshSceneOrderState();
}

bool KromEditorApp::SwitchEditorScene(const std::string& sceneName, bool additive)
{
    if (!m_world || sceneName.empty())
        return false;

    // 1. Aktuelle Szene speichern bevor wir wechseln.
    SaveEditorScene();

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

        LoadEditorScene(/*clearFirst=*/false, /*sceneTag=*/"");
        m_transformSystem.Update(*m_world);
        m_boundsSystem.Update(*m_world);

        if (m_editorFrameCtx)
        {
            ClearEditorHistory(m_editorFrameCtx->state);
            m_editorFrameCtx->lastFileMessage = "Szene geladen: " + sceneName;
        }

        Debug::Log("KromEditorApp: Szenenwechsel → '%s'", sceneName.c_str());
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
                m_editorFrameCtx->lastFileMessage = "Szene bereits geladen: " + sceneName;
            return false;
        }

        m_loadedSceneNames.push_back(sceneName);
        RefreshLoadedScenesState();

        // sceneTag != "" → LoadEditorScene lädt aus GetEditorScenePath(sceneTag)
        // und taggt neue Entities mit diesem Namen (nicht m_currentSceneName).
        if (!LoadEditorScene(/*clearFirst=*/false, /*sceneTag=*/sceneName))
        {
            // Rückgängig machen wenn Laden fehlschlug
            m_loadedSceneNames.erase(
                std::remove(m_loadedSceneNames.begin(), m_loadedSceneNames.end(), sceneName),
                m_loadedSceneNames.end());
            RefreshLoadedScenesState();
            if (m_editorFrameCtx)
                m_editorFrameCtx->lastFileMessage = "Additives Laden fehlgeschlagen: " + sceneName;
            return false;
        }

        m_transformSystem.Update(*m_world);
        m_boundsSystem.Update(*m_world);

        if (m_editorFrameCtx)
            m_editorFrameCtx->lastFileMessage = "Additiv geladen: " + sceneName;

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
        m_editorFrameCtx->lastFileMessage = "Szene entladen: " + sceneName;

    Debug::Log("KromEditorApp: Szene '%s' entladen (%u Entities entfernt).",
               sceneName.c_str(), static_cast<uint32_t>(taggedIds.size()));
    return true;
}

// =============================================================================
// Kscene-Export (Build-System)
// =============================================================================

namespace {

// Quat → Euler-Grad, YXZ-Ordnung — konsistent mit Quat::FromEulerDeg
static math::Vec3 QuatToEulerDeg(const math::Quat& q) noexcept
{
    constexpr float RAD_TO_DEG = 180.f / 3.14159265f;
    const float sinP = 2.f * (q.w * q.x - q.y * q.z);
    const float pitch = (std::abs(sinP) >= 0.9999f)
        ? std::copysign(90.f, sinP)
        : std::asin(sinP) * RAD_TO_DEG;
    const float yaw = std::atan2(
        2.f * (q.x * q.z + q.w * q.y),
        1.f - 2.f * (q.x * q.x + q.y * q.y)) * RAD_TO_DEG;
    const float roll = std::atan2(
        2.f * (q.x * q.y + q.w * q.z),
        1.f - 2.f * (q.x * q.x + q.z * q.z)) * RAD_TO_DEG;
    return {pitch, yaw, roll};
}

// BFS: findet meshAssetPath in Entity + Kinder; entfernt "#mesh/N"-Suffix
static std::string FindMeshPath(const ecs::World& world, EntityID root)
{
    std::vector<EntityID> queue{root};
    while (!queue.empty())
    {
        const EntityID e = queue.back(); queue.pop_back();
        if (!world.IsAlive(e)) continue;
        if (const auto* mc = world.Get<MeshComponent>(e))
        {
            if (!mc->meshAssetPath.empty())
            {
                std::string path = mc->meshAssetPath;
                const size_t hash = path.find('#');
                if (hash != std::string::npos) path.erase(hash);
                return path;
            }
        }
        if (const auto* ch = world.Get<ChildrenComponent>(e))
            for (const EntityID c : ch->children) queue.push_back(c);
    }
    return {};
}

// BFS: findet materialAssetPath in Entity + Kinder (erster Treffer)
static std::string FindMaterialPath(const ecs::World& world, EntityID root)
{
    std::vector<EntityID> queue{root};
    while (!queue.empty())
    {
        const EntityID e = queue.back(); queue.pop_back();
        if (!world.IsAlive(e)) continue;
        if (const auto* mc = world.Get<MaterialComponent>(e))
            if (!mc->materialAssetPath.empty()) return mc->materialAssetPath;
        if (const auto* ch = world.Get<ChildrenComponent>(e))
            for (const EntityID c : ch->children) queue.push_back(c);
    }
    return {};
}

struct RuntimeScriptExport
{
    std::string className;
    std::unordered_map<std::string, engine::script::ScriptFieldValue> fields;
};

static void WriteScriptFieldValue(serialization::JsonWriter& w,
                                  const std::string& name,
                                  const engine::script::ScriptFieldValue& value)
{
    using engine::script::ScriptFieldType;

    switch (value.type)
    {
    case ScriptFieldType::Float: w.WriteFloat(name, value.floatValue); break;
    case ScriptFieldType::Int:   w.WriteInt(name, value.intValue); break;
    case ScriptFieldType::Bool:  w.WriteBool(name, value.boolValue); break;
    case ScriptFieldType::Vec3:  w.WriteVec3(name, value.vec3Value); break;
    case ScriptFieldType::Entity:
    case ScriptFieldType::String:
    case ScriptFieldType::Prefab:
        w.WriteString(name, value.stringValue);
        break;
    }
}

static std::vector<RuntimeScriptExport> FindScriptExports(const ecs::World& world, EntityID root)
{
    std::vector<RuntimeScriptExport> result;
    std::vector<EntityID> queue{root};
    while (!queue.empty())
    {
        const EntityID e = queue.back(); queue.pop_back();
        if (!world.IsAlive(e)) continue;
        if (const auto* scripts = world.Get<engine::script::ScriptList>(e))
        {
            for (const auto& inst : scripts->Instances())
                if (!inst.className.empty())
                    result.push_back({inst.className, inst.fieldValues});
        }
        if (const auto* ch = world.Get<ChildrenComponent>(e))
            for (const EntityID c : ch->children) queue.push_back(c);
    }
    return result;
}

// Konvertiert eine World in das .kscene-JSON (Runtime-Format).
// Wird sowohl vom Live-Export (aktuelle World) als auch vom Batch-Build
// (temporäre World pro Datei) genutzt.
static std::string WorldToKsceneJson(const ecs::World& world, const std::string& sceneName,
                                     const renderer::addons::editor::EditorState* env = nullptr)
{
    serialization::JsonWriter w;
    w.BeginObject();
    w.WriteString("scene", sceneName);
    w.WriteInt("version", 1);
    w.WriteBool("persistent", false);

    // ── Environment-Block ─────────────────────────────────────────────────────
    if (env)
    {
        w.BeginObject("environment");
        w.WriteString("texturePath",   env->environmentTexturePath);
        w.WriteBool  ("enableIBL",     env->environmentEnableIBL);
        w.WriteFloat ("iblIntensity",  env->environmentIntensity);
        w.BeginArray ("backgroundColor");
        for (float v : env->backgroundColor) w.WriteFloat("", v);
        w.EndArray();
        w.BeginArray ("ambientColor");
        w.WriteFloat("", env->ambientColor.x);
        w.WriteFloat("", env->ambientColor.y);
        w.WriteFloat("", env->ambientColor.z);
        w.EndArray();
        w.WriteFloat ("ambientIntensity", env->ambientIntensity);
        w.EndObject();
    }
    w.BeginArray("entities");

    world.ForEachAlive([&](EntityID id)
    {
        if (world.Has<renderer::addons::editor::EditorRuntimeGizmoTag>(id)) return;
        if (world.Has<renderer::addons::editor::EditorMaterialPreviewComponent>(id)) return;
        if (world.Has<renderer::addons::editor::EditorAssetThumbnailComponent>(id)) return;

        // Nur Root-Entities — Child-Entities gehören zum importierten Bundle
        if (world.Has<ParentComponent>(id)) return;

        const std::string meshPath = FindMeshPath(world, id);
        const bool hasMesh   = !meshPath.empty();
        const bool hasLight  = world.Has<LightComponent>(id);
        const bool hasCamera = world.Has<CameraComponent>(id);
        const std::vector<RuntimeScriptExport> scriptExports = FindScriptExports(world, id);
        const bool hasScripts = !scriptExports.empty();

        // Editor-interne Entities ohne Runtime-Relevanz überspringen
        if (!hasMesh && !hasLight && !hasCamera && !hasScripts) return;

        w.BeginObject();

        const std::string name = world.Has<NameComponent>(id)
            ? world.Get<NameComponent>(id)->name
            : "Entity";
        w.WriteString("name", name);
        if (const auto* guid = world.Get<GuidComponent>(id))
            if (!guid->guid.empty())
                w.WriteString("guid", guid->guid);

        if (const auto* tr = world.Get<TransformComponent>(id))
        {
            w.WriteVec3("position", tr->localPosition);
            w.WriteVec3("rotationEulerDeg", QuatToEulerDeg(tr->localRotation));
            w.WriteVec3("scale", tr->localScale);
        }

        if (hasMesh)
        {
            w.WriteString("model", meshPath);
            const std::string matPath = FindMaterialPath(world, id);
            if (!matPath.empty())
                w.WriteString("material", matPath);
        }

        if (hasLight)
        {
            const auto* lc = world.Get<LightComponent>(id);
            w.BeginObject("light");
            switch (lc->type)
            {
            case LightType::Point: w.WriteString("type", "point"); break;
            case LightType::Spot:  w.WriteString("type", "spot");  break;
            default:               w.WriteString("type", "directional"); break;
            }
            w.WriteVec3("color", lc->color);
            w.WriteFloat("intensity", lc->intensity);
            w.WriteFloat("range", lc->range);
            if (lc->type == LightType::Spot)
            {
                w.WriteFloat("spotInnerDeg", lc->spotInnerDeg);
                w.WriteFloat("spotOuterDeg", lc->spotOuterDeg);
            }
            w.WriteBool("castShadows", lc->castShadows);
            w.WriteUint("shadowResolution", lc->shadowSettings.resolution);
            w.WriteFloat("shadowBias", lc->shadowSettings.bias);
            w.WriteFloat("shadowNormalBias", lc->shadowSettings.normalBias);
            w.WriteFloat("shadowMaxDistance", lc->shadowSettings.maxDistance);
            w.WriteFloat("shadowStrength", lc->shadowSettings.strength);
            w.WriteUint("shadowCascadeCount", lc->shadowSettings.cascadeCount);
            w.WriteFloat("shadowCascadeLambda", lc->shadowSettings.cascadeLambda);
            w.EndObject();
        }

        if (hasCamera)
        {
            const auto* cam = world.Get<CameraComponent>(id);
            w.BeginObject("camera");
            w.WriteFloat("fovYDeg", cam->fovYDeg);
            w.WriteFloat("nearPlane", cam->nearPlane);
            w.WriteFloat("farPlane", cam->farPlane);
            w.WriteBool("isMain", cam->isMainCamera);
            w.WriteUint("backgroundMode", static_cast<uint32_t>(cam->backgroundMode));
            w.BeginArray("clearColor");
            for (float v : cam->clearColor) w.WriteFloat("", v);
            w.EndArray();
            w.EndObject();
        }

        if (!scriptExports.empty())
        {
            w.BeginArray("scripts");
            for (const RuntimeScriptExport& script : scriptExports)
            {
                if (script.fields.empty())
                {
                    w.WriteString("", script.className);
                    continue;
                }

                w.BeginObject();
                w.WriteString("class", script.className);
                w.BeginObject("fields");
                for (const auto& [fieldName, value] : script.fields)
                    WriteScriptFieldValue(w, fieldName, value);
                w.EndObject();
                w.EndObject();
            }
            w.EndArray();
        }

        w.EndObject();
    });

    w.EndArray();
    w.EndObject();
    return w.GetString();
}

static bool WriteKsceneFile(const std::string& json,
                             const std::filesystem::path& outPath,
                             const char* debugLabel)
{
    std::error_code ec;
    std::filesystem::create_directories(outPath.parent_path(), ec);
    std::ofstream out(outPath, std::ios::binary | std::ios::trunc);
    if (!out)
    {
        Debug::LogError("KromEditorApp: %s: Datei konnte nicht geöffnet werden: %s",
            debugLabel, outPath.string().c_str());
        return false;
    }
    out << json;
    if (!out.good())
    {
        Debug::LogError("KromEditorApp: %s: Schreibfehler: %s",
            debugLabel, outPath.string().c_str());
        return false;
    }
    Debug::Log("KromEditorApp: %s → %s", debugLabel, outPath.string().c_str());
    return true;
}

} // namespace

// Exportiert die aktuell geladene World (schneller Pfad für "aktuelle Szene").
bool KromEditorApp::ExportAsKscene(const std::filesystem::path& outPath)
{
    if (!m_world)
        return false;
    const std::string sceneName = m_currentSceneName.empty() ? "Scene" : m_currentSceneName;
    const renderer::addons::editor::EditorState* envState =
        m_editorFrameCtx ? &m_editorFrameCtx->state : nullptr;
    const bool ok = WriteKsceneFile(WorldToKsceneJson(*m_world, sceneName, envState), outPath, "ExportAsKscene");
    if (ok && m_editorFrameCtx)
        m_editorFrameCtx->lastFileMessage = "Exportiert: " + outPath.filename().string();
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
            editorJsonPath.string().c_str());
        return false;
    }
    const std::string json(std::istreambuf_iterator<char>(in), {});

    // Temporäre World — keine GPU-Ressourcen, nur Komponenten-Daten
    ecs::World tempWorld(m_componentRegistry);

    serialization::SceneDeserializer deser(tempWorld);
    deser.RegisterDefaultHandlers();
    addons::camera::RegisterCameraDeserializationHandlers(deser);
    addons::lighting::RegisterLightingDeserializationHandlers(deser);
    addons::mesh_renderer::RegisterMeshRendererDeserializationHandlers(deser);
    addons::prefab::RegisterPrefabDeserializationHandlers(deser);
    engine::script::RegisterScriptDeserializationHandlers(deser, &m_scriptRegistry);
    // Editor-Komponenten bewusst nicht registriert — wir brauchen sie nicht

    const auto result = deser.DeserializeFromJson(json);
    if (!result.success)
    {
        Debug::LogError("KromEditorApp: Build: Deserialisierung fehlgeschlagen '%s': %s",
            sceneName.c_str(), result.error.c_str());
        return false;
    }
    engine::script::ResolveScriptEntityReferences(tempWorld, m_scriptRegistry);
    EnsureEntityGuids(tempWorld);

    // ChildrenComponent aus ParentComponent-Links rekonstruieren
    // (wird von FindMeshPath/FindMaterialPath für BFS benötigt)
    std::vector<std::pair<EntityID, EntityID>> parentLinks;
    tempWorld.ForEachAlive([&](EntityID id) {
        if (const auto* p = tempWorld.Get<ParentComponent>(id))
            if (tempWorld.IsAlive(p->parent))
                parentLinks.emplace_back(id, p->parent);
    });
    for (const auto& [child, parent] : parentLinks)
    {
        if (auto* ch = tempWorld.Get<ChildrenComponent>(parent))
            ch->Add(child);
        else
            tempWorld.Add<ChildrenComponent>(parent).Add(child);
    }

    const renderer::addons::editor::EditorState* envState =
        m_editorFrameCtx ? &m_editorFrameCtx->state : nullptr;
    return WriteKsceneFile(WorldToKsceneJson(tempWorld, sceneName, envState), kscenePath, "Build");
}

// Exportiert alle nicht-ausgeschlossenen Szenen des Projekts — der eigentliche Build.
bool KromEditorApp::BuildAllScenes()
{
    if (m_projectRoot.empty())
    {
        Debug::LogError("KromEditorApp: Build: Kein Projekt geöffnet");
        if (m_editorFrameCtx)
            m_editorFrameCtx->lastFileMessage = "Build fehlgeschlagen: Kein Projekt geöffnet.";
        return false;
    }

    // Aktuelle Szene sichern bevor wir iterieren
    SaveEditorScene();

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

        const std::filesystem::path jsonPath  = m_projectRoot / "Assets" / "Scenes" / (name + ".json");
        const std::filesystem::path kscenePath = m_projectRoot / "Assets" / "Scenes" / (name + ".kscene");

        if (ExportSceneFileAsKscene(jsonPath, kscenePath, name))
            ++exported;
        else
            ++failed;
    }

    const std::string msg = "Build: " + std::to_string(exported) + " exportiert"
        + (skipped > 0 ? ", " + std::to_string(skipped) + " übersprungen" : "")
        + (failed  > 0 ? ", " + std::to_string(failed)  + " FEHLER" : "")
        + ".";

    Debug::Log("KromEditorApp: %s", msg.c_str());
    if (m_editorFrameCtx)
        m_editorFrameCtx->lastFileMessage = msg;
    return failed == 0;
}

bool KromEditorApp::ExportProjectBuild()
{
    if (m_projectRoot.empty())
    {
        if (m_editorFrameCtx)
            m_editorFrameCtx->lastFileMessage = "Export fehlgeschlagen: Kein Projekt geoeffnet.";
        return false;
    }

    if (!BuildAllScenes())
        return false;

    if (!BuildRuntimeTarget(m_projectRuntimeTarget, m_projectBackend,
                            m_editorFrameCtx ? &m_editorFrameCtx->state : nullptr))
    {
        if (m_editorFrameCtx)
            m_editorFrameCtx->lastFileMessage = "Export fehlgeschlagen: Runtime-Build fehlgeschlagen.";
        return false;
    }

    const std::filesystem::path exe = RuntimeExecutablePath(m_projectRuntimeTarget, m_projectBackend);
    if (!std::filesystem::exists(exe))
    {
        if (m_editorFrameCtx)
            m_editorFrameCtx->lastFileMessage = "Export fehlgeschlagen: Runtime-EXE nicht gefunden.";
        return false;
    }

    const std::string configName = std::filesystem::path(KROM_BUILD_DIR).filename().string();
    const std::filesystem::path exportDir =
        m_projectRoot / "Build" / (configName + "-" + RuntimeBackendId(m_projectBackend));

    std::error_code ec;
    std::filesystem::create_directories(exportDir, ec);
    if (ec)
    {
        if (m_editorFrameCtx)
            m_editorFrameCtx->lastFileMessage = "Export fehlgeschlagen: Build-Ordner konnte nicht erstellt werden.";
        return false;
    }

    std::filesystem::copy_file(exe, exportDir / exe.filename(),
        std::filesystem::copy_options::overwrite_existing, ec);
    if (ec)
    {
        if (m_editorFrameCtx)
            m_editorFrameCtx->lastFileMessage = "Export fehlgeschlagen: EXE konnte nicht kopiert werden.";
        return false;
    }

    const std::filesystem::path projectFile = m_projectRoot / "krom-project.json";
    std::filesystem::copy_file(projectFile, exportDir / "krom-project.json",
        std::filesystem::copy_options::overwrite_existing, ec);
    if (ec)
    {
        if (m_editorFrameCtx)
            m_editorFrameCtx->lastFileMessage = "Export fehlgeschlagen: Projektdatei konnte nicht kopiert werden.";
        return false;
    }

    const std::filesystem::path sourceAssets = m_projectRoot / "Assets";
    const std::filesystem::path targetAssets = exportDir / "Assets";
    std::filesystem::copy(sourceAssets, targetAssets,
        std::filesystem::copy_options::recursive |
        std::filesystem::copy_options::overwrite_existing, ec);
    if (ec)
    {
        if (m_editorFrameCtx)
            m_editorFrameCtx->lastFileMessage = "Export fehlgeschlagen: Assets konnten nicht kopiert werden.";
        return false;
    }

    const std::filesystem::path d3dCompiler = exe.parent_path() / "d3dcompiler_47.dll";
    if (std::filesystem::exists(d3dCompiler))
    {
        std::filesystem::copy_file(d3dCompiler, exportDir / "d3dcompiler_47.dll",
            std::filesystem::copy_options::overwrite_existing, ec);
        ec.clear();
    }

    if (m_editorFrameCtx)
    {
        m_editorFrameCtx->lastFileMessage = "Exportiert: " + exportDir.string();
        AddEditorConsoleLog(&m_editorFrameCtx->state, engine::LogLevel::Info,
            "[Export] " + exportDir.string());
    }
    return true;
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

    std::string finalSceneName = sceneName;
    if (!m_projectRoot.empty())
    {
        const std::filesystem::path scenesDir = m_projectRoot / "Assets" / "Scenes";
        const std::filesystem::path scenePath =
            renderer::addons::editor::MakeUniqueFilesystemPath(scenesDir, sceneName, ".json");
        finalSceneName = scenePath.stem().string();
    }

    SwitchEditorScene(finalSceneName, /*additive=*/false);

    if (std::find(m_sceneOrder.begin(), m_sceneOrder.end(), finalSceneName) == m_sceneOrder.end())
        m_sceneOrder.push_back(finalSceneName);

    SaveEditorScene();
    RefreshSceneList();

    if (m_editorFrameCtx)
        m_editorFrameCtx->lastFileMessage = "Neue Szene erstellt: " + finalSceneName;

    Debug::Log("KromEditorApp: neue Szene erstellt: '%s'", finalSceneName.c_str());
    return true;
}

bool KromEditorApp::NewEditorSceneInDir(const std::string& sceneName,
                                        const std::string& targetDirStr)
{
    if (sceneName.empty())
        return false;

    const std::filesystem::path targetDir(targetDirStr);
    std::error_code ec;
    std::filesystem::create_directories(targetDir, ec);
    if (ec)
        return false;

    const std::filesystem::path sceneFile =
        renderer::addons::editor::MakeUniqueFilesystemPath(targetDir, sceneName, ".json");
    const std::string finalSceneName = sceneFile.stem().string();

    // Nur die leere Datei schreiben — kein Szenen-Switching, kein SaveProjectFile.
    // Der User kann die neue Szene per Doppelklick im Asset-Browser laden.
    std::ofstream f(sceneFile, std::ios::binary | std::ios::trunc);
    if (!f)
        return false;
    f << "{\"entities\":[]}\n";

    if (m_editorFrameCtx)
        m_editorFrameCtx->lastFileMessage =
            "Szene erstellt: " + sceneFile.filename().string();

    Debug::Log("KromEditorApp: Szene '%s' erstellt in '%s'",
               finalSceneName.c_str(), targetDirStr.c_str());
    return true;
}

bool KromEditorApp::RenameEditorScene(const std::string& oldName, const std::string& newName)
{
    if (oldName.empty() || newName.empty() || oldName == newName)
        return false;

    const std::filesystem::path oldPath = GetEditorScenePath(oldName);
    const std::filesystem::path newPath = GetEditorScenePath(newName);

    // Zielname darf noch nicht existieren.
    if (std::filesystem::exists(newPath))
    {
        if (m_editorFrameCtx)
            m_editorFrameCtx->lastFileMessage = "Umbenennen fehlgeschlagen: '" + newName + "' existiert bereits.";
        return false;
    }

    // Aktuelle Szene erst speichern bevor wir die Datei verschieben.
    if (oldName == m_currentSceneName)
        SaveEditorScene();

    // Datei auf Disk umbenennen.
    std::error_code ec;
    std::filesystem::rename(oldPath, newPath, ec);
    if (ec)
    {
        Debug::LogError("KromEditorApp::RenameEditorScene: rename failed: %s", ec.message().c_str());
        if (m_editorFrameCtx)
            m_editorFrameCtx->lastFileMessage = "Umbenennen fehlgeschlagen: " + ec.message();
        return false;
    }

    // Alle internen Referenzen aktualisieren.
    if (m_currentSceneName == oldName)
        m_currentSceneName = newName;

    for (auto& s : m_loadedSceneNames)
        if (s == oldName) s = newName;

    for (auto& s : m_sceneOrder)
        if (s == oldName) s = newName;

    if (m_buildExcludedScenes.erase(oldName))
        m_buildExcludedScenes.insert(newName);

    // EditorSceneTagComponent auf allen Entities umschreiben.
    if (m_world)
    {
        using namespace renderer::addons::editor;
        m_world->ForEachAlive([&](EntityID id) {
            auto* tag = m_world->Get<EditorSceneTagComponent>(id);
            if (tag && tag->sceneName == oldName)
                tag->sceneName = newName;
        });
    }

    SaveProjectFile();
    RefreshSceneList();

    if (m_editorFrameCtx)
        m_editorFrameCtx->lastFileMessage = "Szene umbenannt: " + oldName + " \xE2\x86\x92 " + newName;

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
                    "Löschen nicht möglich: '" + sceneName + "' ist die einzige Szene.";
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
            m_editorFrameCtx->lastFileMessage = "Löschen fehlgeschlagen: " + ec.message();
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
        m_editorFrameCtx->lastFileMessage = "Szene gelöscht: " + sceneName;

    Debug::Log("KromEditorApp: Szene gelöscht '%s'", sceneName.c_str());
    return true;
}

// =============================================================================

void KromEditorApp::Shutdown()
{
#ifdef KROM_EDITOR_HAS_IMGUI
    if (m_editorEnvironmentHandle.IsValid())
    {
        renderer::RenderSystem& renderSystem = m_renderLoop.GetRenderSystem();
        renderSystem.DestroyEnvironment(m_editorEnvironmentHandle);
        m_editorEnvironmentHandle = renderer::EnvironmentHandle::Invalid();
    }
#endif

    m_assetPipeline.reset();

#ifdef KROM_EDITOR_HAS_IMGUI
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

    if (m_initialized)
        m_renderLoop.Shutdown();

    if (m_platform)
        m_platform->Shutdown();

#ifdef KROM_EDITOR_HAS_IMGUI
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
    windowDesc.resizable  = (m_config.windowMode == platform::WindowMode::Windowed);
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
    m_assetPipeline->SetAssetRoot(assetRoot.string());
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
    }

    const ShaderHandle tonemapVs = m_assetPipeline->LoadShader((ResolveAssetRoot() / tonemapVsPath).string(), assets::ShaderStage::Vertex);
    const ShaderHandle tonemapPs = m_assetPipeline->LoadShader((ResolveAssetRoot() / tonemapPsPath).string(), assets::ShaderStage::Fragment);
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
