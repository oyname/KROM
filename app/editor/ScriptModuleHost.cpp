#include "app/editor/ScriptModuleHost.hpp"
#include "addons/script/ScriptList.hpp"
#include "core/PathUtf8.hpp"
#include "ecs/World.hpp"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <charconv>
#include <cstddef>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string_view>
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

// Eindeutiger Link-PDB-Suffix aus WriteScriptModulePdb.cmake: JJJJMMTT_hhmmss_<16 hex>.
bool IsLinkPdbSuffix(std::string_view suffix)
{
    if (suffix.size() != 32u || suffix[8] != '_' || suffix[15] != '_') return false;
    for (size_t i = 0; i < suffix.size(); ++i)
    {
        if (i == 8u || i == 15u) continue;
        const char c = suffix[i];
        const bool digit = c >= '0' && c <= '9';
        if (i < 15u ? !digit : !(digit || (c >= 'a' && c <= 'f'))) return false;
    }
    return true;
}

// Jeder Link hinterlaesst eine neue PDB. Alte dieses Ziels entfernen; vom Debugger
// gesperrte bleiben liegen und werden beim naechsten Laden erneut versucht.
void RemoveStaleLinkPdbs(const fs::path& library, const fs::path& keep)
{
    const auto prefix = PathToUtf8(library.stem()) + "_";
    std::vector<fs::path> stale;
    std::error_code ec;
    for (const auto& entry : fs::directory_iterator(library.parent_path(), ec))
    {
        const auto name = PathToUtf8(entry.path().filename());
        if (entry.path().extension() != ".pdb" || entry.path().filename() == keep ||
            name.size() <= prefix.size() + 4u || name.rfind(prefix, 0) != 0 ||
            !IsLinkPdbSuffix(std::string_view(name).substr(prefix.size(), name.size() - prefix.size() - 4u)))
            continue;
        stale.push_back(entry.path());
    }
    for (const auto& path : stale)
    {
        std::error_code ignored;
        fs::remove(path, ignored);
    }
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
    // Ein Build ohne Neu-Link (z.B. nur ein neuer Ordner oder ein ungenutzter
    // Header) aendert die DLL nicht; bis zu dessen Start gelten die Quellen als gebaut.
    const auto scriptStamp = library == m_builtLibrary ? (std::max)(stamp, m_builtTime) : stamp;
    const auto files = ScriptFiles(root);
    if (m_module && library == m_sourceLibrary && files != m_scriptFiles) return true;
    for (const auto& file : files)
        if (fs::last_write_time(file) > scriptStamp) return true;
    return false;
}

void ScriptModuleHost::MarkBuilt(const fs::path& projectRoot, const fs::path& library,
                                 fs::file_time_type buildStart)
{
    m_builtLibrary = library;
    m_builtTime = buildStart;
    if (m_module && library == m_sourceLibrary)
        m_scriptFiles = ScriptFiles(projectRoot);
}

fs::path ScriptModuleHost::ReferencedPdb(const fs::path& library)
{
#ifdef _WIN32
    std::ifstream in(library, std::ios::binary);
    if (!in) return {};
    const std::string image((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    auto read = [&](size_t offset, auto& out) {
        if (offset > image.size() || image.size() - offset < sizeof(out)) return false;
        std::memcpy(&out, image.data() + offset, sizeof(out));
        return true;
    };
    IMAGE_DOS_HEADER dos{};
    IMAGE_NT_HEADERS64 nt{};
    if (!read(0, dos) || dos.e_magic != IMAGE_DOS_SIGNATURE || dos.e_lfanew < 0 ||
        !read(size_t(dos.e_lfanew), nt) || nt.Signature != IMAGE_NT_SIGNATURE ||
        nt.OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC)
        return {};
    const auto& debug = nt.OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_DEBUG];
    if (!debug.VirtualAddress || !debug.Size) return {};
    // RVA des Debug-Verzeichnisses ueber die Sektionstabelle in einen Dateioffset umrechnen.
    const size_t sectionTable = size_t(dos.e_lfanew) + offsetof(IMAGE_NT_HEADERS64, OptionalHeader) +
        nt.FileHeader.SizeOfOptionalHeader;
    size_t debugOffset = 0;
    bool mapped = false;
    for (WORD i = 0; i < nt.FileHeader.NumberOfSections && !mapped; ++i)
    {
        IMAGE_SECTION_HEADER section{};
        if (!read(sectionTable + i * sizeof(section), section)) return {};
        if (debug.VirtualAddress >= section.VirtualAddress &&
            debug.VirtualAddress < section.VirtualAddress + section.SizeOfRawData)
        {
            debugOffset = size_t(section.PointerToRawData) + (debug.VirtualAddress - section.VirtualAddress);
            mapped = true;
        }
    }
    if (!mapped) return {};
    constexpr uint32_t kCodeViewRsds = 0x53445352u; // "RSDS"
    for (DWORD i = 0; i < debug.Size / sizeof(IMAGE_DEBUG_DIRECTORY); ++i)
    {
        IMAGE_DEBUG_DIRECTORY entry{};
        if (!read(debugOffset + i * sizeof(entry), entry)) return {};
        uint32_t signature = 0;
        if (entry.Type != IMAGE_DEBUG_TYPE_CODEVIEW || entry.SizeOfData <= 24u ||
            !read(entry.PointerToRawData, signature) || signature != kCodeViewRsds)
            continue;
        // RSDS: Signatur, GUID, Age, danach nullterminierter UTF-8-Pfad.
        const size_t begin = size_t(entry.PointerToRawData) + 24u;
        const size_t end = (std::min)(image.size(), size_t(entry.PointerToRawData) + entry.SizeOfData);
        if (begin >= end) continue;
        const std::string path(image.data() + begin, ::strnlen(image.data() + begin, end - begin));
        return PathFromUtf8(path).filename();
    }
#else
    (void)library;
#endif
    return {};
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
        // Kopie neben der Schatten-DLL und sperrt nie die Build-Ausgabe. Nur die
        // zur DLL gehoerende PDB kopieren, nicht die aller frueheren Links.
        const auto pdb = ReferencedPdb(library);
        if (!pdb.empty() && fs::exists(library.parent_path() / pdb))
            fs::copy_file(library.parent_path() / pdb, copyDirectory / pdb);
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
        RemoveStaleLinkPdbs(library, pdb);
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
