#include "addons/editor/EditorScriptAssets.hpp"
#include "addons/editor/EditorLocalization.hpp"
#include "core/PathUtf8.hpp"
#include "addons/editor/EditorFeature.hpp"
#include "addons/script/ScriptList.hpp"
#include "ecs/Components.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <regex>
#include <set>
#include <sstream>
#include <string_view>
#include <cstdlib>
#include <system_error>
#include <unordered_map>
#include <vector>

namespace engine::renderer::addons::editor {

bool ReadLiveEditorScriptField(const EditorFrameContext& ctx,
    const engine::script::ScriptInstance& instance,
    const engine::script::ScriptFieldMeta& field,
    engine::script::ScriptFieldValue& value)
{
    if (!ctx.playActive || !ctx.scriptRegistry || !instance.script ||
        !ctx.scriptRegistry->ReadField(*instance.script, instance.className, field.name, value))
        return false;
    if (field.type == engine::script::ScriptFieldType::Entity)
    {
        value.stringValue.clear();
        if (ctx.world.IsAlive(value.entityValue))
            if (const auto* guid = ctx.world.Get<GuidComponent>(value.entityValue))
                value.stringValue = guid->guid;
    }
    return true;
}

namespace {

std::filesystem::path ProjectRoot(const EditorFrameContext& ctx)
{
    return ctx.currentProjectRoot.empty()
        ? std::filesystem::path{}
        : PathFromUtf8(ctx.currentProjectRoot).lexically_normal();
}

std::string ToIncludePath(const std::filesystem::path& fromDirectory,
                          const std::filesystem::path& targetFile)
{
    std::error_code ec;
    std::filesystem::path rel = std::filesystem::relative(targetFile, fromDirectory, ec);
    if (ec || rel.empty())
        rel = targetFile;
    return PathToGenericUtf8(rel);
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

    // Unveraenderte Bindings behalten ihren Zeitstempel, sonst wuerde jeder
    // Start-Klick einen unnoetigen DLL-Build ausloesen.
    {
        std::ifstream existing(path, std::ios::binary);
        if (existing && std::string(std::istreambuf_iterator<char>(existing), {}) == text)
            return true;
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
        "    // Public variables show up in the inspector and are saved with the scene.\n"
        "    float speedDegPerSecond = 90.0f;\n"
        "\n"
        "    void OnStart(engine::script::IScriptContext& ctx) override;\n"
        "    void OnUpdate(engine::script::IScriptContext& ctx) override;\n"
        "};\n";
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
        "    // Weitere Helfer: ctx.Translate(), ctx.KeyDown(Krom::Key::W), ctx.Get<T>(), ctx.Destroy()\n"
        "    // Dauerkraefte (ctx.AddForce) besser in OnFixedUpdate — laeuft im festen Physiktakt.\n"
        "    ctx.RotateEulerDeg(0.0f, speedDegPerSecond * ctx.DeltaTime(), 0.0f);\n"
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

// ── Declaration parser ───────────────────────────────────────────────────────
// Reads script classes and their fields straight from the declarations, so a
// script needs no marker comments:
//   - a class at global scope that derives directly from ComponentScript is a script;
//   - each public data member of a supported type is a field, in declaration order;
//   - KROM_HIDE in front of a member keeps it out, KROM_PREFAB turns a
//     std::string into a prefab reference.
// This is a text scan, not a C++ front end: one member per declaration, no
// macros that expand to members, no type aliases.

// Blanks comments, string and character literals and preprocessor lines, keeping
// every offset and line break, so braces and semicolons can be matched by position.
std::string BlankNonCode(const std::string& text)
{
    std::string out = text;
    const size_t n = out.size();
    bool lineStart = true;
    for (size_t i = 0; i < n;)
    {
        const char c = out[i];
        if (c == '\n') { lineStart = true; ++i; continue; }
        if (lineStart && c == '#')
        {
            while (i < n && out[i] != '\n')
            {
                if (out[i] == '\\' && i + 1 < n && (out[i + 1] == '\n' || out[i + 1] == '\r'))
                {
                    out[i] = ' ';
                    i += out[i + 1] == '\r' ? 3 : 2;   // continued onto the next line
                    continue;
                }
                out[i++] = ' ';
            }
            continue;
        }
        if (!std::isspace(static_cast<unsigned char>(c)))
            lineStart = false;

        if (c == '/' && i + 1 < n && out[i + 1] == '/')
        {
            while (i < n && out[i] != '\n')
                out[i++] = ' ';
        }
        else if (c == '/' && i + 1 < n && out[i + 1] == '*')
        {
            out[i] = out[i + 1] = ' ';
            i += 2;
            while (i < n && !(out[i] == '*' && i + 1 < n && out[i + 1] == '/'))
            {
                if (out[i] != '\n') out[i] = ' ';
                ++i;
            }
            if (i < n) { out[i] = ' '; if (i + 1 < n) out[i + 1] = ' '; i += 2; }
        }
        else if (c == '"' || (c == '\'' && (i == 0 || !std::isalnum(static_cast<unsigned char>(out[i - 1])))))
        {
            const char quote = c;
            ++i;
            while (i < n && out[i] != quote && out[i] != '\n')
            {
                if (out[i] == '\\' && i + 1 < n && out[i + 1] != '\n')
                    out[i++] = ' ';
                out[i++] = ' ';
            }
            if (i < n && out[i] == quote) ++i;
        }
        else
        {
            ++i;
        }
    }
    return out;
}

bool IsIdentChar(char c)
{
    return std::isalnum(static_cast<unsigned char>(c)) || c == '_';
}

// Position of the bracket closing the one at `open`, or npos.
size_t MatchBracket(const std::string& code, size_t open, size_t end)
{
    const char openCh = code[open];
    const char closeCh = openCh == '(' ? ')' : openCh == '[' ? ']' : '}';
    int depth = 0;
    for (size_t i = open; i < end; ++i)
    {
        if (code[i] == openCh) ++depth;
        else if (code[i] == closeCh && --depth == 0) return i;
    }
    return std::string::npos;
}

// Whitespace-separated words; "::" joins its neighbours ("std :: string" is one word).
std::vector<std::string> SplitWords(std::string_view text)
{
    std::vector<std::string> words;
    std::string current;
    bool joinNext = false;
    auto flush = [&] {
        if (!current.empty()) { words.push_back(std::move(current)); current.clear(); }
    };
    for (size_t i = 0; i < text.size(); ++i)
    {
        const char c = text[i];
        if (std::isspace(static_cast<unsigned char>(c)))
        {
            if (!joinNext) flush();
            continue;
        }
        if (c == ':' && i + 1 < text.size() && text[i + 1] == ':')
        {
            if (current.empty() && !words.empty()) { current = std::move(words.back()); words.pop_back(); }
            current += "::";
            ++i;
            joinNext = true;
            continue;
        }
        current += c;
        joinNext = false;
    }
    flush();
    return words;
}

bool HasWord(const std::vector<std::string>& words, std::string_view word)
{
    return std::find(words.begin(), words.end(), word) != words.end();
}

// Maps a C++ member type to the field type name used in the bindings; false if unsupported.
bool FieldTypeFromCppType(std::string_view cppType, bool prefab, std::string& outType)
{
    if (const size_t scope = cppType.rfind("::"); scope != std::string_view::npos)
    {
        const std::string_view qualifier = cppType.substr(0, scope);
        cppType = cppType.substr(scope + 2);
        // "string" from another namespace is not std::string.
        if (cppType == "string" && qualifier != "std" && qualifier != "::std")
            return false;
    }
    if (prefab)
    {
        if (cppType != "string") return false;
        outType = "Prefab";
        return true;
    }
    if (cppType == "float")                            outType = "Float";
    else if (cppType == "int" || cppType == "int32_t") outType = "Int";
    else if (cppType == "bool")                        outType = "Bool";
    else if (cppType == "Vec3")                        outType = "Vec3";
    else if (cppType == "string")                      outType = "String";
    else if (cppType == "EntityID" || cppType == "LPENTITY") outType = "Entity";   // LPENTITY: krom.h alias
    else return false;
    return true;
}

// One member declaration without its initializer ("engine::math::Vec3 axis").
bool ParseFieldDeclarator(std::string_view declarator, ScriptMarker::Field& outField)
{
    for (const char c : declarator)
    {
        if (!IsIdentChar(c) && c != ':' && !std::isspace(static_cast<unsigned char>(c)))
            return false;   // pointer, reference, array, template, function, attribute
    }

    std::vector<std::string> words = SplitWords(declarator);
    static constexpr std::string_view kNotAField[] = {
        "static", "const", "constexpr", "using", "typedef", "friend", "virtual", "explicit",
        "template", "enum", "class", "struct", "union", "operator", "volatile", "extern",
        "thread_local", "KROM_HIDE" };
    for (const std::string_view word : kNotAField)
        if (HasWord(words, word))
            return false;

    const bool prefab = HasWord(words, "KROM_PREFAB");
    std::erase_if(words, [](const std::string& word) {
        return word == "KROM_PREFAB" || word == "mutable" || word == "inline";
    });
    if (words.size() != 2u)
        return false;   // "unsigned int x", a bare expression, ...

    const std::string& name = words[1];
    if (name.find(':') != std::string::npos || std::isdigit(static_cast<unsigned char>(name.front())))
        return false;   // bit field or a qualified name
    if (!FieldTypeFromCppType(words[0], prefab, outField.type))
        return false;
    outField.name = name;
    return true;
}

// Public data members of the class body code[bodyBegin, bodyEnd) (without the braces).
void ParseClassFields(const std::string& code, size_t bodyBegin, size_t bodyEnd,
                      bool isStruct, std::vector<ScriptMarker::Field>& outFields)
{
    bool isPublic = isStruct;
    size_t stmtBegin = bodyBegin;
    size_t initAt = std::string::npos;   // where "= ..." or "{ ... }" starts
    bool isFunction = false;             // "(" before any initializer
    bool hasTopComma = false;            // "float a, b;"

    auto reset = [&](size_t next) {
        stmtBegin = next;
        initAt = std::string::npos;
        isFunction = hasTopComma = false;
    };
    auto finish = [&](size_t stmtEnd) {
        if (!isPublic || isFunction || hasTopComma)
            return;
        const size_t declEnd = std::min(initAt, stmtEnd);
        ScriptMarker::Field field;
        if (ParseFieldDeclarator(std::string_view(code).substr(stmtBegin, declEnd - stmtBegin), field))
        {
            const bool known = std::any_of(outFields.begin(), outFields.end(),
                [&](const ScriptMarker::Field& f) { return f.name == field.name; });
            if (!known)
                outFields.push_back(std::move(field));
        }
    };

    for (size_t i = bodyBegin; i < bodyEnd;)
    {
        const char c = code[i];
        if (c == '(' || c == '[')
        {
            if (c == '(' && initAt == std::string::npos)
                isFunction = true;
            const size_t close = MatchBracket(code, i, bodyEnd);
            if (close == std::string::npos) return;
            i = close + 1;
        }
        else if (c == '{')
        {
            const size_t close = MatchBracket(code, i, bodyEnd);
            if (close == std::string::npos) return;
            if (isFunction)
                reset(close + 1);   // function body: the declaration ends here, without ';'
            else if (initAt == std::string::npos)
                initAt = i;         // brace initializer or the body of a nested type
            i = close + 1;
        }
        else if (c == '=')
        {
            if (initAt == std::string::npos)
            {
                // "operator==" is a function name, not an initializer.
                const std::string_view head = std::string_view(code).substr(stmtBegin, i - stmtBegin);
                if (head.find("operator") == std::string_view::npos)
                    initAt = i;
            }
            ++i;
        }
        else if (c == ',')
        {
            hasTopComma = true;
            ++i;
        }
        else if (c == ':')
        {
            if (i + 1 < bodyEnd && code[i + 1] == ':') { i += 2; continue; }
            const std::vector<std::string> words =
                SplitWords(std::string_view(code).substr(stmtBegin, i - stmtBegin));
            if (words.size() == 1u && (words[0] == "public" || words[0] == "private" || words[0] == "protected"))
            {
                isPublic = words[0] == "public";
                reset(i + 1);
            }
            ++i;
        }
        else if (c == ';')
        {
            finish(i);
            reset(i + 1);
            ++i;
        }
        else
        {
            ++i;
        }
    }
}

struct ParsedClass
{
    std::string name;
    std::vector<ScriptMarker::Field> fields;
};

// Script classes declared at global scope in `text`.
std::vector<ParsedClass> ParseScriptClasses(const std::string& text)
{
    std::vector<ParsedClass> classes;
    const std::string code = BlankNonCode(text);
    const size_t n = code.size();
    size_t stmtBegin = 0;   // start of the current global-scope declaration

    for (size_t i = 0; i < n;)
    {
        const char c = code[i];
        if (c == ';' || c == '}') { stmtBegin = ++i; continue; }
        if (c == '{' || c == '(' || c == '[')
        {
            // Namespaces, function bodies, initializers: nothing in there is registered
            // (the bindings refer to classes by their unqualified name).
            const size_t close = MatchBracket(code, i, n);
            if (close == std::string::npos) break;
            i = close + 1;
            if (c == '{') stmtBegin = i;
            continue;
        }
        if (!IsIdentChar(c)) { ++i; continue; }

        size_t wordEnd = i;
        while (wordEnd < n && IsIdentChar(code[wordEnd])) ++wordEnd;
        const std::string_view word = std::string_view(code).substr(i, wordEnd - i);
        if (word != "class" && word != "struct")
        {
            i = wordEnd;
            continue;
        }

        const size_t headEnd = code.find_first_of("{;", wordEnd);
        if (headEnd == std::string::npos) break;
        const std::string_view before = std::string_view(code).substr(stmtBegin, i - stmtBegin);
        const bool plainClass = code[headEnd] == '{' &&
            before.find("template") == std::string_view::npos &&
            before.find("enum") == std::string_view::npos;
        if (!plainClass)
        {
            i = wordEnd;   // forward declaration, template or "enum class": plain code
            continue;
        }

        const size_t bodyEnd = MatchBracket(code, headEnd, n);
        if (bodyEnd == std::string::npos) break;

        // "Name final : public engine::script::ComponentScript"
        const std::string_view head = std::string_view(code).substr(wordEnd, headEnd - wordEnd);
        size_t colon = std::string_view::npos;
        for (size_t k = 0; k < head.size(); ++k)
        {
            if (head[k] != ':') continue;
            if (k + 1 < head.size() && head[k + 1] == ':') { ++k; continue; }
            colon = k;
            break;
        }
        if (colon != std::string_view::npos)
        {
            std::vector<std::string> nameWords = SplitWords(head.substr(0, colon));
            std::erase(nameWords, "final");
            bool derivesFromScript = false;
            for (const std::string& base : SplitWords(head.substr(colon + 1)))
            {
                std::string_view baseName = base;
                if (!baseName.empty() && baseName.back() == ',') baseName.remove_suffix(1);
                if (const size_t scope = baseName.rfind("::"); scope != std::string_view::npos)
                    baseName = baseName.substr(scope + 2);
                derivesFromScript |= baseName == "ComponentScript";
            }
            if (derivesFromScript && !nameWords.empty() && IsValidCppScriptClassName(nameWords.back()))
            {
                ParsedClass parsed;
                parsed.name = nameWords.back();
                ParseClassFields(code, headEnd + 1, bodyEnd, word == "struct", parsed.fields);
                classes.push_back(std::move(parsed));
            }
        }
        i = bodyEnd + 1;
        stmtBegin = i;
    }
    return classes;
}

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

    // Classes without a marker comment are read from their declaration. A class
    // that has markers keeps exactly the fields its markers list.
    for (ParsedClass& parsed : ParseScriptClasses(text))
    {
        const bool hasMarker = std::any_of(markers.begin(), markers.end(),
            [&](const ScriptMarker& marker) { return marker.className == parsed.name; });
        if (hasMarker)
            continue;
        ScriptMarker marker{ parsed.name, file };
        marker.fields = std::move(parsed.fields);
        markers.push_back(std::move(marker));
    }
}

struct ParsedScriptFile
{
    std::filesystem::file_time_type writeTime{};
    std::vector<ScriptMarker> markers;
    bool valid = false;
};

std::unordered_map<std::string, ParsedScriptFile>& ParsedScriptFiles()
{
    static std::unordered_map<std::string, ParsedScriptFile> files;
    return files;
}

// Wird beim Scan gesetzt, wenn eine Datei neu ausgewertet wurde.
bool& ScriptMarkersChanged()
{
    static bool changed = true;
    return changed;
}

std::vector<ScriptMarker> ScanScriptMarkers(const EditorFrameContext& ctx)
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
            const std::string ext = PathToUtf8(path.extension());
            if (ext != ".hpp" && ext != ".h")
                continue;

            // Regex-Auswertung ist teuer (Debug ~ms pro Datei): nur geaenderte Dateien neu lesen.
            const auto writeTime = std::filesystem::last_write_time(path, ec);
            const std::string fileKey = PathToGenericUtf8(path);
            ParsedScriptFile& parsed = ParsedScriptFiles()[fileKey];
            if (ec || !parsed.valid || parsed.writeTime != writeTime)
            {
                parsed.markers.clear();
                ScanFileForMarkers(path, parsed.markers);
                parsed.writeTime = writeTime;
                parsed.valid = !ec;
                ScriptMarkersChanged() = true;
            }
            std::vector<ScriptMarker> fileMarkers = parsed.markers;
            for (auto& marker : fileMarkers)
            {
                const std::string key = marker.className + "|" + PathToUtf8(marker.headerPath);
                if (seen.insert(key).second)
                    markers.push_back(std::move(marker));
            }
        }
    }

    std::sort(markers.begin(), markers.end(),
        [](const ScriptMarker& a, const ScriptMarker& b) {
            if (a.className != b.className)
                return a.className < b.className;
            return PathToGenericUtf8(a.headerPath) < PathToGenericUtf8(b.headerPath);
        });
    return markers;
}

// Der Inspector fragt Klassen und Felder in jedem Frame ab; ein Scan liest alle
// Script-Header von der Platte (im Debug-Build ~50 ms). Daher hoechstens einmal pro
// Sekunde neu abgleichen (nur Zeitstempel, geaenderte Dateien werden neu gelesen);
// Aenderungen an Scripts erscheinen so trotzdem von selbst.
struct ScriptMarkerCache
{
    std::filesystem::path root;
    std::chrono::steady_clock::time_point scannedAt{};
    std::vector<ScriptMarker> markers;
    std::unordered_map<std::string, std::vector<EditorScriptFieldMarker>> fieldsByClass;
    bool valid = false;
};

ScriptMarkerCache& MarkerCache()
{
    static ScriptMarkerCache cache;
    return cache;
}

void StoreScriptMarkers(const std::filesystem::path& root, std::vector<ScriptMarker> markers)
{
    ScriptMarkerCache& cache = MarkerCache();
    // Feld-Standardwerte (eigene Regex-Auswertung) nur verwerfen, wenn sich etwas geaendert hat.
    const bool changed = ScriptMarkersChanged() || cache.root != root ||
        cache.markers.size() != markers.size();
    ScriptMarkersChanged() = false;
    cache.root = root;
    cache.scannedAt = std::chrono::steady_clock::now();
    cache.markers = std::move(markers);
    if (changed)
        cache.fieldsByClass.clear();
    cache.valid = true;
}

const std::vector<ScriptMarker>& FindScriptMarkers(const EditorFrameContext& ctx)
{
    constexpr auto kRescanInterval = std::chrono::seconds(1);
    ScriptMarkerCache& cache = MarkerCache();
    const std::filesystem::path root = ProjectRoot(ctx);
    if (!cache.valid || cache.root != root ||
        std::chrono::steady_clock::now() - cache.scannedAt >= kRescanInterval)
    {
        StoreScriptMarkers(root, ScanScriptMarkers(ctx));
    }
    return cache.markers;
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
    {
        // "axis{ 0.f, 1.f, 0.f }", "axis = { ... }", "axis = Vec3(...)"
        const std::string number = R"(\s*([+-]?(?:\d+(?:\.\d*)?|\.\d+))[fF]?\s*)";
        const std::regex rx(R"(\b)" + fieldName + R"(\s*(?:=\s*)?(?:[A-Za-z_][\w:]*\s*)?[{(])" +
                            number + "," + number + "," + number + R"([})])");
        if (!std::regex_search(text, match, rx)) return false;
        outValue.vec3Value = { std::stof(match[1].str()), std::stof(match[2].str()), std::stof(match[3].str()) };
        return true;
    }
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
        const std::filesystem::path headerPath = targetDir / PathFromUtf8(candidate + ".hpp");
        const std::filesystem::path sourcePath = targetDir / PathFromUtf8(candidate + ".cpp");
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
        result.message = Translate("project.no_project");
        return result;
    }
    if (!IsValidCppScriptClassName(className))
    {
        result.message = Translate("script.invalid_class");
        return result;
    }

    const std::filesystem::path targetDir = directory.empty()
        ? (root / "Assets" / "Scripts")
        : directory;
    const std::string finalClassName = MakeUniqueCppScriptClassName(targetDir, className);
    const std::filesystem::path headerPath = targetDir / PathFromUtf8(finalClassName + ".hpp");
    const std::filesystem::path sourcePath = targetDir / PathFromUtf8(finalClassName + ".cpp");

    std::string error;
    if (!WriteTextFile(headerPath, HeaderTemplate(finalClassName), error))
    {
        result.message = Translate("script.header_write_failed") + error;
        return result;
    }
    if (!WriteTextFile(sourcePath, SourceTemplate(finalClassName), error))
    {
        result.message = Translate("script.source_write_failed") + error;
        return result;
    }

    result = RefreshCppScriptProject(ctx);
    if (!result.ok)
        return result;

    result.primaryPath = headerPath;
    result.message = TranslateWithValue("script.created", finalClassName);
    return result;
}

EditorScriptAssetResult GenerateCppScriptBindings(const EditorFrameContext& ctx)
{
    EditorScriptAssetResult result{};
    const std::filesystem::path root = ProjectRoot(ctx);
    if (root.empty())
    {
        result.message = Translate("project.no_project");
        return result;
    }

    const std::filesystem::path generatedDir = root / "generated";
    const std::filesystem::path outputPath = generatedDir / "ScriptBindings.cpp";
    // Immer frisch scannen: gerade angelegte Scripts muessen in die Bindings.
    std::vector<ScriptMarker> markers = ScanScriptMarkers(ctx);
    StoreScriptMarkers(root, markers);

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
        result.message = Translate("script.bindings_write_failed") + error;
        return result;
    }

    result.ok = true;
    result.primaryPath = outputPath;
    result.message = TranslateWithValue("script.bindings_generated", std::to_string(result.scriptClassNames.size()));
    return result;
}

EditorScriptAssetResult RefreshCppScriptProject(const EditorFrameContext& ctx)
{
    EditorScriptAssetResult result = GenerateCppScriptBindings(ctx);
    if (!result.ok)
        return result;

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

std::vector<std::string> FindCppScriptClassNamesInFile(
    const EditorFrameContext& ctx,
    const std::filesystem::path& file)
{
    std::error_code ec;
    const std::string stem = PathToGenericUtf8(
        std::filesystem::absolute(file, ec).lexically_normal().replace_extension());

    std::vector<std::string> result;
    for (const ScriptMarker& marker : FindScriptMarkers(ctx))
    {
        const std::string markerStem = PathToGenericUtf8(
            std::filesystem::absolute(marker.headerPath, ec).lexically_normal().replace_extension());
        if (markerStem == stem && !marker.className.empty() &&
            std::find(result.begin(), result.end(), marker.className) == result.end())
        {
            result.push_back(marker.className);
        }
    }
    return result;
}

std::filesystem::path FindCppScriptAssetPath(
    const EditorFrameContext& ctx,
    const std::string& className)
{
    for (const ScriptMarker& marker : FindScriptMarkers(ctx))
    {
        if (marker.className != className)
            continue;
        std::error_code ec;
        if (!std::filesystem::is_regular_file(marker.headerPath, ec))
            continue;
        auto sourcePath = marker.headerPath;
        sourcePath.replace_extension(".cpp");
        if (!std::filesystem::is_regular_file(sourcePath, ec))
            sourcePath = marker.headerPath;
        auto absolutePath = std::filesystem::absolute(sourcePath, ec);
        if (!ec)
            return absolutePath.lexically_normal();
    }
    return {};
}

std::vector<EditorScriptFieldMarker> FindCppScriptFields(
    const EditorFrameContext& ctx,
    const std::string& className)
{
    const std::vector<ScriptMarker>& markers = FindScriptMarkers(ctx);
    ScriptMarkerCache& cache = MarkerCache();
    if (const auto cached = cache.fieldsByClass.find(className); cached != cache.fieldsByClass.end())
        return cached->second;

    std::vector<EditorScriptFieldMarker> result;
    std::set<std::string> seen;
    for (const ScriptMarker& marker : markers)
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
    cache.fieldsByClass.emplace(className, result);
    return result;
}

} // namespace engine::renderer::addons::editor
