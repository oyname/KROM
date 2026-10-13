#pragma once

namespace engine {

class ILogger
{
public:
    virtual ~ILogger() = default;

    virtual void Verbose(const char* message) noexcept = 0;
    virtual void Info(const char* message) noexcept = 0;
    virtual void Warning(const char* message) noexcept = 0;
    virtual void Error(const char* message) noexcept = 0;
};

[[nodiscard]] ILogger& GetDefaultLogger() noexcept;

} // namespace engine
