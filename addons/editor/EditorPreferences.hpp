#pragma once

#include <array>
#include <filesystem>
#include <string>

namespace engine::renderer::addons::editor {

struct EditorPreferences
{
    bool lightTheme = false;
    float uiScale = 1.f;
    bool reopenProject = false;
    float mouseSensitivity = 1.f;
    bool invertMouseY = false;
    float gizmoScale = 1.f;
    int backupMinutes = 0;
    std::array<int, 6> shortcutKeys{};
    std::array<std::array<float, 3>, 2> snapPresets{{{.05f, 1.f, .1f}, {1.f, 45.f, .5f}}};
    std::string lastProject;
};

std::filesystem::path EditorPreferencesDirectory();
const EditorPreferences& GetEditorPreferences();
bool SaveEditorPreferences(const EditorPreferences& preferences);

} // namespace engine::renderer::addons::editor
