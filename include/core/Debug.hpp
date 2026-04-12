#pragma once
// =============================================================================
// KROM Engine - core/Debug.hpp
// Logging-System - alle Messages mit Dateiname als Präfix.
// Coding Rule: Debug::Log("dateiname.cpp: message")
// =============================================================================
#include <cstdint>
#include <cstdio>
#include <cstdarg>
#include <string>

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

private:
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
        char buf[2048];
        std::vsnprintf(buf, sizeof(buf), fmt, args);
        std::fprintf(level >= LogLevel::Warning ? stderr : stdout, "%s%s\n", prefix, buf);
    }
};

} // namespace engine
