#pragma once
#include "core/Types.hpp"
#include "core/Debug.hpp"
#include "addons/script/ScriptRegistry.hpp"

namespace engine::script {
inline constexpr uint32_t ScriptModuleAbiVersion = 1;

struct ScriptModuleAbi
{
    uint32_t version = ScriptModuleAbiVersion;
    uint32_t apiSize = 0;
    uint32_t compiler = 0;
    uint32_t iteratorDebug = 0;
    uint32_t debug = 0;
    uint32_t pointerSize = sizeof(void*);
};

struct ScriptHostApi
{
    uint32_t size = sizeof(ScriptHostApi);
    uint32_t version = ScriptModuleAbiVersion;
    ComponentTypeResolver resolveComponent = nullptr;
    void (*log)(LogLevel, const char*) = nullptr;
    LogLevel minLogLevel = LogLevel::Info;
};

inline ScriptModuleAbi CurrentScriptModuleAbi() noexcept
{
    ScriptModuleAbi abi;
    abi.apiSize = sizeof(ScriptHostApi);
#ifdef _MSC_FULL_VER
    abi.compiler = _MSC_FULL_VER;
#elif defined(__GNUC__)
    abi.compiler = __GNUC__ * 10000 + __GNUC_MINOR__ * 100 + __GNUC_PATCHLEVEL__;
#endif
#ifdef _ITERATOR_DEBUG_LEVEL
    abi.iteratorDebug = _ITERATOR_DEBUG_LEVEL;
#endif
#ifndef NDEBUG
    abi.debug = 1;
#endif
    return abi;
}

inline bool CompatibleScriptModuleAbi(const ScriptModuleAbi& abi) noexcept
{
    const auto host = CurrentScriptModuleAbi();
    return abi.version == host.version && abi.apiSize == host.apiSize &&
        abi.compiler == host.compiler && abi.iteratorDebug == host.iteratorDebug &&
        abi.debug == host.debug && abi.pointerSize == host.pointerSize;
}
using ScriptModuleAbiFn = ScriptModuleAbi (*)();
using ScriptModuleInitFn = bool (*)(const ScriptHostApi*);
using RegisterGameScriptsFn = void (*)(ScriptRegistry&);
}

#if defined(_WIN32)
#define KROM_SCRIPT_EXPORT extern "C" __declspec(dllexport)
#else
#define KROM_SCRIPT_EXPORT extern "C" __attribute__((visibility("default")))
#endif

