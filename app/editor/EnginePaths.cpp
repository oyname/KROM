#include "app/editor/EnginePaths.hpp"
#include "core/PathUtf8.hpp"

#include <array>
#include <algorithm>
#include <charconv>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <map>
#include <stdexcept>
#include <string_view>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <shlobj.h>
#elif defined(__linux__)
#include <unistd.h>
#elif defined(__APPLE__)
#include <mach-o/dyld.h>
#endif

namespace engine::app {
namespace {

// The v1/v2 manifest is a flat object with string fields and schemaVersion.
// Keep this reader strict: the scene JSON
// parser accepts incomplete documents and cannot decode JSON Unicode escapes.
class ManifestReader
{
public:
    explicit ManifestReader(std::string_view text) : m_text(text) {}
    std::map<std::string, std::string> Read()
    {
        std::map<std::string, std::string> values;
        Expect('{');
        if (!Take('}'))
        {
            do
            {
                const auto key = String();
                Expect(':');
                std::string value;
                if (key == "schemaVersion") { if (Take('1')) value = "1"; else { Expect('2'); value = "2"; } }
                else value = String();
                if (!values.emplace(key, value).second) Fail();
                if (Take('}')) break;
                Expect(',');
            } while (true);
        }
        Space();
        if (m_pos != m_text.size() || !values.contains("schemaVersion")) Fail();
        return values;
    }

private:
    std::string_view m_text;
    size_t m_pos = 0;
    [[noreturn]] void Fail() const { throw std::runtime_error("Expected an SDK manifest (schemaVersion: 1 or 2 and string fields)."); }
    void Space() { while (m_pos < m_text.size() && (m_text[m_pos] == ' ' || m_text[m_pos] == '\t' || m_text[m_pos] == '\n' || m_text[m_pos] == '\r')) ++m_pos; }
    bool Take(char ch) { Space(); if (m_pos == m_text.size() || m_text[m_pos] != ch) return false; ++m_pos; return true; }
    void Expect(char ch) { if (!Take(ch)) Fail(); }
    unsigned Hex()
    {
        unsigned value = 0;
        for (int i = 0; i < 4; ++i)
        {
            if (m_pos == m_text.size()) Fail();
            const char ch = m_text[m_pos++];
            const int digit = ch >= '0' && ch <= '9' ? ch - '0' : ch >= 'a' && ch <= 'f' ? ch - 'a' + 10 : ch >= 'A' && ch <= 'F' ? ch - 'A' + 10 : -1;
            if (digit < 0) Fail();
            value = value * 16 + static_cast<unsigned>(digit);
        }
        return value;
    }
    std::string String()
    {
        Expect('"');
        std::string result;
        while (m_pos < m_text.size())
        {
            char ch = m_text[m_pos++];
            if (ch == '"') return result;
            if (static_cast<unsigned char>(ch) < 32) Fail();
            if (ch != '\\') { result += ch; continue; }
            if (m_pos == m_text.size()) Fail();
            switch (ch = m_text[m_pos++])
            {
            case '"': case '\\': case '/': result += ch; break;
            case 'b': result += '\b'; break;
            case 'f': result += '\f'; break;
            case 'n': result += '\n'; break;
            case 'r': result += '\r'; break;
            case 't': result += '\t'; break;
            case 'u':
            {
                unsigned cp = Hex();
                if (cp >= 0xd800 && cp <= 0xdbff)
                {
                    if (m_text.substr(m_pos, 2) != "\\u") Fail();
                    m_pos += 2;
                    const unsigned low = Hex();
                    if (low < 0xdc00 || low > 0xdfff) Fail();
                    cp = 0x10000 + ((cp - 0xd800) << 10) + low - 0xdc00;
                }
                else if (cp >= 0xdc00 && cp <= 0xdfff) Fail();
                if (cp <= 0x7f) result += static_cast<char>(cp);
                else
                {
                    if (cp > 0xffff) result += static_cast<char>(0xf0 | (cp >> 18));
                    if (cp > 0x7ff) result += static_cast<char>((cp > 0xffff ? 0x80 : 0xe0) | ((cp >> 12) & 0x3f));
                    result += static_cast<char>((cp > 0x7ff ? 0x80 : 0xc0) | ((cp >> 6) & 0x3f));
                    result += static_cast<char>(0x80 | (cp & 0x3f));
                }
                break;
            }
            default: Fail();
            }
        }
        Fail();
    }
};

std::filesystem::path UserDataDirectory()
{
#ifdef _WIN32
    if (const auto* value = _wgetenv(L"LOCALAPPDATA"); value && *value)
        return std::filesystem::path(value) / "KROM";
#else
    if (const auto* value = std::getenv("XDG_CONFIG_HOME"); value && *value)
        if (const auto path = PathFromUtf8(value); path.is_absolute()) return path / "krom";
    if (const auto* value = std::getenv("HOME"); value && *value)
        return PathFromUtf8(value) / ".config" / "krom";
#endif
    return {};
}

std::filesystem::path DocumentsDirectory()
{
#ifdef _WIN32
    PWSTR value = nullptr;
    const auto status = SHGetKnownFolderPath(FOLDERID_Documents, KF_FLAG_DONT_VERIFY, nullptr, &value);
    const std::filesystem::path path = SUCCEEDED(status) && value ? value : L"";
    CoTaskMemFree(value);
    return path;
#else
    if (const auto* value = std::getenv("HOME"); value && *value) return PathFromUtf8(value) / "Documents";
    return {};
#endif
}

} // namespace

std::filesystem::path CurrentExecutablePath()
{
#ifdef _WIN32
    std::wstring buffer(MAX_PATH, L'\0');
    for (;;)
    {
        const DWORD length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (!length) return {};
        if (length < buffer.size()) return std::filesystem::path(buffer.substr(0, length));
        buffer.resize(buffer.size() * 2);
    }
#elif defined(__linux__)
    std::string buffer(4096, '\0');
    const auto length = readlink("/proc/self/exe", buffer.data(), buffer.size());
    if (length > 0 && static_cast<size_t>(length) < buffer.size()) return PathFromUtf8(buffer.substr(0, length));
    return {};
#elif defined(__APPLE__)
    uint32_t size = 0;
    _NSGetExecutablePath(nullptr, &size);
    std::string buffer(size, '\0');
    if (_NSGetExecutablePath(buffer.data(), &size) == 0) return PathFromUtf8(buffer.c_str());
    return {};
#else
    return {};
#endif
}

EnginePaths ResolveEnginePaths(const std::filesystem::path& executable,
                              const std::filesystem::path& userDataDirectory,
                              const std::filesystem::path& documentsDirectory)
{
    EnginePaths paths;
    paths.userDataDir = userDataDirectory;
    if (executable.empty() || !executable.is_absolute())
    {
        paths.error = "Cannot determine the absolute editor executable path.";
        return paths;
    }
    const auto exeDir = executable.parent_path();
    for (const auto& directory : std::array{exeDir, exeDir.parent_path()})
    {
        const auto manifest = directory / "krom-sdk.json";
        std::error_code ec;
        const bool exists = std::filesystem::exists(manifest, ec);
        if (!ec && !exists) continue;
        paths.sdk = true;
        paths.sdkRoot = directory;
        try
        {
            if (ec) throw std::runtime_error(ec.message());
            std::ifstream in(manifest, std::ios::binary);
            if (!in) throw std::runtime_error("Cannot read manifest.");
            const std::string text((std::istreambuf_iterator<char>(in)), {});
            if (in.bad()) throw std::runtime_error("Cannot read manifest.");
            const auto values = ManifestReader(text).Read();
            auto resource = [&](const char* key, const std::filesystem::path& fallback) {
                const auto it = values.find(key);
                if (it == values.end()) return fallback;
                if (!IsPortableAssetPath(it->second) || it->second.empty() || it->second.find('\0') != std::string::npos)
                    throw std::runtime_error(std::string(key) + " must be relative to the SDK root.");
                return (directory / PathFromUtf8(it->second)).lexically_normal();
            };
            for (const auto& [key, value] : values)
                if (key != "schemaVersion" && key != "assetsDir" && key != "shadersDir" && key != "editorDir" && key != "sdkCmakeDir" && key != "projectTemplateDir" &&
                    !(values.at("schemaVersion") == "2" && (key == "sdkBuildId" || key == "system" || key == "architecture" || key == "compilerFamily" || key == "compilerVersion" || key == "standardLibrary" || key == "toolset" || key == "runtime" || key == "configuration")))
                    throw std::runtime_error("Unknown manifest field: " + key);
            paths.assetsDir = resource("assetsDir", exeDir / "assets");
            paths.shadersDir = resource("shadersDir", exeDir / "shaders");
            paths.editorDir = resource("editorDir", exeDir / "editor");
            paths.sdkCmakeDir = resource("sdkCmakeDir", directory / "cmake");
            paths.projectTemplateDir = resource("projectTemplateDir", directory / "templates" / "project");
            for (const auto& resourceDir : {paths.assetsDir, paths.shadersDir, paths.editorDir})
            {
                std::error_code resourceError;
                if (!std::filesystem::is_directory(resourceDir, resourceError))
                    throw std::runtime_error("SDK resource directory missing or unreadable: " + PathToUtf8(resourceDir));
            }
            if (!userDataDirectory.is_absolute() || !documentsDirectory.is_absolute())
                throw std::runtime_error("Cannot determine user data and Documents directories.");
            paths.projectsDir = documentsDirectory / "KromProjects";
            paths.shaderCacheDir = userDataDirectory / "cache" / "shader_artifacts";
            paths.standaloneScenesDir = userDataDirectory / "scenes";
            if (values.at("schemaVersion") == "2")
            {
                auto field = [&](const char* key) -> const std::string& {
                    const auto it = values.find(key);
                    if (it == values.end() || it->second.empty()) throw std::runtime_error(std::string("Missing SDK metadata: ") + key);
                    return it->second;
                };
                if (field("system") != "Windows" || field("architecture") != "x64" || field("compilerFamily") != "MSVC" ||
                    field("standardLibrary") != "msvc" || field("runtime") != "MD" || field("configuration") != "Release")
                    throw std::runtime_error("Unsupported SDK platform, runtime or configuration (requires Windows x64 MSVC Release /MD).");
                const auto& version = field("compilerVersion");
                const auto parsed = std::from_chars(version.data(), version.data() + version.size(), paths.sdkCompilerVersion);
                if (parsed.ec != std::errc{} || parsed.ptr != version.data() + version.size()) throw std::runtime_error("Invalid SDK compiler version.");
                paths.sdkToolset = field("toolset");
                const auto& id = field("sdkBuildId");
                if (id.size() != 64 || !std::all_of(id.begin(), id.end(), [](char ch) { return (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f'); }))
                    throw std::runtime_error("Invalid SDK build ID.");
#if defined(KROM_SDK_BUILD_ID) && defined(_MSC_FULL_VER) && defined(_M_X64) && defined(NDEBUG) && defined(_DLL) && !defined(_DEBUG)
                if (id != KROM_SDK_BUILD_ID || paths.sdkCompilerVersion != _MSC_FULL_VER)
                    throw std::runtime_error("SDK manifest and editor build do not match.");
#else
                throw std::runtime_error("This editor was not built as a Windows x64 Release SDK editor.");
#endif
                if (!std::filesystem::is_regular_file(paths.sdkCmakeDir / "KromConfig.cmake") ||
                    !std::filesystem::is_regular_file(paths.projectTemplateDir / "CMakeLists.txt"))
                    throw std::runtime_error("SDK CMake package or project template is missing.");
                paths.sdkBuildId = id;
            }
        }
        catch (const std::exception& e)
        {
            paths.error = "Invalid SDK manifest " + PathToUtf8(manifest) + ": " + e.what();
        }
        return paths; // A broken nearer manifest must never select another SDK or source tree.
    }

    // Preserve the two historical source roots independently, including builds
    // whose __FILE__ spelling differs from CMake's source directory.
    const auto resources = PathFromUtf8(__FILE__).parent_path().parent_path().parent_path();
    paths.assetsDir = resources / "assets";
    paths.shadersDir = resources / "shaders";
    paths.editorDir = resources / "editor";
    paths.projectsDir = resources / "projects";
    paths.engineSourceDir = PathFromUtf8(KROM_SOURCE_DIR);
    paths.engineBuildDir = PathFromUtf8(KROM_BUILD_DIR);
    paths.shaderCacheDir = resources / "shader_artifacts";
    paths.standaloneScenesDir = resources / "editor";
    return paths;
}

const EnginePaths& GetEnginePaths()
{
    static const auto paths = ResolveEnginePaths(CurrentExecutablePath(), UserDataDirectory(), DocumentsDirectory());
    return paths;
}

std::filesystem::path EnginePaths::RuntimeBuildDir(const char* configuration) const
{
    if (!BuildsAvailable() || sdk) return {};
    return engineBuildDir.parent_path() / PathFromUtf8(std::string("runtime-") + configuration);
}

std::filesystem::path EnginePaths::ProjectBuildDir(const std::filesystem::path& project, const std::string& toolset) const
{
    if (!sdk || !BuildsAvailable() || !project.is_absolute() || toolset.empty() || toolset.front() < '0' || toolset.front() > '9' ||
        !std::all_of(toolset.begin(), toolset.end(), [](char c) { return (c >= '0' && c <= '9') || c == '.'; })) return {};
    // A full SHA256 makes CMake's compiler probes exceed Windows' path limit
    // even for ordinary project directories. The full ID is still verified in
    // the package, module ABI and CMake cache; only the directory is abbreviated.
    return project / ".krom" / "build" / sdkBuildId.substr(0, 16) / "win64" / PathFromUtf8(toolset) / "Release";
}

std::filesystem::path EnginePaths::ProjectBrowseDir() const
{
    return sdk ? projectsDir : std::filesystem::current_path() / "projects";
}

} // namespace engine::app
