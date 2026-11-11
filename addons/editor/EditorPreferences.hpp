#pragma once

#include <array>
#include <filesystem>
#include <string>

namespace engine::renderer::addons::editor {

struct EditorPreferences
{
    bool lightTheme = false;
    float uiScale = 1.f;
    // Base font size in pixels before UI scale (editor default 16).
    float fontSize = 16.f;
    bool reopenProject = false;
    float mouseSensitivity = 1.f;
    bool invertMouseY = false;
    float gizmoScale = 1.f;
    int backupMinutes = 0;
    // Material and animation editor write every change at once. Off: only the
    // Save button writes, and leaving unsaved changes asks first.
    bool autoSaveEditors = true;
    std::array<int, 6> shortcutKeys{};
    std::array<std::array<float, 3>, 2> snapPresets{{{.05f, 1.f, .1f}, {1.f, 45.f, .5f}}};
    std::string lastProject;
    // Machine-specific tool overrides (UTF-8 paths); empty = automatic discovery.
    // Stored per user, never in the project's editor.settings.
    std::string toolVisualStudio;
    std::string toolCMake;
    std::string toolNinja;
    std::string toolDxc;
    std::string toolGlslang;
};

std::filesystem::path EditorPreferencesDirectory();
const EditorPreferences& GetEditorPreferences();
bool SaveEditorPreferences(const EditorPreferences& preferences);
// Changes the auto-save choice for this session only, without writing the file (tests).
void SetAutoSaveEditorsForSession(bool enabled);
// Hands the shader compiler overrides to the renderer. Called on load and save.
void ApplyEditorToolPreferences(const EditorPreferences& preferences);

} // namespace engine::renderer::addons::editor
