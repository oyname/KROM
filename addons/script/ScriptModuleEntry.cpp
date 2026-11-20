#include "addons/script/ScriptModuleApi.hpp"

// Embedded in the DLL itself: a stale DLL at the same output path must never be
// accepted merely because its target name and engine ABI happen to match.
KROM_SCRIPT_EXPORT const char* KromScriptModuleProjectRoot()
{
#ifdef KROM_SCRIPT_PROJECT_ROOT
    return KROM_SCRIPT_PROJECT_ROOT;
#else
    return "";
#endif
}

KROM_SCRIPT_EXPORT uint32_t KromScriptModuleApiVersion()
{
    return engine::script::ScriptModuleAbiVersion;
}

KROM_SCRIPT_EXPORT engine::script::ScriptModuleAbi KromScriptModuleAbi()
{
    return engine::script::CurrentScriptModuleAbi();
}

KROM_SCRIPT_EXPORT bool KromScriptModuleInit(const engine::script::ScriptHostApi* api)
{
    using namespace engine;
    if (!api || api->size != sizeof(script::ScriptHostApi) ||
        api->version != script::ScriptModuleAbiVersion || !api->resolveComponent)
        return false;
    // Statische Script-Initialisierer duerfen vor Init keine lokalen IDs cachen.
    // Lazy Get allein verhindert das bei benutzerdefinierten Initialisierern nicht.
    if (HasLocalComponentTypeIds())
        return false;
    SetComponentTypeResolver(api->resolveComponent);
    Debug::MinLevel = api->minLogLevel;
    const auto log = api->log;
    Debug::onLog = [log](LogLevel level, const char* message) {
        if (log) log(level, message);
    };
    return true;
}

#ifndef KROM_HAS_GAME_SCRIPTS
KROM_SCRIPT_EXPORT void KromRegisterGameScripts(engine::script::ScriptRegistry&) {}
#endif
