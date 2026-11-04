#include "addons/editor/EditorPreferences.hpp"
#include "core/AtomicTextFile.hpp"
#include "serialization/SceneSerializer.hpp"

#include <algorithm>
#include <cmath>
#include <iterator>

namespace engine::renderer::addons::editor {
namespace {

EditorPreferences LoadPreferences()
{
    EditorPreferences result;
    const auto directory = EditorPreferencesDirectory();
    if (directory.empty()) return result;
    std::ifstream in(directory / "editor-preferences.json", std::ios::binary);
    if (!in) return result;
    const std::string json((std::istreambuf_iterator<char>(in)), {});
    std::string error;
    const auto root = serialization::JsonParser::Parse(json, error);
    if (!error.empty() || !root.IsObject()) return result;
    const auto flag = [&](const char* key) {
        const auto* value = root.Get(key);
        return value && value->IsBool() && value->AsBool();
    };
    const auto number = [&](const char* key, double fallback, double min, double max) {
        const auto* value = root.Get(key);
        return value && value->IsNumber() && std::isfinite(value->numVal)
            ? std::clamp(value->numVal, min, max) : fallback;
    };
    result.lightTheme = flag("lightTheme");
    result.reopenProject = flag("reopenProject");
    result.invertMouseY = flag("invertMouseY");
    result.uiScale = static_cast<float>(number("uiScale", 1, .8, 1.5));
    result.mouseSensitivity = static_cast<float>(number("mouseSensitivity", 1, .1, 4));
    result.gizmoScale = static_cast<float>(number("gizmoScale", 1, .5, 2));
    result.backupMinutes = static_cast<int>(number("backupMinutes", 0, 0, 120));
    if (const auto* project = root.Get("lastProject"); project && project->IsString())
        result.lastProject = project->AsString();
    if (const auto* keys = root.Get("shortcuts"); keys && keys->IsArray())
        for (size_t i = 0; i < result.shortcutKeys.size(); ++i)
            if (const auto& key = keys->At(i); key.IsNumber() && key.numVal >= 0 && key.numVal <= 10000)
                result.shortcutKeys[i] = key.AsInt();
    if (const auto* presets = root.Get("snapPresets"); presets && presets->IsArray())
        for (size_t i = 0; i < result.snapPresets.size(); ++i)
            for (size_t j = 0; j < result.snapPresets[i].size(); ++j)
                if (const auto& value = presets->At(i).At(j); value.IsNumber() && value.numVal > 0 && value.numVal <= 1000)
                    result.snapPresets[i][j] = value.AsFloat();
    return result;
}

EditorPreferences& CurrentPreferences()
{
    static EditorPreferences preferences = LoadPreferences();
    return preferences;
}

} // namespace

const EditorPreferences& GetEditorPreferences() { return CurrentPreferences(); }

bool SaveEditorPreferences(const EditorPreferences& preferences)
{
    const auto directory = EditorPreferencesDirectory();
    if (directory.empty()) return false;
    serialization::JsonWriter out;
    out.BeginObject();
    out.WriteBool("lightTheme", preferences.lightTheme);
    out.WriteFloat("uiScale", preferences.uiScale);
    out.WriteBool("reopenProject", preferences.reopenProject);
    out.WriteFloat("mouseSensitivity", preferences.mouseSensitivity);
    out.WriteBool("invertMouseY", preferences.invertMouseY);
    out.WriteFloat("gizmoScale", preferences.gizmoScale);
    out.WriteInt("backupMinutes", preferences.backupMinutes);
    out.WriteString("lastProject", preferences.lastProject);
    out.BeginArray("shortcuts");
    for (int key : preferences.shortcutKeys) out.WriteInt("", key);
    out.EndArray();
    out.BeginArray("snapPresets");
    for (const auto& preset : preferences.snapPresets)
    {
        out.BeginArray("");
        for (float value : preset) out.WriteFloat("", value);
        out.EndArray();
    }
    out.EndArray();
    out.EndObject();
    if (!WriteTextFileAtomically(directory / "editor-preferences.json", out.GetString()))
        return false;
    CurrentPreferences() = preferences;
    return true;
}

} // namespace engine::renderer::addons::editor
