#pragma once

#include <chrono>
#include <ctime>
#include <filesystem>
#include <fstream>

namespace engine::renderer::addons::editor {

inline void RecordProjectOpened(const std::filesystem::path& projectRoot)
{
    std::error_code ec;
    const auto directory = projectRoot / "editor";
    std::filesystem::create_directories(directory, ec);
    if (ec) return;
    std::ofstream out(directory / "last-opened.txt", std::ios::trunc);
    if (out) out << std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
}

inline std::time_t ReadProjectLastOpened(const std::filesystem::path& projectRoot)
{
    std::ifstream in(projectRoot / "editor" / "last-opened.txt");
    std::time_t value = 0;
    if (!(in >> value) || value < 0) return 0;
    return value;
}

} // namespace engine::renderer::addons::editor
