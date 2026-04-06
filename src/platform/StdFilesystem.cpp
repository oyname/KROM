// =============================================================================
// KROM Engine - src/platform/StdFilesystem.cpp
// Implementierung von StdFilesystem - fstream + std::filesystem.
// Einzige Translation Unit die std::filesystem includiert (by design).
// =============================================================================
#include "platform/StdFilesystem.hpp"
#include <fstream>
#include <filesystem>

namespace engine::platform {

bool StdFilesystem::ReadFile(const char* path, std::vector<uint8_t>& out)
{
    std::ifstream f(path ? path : "", std::ios::binary | std::ios::ate);
    if (!f) return false;
    const auto size = f.tellg();
    f.seekg(0);
    out.resize(static_cast<size_t>(size));
    f.read(reinterpret_cast<char*>(out.data()), size);
    return static_cast<bool>(f);
}

bool StdFilesystem::ReadText(const char* path, std::string& out)
{
    std::ifstream f(path ? path : "");
    if (!f) return false;
    out.assign(std::istreambuf_iterator<char>(f), {});
    return static_cast<bool>(f);
}

bool StdFilesystem::WriteFile(const char* path, const void* data, size_t size)
{
    std::ofstream f(path ? path : "", std::ios::binary);
    if (!f) return false;
    f.write(static_cast<const char*>(data), static_cast<std::streamsize>(size));
    return static_cast<bool>(f);
}

bool StdFilesystem::WriteText(const char* path, const std::string& text)
{
    return WriteFile(path, text.data(), text.size());
}

bool StdFilesystem::FileExists(const char* path) const
{
    return std::filesystem::exists(path ? path : "");
}

FileStats StdFilesystem::GetFileStats(const char* path) const
{
    std::error_code ec;
    const auto s = std::filesystem::status(path ? path : "", ec);
    if (ec || !std::filesystem::exists(s)) return {};

    FileStats fs;
    fs.exists      = true;
    fs.isDirectory = std::filesystem::is_directory(s);

    if (!fs.isDirectory)
        fs.sizeBytes = std::filesystem::file_size(path ? path : "", ec);

    const auto wt = std::filesystem::last_write_time(path ? path : "", ec);
    if (!ec)
        fs.lastModifiedTimestamp = static_cast<uint64_t>(wt.time_since_epoch().count());

    return fs;
}

std::string StdFilesystem::ResolveAssetPath(const char* relativePath) const
{
    return (std::filesystem::path(m_root) / (relativePath ? relativePath : "")).string();
}

void StdFilesystem::SetAssetRoot(const char* rootPath)
{
    m_root = rootPath ? rootPath : "";
}

bool StdFilesystem::CreateDirectories(const char* path)
{
    std::error_code ec;
    return std::filesystem::create_directories(path ? path : "", ec);
}

void StdFilesystem::ListFiles(const char* directory, const char* filter,
                               std::vector<std::string>& outPaths)
{
    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator(directory ? directory : "", ec))
    {
        if (!entry.is_regular_file()) continue;
        const auto p = entry.path().string();
        if (!filter || p.find(filter) != std::string::npos)
            outPaths.push_back(p);
    }
}

} // namespace engine::platform
