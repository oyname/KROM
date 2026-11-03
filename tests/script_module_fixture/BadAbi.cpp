#include "addons/script/ScriptModuleApi.hpp"
KROM_SCRIPT_EXPORT uint32_t KromScriptModuleApiVersion() { return engine::script::ScriptModuleAbiVersion; }
KROM_SCRIPT_EXPORT engine::script::ScriptModuleAbi KromScriptModuleAbi()
{
    auto abi = engine::script::CurrentScriptModuleAbi();
    ++abi.version;
    return abi;
}
KROM_SCRIPT_EXPORT bool KromScriptModuleInit(const engine::script::ScriptHostApi*) { return false; }
KROM_SCRIPT_EXPORT void KromRegisterGameScripts(engine::script::ScriptRegistry&) {}
