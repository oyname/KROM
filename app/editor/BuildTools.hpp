#pragma once
#include "app/editor/BuildProcess.hpp"
#include <cstdint>

namespace engine::app::build {
struct BuildTools
{
    std::filesystem::path installation;
    std::string toolset; // MSVC toolset folder, e.g. "14.44.35207"; empty: installation default.
    std::filesystem::path cmake;
    std::filesystem::path ninja;
    std::filesystem::path compiler;
    uint32_t compilerVersion = 0; // MSVC _MSC_FULL_VER on Windows.
    std::vector<std::string> environment;
};

// User-chosen tools (editor settings); empty fields use automatic discovery.
struct BuildToolOverrides
{
    std::filesystem::path installation; // Visual Studio / Build Tools root (contains Common7)
    std::filesystem::path cmake;
    std::filesystem::path ninja;
    bool operator==(const BuildToolOverrides&) const = default;
};

struct ToolDiscovery
{
    BuildTools tools;
    std::string errorKey;
    std::string detail;
    bool installationOverridden = false; // false: found through vswhere
    bool Succeeded() const { return errorKey.empty(); }
};

// Discovery runs vswhere, VsDevCmd and a compiler probe (seconds): call it from a
// worker thread, never from the editor's main loop.
// preferredCompiler (_MSC_FULL_VER, 0 = none) selects among all installations and
// toolsets the one whose compiler matches exactly; otherwise the newest usable one.
// An installation override replaces vswhere (only that installation's toolsets are
// tried); CMake/Ninja overrides replace the installation's copies and PATH. An
// override that does not exist is an error, never a silent fallback.
ToolDiscovery DiscoverBuildTools(uint32_t preferredCompiler = 0, const BuildToolOverrides& overrides = {});
// Successful discovery is cached for this editor session and these overrides;
// failures can be retried.
ToolDiscovery GetBuildTools(uint32_t preferredCompiler = 0, const BuildToolOverrides& overrides = {});

// Toolset folder names ("14.44.35207") newest first; with preferredCompiler the
// toolsets of the same minor version (14.44 <-> _MSC_VER 1944) come first.
std::vector<std::string> OrderToolsets(std::vector<std::string> toolsets, uint32_t preferredCompiler);
bool ToolsetMatchesCompiler(const std::string& toolset, uint32_t compilerVersion);

std::filesystem::path FindBuildExecutable(const std::filesystem::path& preferred,
                                         const std::string& name, const std::string& searchPath);
std::string EnvironmentValue(const std::vector<std::string>& environment, const std::string& name);
bool CompilerCompatible(uint32_t editorVersion, uint32_t buildVersion, bool scriptBuild);
bool CheckBuildCache(const std::filesystem::path& buildDirectory, const BuildTools& tools,
                     std::string& errorKey, std::string& detail);
// Ninja recorded in an existing build folder's CMakeCache.txt (no discovery needed);
// empty if the cache or the executable is missing.
std::filesystem::path CachedMakeProgram(const std::filesystem::path& buildDirectory);
} // namespace engine::app::build
