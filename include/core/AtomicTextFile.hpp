#pragma once

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string_view>

namespace engine {

inline bool WriteTextFileAtomically(const std::filesystem::path& path, std::string_view text)
{
    if (path.empty())
        return false;
    std::error_code error;
    if (path.has_parent_path())
        std::filesystem::create_directories(path.parent_path(), error);
    if (error)
        return false;

    auto pending = path;
    pending += "." + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".tmp";
    std::ofstream out(pending, std::ios::binary | std::ios::trunc);
    out << text;
    out.close();
    if (out)
    {
        std::filesystem::rename(pending, path, error);
        if (!error)
            return true;
    }
    std::filesystem::remove(pending, error);
    return false;
}

} // namespace engine
