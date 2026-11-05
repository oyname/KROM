#pragma once
#include <atomic>
#include <chrono>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace engine::app::build {

// Arguments and environment entries are UTF-8. No shell expansion is performed.
struct ProcessCommand
{
    std::filesystem::path executable;
    std::vector<std::string> arguments;
    std::filesystem::path workingDirectory;
    std::vector<std::string> environment; // Empty: inherit the editor's environment.
};

struct ProcessResult
{
    int exitCode = -1;
    bool cancelled = false;
    std::string error;
    bool Succeeded() const { return exitCode == 0 && !cancelled && error.empty(); }
};

using ProcessOutput = std::function<void(const std::string&)>;
ProcessResult RunProcess(const ProcessCommand& command, const ProcessOutput& output = {},
                         const std::atomic<bool>* cancel = nullptr,
                         std::chrono::milliseconds timeout = {});
ProcessResult CaptureProcessOutput(const ProcessCommand& command, std::string& output);

// Windows toolchain bootstrap only. Builds themselves never run through a shell.
// toolset ("14.44.35207", digits and dots only) is passed as -vcvars_ver; empty
// selects the installation's default toolset.
bool ReadMsvcEnvironment(const std::filesystem::path& setupScript, const std::string& toolset,
                         std::vector<std::string>& environment, std::string& error);

} // namespace engine::app::build
