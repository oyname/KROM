#pragma once
#include "core/Types.hpp"
#include "core/Debug.hpp"
#include "addons/script/ScriptRegistry.hpp"
#include "addons/script/ScriptModuleAbiPolicy.hpp"
#include <cstring>

namespace engine::script {
// 5: script::RenderSettings gained ColorGrading (size change of a by-value API type).
// 6: script::RenderSettings gained burleyDiffuse.
// 7: explicit compiler/architecture identity and immutable SDK build ID.
inline constexpr uint32_t ScriptModuleAbiVersion = 7;

struct ScriptModuleAbi
{
    uint32_t version = ScriptModuleAbiVersion;
    uint32_t apiSize = 0;
    uint32_t compiler = 0;
    uint32_t iteratorDebug = 0;
    uint32_t debug = 0;
    uint32_t pointerSize = sizeof(void*);
    uint32_t runtime = 0;
    uint32_t compilerFamily = 0; // 1: MSVC, 2: GCC, 3: Clang.
    uint32_t architecture = 0; // 1: x64, 2: x86, 3: ARM64.
    char sdkBuildId[65] = {};
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
#if defined(_M_X64) || defined(__x86_64__)
    abi.architecture = 1;
#elif defined(_M_IX86) || defined(__i386__)
    abi.architecture = 2;
#elif defined(_M_ARM64) || defined(__aarch64__)
    abi.architecture = 3;
#endif
#ifdef KROM_SDK_BUILD_ID
    static_assert(sizeof(KROM_SDK_BUILD_ID) == sizeof(abi.sdkBuildId));
    std::memcpy(abi.sdkBuildId, KROM_SDK_BUILD_ID, sizeof(abi.sdkBuildId));
#endif
    abi.apiSize = sizeof(ScriptHostApi);
#ifdef _DLL
    abi.runtime |= 1u;
#endif
#ifdef _MT
    abi.runtime |= 2u;
#endif
#ifdef _DEBUG
    abi.runtime |= 4u;
#endif
#ifdef _MSC_FULL_VER
    abi.compiler = _MSC_FULL_VER;
    abi.compilerFamily = 1;
#elif defined(__GNUC__)
    abi.compiler = __GNUC__ * 10000 + __GNUC_MINOR__ * 100 + __GNUC_PATCHLEVEL__;
    abi.compilerFamily = 2;
#endif
#ifdef __clang__
    abi.compilerFamily = 3;
#endif
#ifdef _ITERATOR_DEBUG_LEVEL
    abi.iteratorDebug = _ITERATOR_DEBUG_LEVEL;
#endif
#ifndef NDEBUG
    abi.debug = 1;
#endif
    return abi;
}

inline bool CompatibleScriptModuleAbi(const ScriptModuleAbi& abi, const ScriptModuleAbi& host) noexcept
{
    const bool sdk = host.sdkBuildId[0] != 0;
    const bool compiler = sdk && host.compilerFamily == 1 && abi.compilerFamily == 1
        ? CompatibleMsvcSdkCompiler(host.compiler, abi.compiler) : abi.compiler == host.compiler;
    return abi.version == host.version && abi.apiSize == host.apiSize &&
        compiler && abi.compilerFamily == host.compilerFamily && abi.architecture == host.architecture &&
        std::memcmp(abi.sdkBuildId, host.sdkBuildId, sizeof(host.sdkBuildId)) == 0 &&
        abi.iteratorDebug == host.iteratorDebug &&
        abi.debug == host.debug && abi.pointerSize == host.pointerSize && abi.runtime == host.runtime;
}
inline bool CompatibleScriptModuleAbi(const ScriptModuleAbi& abi) noexcept
{
    return CompatibleScriptModuleAbi(abi, CurrentScriptModuleAbi());
}
using ScriptModuleAbiFn = ScriptModuleAbi (*)();
// Feste primitive Signatur: vor dem versionierten POD-Getter pruefen.
using ScriptModuleVersionFn = uint32_t (*)();
using ScriptModuleInitFn = bool (*)(const ScriptHostApi*);
using RegisterGameScriptsFn = void (*)(ScriptRegistry&);
}

#if defined(_WIN32)
#define KROM_SCRIPT_EXPORT extern "C" __declspec(dllexport)
#else
#define KROM_SCRIPT_EXPORT extern "C" __attribute__((visibility("default")))
#endif

