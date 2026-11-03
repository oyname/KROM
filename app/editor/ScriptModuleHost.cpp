#include "app/editor/ScriptModuleHost.hpp"
#include "addons/script/ScriptList.hpp"
#include "core/PathUtf8.hpp"
#include "ecs/World.hpp"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <charconv>
#include <fstream>
#include <system_error>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

namespace engine::app {
namespace {
namespace fs = std::filesystem;
std::vector<fs::path> ScriptFiles(const fs::path& root)
{
    std::vector<fs::path> files;
    for (const auto& folder : {root / "Assets/Scripts", root / "scripts"})
    {
        std::error_code ec;
        if (!fs::exists(folder, ec)) continue;
        // Verzeichnis-Zeitstempel erfassen auch entfernte/umbenannte Quellen,
        // wenn der Editor erst danach geoeffnet wurde.
        files.push_back(folder);
        for (const auto& entry : fs::recursive_directory_iterator(folder))
        {
            if (entry.is_directory()) { files.push_back(entry.path()); continue; }
            const auto ext = entry.path().extension();
            if (entry.is_regular_file() && (ext == ".h" || ext == ".hpp" || ext == ".cpp" || ext == ".cxx"))
                files.push_back(entry.path());
        }
    }
    if (fs::exists(root / "generated/ScriptBindings.cpp"))
        files.push_back(root / "generated/ScriptBindings.cpp");
    std::sort(files.begin(), files.end());
    return files;
}

void CloseModule(void* module)
{
#ifdef _WIN32
    if (module) FreeLibrary(static_cast<HMODULE>(module));
#else
    (void)module;
#endif
}

void ForwardLog(LogLevel level, const char* message)
{
    if (Debug::onLog) Debug::onLog(level, message);
}

#ifdef _WIN32
bool SessionOwnerMayBeAlive(const fs::path& directory)
{
    const auto name = PathToUtf8(directory.filename());
    const auto end = name.find('_', 8);
    if (end == std::string::npos) return true;
    DWORD pid = 0;
    const auto parsed = std::from_chars(name.data() + 8, name.data() + end, pid);
    if (parsed.ec != std::errc{} || parsed.ptr != name.data() + end) return true;
    HANDLE process = OpenProcess(SYNCHRONIZE, FALSE, pid);
    if (!process) return GetLastError() != ERROR_INVALID_PARAMETER;
    const bool alive = WaitForSingleObject(process, 0) != WAIT_OBJECT_0;
    CloseHandle(process);
    return alive;
}
#endif
}

ScriptModuleHost::~ScriptModuleHost()
{
    // App muss alle Instanzen/Factories vorher abloesen. Im Fehlerfall die DLL lieber
    // geladen lassen als spaeter aus deren bereits entladenem Code zu zerstoeren.
    if (m_module) Debug::LogError("ScriptModuleHost: Unload vor Zerstoerung fehlt.");
}

std::filesystem::path ScriptModuleHost::LibraryPath(const fs::path& buildDir, const std::string& target)
{
    return buildDir / "bin" / PathFromUtf8("krom_scripts_" + target + ".dll");
}

bool ScriptModuleHost::NeedsBuild(const fs::path& root, const fs::path& library) const
{
    if (!fs::exists(library)) return true;
    const auto stamp = fs::last_write_time(library);
    for (const char* name : {"engine_core.lib", "engine_script.lib"})
    {
        const auto dependency = library.parent_path().parent_path() / "lib" / name;
        if (fs::exists(dependency) && fs::last_write_time(dependency) > stamp) return true;
    }
    const auto files = ScriptFiles(root);
    if (m_module && library == m_sourceLibrary && files != m_scriptFiles) return true;
    for (const auto& file : files)
        if (fs::last_write_time(file) > stamp) return true;
    return false;
}

bool ScriptModuleHost::SourceLibraryChanged(const fs::path& library) const
{
    return !m_module || library != m_sourceLibrary || !fs::exists(library) ||
        fs::last_write_time(library) != m_sourceTime;
}

bool ScriptModuleHost::CheckEngineLibraries(const fs::path& buildDir, const fs::path& exe, std::string& error)
{
    std::error_code ec;
    const auto exeTime = fs::last_write_time(exe, ec);
    if (ec) { error = "Editor-Zeitstempel konnte nicht gelesen werden."; return false; }
    for (const char* name : {"engine_core.lib", "engine_script.lib"})
    {
        const auto path = buildDir / "lib" / name;
        const auto time = fs::last_write_time(path, ec);
        if (ec) { error = "Engine-Library fehlt: " + PathToUtf8(path); return false; }
        if (time > exeTime)
        {
            error = "Engine geaendert - Editor neu bauen (" + std::string(name) + ").";
            return false;
        }
    }
    return true;
}

bool ScriptModuleHost::Swap(const fs::path& library, ecs::World& world,
                            script::ScriptRegistry& registry, std::string& error, const fs::path& projectRoot)
{
#ifdef _WIN32
    void* candidate = nullptr;
    script::ScriptRegistry candidateRegistry;
    fs::path copyDirectory;
    try
    {
        static std::atomic<uint64_t> serial{0};
        const auto stamp = fs::last_write_time(library);
        for (const char* name : {"engine_core.lib", "engine_script.lib"})
        {
            const auto dependency = library.parent_path().parent_path() / "lib" / name;
            if (fs::exists(dependency) && fs::last_write_time(dependency) > stamp)
                throw std::runtime_error("Script-DLL ist aelter als die Engine. Scripts beim Start neu bauen.");
        }
        const auto copyRoot = library.parent_path().parent_path() / "script_modules" / library.stem();
        fs::create_directories(copyRoot);
        // Nur verwaiste Kopien dieses Projekts entfernen. Geladene Dateien sind unter
        // Windows gesperrt; deren Verzeichnisse bleiben fuer andere Editoren bestehen.
        if (!m_module)
            for (const auto& entry : fs::directory_iterator(copyRoot))
            {
                if (!entry.is_directory() || PathToUtf8(entry.path().filename()).rfind("session_", 0) != 0 ||
                    SessionOwnerMayBeAlive(entry.path())) continue;
                std::error_code ignored;
                fs::remove_all(entry.path(), ignored);
            }
        copyDirectory = copyRoot / ("session_" + std::to_string(GetCurrentProcessId()) + "_" +
            std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "_" + std::to_string(++serial));
        fs::create_directories(copyDirectory);
        const auto copy = copyDirectory / library.filename();
        fs::copy_file(library, copy);
        // PDBALTPATH enthaelt nur den Basisnamen: der Debugger findet diese private
        // Kopie neben der Schatten-DLL und sperrt nie die Build-Ausgabe.
        const auto prefix = PathToUtf8(library.stem()) + "_";
        for (const auto& entry : fs::directory_iterator(library.parent_path()))
            if (entry.path().extension() == ".pdb" && (entry.path().stem() == library.stem() ||
                PathToUtf8(entry.path().stem()).rfind(prefix, 0) == 0))
                fs::copy_file(entry.path(), copyDirectory / entry.path().filename());
        candidate = LoadLibraryExW(copy.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
        if (!candidate) throw std::runtime_error("Script-DLL konnte nicht geladen werden (Windows " + std::to_string(GetLastError()) + ").");
        const auto module = static_cast<HMODULE>(candidate);
        const auto version = reinterpret_cast<script::ScriptModuleVersionFn>(GetProcAddress(module, "KromScriptModuleApiVersion"));
        const auto abi = reinterpret_cast<script::ScriptModuleAbiFn>(GetProcAddress(module, "KromScriptModuleAbi"));
        const auto init = reinterpret_cast<script::ScriptModuleInitFn>(GetProcAddress(module, "KromScriptModuleInit"));
        const auto registerScripts = reinterpret_cast<script::RegisterGameScriptsFn>(GetProcAddress(module, "KromRegisterGameScripts"));
        if (!version || version() != script::ScriptModuleAbiVersion || !abi || !init || !registerScripts ||
            !script::CompatibleScriptModuleAbi(abi()))
            throw std::runtime_error("Script-DLL inkompatibel: ABI, Compiler oder Debug/Release-Konfiguration abweichend. Editor und Scripts neu bauen.");
        script::ScriptHostApi api;
        api.resolveComponent = &ResolveComponentTypeId;
        api.log = &ForwardLog;
        api.minLogLevel = Debug::MinLevel;
        if (!init(&api)) throw std::runtime_error("Script-DLL Init fehlgeschlagen (Komponenten-Zugriff in statischem Initialisierer?).");
        registerScripts(candidateRegistry);
        // Alles, was vor dem Detach scheitern kann, vorbereiten; alte DLL bleibt aktiv.
        auto scriptFiles = projectRoot.empty() ? std::vector<fs::path>{} : ScriptFiles(projectRoot);
        script::DetachScriptObjects(world, registry);
        registry.Clear();
        CloseModule(m_module);
        if (!m_copyDirectory.empty()) { std::error_code ignored; fs::remove_all(m_copyDirectory, ignored); }
        m_module = candidate;
        candidate = nullptr;
        m_copyDirectory = std::move(copyDirectory);
        m_sourceLibrary = library;
        m_sourceTime = stamp;
        m_scriptFiles = std::move(scriptFiles);
        registry = std::move(candidateRegistry);
        script::ReattachScriptObjects(world, registry);
        return true;
    }
    catch (const std::exception& exception) { error = exception.what(); }
    catch (...) { error = "Unbekannter Fehler beim Laden der Script-DLL."; }
    candidateRegistry.Clear();
    CloseModule(candidate);
    if (!copyDirectory.empty()) { std::error_code ignored; fs::remove_all(copyDirectory, ignored); }
    return false;
#else
    (void)library; (void)world; (void)registry;
    error = "Script-DLLs werden nur unter Windows unterstuetzt.";
    return false;
#endif
}

void ScriptModuleHost::Unload(ecs::World* world, script::ScriptRegistry& registry)
{
    if (world) script::DetachScriptObjects(*world, registry);
    registry.Clear();
    CloseModule(m_module);
    m_module = nullptr;
    m_sourceLibrary.clear();
    m_scriptFiles.clear();
    if (!m_copyDirectory.empty()) { std::error_code ignored; fs::remove_all(m_copyDirectory, ignored); }
    m_copyDirectory.clear();
}
}
