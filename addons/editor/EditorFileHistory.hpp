#pragma once

#include "addons/editor/EditorCommands.hpp"

#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace engine::renderer::addons::editor {

// A file or folder that moves inside the asset root. Both paths are relative to
// the asset root, with forward slashes and UTF-8.
struct AssetPathRelocation
{
    std::string from;
    std::string to;
    bool directory = false;
};

// New value for a reference to the relocated path ("Models/a.glb", "Models/a.glb#mesh/0",
// for folders also "Models/sub/a.glb"); nullopt if value refers to something else.
[[nodiscard]] std::optional<std::string> RelocateAssetReference(std::string_view value,
                                                                const AssetPathRelocation& relocation);
// Rewrites references in the string values of a JSON document, including JSON
// documents stored as strings (prefab baselines). All other bytes stay unchanged.
// nullopt if the text is not JSON or holds no reference.
[[nodiscard]] std::optional<std::string> RelocateJsonAssetReferences(const std::string& json,
                                                                     const AssetPathRelocation& relocation);

// Files and folders moved aside for undo. While stored they live in a private
// folder under <project>/editor/.history-trash; when the last history entry that
// holds the trash is released, stored content is deleted for good.
class EditorTrash
{
public:
    EditorTrash(std::filesystem::path folder, std::vector<std::filesystem::path> originals);
    ~EditorTrash();
    EditorTrash(const EditorTrash&) = delete;
    EditorTrash& operator=(const EditorTrash&) = delete;

    // Moves every existing original into the trash; all or nothing.
    bool Store(std::string& error);
    // Moves the stored content back. Fails without changes if an original path is taken.
    bool Restore(std::string& error);

    [[nodiscard]] bool IsStored() const noexcept { return m_stored; }
    [[nodiscard]] const std::vector<std::filesystem::path>& Originals() const noexcept { return m_originals; }

private:
    std::filesystem::path m_folder;
    std::vector<std::filesystem::path> m_originals;
    std::vector<bool> m_present;
    bool m_stored = false;
};

[[nodiscard]] std::shared_ptr<EditorTrash> CreateEditorTrash(const EditorFrameContext& ctx,
                                                             std::vector<std::filesystem::path> originals);

// Adds operation to the open transaction or records it as its own history entry.
// Does nothing while history is replayed.
bool RecordEditorFileOperation(EditorFrameContext& ctx, const char* label, EditorHistoryOperation operation);

// Moves or renames a file or folder (with a model's .clips settings) as one undo step.
// References in scenes, prefabs, materials, controllers and in the open scene follow the
// new path. Fails without changes if the destination exists.
bool MoveEditorAssetPath(EditorFrameContext& ctx, const std::filesystem::path& source,
                         const std::filesystem::path& destination, const char* label = "history.move_asset");
// Deletes a file or folder; undo restores it from the trash.
bool DeleteEditorAssetPath(EditorFrameContext& ctx, const std::filesystem::path& path);

} // namespace engine::renderer::addons::editor
