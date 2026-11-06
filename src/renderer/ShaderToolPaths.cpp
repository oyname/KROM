#include "renderer/ShaderToolPaths.hpp"
#include "core/PathUtf8.hpp"
#include <array>
#include <cstdlib>
#include <mutex>
#include <string_view>

namespace engine::renderer {
namespace {

std::mutex& OverrideMutex()
{
    static std::mutex mutex;
    return mutex;
}

std::array<std::filesystem::path, 2>& Overrides()
{
    static std::array<std::filesystem::path, 2> overrides;
    return overrides;
}

size_t Index(ShaderTool tool) { return tool == ShaderTool::Dxc ? 0u : 1u; }

bool IsFile(const std::filesystem::path& path)
{
    std::error_code ec;
    return !path.empty() && std::filesystem::is_regular_file(path, ec);
}

// Environment value as a path; wide on Windows so non-ANSI folders survive.
std::filesystem::path EnvironmentPath(const char* name)
{
#ifdef _WIN32
    std::wstring wide(name, name + std::char_traits<char>::length(name));
    const wchar_t* value = _wgetenv(wide.c_str());
    return value && *value ? std::filesystem::path(value) : std::filesystem::path{};
#else
    const char* value = std::getenv(name);
    return value && *value ? PathFromUtf8(value) : std::filesystem::path{};
#endif
}

std::filesystem::path SearchPath(std::string_view executable)
{
#ifdef _WIN32
    const wchar_t* pathVar = _wgetenv(L"PATH");
    if (!pathVar) return {};
    const std::wstring list(pathVar);
    constexpr wchar_t separator = L';';
#else
    const char* pathVar = std::getenv("PATH");
    if (!pathVar) return {};
    const std::string list(pathVar);
    constexpr char separator = ':';
#endif
    size_t start = 0;
    while (start <= list.size())
    {
        const size_t end = list.find(separator, start);
        auto segment = list.substr(start, end == list.npos ? list.npos : end - start);
        if (segment.size() >= 2 && segment.front() == '"' && segment.back() == '"')
            segment = segment.substr(1, segment.size() - 2);
        const std::filesystem::path directory(segment);
        // Never search the current directory implicitly.
        if (!segment.empty() && directory.is_absolute())
        {
            const auto candidate = directory / PathFromUtf8(std::string(executable));
            if (IsFile(candidate)) return candidate;
        }
        if (end == list.npos) break;
        start = end + 1;
    }
    return {};
}

} // namespace

void SetShaderToolOverride(ShaderTool tool, const std::filesystem::path& path)
{
    std::lock_guard lock(OverrideMutex());
    Overrides()[Index(tool)] = path;
}

std::filesystem::path GetShaderToolOverride(ShaderTool tool)
{
    std::lock_guard lock(OverrideMutex());
    return Overrides()[Index(tool)];
}

ShaderToolLocation FindShaderTool(ShaderTool tool)
{
    return FindShaderTool(tool, GetShaderToolOverride(tool));
}

ShaderToolLocation FindShaderTool(ShaderTool tool, const std::filesystem::path& overridePath)
{
    if (!overridePath.empty())
        return {IsFile(overridePath) ? overridePath : std::filesystem::path{}, "override"};

#ifdef _WIN32
    constexpr std::string_view bin = "Bin";
    const std::array<std::string_view, 2> names = tool == ShaderTool::Dxc
        ? std::array<std::string_view, 2>{"dxc.exe", ""}
        : std::array<std::string_view, 2>{"glslangValidator.exe", "glslc.exe"};
#else
    constexpr std::string_view bin = "bin";
    const std::array<std::string_view, 2> names = tool == ShaderTool::Dxc
        ? std::array<std::string_view, 2>{"dxc", ""}
        : std::array<std::string_view, 2>{"glslangValidator", "glslc"};
#endif
    if (tool == ShaderTool::Dxc)
        if (const auto explicitPath = EnvironmentPath("DXC_PATH"); IsFile(explicitPath))
            return {explicitPath, "DXC_PATH"};
    if (const auto sdk = EnvironmentPath("VULKAN_SDK"); !sdk.empty())
        for (const auto name : names)
            if (!name.empty())
                if (const auto candidate = sdk / std::string(bin) / std::string(name); IsFile(candidate))
                    return {candidate, "VULKAN_SDK"};
    for (const auto name : names)
        if (!name.empty())
            if (auto candidate = SearchPath(name); !candidate.empty())
                return {std::move(candidate), "PATH"};
    return {};
}

} // namespace engine::renderer
