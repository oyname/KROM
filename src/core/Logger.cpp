#include "core/Logger.hpp"

#include "core/Debug.hpp"

namespace engine {

namespace {

class DebugLogger final : public ILogger
{
public:
    void Verbose(const char* message) noexcept override
    {
        Debug::LogVerbose("%s", message != nullptr ? message : "");
    }

    void Info(const char* message) noexcept override
    {
        Debug::Log("%s", message != nullptr ? message : "");
    }

    void Warning(const char* message) noexcept override
    {
        Debug::LogWarning("%s", message != nullptr ? message : "");
    }

    void Error(const char* message) noexcept override
    {
        Debug::LogError("%s", message != nullptr ? message : "");
    }
};

} // namespace

ILogger& GetDefaultLogger() noexcept
{
    static DebugLogger logger;
    return logger;
}

} // namespace engine
