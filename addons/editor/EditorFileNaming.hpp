#pragma once

#include <filesystem>
#include <string>

namespace engine::renderer::addons::editor {

[[nodiscard]] std::filesystem::path MakeUniqueFilesystemPath(
    const std::filesystem::path& desiredPath);

[[nodiscard]] std::filesystem::path MakeUniqueFilesystemPath(
    const std::filesystem::path& directory,
    const std::string& stem,
    const std::string& extension);

} // namespace engine::renderer::addons::editor
