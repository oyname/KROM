#include "app/editor/BuildProcess.hpp"
#include "core/PathUtf8.hpp"
#include <algorithm>
#include <cstdint>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

namespace engine::app::build {
#ifdef _WIN32
namespace {
struct Handle
{
    HANDLE value = nullptr;
    ~Handle() { if (value && value != INVALID_HANDLE_VALUE) CloseHandle(value); }
};

std::wstring QuoteArgument(const std::wstring& argument)
{
    std::wstring quoted = L"\"";
    size_t slashes = 0;
    for (wchar_t ch : argument)
    {
        if (ch == L'\\') { ++slashes; continue; }
        quoted.append(ch == L'"' ? slashes * 2 + 1 : slashes, L'\\');
        quoted += ch;
        slashes = 0;
    }
    quoted.append(slashes * 2, L'\\');
    quoted += L'"';
    return quoted;
}

ProcessResult RunWindowsProcess(const ProcessCommand& command, std::wstring commandLine,
                               const ProcessOutput& output, const std::atomic<bool>* cancel,
                               std::chrono::milliseconds timeout)
{
    ProcessResult result;
    auto fail = [&](const char* operation) {
        result.error = std::string(operation) + " (Windows " + std::to_string(GetLastError()) + ")";
        return result;
    };
    if (cancel && cancel->load(std::memory_order_acquire))
    {
        result.cancelled = true;
        return result;
    }
    Handle readPipe, writePipe, job, processHandle, threadHandle;
    SECURITY_ATTRIBUTES security{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
    if (!CreatePipe(&readPipe.value, &writePipe.value, &security, 0) ||
        !SetHandleInformation(readPipe.value, HANDLE_FLAG_INHERIT, 0))
        return fail("CreatePipe");
    job.value = CreateJobObjectW(nullptr, nullptr);
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (!job.value || !SetInformationJobObject(job.value, JobObjectExtendedLimitInformation, &limits, sizeof(limits)))
        return fail("CreateJobObject");

    std::vector<std::wstring> entries;
    for (const auto& entry : command.environment) entries.push_back(WideFromUtf8(entry));
    std::sort(entries.begin(), entries.end(), [](const auto& a, const auto& b) {
        return _wcsicmp(a.c_str(), b.c_str()) < 0;
    });
    std::vector<wchar_t> block;
    for (const auto& entry : entries)
    {
        block.insert(block.end(), entry.begin(), entry.end());
        block.push_back(L'\0');
    }
    block.push_back(L'\0');

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdOutput = writePipe.value;
    startup.hStdError = writePipe.value;
    PROCESS_INFORMATION process{};
    const auto workingDirectory = command.workingDirectory.wstring();
    if (!CreateProcessW(command.executable.c_str(), commandLine.data(), nullptr, nullptr, TRUE,
                        CREATE_SUSPENDED | CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT,
                        entries.empty() ? nullptr : block.data(),
                        workingDirectory.empty() ? nullptr : workingDirectory.c_str(), &startup, &process))
        return fail("CreateProcess");
    processHandle.value = process.hProcess;
    threadHandle.value = process.hThread;
    if (!AssignProcessToJobObject(job.value, process.hProcess))
    {
        const auto failure = fail("AssignProcessToJobObject");
        TerminateProcess(process.hProcess, 1);
        WaitForSingleObject(process.hProcess, INFINITE);
        return failure;
    }
    if (ResumeThread(process.hThread) == DWORD(-1)) return fail("ResumeThread");
    CloseHandle(writePipe.value);
    writePipe.value = nullptr;
    const auto started = std::chrono::steady_clock::now();
    for (;;)
    {
        if ((cancel && cancel->load(std::memory_order_acquire)) ||
            (timeout.count() > 0 && std::chrono::steady_clock::now() - started >= timeout))
        {
            result.cancelled = true;
            if (!(cancel && cancel->load(std::memory_order_acquire))) result.error = "Process timed out";
            TerminateJobObject(job.value, 1);
            WaitForSingleObject(process.hProcess, INFINITE);
            break;
        }
        DWORD available = 0;
        if (PeekNamedPipe(readPipe.value, nullptr, 0, nullptr, &available, nullptr) && available)
        {
            char buffer[4096];
            DWORD count = 0;
            if (ReadFile(readPipe.value, buffer, (std::min)(available, DWORD(sizeof(buffer))), &count, nullptr) && count)
            {
                if (output) output(std::string(buffer, count));
                continue;
            }
        }
        if (WaitForSingleObject(process.hProcess, 25) == WAIT_OBJECT_0)
        {
            DWORD tail = 0;
            if (PeekNamedPipe(readPipe.value, nullptr, 0, nullptr, &tail, nullptr) && tail) continue;
            break;
        }
    }
    DWORD exitCode = 1;
    GetExitCodeProcess(process.hProcess, &exitCode);
    result.exitCode = static_cast<int>(exitCode);
    return result;
}
} // namespace
#endif

ProcessResult RunProcess(const ProcessCommand& command, const ProcessOutput& output,
                         const std::atomic<bool>* cancel, std::chrono::milliseconds timeout)
{
#ifdef _WIN32
    auto commandLine = QuoteArgument(command.executable.wstring());
    for (const auto& argument : command.arguments) commandLine += L" " + QuoteArgument(WideFromUtf8(argument));
    return RunWindowsProcess(command, std::move(commandLine), output, cancel, timeout);
#else
    (void)command; (void)output; (void)cancel; (void)timeout;
    return {-1, false, "Build process execution is not implemented for this platform yet"};
#endif
}

ProcessResult CaptureProcessOutput(const ProcessCommand& command, std::string& output)
{
    output.clear();
    return RunProcess(command, [&](const std::string& chunk) { output += chunk; }, nullptr, std::chrono::seconds(30));
}

bool ReadMsvcEnvironment(const std::filesystem::path& setupScript,
                         std::vector<std::string>& environment, std::string& error)
{
#ifdef _WIN32
    wchar_t systemDirectory[MAX_PATH]{};
    if (!GetSystemDirectoryW(systemDirectory, MAX_PATH)) { error = "GetSystemDirectory failed"; return false; }
    ProcessCommand command;
    command.executable = std::filesystem::path(systemDirectory) / "cmd.exe";
    const auto* inherited = GetEnvironmentStringsW();
    if (!inherited) { error = "GetEnvironmentStrings failed"; return false; }
    for (const wchar_t* entry = inherited; *entry; entry += wcslen(entry) + 1)
    {
        if (_wcsnicmp(entry, L"KROM_BUILD_VS_SETUP=", 19) != 0)
            command.environment.push_back(PathToUtf8(std::filesystem::path(entry)));
    }
    FreeEnvironmentStringsW(const_cast<wchar_t*>(inherited));
    command.environment.push_back("KROM_BUILD_VS_SETUP=" + PathToUtf8(setupScript));
    // /u makes SET's output UTF-16LE, independent of the user's console code page.
    // Disable delayed expansion so exclamation marks in installation paths survive.
    // Expand the path once through an environment variable. Literal percent
    // sequences in the installation path must not be interpreted as variables.
    const auto commandLine = QuoteArgument(command.executable.wstring()) +
        L" /d /u /v:off /s /c \"\"%KROM_BUILD_VS_SETUP%\" -arch=x64 -host_arch=x64 >nul && set\"";
    std::string bytes;
    const auto result = RunWindowsProcess(command, commandLine,
        [&](const std::string& chunk) { bytes += chunk; }, nullptr, std::chrono::seconds(30));
    if (!result.Succeeded() || bytes.size() % 2)
    {
        error = result.error.empty() ? "VsDevCmd failed (exit " + std::to_string(result.exitCode) + ")" : result.error;
        return false;
    }
    std::wstring decoded;
    for (size_t i = 0; i < bytes.size(); i += 2)
        decoded += wchar_t(uint8_t(bytes[i]) | (uint16_t(uint8_t(bytes[i + 1])) << 8));
    environment.clear();
    size_t start = 0;
    while (start < decoded.size())
    {
        const auto end = decoded.find(L'\n', start);
        auto entry = decoded.substr(start, end == std::wstring::npos ? end : end - start);
        if (!entry.empty() && entry.back() == L'\r') entry.pop_back();
        if (entry.find(L'=', 1) != std::wstring::npos && _wcsnicmp(entry.c_str(), L"KROM_BUILD_VS_SETUP=", 19) != 0)
            environment.push_back(PathToUtf8(std::filesystem::path(entry)));
        if (end == std::wstring::npos) break;
        start = end + 1;
    }
    return !environment.empty();
#else
    (void)setupScript; (void)environment;
    error = "MSVC environment setup is only available on Windows";
    return false;
#endif
}
} // namespace engine::app::build
