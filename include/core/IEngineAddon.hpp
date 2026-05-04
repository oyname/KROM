#pragma once

#include <string_view>

namespace engine {

struct AddonContext;

class IEngineAddon
{
public:
    virtual ~IEngineAddon() = default;

    [[nodiscard]] virtual const char* Name() const noexcept = 0;
    [[nodiscard]] virtual std::string_view Version() const noexcept { return {}; }
    virtual bool Register(AddonContext& context) = 0;
    virtual void Unregister(AddonContext& context) = 0;
};

} // namespace engine
