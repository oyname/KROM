#pragma once
// =============================================================================
// KROM Engine - platform/IFilesystem.hpp
//
// Plattformneutrale Dateisystem-Abstraktion.
// Engine-Systeme (TextureLoader, ShaderCompiler, Serializer) rufen nur
// IFilesystem auf - kein direktes fopen/std::ifstream im Engine-Code.
//
// Implementierungen: StdFilesystem (fstream), NullFilesystem (in-memory/Tests)
// =============================================================================
#include <cstdint>
#include <string>
#include <vector>
#include <memory>

namespace engine::platform {

struct FileStats
{
    uint64_t sizeBytes             = 0u;
    uint64_t lastModifiedTimestamp = 0u; // plattformspezifischer Zeitstempel (für Hot-Reload)
    bool     exists                = false;
    bool     isDirectory           = false;
};

class IFilesystem
{
public:
    virtual ~IFilesystem() = default;

    [[nodiscard]] virtual bool ReadFile (const char* path, std::vector<uint8_t>& out) = 0;
    [[nodiscard]] virtual bool ReadText (const char* path, std::string& out) = 0;
    [[nodiscard]] virtual bool WriteFile(const char* path, const void* data, size_t size) = 0;
    [[nodiscard]] virtual bool WriteText(const char* path, const std::string& text) = 0;

    [[nodiscard]] virtual bool      FileExists  (const char* path) const = 0;
    [[nodiscard]] virtual FileStats GetFileStats(const char* path) const = 0;

    // Löst relativen Asset-Pfad auf absoluten Pfad auf
    [[nodiscard]] virtual std::string ResolveAssetPath(const char* relativePath) const = 0;
    virtual void SetAssetRoot(const char* rootPath) = 0;

    [[nodiscard]] virtual bool CreateDirectories(const char* path) = 0;
    virtual void ListFiles(const char* directory, const char* filter,
                            std::vector<std::string>& outPaths) = 0;
};

} // namespace engine::platform
