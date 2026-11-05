#pragma once
#include "app/editor/BuildProcess.hpp"
#include <cstdint>

namespace engine::app::build {
struct BuildTools
{
    std::filesystem::path installation;
    std::filesystem::path cmake;
    std::filesystem::path ninja;
    std::filesystem::path compiler;
    uint32_t compilerVersion = 0; // MSVC _MSC_FULL_VER on Windows.
    std::vector<std::string> environment;
};

struct ToolDiscovery
{
    BuildTools tools;
    std::string errorKey;
    std::string detail;
    bool Succeeded() const { return errorKey.empty(); }
};

// Successful discovery is cached for this editor session; failures can be retried.
ToolDiscovery DiscoverBuildTools();
ToolDiscovery GetBuildTools();
std::filesystem::path FindBuildExecutable(const std::filesystem::path& preferred,
                                         const std::string& name, const std::string& searchPath);
std::string EnvironmentValue(const std::vector<std::string>& environment, const std::string& name);
bool CompilerCompatible(uint32_t editorVersion, uint32_t buildVersion, bool scriptBuild);
bool CheckBuildCache(const std::filesystem::path& buildDirectory, const BuildTools& tools,
                     std::string& errorKey, std::string& detail);
} // namespace engine::app::build
