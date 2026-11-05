#include "app/editor/BuildTools.hpp"
#include "core/PathUtf8.hpp"
#include <chrono>
#include <fstream>
#include <iostream>
#include <thread>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

using namespace engine;
using namespace engine::app::build;

namespace {
int failures = 0;
void Check(bool ok, const char* description)
{
    std::cout << (ok ? "PASS: " : "FAIL: ") << description << '\n';
    if (!ok) ++failures;
}

struct ScratchDirectory
{
    std::filesystem::path path = std::filesystem::temp_directory_path() /
        PathFromUtf8("krom-build-tools-Größe " + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    ScratchDirectory()
    {
        if (!std::filesystem::create_directory(path)) throw std::runtime_error("Cannot create test directory");
    }
    ~ScratchDirectory()
    {
        std::error_code ec;
        if (path.parent_path() == std::filesystem::temp_directory_path()) std::filesystem::remove_all(path, ec);
    }
};
} // namespace

#ifdef _WIN32
int wmain(int argc, wchar_t** argv)
{
    if (argc > 1 && std::wstring(argv[1]) == L"--echo")
    {
        for (int i = 2; i < argc; ++i) std::cout << PathToUtf8(std::filesystem::path(argv[i])) << '\n';
        return 0;
    }
    if (argc > 1 && std::wstring(argv[1]) == L"--environment")
    {
        const auto* value = _wgetenv(L"KROM_BUILD_PROCESS_TEST");
        if (value) std::cout << PathToUtf8(std::filesystem::path(value));
        std::cout << '\n' << PathToUtf8(std::filesystem::current_path());
        return value ? 0 : 5;
    }
    if (argc > 1 && std::wstring(argv[1]) == L"--exit") return 17;
    if (argc > 1 && std::wstring(argv[1]) == L"--sleep")
    {
        std::this_thread::sleep_for(std::chrono::seconds(20));
        return 0;
    }
    ScratchDirectory scratch;
    const auto executable = std::filesystem::absolute(argv[0]);
    const auto copy = scratch.path / "test tool.exe";
    std::filesystem::copy_file(executable, copy);
    const std::vector<std::string> arguments = {"", "hello world", "Größe", "trailing\\", "two\\\\\"quotes", "& % ! ^ ( )"};
    ProcessCommand echo{copy, {"--echo"}, scratch.path};
    echo.arguments.insert(echo.arguments.end(), arguments.begin(), arguments.end());
    std::string output, expected;
    for (const auto& argument : arguments) expected += argument + "\r\n";
    const auto echoed = CaptureProcessOutput(echo, output);
    Check(echoed.Succeeded() && output == expected, "native argument quoting: Unicode, empty arguments, quotes, backslashes, shell characters");

    ProcessCommand environment{copy, {"--environment"}, scratch.path,
        {"KROM_BUILD_PROCESS_TEST=Größe & ! % value", "SystemRoot=" + PathToUtf8(std::filesystem::path(_wgetenv(L"SystemRoot")))}};
    const auto envResult = CaptureProcessOutput(environment, output);
    Check(envResult.Succeeded() && output == "Größe & ! % value\r\n" + PathToUtf8(scratch.path),
        "explicit Unicode environment and working directory");
    Check(CaptureProcessOutput({copy, {"--exit"}}, output).exitCode == 17, "nonzero exit code preserved");
    Check(!CaptureProcessOutput({scratch.path / "missing.exe"}, output).Succeeded(), "missing executable reported");
    std::atomic<bool> cancel{true};
    Check(RunProcess({copy, {"--sleep"}}, {}, &cancel).cancelled, "cancel before launch");
    cancel.store(false);
    const auto started = std::chrono::steady_clock::now();
    std::thread canceller([&] { std::this_thread::sleep_for(std::chrono::milliseconds(150)); cancel.store(true); });
    const auto cancelled = RunProcess({copy, {"--sleep"}}, {}, &cancel);
    canceller.join();
    Check(cancelled.cancelled && std::chrono::steady_clock::now() - started < std::chrono::seconds(5), "running process cancelled promptly");
    Check(RunProcess({copy, {"--sleep"}}, {}, nullptr, std::chrono::milliseconds(100)).cancelled, "discovery timeout stops process");

    const auto preferred = scratch.path / "preferred.exe";
    const auto fallback = scratch.path / "fallback.exe";
    std::ofstream(preferred).put('x');
    std::ofstream(fallback).put('x');
    const auto search = "\"" + PathToUtf8(scratch.path) + "\"";
    Check(FindBuildExecutable(preferred, "fallback.exe", search) == preferred, "installation tools preferred over PATH");
    Check(FindBuildExecutable({}, "fallback.exe", search) == fallback, "quoted Unicode PATH fallback");
    Check(FindBuildExecutable({}, "missing.exe", search).empty(), "missing PATH tool reported");
    Check(EnvironmentValue({"Path=one", "OTHER=two"}, "PATH") == "one", "Windows environment names ignore case");

    Check(CompilerCompatible(194435219, 194435219, true), "matching compiler accepted for scripts");
    Check(!CompilerCompatible(194435219, 194435220, true), "newer script compiler rejected until ABI policy changes");
    Check(!CompilerCompatible(194435219, 194435218, false), "older runtime compiler rejected");
    Check(CompilerCompatible(194435219, 194435220, false), "newer runtime compiler accepted");
    Check(!CompilerCompatible(194435219, 204435219, false), "unknown compiler family rejected");

    BuildTools fakeTools;
    fakeTools.compiler = preferred;
    fakeTools.ninja = fallback;
    auto writeCache = [&](const std::filesystem::path& compiler) {
        std::ofstream cache(scratch.path / "CMakeCache.txt");
        cache << "CMAKE_CXX_COMPILER:FILEPATH=" << PathToGenericUtf8(compiler) << '\n'
              << "CMAKE_MAKE_PROGRAM:FILEPATH=" << PathToGenericUtf8(fallback) << '\n'
              << "CMAKE_GENERATOR:INTERNAL=Ninja\n";
    };
    std::string key, detail;
    writeCache(preferred);
    Check(CheckBuildCache(scratch.path, fakeTools, key, detail), "matching cached toolchain accepted");
    writeCache(copy);
    Check(!CheckBuildCache(scratch.path, fakeTools, key, detail) && key == "build.cache_toolchain_mismatch",
        "different cached compiler rejected before build");

    // Test bootstrap quoting independently of any particular VS installation.
    const auto batch = scratch.path / "setup %PATH% ! &.bat";
    {
        std::ofstream script(batch, std::ios::binary);
        script << "@echo off\r\nset KROM_ENV_BOOTSTRAP=ok\r\n";
    }
    std::vector<std::string> bootstrapped;
    std::string bootstrapError;
    Check(ReadMsvcEnvironment(batch, bootstrapped, bootstrapError) && EnvironmentValue(bootstrapped, "KROM_ENV_BOOTSTRAP") == "ok",
        "batch environment bootstrap supports Unicode and shell characters in installation path");

    const auto discovery = DiscoverBuildTools();
    if (!discovery.Succeeded()) std::cout << discovery.errorKey << ": " << discovery.detail << '\n';
    Check(discovery.Succeeded(), "installed Visual Studio discovered through vswhere");
    if (discovery.Succeeded())
    {
        const auto& tools = discovery.tools;
        std::cout << "VS: " << PathToUtf8(tools.installation) << "\nMSVC: " << tools.compilerVersion << '\n';
        Check(CompilerCompatible(_MSC_FULL_VER, tools.compilerVersion, true), "compiler probe matches compiler used for tests");
        Check(CaptureProcessOutput({tools.cmake, {"--version"}, {}, tools.environment}, output).Succeeded(), "discovered CMake starts directly");
        Check(CaptureProcessOutput({tools.ninja, {"--version"}, {}, tools.environment}, output).Succeeded(), "discovered Ninja starts directly");
        Check(GetBuildTools().Succeeded() && GetBuildTools().tools.compilerVersion == tools.compilerVersion, "session tool discovery cached");

        const auto source = scratch.path / "source";
        const auto build = scratch.path / "build";
        std::filesystem::create_directory(source);
        std::ofstream(source / "CMakeLists.txt") <<
            "cmake_minimum_required(VERSION 3.22)\nproject(BuildProbe LANGUAGES CXX)\nadd_executable(probe main.cpp)\n";
        std::ofstream(source / "main.cpp") << "int main() { return 17; }\n";
        const auto configured = CaptureProcessOutput({tools.cmake,
            {"-S", PathToUtf8(source), "-B", PathToUtf8(build), "-G", "Ninja", "-DCMAKE_BUILD_TYPE=Debug",
             "-DCMAKE_CXX_COMPILER=" + PathToGenericUtf8(tools.compiler),
             "-DCMAKE_MAKE_PROGRAM=" + PathToGenericUtf8(tools.ninja)}, source, tools.environment}, output);
        if (!configured.Succeeded()) std::cout << output << '\n';
        Check(configured.Succeeded(), "CMake configures a real C++ project in a Unicode path with the captured developer environment");
        if (configured.Succeeded())
        {
            const auto built = CaptureProcessOutput({tools.ninja, {"-C", PathToUtf8(build), "probe"}, source, tools.environment}, output);
            if (!built.Succeeded()) std::cout << output << '\n';
            Check(built.Succeeded(), "Ninja compiles and links directly without a batch build script");
            if (built.Succeeded())
                Check(CaptureProcessOutput({build / "probe.exe"}, output).exitCode == 17, "compiled probe executes correctly");
        }
    }
    std::cout << "Failures: " << failures << '\n';
    return failures ? 1 : 0;
}
#else
int main()
{
    std::cout << "Windows toolchain integration tests skipped on this platform\n";
    return 0;
}
#endif
