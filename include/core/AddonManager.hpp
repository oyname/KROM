#pragma once

#include "core/IEngineAddon.hpp"
#include <memory>
#include <vector>

namespace engine {

struct AddonContext;

class AddonManager
{
public:
    [[nodiscard]] bool AddAddon(std::unique_ptr<IEngineAddon> addon);
    [[nodiscard]] bool RegisterAll(AddonContext& context);
    void UnregisterAll(AddonContext& context) noexcept;
    void Reset() noexcept;

private:
    struct AddonEntry
    {
        std::unique_ptr<IEngineAddon> addon;
        bool registered = false;
    };

    std::vector<AddonEntry> m_addons;
};

} // namespace engine
