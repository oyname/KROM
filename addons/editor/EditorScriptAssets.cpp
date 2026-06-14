#include "addons/editor/EditorScriptAssets.hpp"
#include "addons/editor/EditorFeature.hpp"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <regex>
#include <set>
#include <sstream>
#include <cstdlib>
#include <system_error>
#include <unordered_map>
#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace engine::renderer::addons::editor {
namespace {

std::filesystem::path ProjectRoot(const EditorFrameContext& ctx)
{
    return ctx.currentProjectRoot.empty()
        ? std::filesystem::path{}
        : std::filesystem::path(ctx.currentProjectRoot).lexically_normal();
}

std::string ToIncludePath(const std::filesystem::path& fromDirectory,
                          const std::filesystem::path& targetFile)
{
    std::error_code ec;
    std::filesystem::path rel = std::filesystem::relative(targetFile, fromDirectory, ec);
    if (ec || rel.empty())
        rel = targetFile;
    return rel.generic_string();
}

bool WriteTextFile(const std::filesystem::path& path,
                   const std::string& text,
                   std::string& outError)
{
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    if (ec)
    {
        outError = ec.message();
        return false;
    }

    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out)
    {
        outError = "file could not be opened";
        return false;
    }
    out << text;
    if (!out.good())
    {
        outError = "write failed";
        return false;
    }
    return true;
}

std::string QuoteCommandArg(const std::string& value)
{
    std::string out = "\"";
    for (char ch : value)
    {
        if (ch == '"')
            out += "\\\"";
        else
            out += ch;
    }
    out += "\"";
    return out;
}

std::string StripOuterQuotes(std::string value)
{
    while (value.size() >= 2u && value.front() == '"' && value.back() == '"')
        value = value.substr(1u, value.size() - 2u);
    return value;
}

#if defined(_WIN32)
std::string ShortWindowsPathOrOriginal(const std::string& value)
{
    const std::string preferred = std::filesystem::path(value).make_preferred().string();
    const DWORD required = GetShortPathNameA(preferred.c_str(), nullptr, 0u);
    if (required == 0u)
        return preferred;

    std::string result(required, '\0');
    const DWORD written = GetShortPathNameA(preferred.c_str(), result.data(), required);
    if (written == 0u || written >= required)
        return preferred;

    result.resize(written);
    return result;
}
#endif

bool ReconfigureCMakeForNewScripts(std::string& outError)
{
#if defined(KROM_SOURCE_DIR) && defined(KROM_BUILD_DIR) && defined(KROM_CMAKE_COMMAND)
    const std::string cmake = StripOuterQuotes(KROM_CMAKE_COMMAND);
    const std::string sourceDir = StripOuterQuotes(KROM_SOURCE_DIR);
    const std::string buildDir = StripOuterQuotes(KROM_BUILD_DIR);
#if defined(_WIN32)
    const std::string cmakeCommand = ShortWindowsPathOrOriginal(cmake);
    const std::string command = cmakeCommand + " -S " +
        QuoteCommandArg(sourceDir) + " -B " + QuoteCommandArg(buildDir) +
        " -DCMAKE_EXPORT_COMPILE_COMMANDS=ON";
#else
    const std::string args =
        QuoteCommandArg(cmake) +
        " -S " + QuoteCommandArg(sourceDir) +
        " -B " + QuoteCommandArg(buildDir) +
        " -DCMAKE_EXPORT_COMPILE_COMMANDS=ON";
    const std::string command = args;
#endif
    const int result = std::system(command.c_str());
    if (result == 0)
        return true;

    outError = "CMake-Reconfigure fehlgeschlagen (Exit " + std::to_string(result) + ").";
    return false;
#else
    outError = "CMake-Pfade sind im Editor-Build nicht verfuegbar.";
    return false;
#endif
}

std::string HeaderTemplate(const std::string& className)
{
    std::ostringstream out;
    out <<
        "#pragma once\n"
        "#include \"krom.h\"\n"
        "\n"
        "class " << className << " final : public engine::script::ComponentScript\n"
        "{\n"
        "public:\n"
        "    float speedDegPerSecond = 90.0f;\n"
        "\n"
        "    void OnStart(engine::script::IScriptContext& ctx) override;\n"
        "    void OnUpdate(engine::script::IScriptContext& ctx) override;\n"
        "};\n"
        "\n"
        "// KROM_SCRIPT(" << className << ")\n"
        "// KROM_FIELD(" << className << ", speedDegPerSecond, Float)\n";
    return out.str();
}

std::string SourceTemplate(const std::string& className)
{
    std::ostringstream out;
    out <<
        "#include \"" << className << ".hpp\"\n"
        "\n"
        "void " << className << "::OnStart(engine::script::IScriptContext&)\n"
        "{\n"
        "}\n"
        "\n"
        "void " << className << "::OnUpdate(engine::script::IScriptContext& ctx)\n"
        "{\n"
        "    (void)ctx;\n"
        "}\n";
    return out.str();
}

struct ScriptMarker
{
    std::string className;
    std::filesystem::path headerPath;
    struct Field
    {
        std::string name;
        std::string type;
    };
    std::vector<Field> fields;
};

void ScanFileForMarkers(const std::filesystem::path& file,
                        std::vector<ScriptMarker>& markers)
{
    std::ifstream in(file, std::ios::binary);
    if (!in)
        return;

    std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    static const std::regex markerRegex(R"(KROM_SCRIPT\s*\(\s*([A-Za-z_][A-Za-z0-9_]*)\s*\))");
    static const std::regex fieldRegex(R"(KROM_FIELD\s*\(\s*([A-Za-z_][A-Za-z0-9_]*)\s*,\s*([A-Za-z_][A-Za-z0-9_]*)\s*,\s*(Float|Int|Bool|Vec3|String|Prefab|Entity)\s*\))");

    for (std::sregex_iterator it(text.begin(), text.end(), markerRegex), end; it != end; ++it)
    {
        markers.push_back(ScriptMarker{ (*it)[1].str(), file });
    }
    for (std::sregex_iterator it(text.begin(), text.end(), fieldRegex), end; it != end; ++it)
    {
        const std::string className = (*it)[1].str();
        const std::string fieldName = (*it)[2].str();
        const std::string fieldType = (*it)[3].str();
        auto markerIt = std::find_if(markers.begin(), markers.end(),
            [&](const ScriptMarker& marker) {
                return marker.className == className && marker.headerPath == file;
            });
        if (markerIt == markers.end())
        {
            markers.push_back(ScriptMarker{ className, file });
            markerIt = std::prev(markers.end());
        }
        markerIt->fields.push_back({fieldName, fieldType});
    }
}

std::vector<ScriptMarker> FindScriptMarkers(const EditorFrameContext& ctx)
{
    std::vector<ScriptMarker> markers;
    const std::filesystem::path root = ProjectRoot(ctx);
    if (root.empty())
        return markers;

    const std::filesystem::path assetScripts = root / "Assets" / "Scripts";
    const std::filesystem::path rootScripts = root / "scripts";
    const std::filesystem::path roots[] = { assetScripts, rootScripts };

    std::set<std::string> seen;
    for (const auto& scanRoot : roots)
    {
        std::error_code ec;
        if (!std::filesystem::exists(scanRoot, ec))
            continue;

        for (const auto& entry : std::filesystem::recursive_directory_iterator(scanRoot, ec))
        {
            if (ec || entry.is_directory(ec))
                continue;
            const std::filesystem::path path = entry.path();
            const std::string ext = path.extension().string();
            if (ext != ".hpp" && ext != ".h")
                continue;

            std::vector<ScriptMarker> fileMarkers;
            ScanFileForMarkers(path, fileMarkers);
            for (auto& marker : fileMarkers)
            {
                const std::string key = marker.className + "|" + marker.headerPath.string();
                if (seen.insert(key).second)
                    markers.push_back(std::move(marker));
            }
        }
    }

    std::sort(markers.begin(), markers.end(),
        [](const ScriptMarker& a, const ScriptMarker& b) {
            if (a.className != b.className)
                return a.className < b.className;
            return a.headerPath.generic_string() < b.headerPath.generic_string();
        });
    return markers;
}

bool ScriptFieldTypeFromMarker(std::string_view typeName, engine::script::ScriptFieldType& outType)
{
    using engine::script::ScriptFieldType;
    if (typeName == "Float")  { outType = ScriptFieldType::Float; return true; }
    if (typeName == "Int")    { outType = ScriptFieldType::Int; return true; }
    if (typeName == "Bool")   { outType = ScriptFieldType::Bool; return true; }
    if (typeName == "Vec3")   { outType = ScriptFieldType::Vec3; return true; }
    if (typeName == "String") { outType = ScriptFieldType::String; return true; }
    if (typeName == "Prefab") { outType = ScriptFieldType::Prefab; return true; }
    if (typeName == "Entity") { outType = ScriptFieldType::Entity; return true; }
    return false;
}

bool ReadSimpleFieldDefault(const std::filesystem::path& headerPath,
                            const std::string& fieldName,
                            engine::script::ScriptFieldType type,
                            engine::script::ScriptFieldValue& outValue)
{
    std::ifstream in(headerPath, std::ios::binary);
    if (!in)
        return false;
    const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());

    const std::string ident = R"(\b)" + fieldName + R"(\s*=\s*)";
    std::smatch match;
    outValue.type = type;

    switch (type)
    {
    case engine::script::ScriptFieldType::Float:
    {
        const std::regex rx(ident + R"(([+-]?(?:\d+(?:\.\d*)?|\.\d+)(?:[fF])?))");
        if (!std::regex_search(text, match, rx)) return false;
        outValue.floatValue = std::stof(match[1].str());
        return true;
    }
    case engine::script::ScriptFieldType::Int:
    {
        const std::regex rx(ident + R"(([+-]?\d+))");
        if (!std::regex_search(text, match, rx)) return false;
        outValue.intValue = std::stoi(match[1].str());
        return true;
    }
    case engine::script::ScriptFieldType::Bool:
    {
        const std::regex rx(ident + R"((true|false))");
        if (!std::regex_search(text, match, rx)) return false;
        outValue.boolValue = match[1].str() == "true";
        return true;
    }
    case engine::script::ScriptFieldType::String:
    case engine::script::ScriptFieldType::Prefab:
    {
        const std::regex rx(ident + "\"([^\"]*)\"");
        if (!std::regex_search(text, match, rx)) return false;
        outValue.stringValue = match[1].str();
        return true;
    }
    case engine::script::ScriptFieldType::Vec3:
    case engine::script::ScriptFieldType::Entity:
        return false;
    }
    return false;
}

} // namespace

bool IsValidCppScriptClassName(const std::string& name)
{
    if (name.empty())
        return false;
    const unsigned char first = static_cast<unsigned char>(name.front());
    if (!(std::isalpha(first) || name.front() == '_'))
        return false;
    for (char ch : name)
    {
        const unsigned char c = static_cast<unsigned char>(ch);
        if (!(std::isalnum(c) || ch == '_'))
            return false;
    }
    return true;
}

std::string MakeUniqueCppScriptClassName(const std::filesystem::path& targetDir,
                                         const std::string& desiredClassName)
{
    std::string candidate = desiredClassName;
    for (uint32_t i = 2u; i < 10000u; ++i)
    {
        const std::filesystem::path headerPath = targetDir / (candidate + ".hpp");
        const std::filesystem::path sourcePath = targetDir / (candidate + ".cpp");
        if (!std::filesystem::exists(headerPath) && !std::filesystem::exists(sourcePath))
            return candidate;
        candidate = desiredClassName + std::to_string(i);
    }
    return desiredClassName + "9999";
}

EditorScriptAssetResult CreateCppScriptAsset(
    const EditorFrameContext& ctx,
    const std::filesystem::path& directory,
    const std::string& className)
{
    EditorScriptAssetResult result{};
    const std::filesystem::path root = ProjectRoot(ctx);
    if (root.empty())
    {
        result.message = "Kein Projekt geoeffnet.";
        return result;
    }
    if (!IsValidCppScriptClassName(className))
    {
        result.message = "Ungueltiger C++ Klassenname.";
        return result;
    }

    const std::filesystem::path targetDir = directory.empty()
        ? (root / "Assets" / "Scripts")
        : directory;
    const std::string finalClassName = MakeUniqueCppScriptClassName(targetDir, className);
    const std::filesystem::path headerPath = targetDir / (finalClassName + ".hpp");
    const std::filesystem::path sourcePath = targetDir / (finalClassName + ".cpp");

    std::string error;
    if (!WriteTextFile(headerPath, HeaderTemplate(finalClassName), error))
    {
        result.message = "Header konnte nicht geschrieben werden: " + error;
        return result;
    }
    if (!WriteTextFile(sourcePath, SourceTemplate(finalClassName), error))
    {
        result.message = "Source konnte nicht geschrieben werden: " + error;
        return result;
    }

    result = RefreshCppScriptProject(ctx);
    if (!result.ok)
        return result;

    result.primaryPath = headerPath;
    result.message = "C++ Script erstellt: " + finalClassName + " (CMake aktualisiert).";
    return result;
}

EditorScriptAssetResult GenerateCppScriptBindings(const EditorFrameContext& ctx)
{
    EditorScriptAssetResult result{};
    const std::filesystem::path root = ProjectRoot(ctx);
    if (root.empty())
    {
        result.message = "Kein Projekt geoeffnet.";
        return result;
    }

    const std::filesystem::path generatedDir = root / "generated";
    const std::filesystem::path outputPath = generatedDir / "ScriptBindings.cpp";
    const std::vector<ScriptMarker> markers = FindScriptMarkers(ctx);

    std::ostringstream out;
    out <<
        "// Generated by Krom Editor. Do not edit manually.\n"
        "#include \"addons/script/ScriptRegistry.hpp\"\n";

    std::set<std::filesystem::path> included;
    for (const ScriptMarker& marker : markers)
    {
        const std::filesystem::path normalized = marker.headerPath.lexically_normal();
        if (!included.insert(normalized).second)
            continue;
        out << "#include \"" << ToIncludePath(generatedDir, normalized) << "\"\n";
    }

    out <<
        "\n"
        "extern \"C\" void KromRegisterGameScripts(engine::script::ScriptRegistry& registry)\n"
        "{\n";

    std::set<std::string> registered;
    for (const ScriptMarker& marker : markers)
    {
        if (!registered.insert(marker.className).second)
            continue;
        out << "    registry.Register<" << marker.className << ">(\""
            << marker.className << "\");\n";

        std::vector<ScriptMarker::Field> classFields;
        std::set<std::string> fieldNames;
        for (const ScriptMarker& fieldMarker : markers)
        {
            if (fieldMarker.className != marker.className)
                continue;
            for (const auto& field : fieldMarker.fields)
            {
                if (fieldNames.insert(field.name).second)
                    classFields.push_back(field);
            }
        }

        for (const auto& field : classFields)
        {
            out << "    registry.RegisterField<" << marker.className << ">(\""
                << marker.className << "\", \"" << field.name
                << "\", engine::script::ScriptFieldType::" << field.type
                << ", &" << marker.className << "::" << field.name << ");\n";
        }
        result.scriptClassNames.push_back(marker.className);
    }

    out << "}\n";

    std::string error;
    if (!WriteTextFile(outputPath, out.str(), error))
    {
        result.message = "ScriptBindings.cpp konnte nicht geschrieben werden: " + error;
        return result;
    }

    result.ok = true;
    result.primaryPath = outputPath;
    result.message = "Script-Bindings generiert: " + std::to_string(result.scriptClassNames.size()) + " Script(s).";
    return result;
}

EditorScriptAssetResult RefreshCppScriptProject(const EditorFrameContext& ctx)
{
    EditorScriptAssetResult result = GenerateCppScriptBindings(ctx);
    if (!result.ok)
        return result;

    std::string reconfigureError;
    if (ReconfigureCMakeForNewScripts(reconfigureError))
    {
        result.message += " CMake aktualisiert.";
    }
    else
    {
        result.ok = false;
        result.message += " " + reconfigureError;
    }
    return result;
}

std::vector<std::string> FindCppScriptClassNames(const EditorFrameContext& ctx)
{
    std::vector<std::string> result;
    std::set<std::string> seen;
    for (const ScriptMarker& marker : FindScriptMarkers(ctx))
    {
        if (!marker.className.empty() && seen.insert(marker.className).second)
            result.push_back(marker.className);
    }
    std::sort(result.begin(), result.end());
    return result;
}

std::vector<EditorScriptFieldMarker> FindCppScriptFields(
    const EditorFrameContext& ctx,
    const std::string& className)
{
    std::vector<EditorScriptFieldMarker> result;
    std::set<std::string> seen;
    for (const ScriptMarker& marker : FindScriptMarkers(ctx))
    {
        if (marker.className != className)
            continue;
        for (const auto& field : marker.fields)
        {
            if (!seen.insert(field.name).second)
                continue;
            engine::script::ScriptFieldType type{};
            if (ScriptFieldTypeFromMarker(field.type, type))
            {
                EditorScriptFieldMarker outField{};
                outField.name = field.name;
                outField.type = type;
                outField.hasDefaultValue =
                    ReadSimpleFieldDefault(marker.headerPath, field.name, type, outField.defaultValue);
                result.push_back(std::move(outField));
            }
        }
    }
    return result;
}

} // namespace engine::renderer::addons::editor
