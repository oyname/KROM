#include "addons/editor/EditorFileNaming.hpp"
#include "core/PathUtf8.hpp"

#include <cstdint>
#include <system_error>

namespace engine::renderer::addons::editor {

std::filesystem::path MakeUniqueFilesystemPath(const std::filesystem::path& desiredPath)
{
    std::error_code ec;
    if (!std::filesystem::exists(desiredPath, ec))
        return desiredPath;

    const std::filesystem::path directory = desiredPath.parent_path();
    const std::string stem = PathToUtf8(desiredPath.stem());
    const std::string extension = PathToUtf8(desiredPath.extension());

    return MakeUniqueFilesystemPath(directory, stem, extension);
}

std::filesystem::path MakeUniqueFilesystemPath(const std::filesystem::path& directory,
                                               const std::string& stem,
                                               const std::string& extension)
{
    const std::string safeStem = stem.empty() ? std::string("Asset") : stem;
    const std::string ext = extension.empty() || extension.front() == '.'
        ? extension
        : "." + extension;

    std::filesystem::path candidate = directory / PathFromUtf8(safeStem + ext);
    std::error_code ec;
    if (!std::filesystem::exists(candidate, ec))
        return candidate;

    for (uint32_t i = 2u; i < 10000u; ++i)
    {
        candidate = directory / PathFromUtf8(safeStem + " (" + std::to_string(i) + ")" + ext);
        ec.clear();
        if (!std::filesystem::exists(candidate, ec))
            return candidate;
    }

    return directory / PathFromUtf8(safeStem + " (9999)" + ext);
}

} // namespace engine::renderer::addons::editor
