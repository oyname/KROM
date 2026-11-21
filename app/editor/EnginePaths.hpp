#pragma once

#include <filesystem>
#include <cstdint>
#include <string>

namespace engine::app {

// Resource directories are read-only in a packaged SDK. User state and caches
// must never be derived from them. Empty source/build paths mean unavailable.
struct EnginePaths
{
    bool sdk = false;
    std::string error;
    std::filesystem::path sdkRoot;
    std::filesystem::path assetsDir;
    std::filesystem::path shadersDir;
    std::filesystem::path editorDir;
    std::filesystem::path projectsDir;
    std::filesystem::path engineSourceDir;
    std::filesystem::path engineBuildDir;
    std::filesystem::path sdkCmakeDir;
    std::filesystem::path projectTemplateDir;
    std::filesystem::path userDataDir;
    std::filesystem::path shaderCacheDir;
    std::filesystem::path standaloneScenesDir;
    std::string sdkBuildId;
    std::string sdkToolset;
    uint32_t sdkCompilerVersion = 0;

    [[nodiscard]] bool BuildsAvailable() const noexcept { return error.empty() && (!sdk || !sdkBuildId.empty()); }
    // SDK mode: <project>/.krom/b/<12 hex>, a hash of build ID, platform, toolset
    // and configuration. Empty outside SDK mode or for invalid inputs.
    [[nodiscard]] std::filesystem::path ProjectBuildDir(const std::filesystem::path& project,
                                                       const std::string& toolset) const;
    // Plain-text values behind the hashed directory name (kProjectBuildInfoFile).
    [[nodiscard]] std::string ProjectBuildInfo(const std::string& toolset) const;
    [[nodiscard]] std::filesystem::path RuntimeBuildDir(const char* configuration) const;
    [[nodiscard]] std::filesystem::path ProjectBrowseDir() const;
};

inline constexpr const char* kProjectBuildInfoFile = "krom-build-info.txt";
// cl.exe is not long-path aware and CMake refuses object paths above 250
// characters. Its compiler probes are the deepest paths below a build directory
// (CMakeFiles/CMakeScratch/TryCompile-*/CMakeFiles/cmTC_*.dir/*.obj, ~92).
inline constexpr size_t kMaxCompilerObjectPath = 250;
inline constexpr size_t kProjectBuildPathReserve = 95;
// Characters by which buildDir is too deep for its object paths; 0 means it fits.
[[nodiscard]] size_t ProjectBuildPathOverflow(const std::filesystem::path& buildDir) noexcept;

// Pure path resolution and manifest reads: does not create or write anything.
// Explicit user directories make relocated packages testable without changing
// process environment variables or the real user's preferences.
[[nodiscard]] EnginePaths ResolveEnginePaths(const std::filesystem::path& executable,
                                            const std::filesystem::path& userDataDirectory,
                                            const std::filesystem::path& documentsDirectory);
[[nodiscard]] const EnginePaths& GetEnginePaths();
[[nodiscard]] std::filesystem::path CurrentExecutablePath();

} // namespace engine::app
