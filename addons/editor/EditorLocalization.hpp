#pragma once

#include <string>
#include <string_view>

namespace engine::renderer::addons::editor {

enum class EditorLanguage { German, English };

EditorLanguage GetEditorLanguage();
bool SetEditorLanguage(EditorLanguage language);
const char* Translate(const char* key);
std::string TranslateLabel(const char* key, std::string_view legacyId);
std::string TranslateWithValue(const char* key, std::string_view value);

} // namespace engine::renderer::addons::editor
