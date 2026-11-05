#pragma once
// =============================================================================
// KROM Engine - core/Debug.hpp
// Logging-System - alle Messages mit Dateiname als Präfix.
// Coding Rule: Debug::Log("dateiname.cpp: message")
// =============================================================================
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdarg>
#include <functional>
#include <string>

#ifdef _WIN32
// Same declaration as in <windows.h> (kernel32), kept here so this header does
// not pull in the Windows headers.
extern "C" __declspec(dllimport) int __stdcall SetConsoleOutputCP(unsigned int codePageId);
#endif

namespace engine {

enum class LogLevel : uint8_t
{
    Verbose = 0,
    Info    = 1,
    Warning = 2,
    Error   = 3,
    Fatal   = 4,
};

class Debug
{
public:
#ifndef NDEBUG
    static inline LogLevel MinLevel = LogLevel::Info;
#else
    static inline LogLevel MinLevel = LogLevel::Error;
#endif

    static void ResetMinLevelForBuild() noexcept
    {
#ifndef NDEBUG
        MinLevel = LogLevel::Info;
#else
        MinLevel = LogLevel::Error;
#endif
    }

    static void Log(const char* msg, ...) noexcept
    {
        if (LogLevel::Info < MinLevel) return;
        va_list args;
        va_start(args, msg);
        Print(LogLevel::Info, msg, args);
        va_end(args);
    }

    static void LogWarning(const char* msg, ...) noexcept
    {
        if (LogLevel::Warning < MinLevel) return;
        va_list args;
        va_start(args, msg);
        Print(LogLevel::Warning, msg, args);
        va_end(args);
    }

    static void LogError(const char* msg, ...) noexcept
    {
        va_list args;
        va_start(args, msg);
        Print(LogLevel::Error, msg, args);
        va_end(args);
    }

    static void LogVerbose(const char* msg, ...) noexcept
    {
        if (LogLevel::Verbose < MinLevel) return;
        va_list args;
        va_start(args, msg);
        Print(LogLevel::Verbose, msg, args);
        va_end(args);
    }

    static void Assert(bool condition, const char* msg) noexcept
    {
        if (!condition)
        {
            LogError("Assert FAILED: %s", msg);
        }
    }

    // Optionaler Callback — wird zusätzlich zu stdout/stderr aufgerufen.
    // Gesetzt vom Editor um Meldungen in der Console anzuzeigen.
    static inline std::function<void(LogLevel, const char*)> onLog;

    using LogObserver = void (*)(LogLevel, const char*);

    static void SetLogObserver(LogObserver observer) noexcept
    {
        s_observer.store(observer);
    }

    static void NotifyLog(LogLevel level, const char* message)
    {
        if (onLog) onLog(level, message);
        if (const auto observer = s_observer.load())
            observer(level, message);
    }

private:
    static inline std::atomic<LogObserver> s_observer{nullptr};

    // Log text is UTF-8 (paths, arrows, umlauts). The Windows console defaults to
    // the OEM code page and would show it garbled ("→" as "ÔåÆ"); switch it to
    // UTF-8 once. Harmless without a console.
    static void EnsureUtf8Console() noexcept
    {
#ifdef _WIN32
        static const bool s_utf8Console = [] {
            SetConsoleOutputCP(65001u); // CP_UTF8
            return true;
        }();
        (void)s_utf8Console;
#endif
    }

    static void Print(LogLevel level, const char* fmt, va_list args) noexcept
    {
        const char* prefix = "";
        switch (level) {
            case LogLevel::Verbose: prefix = "[VERB] "; break;
            case LogLevel::Info:    prefix = "[INFO] "; break;
            case LogLevel::Warning: prefix = "[WARN] "; break;
            case LogLevel::Error:   prefix = "[ERR ] "; break;
            case LogLevel::Fatal:   prefix = "[FATL] "; break;
        }
        EnsureUtf8Console();
        char buf[2048];
        std::vsnprintf(buf, sizeof(buf), fmt, args);
        std::fprintf(level >= LogLevel::Warning ? stderr : stdout, "%s%s\n", prefix, buf);
        NotifyLog(level, buf);
    }
};

} // namespace engine
