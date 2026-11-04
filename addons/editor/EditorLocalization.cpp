#include "addons/editor/EditorLocalization.hpp"

#include "EditorCatalogs.hpp"
#include "core/PathUtf8.hpp"
#include "serialization/SceneSerializer.hpp"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>

namespace engine::renderer::addons::editor {
namespace {

using Catalog = std::map<std::string, std::string, std::less<>>;

Catalog ReadCatalog(const char* json)
{
    std::string error;
    auto root = serialization::JsonParser::Parse(json, error);
    Catalog catalog;
    if (error.empty() && root.IsObject())
        for (auto& [key, value] : root.objectVal)
            if (value.IsString() && !value.strVal.empty())
                catalog.emplace(std::move(key), std::move(value.strVal));
    return catalog;
}

std::filesystem::path PreferenceFile()
{
#ifdef _WIN32
    if (const wchar_t* appData = _wgetenv(L"LOCALAPPDATA"); appData && *appData)
        return std::filesystem::path(appData) / "KROM" / "editor-language.txt";
#else
    if (const char* config = std::getenv("XDG_CONFIG_HOME"); config && *config)
    {
        const auto root = PathFromUtf8(config);
        if (root.is_absolute()) return root / "krom" / "editor-language.txt";
    }
    if (const char* home = std::getenv("HOME"); home && *home)
        return PathFromUtf8(home) / ".config" / "krom" / "editor-language.txt";
#endif
    return {};
}

std::atomic<EditorLanguage>& CurrentLanguage()
{
    static std::atomic<EditorLanguage> language = [] {
        const auto path = PreferenceFile();
        std::ifstream in(path);
        std::string code;
        return in >> code && code == "en" ? EditorLanguage::English : EditorLanguage::German;
    }();
    return language;
}

} // namespace

EditorLanguage GetEditorLanguage()
{
    return CurrentLanguage();
}

bool SetEditorLanguage(EditorLanguage language)
{
    if (language == CurrentLanguage()) return true;
    const auto path = PreferenceFile();
    if (path.empty()) return false;

    std::error_code error;
    std::filesystem::create_directories(path.parent_path(), error);
    if (error) return false;
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    auto pending = path;
    pending += "." + std::to_string(stamp) + ".tmp";
    std::ofstream out(pending, std::ios::trunc);
    out << (language == EditorLanguage::English ? "en\n" : "de\n");
    out.close();
    if (out) std::filesystem::rename(pending, path, error);
    if (!out || error)
    {
        std::filesystem::remove(pending, error);
        return false;
    }

    CurrentLanguage() = language;
    return true;
}

const char* Translate(const char* key)
{
    static const Catalog english = ReadCatalog(kEnglishCatalog);
    static const Catalog german = ReadCatalog(kGermanCatalog);
    if (GetEditorLanguage() == EditorLanguage::German)
        if (const auto it = german.find(key); it != german.end())
            return it->second.c_str();
    const auto it = english.find(key);
    return it != english.end() ? it->second.c_str() : key;
}

std::string TranslateWithValue(const char* key, std::string_view value)
{
    std::string text = Translate(key);
    constexpr std::string_view placeholder = "{value}";
    const auto offset = text.find(placeholder);
    if (offset != std::string::npos)
        text.replace(offset, placeholder.size(), value);
    return text;
}

std::string TranslateLabel(const char* key, std::string_view legacyId)
{
    // ImGui hashes only the part after ###. Keep existing docking and widget IDs.
    std::string label = Translate(key);
    label += "###";
    label += legacyId;
    return label;
}

} // namespace engine::renderer::addons::editor
