#include "app/editor/BuildTools.hpp"
#include "core/PathUtf8.hpp"
#include <algorithm>
#include <charconv>
#include <fstream>
#include <mutex>
#include <sstream>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

namespace engine::app::build {
namespace {
bool SameName(const std::string& a, const std::string& b)
{
#ifdef _WIN32
    return _stricmp(a.c_str(), b.c_str()) == 0;
#else
    return a == b;
#endif
}

bool IsFile(const std::filesystem::path& path)
{
    std::error_code ec;
    return !path.empty() && std::filesystem::is_regular_file(path, ec);
}

bool SameFile(const std::filesystem::path& a, const std::filesystem::path& b)
{
    std::error_code ec;
    return std::filesystem::equivalent(a, b, ec) && !ec;
}

#ifdef _WIN32
std::string SystemEnvironment(const wchar_t* name)
{
    const auto* value = _wgetenv(name);
    return value ? PathToUtf8(std::filesystem::path(value)) : std::string{};
}

bool ProbeCompiler(BuildTools& tools, std::string& error)
{
    wchar_t tempDirectory[MAX_PATH]{};
    wchar_t tempFile[MAX_PATH]{};
    if (!GetTempPathW(MAX_PATH, tempDirectory) || !GetTempFileNameW(tempDirectory, L"krm", 0, tempFile))
    {
        error = "Cannot create compiler probe";
        return false;
    }
    struct TemporaryFile
    {
        std::filesystem::path path;
        ~TemporaryFile() { std::error_code ec; std::filesystem::remove(path, ec); }
    } source{tempFile};
    {
        std::ofstream file(source.path, std::ios::binary);
        file << "#ifdef _MSC_FULL_VER\nKROM_MSVC_VERSION _MSC_FULL_VER\n#endif\n";
        if (!file) { error = "Cannot write compiler probe"; return false; }
    }
    ProcessCommand probe{tools.compiler, {"/nologo", "/EP", "/TC", PathToUtf8(source.path)}, {}, tools.environment};
    std::string output;
    const auto result = CaptureProcessOutput(probe, output);
    const std::string marker = "KROM_MSVC_VERSION ";
    const auto position = output.find(marker);
    if (!result.Succeeded() || position == std::string::npos)
    {
        error = result.error.empty() ? output : result.error;
        return false;
    }
    const char* begin = output.data() + position + marker.size();
    while (begin < output.data() + output.size() && (*begin == ' ' || *begin == '\t')) ++begin;
    return std::from_chars(begin, output.data() + output.size(), tools.compilerVersion).ec == std::errc{} &&
        tools.compilerVersion != 0;
}
#endif
} // namespace

std::string EnvironmentValue(const std::vector<std::string>& environment, const std::string& name)
{
    for (const auto& entry : environment)
    {
        const auto separator = entry.find('=');
        if (separator != std::string::npos && SameName(entry.substr(0, separator), name))
            return entry.substr(separator + 1);
    }
    return {};
}

std::filesystem::path FindBuildExecutable(const std::filesystem::path& preferred,
                                         const std::string& name, const std::string& searchPath)
{
    if (IsFile(preferred)) return preferred;
#ifdef _WIN32
    constexpr char separator = ';';
#else
    constexpr char separator = ':';
#endif
    std::istringstream paths(searchPath);
    std::string directory;
    while (std::getline(paths, directory, separator))
    {
        if (directory.size() >= 2 && directory.front() == '"' && directory.back() == '"')
            directory = directory.substr(1, directory.size() - 2);
        // Do not implicitly search the current directory for build tools.
        const auto path = PathFromUtf8(directory);
        if (path.empty() || !path.is_absolute()) continue;
        const auto candidate = path / PathFromUtf8(name);
        if (IsFile(candidate)) return candidate;
    }
    return {};
}

ToolDiscovery DiscoverBuildTools()
{
    ToolDiscovery discovery;
    auto fail = [&](const char* key, std::string detail) {
        discovery.errorKey = key;
        discovery.detail = std::move(detail);
        return discovery;
    };
#ifdef _WIN32
    const auto vswhere = PathFromUtf8(SystemEnvironment(L"ProgramFiles(x86)")) /
        "Microsoft Visual Studio/Installer/vswhere.exe";
    if (!IsFile(vswhere)) return fail("build.vswhere_missing", PathToUtf8(vswhere));
    ProcessCommand query{vswhere, {"-latest", "-products", "*", "-requires",
        "Microsoft.VisualStudio.Component.VC.Tools.x86.x64", "-property", "installationPath", "-utf8"}};
    std::string installation;
    const auto found = CaptureProcessOutput(query, installation);
    if (!found.Succeeded()) return fail("build.vs_search_failed", found.error);
    while (!installation.empty() && (installation.back() == '\r' || installation.back() == '\n')) installation.pop_back();
    if (installation.empty()) return fail("build.vs_cpp_missing", {});
    auto& tools = discovery.tools;
    tools.installation = PathFromUtf8(installation);
    const auto setup = tools.installation / "Common7/Tools/VsDevCmd.bat";
    if (!IsFile(setup)) return fail("build.vs_environment_failed", PathToUtf8(setup));
    std::string error;
    if (!ReadMsvcEnvironment(setup, tools.environment, error)) return fail("build.vs_environment_failed", error);
    const auto cmakeRoot = tools.installation / "Common7/IDE/CommonExtensions/Microsoft/CMake";
    // Use the editor's original PATH as well as the developer environment's PATH.
    const auto path = EnvironmentValue(tools.environment, "PATH") + ";" + SystemEnvironment(L"PATH");
    tools.cmake = FindBuildExecutable(cmakeRoot / "CMake/bin/cmake.exe", "cmake.exe", path);
    tools.ninja = FindBuildExecutable(cmakeRoot / "Ninja/ninja.exe", "ninja.exe", path);
    tools.compiler = FindBuildExecutable({}, "cl.exe", EnvironmentValue(tools.environment, "PATH"));
    if (tools.cmake.empty()) return fail("build.cmake_missing", {});
    if (tools.ninja.empty()) return fail("build.ninja_missing", {});
    if (tools.compiler.empty()) return fail("build.compiler_missing", {});
    // Do not inherit custom compiler flags from a developer's shell. ABI-affecting
    // choices must come from the project's CMake configuration.
    std::erase_if(tools.environment, [](const std::string& entry) {
        const auto name = entry.substr(0, entry.find('='));
        return SameName(name, "CL") || SameName(name, "_CL_") || SameName(name, "LINK") ||
            SameName(name, "_LINK_") || SameName(name, "VSLANG");
    });
    tools.environment.push_back("VSLANG=1033");
    if (!ProbeCompiler(tools, error)) return fail("build.compiler_probe_failed", error);
#else
    return fail("build.windows_only", {});
#endif
    return discovery;
}

ToolDiscovery GetBuildTools()
{
    static ToolDiscovery discovery;
    static bool ready = false;
    static std::mutex mutex;
    std::lock_guard lock(mutex);
    if (!ready)
    {
        discovery = DiscoverBuildTools();
        ready = discovery.Succeeded();
    }
    return discovery;
}

bool CompilerCompatible(uint32_t editorVersion, uint32_t buildVersion, bool scriptBuild)
{
    if (!editorVersion || !buildVersion) return false;
    // ScriptModuleApi currently compares _MSC_FULL_VER exactly. Do not relax
    // that contract until the SDK defines and validates its ABI policy.
    if (scriptBuild) return buildVersion == editorVersion;
    return buildVersion / 10000000 == editorVersion / 10000000 && buildVersion >= editorVersion;
}

bool CheckBuildCache(const std::filesystem::path& buildDirectory, const BuildTools& tools,
                     std::string& errorKey, std::string& detail)
{
    errorKey.clear();
    detail.clear();
    std::ifstream cache(buildDirectory / "CMakeCache.txt");
    std::string line;
    while (std::getline(cache, line))
    {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const auto colon = line.find(':');
        const auto equals = line.find('=', colon);
        if (colon == std::string::npos || equals == std::string::npos) continue;
        const auto key = line.substr(0, colon);
        const auto value = line.substr(equals + 1);
        if ((key == "CMAKE_CXX_COMPILER" && !SameFile(PathFromUtf8(value), tools.compiler)) ||
            (key == "CMAKE_MAKE_PROGRAM" && !SameFile(PathFromUtf8(value), tools.ninja)) ||
            (key == "CMAKE_GENERATOR" && value != "Ninja"))
        {
            errorKey = "build.cache_toolchain_mismatch";
            detail = key + "=" + value + " (" + PathToUtf8(buildDirectory) + ")";
            return false;
        }
    }
    return true;
}
} // namespace engine::app::build
