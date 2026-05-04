#include "core/AddonManager.hpp"

#include "core/AddonContext.hpp"
#include "core/Debug.hpp"

#include <string>

namespace engine {

bool AddonManager::AddAddon(std::unique_ptr<IEngineAddon> addon)
{
    if (!addon)
    {
        Debug::LogError("AddonManager: rejecting null addon");
        return false;
    }

    const char* name = addon->Name();
    if (name == nullptr || name[0] == '\0')
    {
        Debug::LogError("AddonManager: rejecting addon with empty name");
        return false;
    }

    for (const AddonEntry& entry : m_addons)
    {
        if (entry.addon && std::string(entry.addon->Name()) == name)
        {
            Debug::LogError("AddonManager: duplicate addon name '%s'", name);
            return false;
        }
    }

    m_addons.push_back(AddonEntry{ std::move(addon), false });
    return true;
}

bool AddonManager::RegisterAll(AddonContext& context)
{
    for (AddonEntry& entry : m_addons)
    {
        if (entry.registered)
            continue;

        // Mark before calling Register() so UnregisterAll() will invoke
        // Unregister() on this addon even if Register() only partially succeeded
        // (e.g. components registered but feature registration failed).
        entry.registered = true;

        if (!entry.addon->Register(context))
        {
            Debug::LogError("AddonManager: addon registration failed: %s", entry.addon->Name());
            UnregisterAll(context);
            return false;
        }
    }

    return true;
}

void AddonManager::UnregisterAll(AddonContext& context) noexcept
{
    for (auto it = m_addons.rbegin(); it != m_addons.rend(); ++it)
    {
        if (!it->registered || !it->addon)
            continue;

        it->addon->Unregister(context);
        it->registered = false;
    }
}

void AddonManager::Reset() noexcept
{
    m_addons.clear();
}

} // namespace engine
