#pragma once
#include "addons/script/ScriptModuleApi.hpp"
#include <filesystem>
#include <string>
#include <string_view>
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
    // Nach erfolgreichem Build ohne Neu-Link: Quellen gelten bis buildStart als gebaut.
    void MarkBuilt(const std::filesystem::path& projectRoot, const std::filesystem::path& library,
                   std::filesystem::file_time_type buildStart);
    // PDB-Name aus dem CodeView-Eintrag der DLL (nur Dateiname), leer wenn keiner.
    static std::filesystem::path ReferencedPdb(const std::filesystem::path& library);

    static std::filesystem::path LibraryPath(const std::filesystem::path& buildDir,
                                             const std::string& projectTarget);
    static bool CheckEngineLibraries(const std::filesystem::path& buildDir,
                                    const std::filesystem::path& editorExe, std::string& error);
    // Header, die Editor-Objekte und Script-DLL gemeinsam einbinden und die neuer als
    // die Editor-EXE sind (Ausgaben von `ninja -t inputs <exe>` und `ninja -t deps`).
    static std::vector<std::filesystem::path> FindStaleSharedHeaders(
        std::string_view editorInputs, std::string_view deps, const std::string& scriptTarget,
        const std::filesystem::path& buildDir, const std::filesystem::path& sourceDir,
        std::filesystem::file_time_type editorTime);
    // Fragt Ninja ab (ohne Compiler-Umgebung). Fehlt Ninja, wird nicht geprueft.
    static bool CheckSharedEngineHeaders(const std::filesystem::path& ninja,
                                         const std::filesystem::path& buildDir,
                                         const std::filesystem::path& sourceDir,
                                         const std::filesystem::path& editorExe,
                                         const std::string& scriptTarget, std::string& error);
private:
    void* m_module = nullptr;
    std::filesystem::path m_copyDirectory;
    std::filesystem::path m_sourceLibrary;
    std::filesystem::file_time_type m_sourceTime{};
    std::filesystem::path m_builtLibrary;
    std::filesystem::file_time_type m_builtTime{};
    std::vector<std::filesystem::path> m_scriptFiles;
};
}
