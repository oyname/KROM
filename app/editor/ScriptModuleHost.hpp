#pragma once
#include "addons/script/ScriptModuleApi.hpp"
#include <filesystem>
#include <string>
#include <vector>

namespace engine::ecs { class World; }
namespace engine::app {

class ScriptModuleHost
{
public:
    ScriptModuleHost() = default;
    ~ScriptModuleHost();
    ScriptModuleHost(const ScriptModuleHost&) = delete;
    ScriptModuleHost& operator=(const ScriptModuleHost&) = delete;

    // Erst Kandidaten pruefen/registrieren, dann alte Objekte und Factories freigeben.
    bool Swap(const std::filesystem::path& library, ecs::World& world,
              script::ScriptRegistry& registry, std::string& error,
              const std::filesystem::path& projectRoot = {});
    void Unload(ecs::World* world, script::ScriptRegistry& registry);
    bool IsLoaded() const noexcept { return m_module != nullptr; }
    bool NeedsBuild(const std::filesystem::path& projectRoot,
                    const std::filesystem::path& library) const;
    bool SourceLibraryChanged(const std::filesystem::path& library) const;

    static std::filesystem::path LibraryPath(const std::filesystem::path& buildDir,
                                             const std::string& projectTarget);
    static bool CheckEngineLibraries(const std::filesystem::path& buildDir,
                                    const std::filesystem::path& editorExe, std::string& error);
private:
    void* m_module = nullptr;
    std::filesystem::path m_copyDirectory;
    std::filesystem::path m_sourceLibrary;
    std::filesystem::file_time_type m_sourceTime{};
    std::vector<std::filesystem::path> m_scriptFiles;
};
}
