#pragma once

#include <filesystem>
#include <string>
#include <vector>

#include "addons/script/ScriptRegistry.hpp"
namespace engine::script { struct ScriptInstance; }

namespace engine::renderer::addons::editor {

struct EditorFrameContext;

// Liefert im Editor-Play/Pause den aktuellen Instanzwert fuer den Inspector.
bool ReadLiveEditorScriptField(const EditorFrameContext& ctx,
    const engine::script::ScriptInstance& instance,
    const engine::script::ScriptFieldMeta& field,
    engine::script::ScriptFieldValue& value);

struct EditorScriptAssetResult
{
    bool ok = false;
    std::string message;
    std::filesystem::path primaryPath;
    std::vector<std::string> scriptClassNames;
};

[[nodiscard]] bool IsValidCppScriptClassName(const std::string& name);

[[nodiscard]] EditorScriptAssetResult CreateCppScriptAsset(
    const EditorFrameContext& ctx,
    const std::filesystem::path& directory,
    const std::string& className);

[[nodiscard]] EditorScriptAssetResult GenerateCppScriptBindings(
    const EditorFrameContext& ctx);

[[nodiscard]] EditorScriptAssetResult RefreshCppScriptProject(
    const EditorFrameContext& ctx);

[[nodiscard]] std::vector<std::string> FindCppScriptClassNames(
    const EditorFrameContext& ctx);

// Implementierung neben dem Klassen-Header, sonst der Header selbst.
[[nodiscard]] std::filesystem::path FindCppScriptAssetPath(
    const EditorFrameContext& ctx,
    const std::string& className);

struct EditorScriptFieldMarker
{
    std::string name;
    engine::script::ScriptFieldType type = engine::script::ScriptFieldType::Float;
    engine::script::ScriptFieldValue defaultValue;
    bool hasDefaultValue = false;
};

[[nodiscard]] std::vector<EditorScriptFieldMarker> FindCppScriptFields(
    const EditorFrameContext& ctx,
    const std::string& className);

} // namespace engine::renderer::addons::editor
