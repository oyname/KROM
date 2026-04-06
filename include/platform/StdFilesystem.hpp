#pragma once
// =============================================================================
// KROM Engine - platform/StdFilesystem.hpp
// Produktions-IFilesystem via std::fstream + std::filesystem.
//
// Implementierung: src/platform/StdFilesystem.cpp  (NICHT header-only)
// Begründung: std::filesystem ist compile-zeitlich teuer. Eine header-only
// Implementierung würde die gesamten filesystem-Headers in jede includierte
// Translation Unit ziehen. Die Auslagerung in eine .cpp begrenzt das auf
// einen einzigen Compile-Unit.
// =============================================================================
#include "platform/IFilesystem.hpp"
#include <string>

namespace engine::platform {

class StdFilesystem final : public IFilesystem
{
public:
    [[nodiscard]] bool ReadFile (const char* path, std::vector<uint8_t>& out) override;
    [[nodiscard]] bool ReadText (const char* path, std::string& out)          override;
    [[nodiscard]] bool WriteFile(const char* path, const void* data, size_t size) override;
    [[nodiscard]] bool WriteText(const char* path, const std::string& text)   override;

    [[nodiscard]] bool      FileExists  (const char* path) const override;
    [[nodiscard]] FileStats GetFileStats(const char* path) const override;

    [[nodiscard]] std::string ResolveAssetPath(const char* relativePath) const override;
    void SetAssetRoot(const char* rootPath) override;

    [[nodiscard]] bool CreateDirectories(const char* path) override;
    void ListFiles(const char* directory, const char* filter,
                   std::vector<std::string>& outPaths) override;

private:
    std::string m_root;
};

} // namespace engine::platform
