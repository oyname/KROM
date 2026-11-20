#include "platform/DynamicModule.hpp"
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace engine::platform {
DynamicModule LoadDynamicModule(const std::filesystem::path& file, std::string& error)
{
    error.clear();
#ifdef _WIN32
    std::error_code pathError;
    auto absolute = std::filesystem::absolute(file, pathError).lexically_normal().make_preferred();
    if (pathError) { error = pathError.message(); return nullptr; }
    auto native = absolute.native();
    // The loader can exceed its internal path buffers even below MAX_PATH
    // (including dependency search paths). Always use the extended absolute
    // form, including UNC paths, rather than guessing a safe filename length.
    if (native.rfind(L"\\\\?\\", 0) != 0)
        native = native.rfind(L"\\\\", 0) == 0 ? L"\\\\?\\UNC\\" + native.substr(2) : L"\\\\?\\" + native;
    const auto module = LoadLibraryExW(native.c_str(), nullptr,
        LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    if (!module) error = "LoadLibrary (Windows " + std::to_string(GetLastError()) + ")";
#else
    const auto module = dlopen(file.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (!module) { const auto* reason = dlerror(); error = reason ? reason : "dlopen failed"; }
#endif
    return module;
}

void* FindDynamicSymbol(DynamicModule module, const char* name)
{
    if (!module) return nullptr;
#ifdef _WIN32
    return reinterpret_cast<void*>(GetProcAddress(static_cast<HMODULE>(module), name));
#else
    return dlsym(module, name);
#endif
}

void UnloadDynamicModule(DynamicModule module)
{
    if (!module) return;
#ifdef _WIN32
    FreeLibrary(static_cast<HMODULE>(module));
#else
    dlclose(module);
#endif
}
}
