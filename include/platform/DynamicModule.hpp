#pragma once
#include <filesystem>
#include <string>

namespace engine::platform {
// The caller releases script objects and symbol-backed callbacks before unloading.
using DynamicModule = void*;
[[nodiscard]] DynamicModule LoadDynamicModule(const std::filesystem::path& file, std::string& error);
[[nodiscard]] void* FindDynamicSymbol(DynamicModule module, const char* name);
void UnloadDynamicModule(DynamicModule module);
}
