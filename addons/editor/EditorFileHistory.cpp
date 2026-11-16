#include "addons/editor/EditorFileHistory.hpp"

#include "addons/editor/EditorAssetBrowser.hpp"
#include "addons/editor/EditorLocalization.hpp"
#include "addons/editor/EditorScriptAssets.hpp"
#include "addons/editor/EditorThumbnailCache.hpp"
#include "assets/AssetPipeline.hpp"
#include "core/AtomicTextFile.hpp"
#include "core/Debug.hpp"
#include "core/PathUtf8.hpp"
#include "serialization/SceneSerializer.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <system_error>
#include <unordered_set>
#include <utility>

namespace engine::renderer::addons::editor {
namespace {

namespace fs = std::filesystem;

std::string QuoteJson(std::string_view text)
{
    std::string out;
    out.reserve(text.size() + 2u);
    out += '"';
    for (const char ch : text)
    {
        switch (ch)
        {
        case '"':  out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        case '\b': out += "\\b"; break;
        case '\f': out += "\\f"; break;
        default:
            if (static_cast<unsigned char>(ch) < 0x20u)
            {
                char escaped[8];
                std::snprintf(escaped, sizeof(escaped), "\\u%04x", static_cast<unsigned>(static_cast<unsigned char>(ch)));
                out += escaped;
            }
            else
                out += ch;
        }
    }
    out += '"';
    return out;
}

bool LooksLikeJsonDocument(std::string_view text)
{
    const size_t first = text.find_first_not_of(" \t\r\n");
    return first != std::string_view::npos && (text[first] == '{' || text[first] == '[');
}

struct TextEdit
{
    size_t offset;
    size_t length;
    std::string text;
};

void CollectReferenceEdits(const serialization::JsonValue& value, const AssetPathRelocation& relocation,
                           std::vector<TextEdit>& edits)
{
    if (value.IsString())
    {
        if (auto replaced = RelocateAssetReference(value.AsString(), relocation))
            edits.push_back({value.stringOffset, value.stringLength, QuoteJson(*replaced)});
        else if (LooksLikeJsonDocument(value.AsString()))
        {
            if (auto nested = RelocateJsonAssetReferences(value.AsString(), relocation))
                edits.push_back({value.stringOffset, value.stringLength, QuoteJson(*nested)});
        }
        return;
    }
    for (const auto& item : value.arrayVal)
        CollectReferenceEdits(item, relocation, edits);
    for (const auto& [key, item] : value.objectVal)
        CollectReferenceEdits(item, relocation, edits);
}

fs::path EditorAssetRoot(const EditorFrameContext& ctx)
{
    const fs::path root = ctx.assetPipeline ? ctx.assetPipeline->GetAssetRoot() : fs::current_path() / "assets";
    std::error_code ec;
    const fs::path absolute = fs::absolute(root, ec);
    return (ec ? root : absolute).lexically_normal();
}

// Path below root, generic and UTF-8; nullopt outside root.
std::optional<std::string> RelativeBelow(const fs::path& root, const fs::path& path)
{
    std::error_code ec;
    const fs::path relative = fs::relative(path, root, ec);
    if (ec || relative.empty() || relative == "." || *relative.begin() == "..")
        return std::nullopt;
    return PathToGenericUtf8(relative);
}

// path itself or something inside it, mapped below replacement; nullopt otherwise.
std::optional<fs::path> MapBelow(const fs::path& path, const fs::path& from, const fs::path& to)
{
    if (path.empty())
        return std::nullopt;
    const fs::path relative = path.lexically_normal().lexically_relative(from.lexically_normal());
    if (relative.empty() || *relative.begin() == "..")
        return std::nullopt;
    return relative == "." ? to : (to / relative).lexically_normal();
}

bool IsInside(const fs::path& path, const fs::path& folder)
{
    return MapBelow(path, folder, folder).has_value();
}

bool IsReferenceFile(const fs::path& path)
{
    std::string ext = PathToUtf8(path.extension());
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return ext == ".json" || ext == ".kscene" || ext == ".mat" || ext == ".prefab" ||
           ext == ".animctrl" || ext == ".particle";
}

bool IsScriptSource(const fs::path& path)
{
    const std::string ext = PathToUtf8(path.extension());
    return ext == ".hpp" || ext == ".h" || ext == ".cpp" || ext == ".cxx";
}

bool ContainsScriptSource(const fs::path& path)
{
    std::error_code ec;
    if (!fs::is_directory(path, ec))
        return IsScriptSource(path);
    for (fs::recursive_directory_iterator it(path, fs::directory_options::skip_permission_denied, ec), end;
         !ec && it != end; it.increment(ec))
    {
        if (IsScriptSource(it->path()))
            return true;
    }
    return false;
}

bool ReadTextFile(const fs::path& path, std::string& text)
{
    std::ifstream input(path, std::ios::binary);
    if (!input)
        return false;
    text.assign(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
    return !input.bad();
}

// Rename; a different volume falls back to copy and delete.
bool MovePath(const fs::path& from, const fs::path& to, std::error_code& ec)
{
    fs::rename(from, to, ec);
    if (!ec)
        return true;
    if (ec != std::errc::cross_device_link)
        return false;
    std::error_code copyError;
    fs::copy(from, to, fs::copy_options::recursive, copyError);
    if (copyError)
    {
        std::error_code ignored;
        fs::remove_all(to, ignored);
        ec = copyError;
        return false;
    }
    fs::remove_all(from, ec);
    return !ec;
}

std::string ThumbnailRel(const EditorFrameContext& ctx, const fs::path& asset)
{
    if (ctx.currentProjectRoot.empty())
        return {};
    return RelativeBelow(PathFromUtf8(ctx.currentProjectRoot) / "Assets", asset).value_or(std::string{});
}

void MoveThumbnails(const EditorFrameContext& ctx, const fs::path& from, const fs::path& to, bool directory)
{
    if (ctx.currentProjectRoot.empty())
        return;
    const fs::path project = PathFromUtf8(ctx.currentProjectRoot);
    const auto move = [&](const fs::path& oldPath, const fs::path& newPath) {
        const std::string oldRel = ThumbnailRel(ctx, oldPath);
        const std::string newRel = ThumbnailRel(ctx, newPath);
        if (!oldRel.empty() && !newRel.empty())
            RenameCachedThumbnail(project, oldRel, newRel);
    };
    if (!directory)
    {
        move(from, to);
        return;
    }
    std::error_code ec;
    for (fs::recursive_directory_iterator it(to, fs::directory_options::skip_permission_denied, ec), end;
         !ec && it != end; it.increment(ec))
    {
        if (it->is_regular_file(ec))
            move(from / it->path().lexically_relative(to), it->path());
    }
}

void ForgetThumbnails(const EditorFrameContext& ctx, const std::vector<std::string>& relativePaths)
{
    if (ctx.currentProjectRoot.empty())
        return;
    const fs::path project = PathFromUtf8(ctx.currentProjectRoot);
    for (const auto& rel : relativePaths)
        RemoveCachedThumbnail(project, rel);
}

std::vector<std::string> CollectThumbnailPaths(const EditorFrameContext& ctx, const fs::path& path)
{
    std::vector<std::string> paths;
    std::error_code ec;
    if (!fs::is_directory(path, ec))
    {
        if (auto rel = ThumbnailRel(ctx, path); !rel.empty())
            paths.push_back(std::move(rel));
        return paths;
    }
    for (fs::recursive_directory_iterator it(path, fs::directory_options::skip_permission_denied, ec), end;
         !ec && it != end; it.increment(ec))
    {
        if (it->is_regular_file(ec))
            if (auto rel = ThumbnailRel(ctx, it->path()); !rel.empty())
                paths.push_back(std::move(rel));
    }
    return paths;
}

void RefreshScripts(EditorFrameContext& ctx)
{
    const EditorScriptAssetResult result = RefreshCppScriptProject(ctx);
    if (!result.message.empty())
        ctx.lastFileMessage = result.message;
}

// Browser and editor state that names the moved path, in both directions.
void UpdateLiveState(EditorFrameContext& ctx, const fs::path& from, const fs::path& to,
                     const AssetPathRelocation& relocation)
{
    if (!relocation.from.empty())
    {
        if (auto material = RelocateAssetReference(ctx.state.selectedMaterialAssetPath, relocation))
        {
            ctx.state.selectedMaterialAssetPath = *material;
            ctx.state.selectedMaterialAssetNameDraft = PathToUtf8(PathFromUtf8(*material).stem());
        }
        if (auto environment = RelocateAssetReference(ctx.state.environmentTexturePath, relocation))
            ctx.state.environmentTexturePath = *environment;
    }

    if (AssetBrowserState* browser = ctx.state.assetBrowser)
    {
        for (fs::path* path : {&browser->selectedPath, &browser->currentDir, &browser->highlightPath,
                               &browser->renamingPath})
        {
            if (auto mapped = MapBelow(*path, from, to))
                *path = *mapped;
        }
        std::unordered_set<std::string> expanded;
        for (const auto& key : browser->expandedDirs)
        {
            const auto mapped = MapBelow(PathFromUtf8(key), from, to);
            expanded.insert(mapped ? PathToUtf8(*mapped) : key);
        }
        browser->expandedDirs = std::move(expanded);
        browser->listedDir.clear();
    }
}

// Paths that disappeared: browser state falls back to the parent folder.
void ForgetLiveState(EditorFrameContext& ctx, const fs::path& removed)
{
    AssetBrowserState* browser = ctx.state.assetBrowser;
    if (!browser)
        return;
    for (fs::path* path : {&browser->selectedPath, &browser->highlightPath, &browser->renamingPath})
    {
        if (IsInside(*path, removed))
            path->clear();
    }
    if (IsInside(browser->currentDir, removed))
        browser->currentDir = removed.parent_path();
    for (auto it = browser->expandedDirs.begin(); it != browser->expandedDirs.end();)
        it = IsInside(PathFromUtf8(*it), removed) ? browser->expandedDirs.erase(it) : std::next(it);
    browser->listedDir.clear();
}

fs::path ClipSettingsPath(const fs::path& model)
{
    fs::path sidecar = model;
    sidecar += ".clips";
    return sidecar;
}

bool ApplyRelocation(EditorFrameContext& ctx, const fs::path& source, const fs::path& destination,
                     std::string& error)
{
    std::error_code ec;
    if (!fs::exists(source, ec))
    {
        error = Translate("history.failed_prefix") + PathToUtf8(source.filename());
        return false;
    }
    // Windows: a case-only rename names the same file.
    const bool sameFile = fs::exists(destination, ec) && fs::equivalent(source, destination, ec) && !ec;
    const bool directory = fs::is_directory(source, ec);

    std::vector<std::pair<fs::path, fs::path>> moves{{source, destination}};
    if (!directory)
    {
        const fs::path sidecar = ClipSettingsPath(source);
        if (fs::exists(sidecar, ec))
            moves.emplace_back(sidecar, ClipSettingsPath(destination));
    }
    for (const auto& [from, to] : moves)
    {
        if (fs::exists(to, ec) && !sameFile)
        {
            error = TranslateWithValue("common.rename_exists", PathToUtf8(to.filename()));
            return false;
        }
    }

    size_t moved = 0;
    const auto revertMoves = [&] {
        while (moved > 0)
        {
            const auto& [from, to] = moves[--moved];
            std::error_code revertError;
            if (!MovePath(to, from, revertError))
                Debug::LogError("Editor: moving %s back failed: %s", PathToUtf8(to).c_str(),
                                revertError.message().c_str());
        }
    };
    for (const auto& [from, to] : moves)
    {
        if (!MovePath(from, to, ec))
        {
            revertMoves();
            error = Translate("assets.move_failed") + ec.message();
            return false;
        }
        ++moved;
    }

    const fs::path root = EditorAssetRoot(ctx);
    AssetPathRelocation relocation;
    if (auto from = RelativeBelow(root, source))
        if (auto to = RelativeBelow(root, destination))
            relocation = {std::move(*from), std::move(*to), directory};

    if (!relocation.from.empty() && relocation.from != relocation.to)
    {
        std::vector<fs::path> candidates;
        for (fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, ec), end;
             !ec && it != end; it.increment(ec))
        {
            std::error_code typeError;
            if (it->is_regular_file(typeError) && IsReferenceFile(it->path()))
                candidates.push_back(it->path());
        }

        std::vector<std::pair<fs::path, std::string>> written;
        const auto revertWrites = [&] {
            for (auto it = written.rbegin(); it != written.rend(); ++it)
                if (!WriteTextFileAtomically(it->first, it->second))
                    Debug::LogError("Editor: restoring %s failed", PathToUtf8(it->first).c_str());
        };
        for (const fs::path& file : candidates)
        {
            std::string text;
            if (!ReadTextFile(file, text))
                continue;
            auto updated = RelocateJsonAssetReferences(text, relocation);
            if (!updated)
                continue;
            if (!WriteTextFileAtomically(file, *updated))
            {
                revertWrites();
                revertMoves();
                error = Translate("assets.move_failed") + PathToUtf8(file);
                return false;
            }
            written.emplace_back(file, std::move(text));
        }
    }

    for (const auto& [from, to] : moves)
        MoveThumbnails(ctx, from, to, directory && from == source);
    UpdateLiveState(ctx, source, destination, relocation);
    if (ContainsScriptSource(destination))
        RefreshScripts(ctx);
    return true;
}

std::atomic<uint64_t> g_trashCounter{0};

} // namespace

std::optional<std::string> RelocateAssetReference(std::string_view value, const AssetPathRelocation& relocation)
{
    if (relocation.from.empty() || value.size() < relocation.from.size())
        return std::nullopt;
    std::string normalized(value);
    std::replace(normalized.begin(), normalized.end(), '\\', '/');
    if (!normalized.starts_with(relocation.from))
        return std::nullopt;
    const std::string_view rest = std::string_view(normalized).substr(relocation.from.size());
    if (rest.empty() || rest.front() == '#' || (relocation.directory && rest.front() == '/'))
        return relocation.to + std::string(rest);
    return std::nullopt;
}

std::optional<std::string> RelocateJsonAssetReferences(const std::string& json, const AssetPathRelocation& relocation)
{
    if (relocation.from.empty())
        return std::nullopt;
    if (json.find(relocation.from) == std::string::npos)
    {
        // Stored with backslashes, escaped once per JSON level.
        std::string escaped;
        for (const char ch : relocation.from)
            escaped += ch == '/' ? std::string("\\\\") : std::string(1, ch);
        if (json.find(escaped) == std::string::npos)
            return std::nullopt;
    }
    std::string error;
    const auto root = serialization::JsonParser::Parse(json, error);
    if (!error.empty())
        return std::nullopt;
    std::vector<TextEdit> edits;
    CollectReferenceEdits(root, relocation, edits);
    if (edits.empty())
        return std::nullopt;
    std::sort(edits.begin(), edits.end(), [](const TextEdit& a, const TextEdit& b) { return a.offset > b.offset; });
    std::string result = json;
    for (const auto& edit : edits)
        result.replace(edit.offset, edit.length, edit.text);
    return result;
}

EditorTrash::EditorTrash(fs::path folder, std::vector<fs::path> originals)
    : m_folder(std::move(folder)), m_originals(std::move(originals)), m_present(m_originals.size(), false)
{
}

EditorTrash::~EditorTrash()
{
    std::error_code ec;
    fs::remove_all(m_folder, ec);
    if (ec)
        Debug::LogError("Editor: removing deleted files failed: %s", PathToUtf8(m_folder).c_str());
    fs::remove(m_folder.parent_path(), ec); // only succeeds once the trash root is empty
}

bool EditorTrash::Store(std::string& error)
{
    if (m_stored)
        return true;
    std::vector<size_t> moved;
    const auto stored = [&](size_t index) {
        return m_folder / std::to_string(index) / m_originals[index].filename();
    };
    const auto revert = [&] {
        for (auto it = moved.rbegin(); it != moved.rend(); ++it)
        {
            std::error_code ec;
            if (!MovePath(stored(*it), m_originals[*it], ec))
                Debug::LogError("Editor: restoring %s failed: %s", PathToUtf8(m_originals[*it]).c_str(),
                                ec.message().c_str());
        }
    };
    for (size_t index = 0; index < m_originals.size(); ++index)
    {
        std::error_code ec;
        m_present[index] = fs::exists(m_originals[index], ec);
        if (!m_present[index])
            continue;
        fs::create_directories(stored(index).parent_path(), ec);
        if (ec || !MovePath(m_originals[index], stored(index), ec))
        {
            revert();
            error = PathToUtf8(m_originals[index].filename()) + " - " + ec.message();
            return false;
        }
        moved.push_back(index);
    }
    m_stored = true;
    return true;
}

bool EditorTrash::Restore(std::string& error)
{
    if (!m_stored)
        return true;
    for (size_t index = 0; index < m_originals.size(); ++index)
    {
        std::error_code ec;
        if (m_present[index] && fs::exists(m_originals[index], ec))
        {
            error = TranslateWithValue("history.restore_exists", PathToUtf8(m_originals[index].filename()));
            return false;
        }
    }
    std::vector<size_t> moved;
    for (size_t index = 0; index < m_originals.size(); ++index)
    {
        if (!m_present[index])
            continue;
        const fs::path stored = m_folder / std::to_string(index) / m_originals[index].filename();
        std::error_code ec;
        fs::create_directories(m_originals[index].parent_path(), ec);
        if (!MovePath(stored, m_originals[index], ec))
        {
            for (auto it = moved.rbegin(); it != moved.rend(); ++it)
            {
                std::error_code revertError;
                MovePath(m_originals[*it], m_folder / std::to_string(*it) / m_originals[*it].filename(), revertError);
            }
            error = PathToUtf8(m_originals[index].filename()) + " - " + ec.message();
            return false;
        }
        moved.push_back(index);
    }
    m_stored = false;
    return true;
}

std::shared_ptr<EditorTrash> CreateEditorTrash(const EditorFrameContext& ctx, std::vector<fs::path> originals)
{
    const fs::path root = !ctx.currentProjectRoot.empty()
        ? PathFromUtf8(ctx.currentProjectRoot) / "editor" / ".history-trash"
        : EditorAssetRoot(ctx).parent_path() / ".krom-history-trash";
    const auto stamp = std::chrono::system_clock::now().time_since_epoch().count();
    const std::string name = std::to_string(stamp) + "-" + std::to_string(g_trashCounter.fetch_add(1));
    return std::make_shared<EditorTrash>(root / name, std::move(originals));
}

bool RecordEditorFileOperation(EditorFrameContext& ctx, const char* label, EditorHistoryOperation operation)
{
    if (AddEditorTransactionOperation(ctx, operation))
        return true;
    if (!BeginEditorTransaction(ctx, label))
        return false;
    AddEditorTransactionOperation(ctx, std::move(operation));
    return CommitEditorTransaction(ctx);
}

bool MoveEditorAssetPath(EditorFrameContext& ctx, const fs::path& source, const fs::path& destination,
                         const char* label)
{
    if (source.empty() || destination.empty())
        return false;
    const fs::path from = source.lexically_normal();
    const fs::path to = destination.lexically_normal();
    if (from == to)
        return false;
    std::error_code ec;
    if (fs::is_directory(from, ec) && IsInside(to, from))
    {
        ctx.lastFileMessage = Translate("assets.move_self");
        return false;
    }

    const bool ownTransaction = BeginEditorTransaction(ctx, label);
    std::string error;
    if (!ApplyRelocation(ctx, from, to, error))
    {
        if (ownTransaction)
            CommitEditorTransaction(ctx);
        ctx.lastFileMessage = error;
        Debug::LogError("Editor: moving %s failed: %s", PathToUtf8(from).c_str(), error.c_str());
        return false;
    }

    AddEditorTransactionOperation(ctx, EditorHistoryOperation{
        [from, to](EditorFrameContext& context) {
            std::string message;
            if (ApplyRelocation(context, to, from, message))
                return true;
            context.lastFileMessage = message;
            return false;
        },
        [from, to](EditorFrameContext& context) {
            std::string message;
            if (ApplyRelocation(context, from, to, message))
                return true;
            context.lastFileMessage = message;
            return false;
        }});

    const fs::path root = EditorAssetRoot(ctx);
    const auto fromRel = RelativeBelow(root, from);
    const auto toRel = RelativeBelow(root, to);
    if (fromRel && toRel && *fromRel != *toRel)
    {
        const AssetPathRelocation relocation{*fromRel, *toRel, fs::is_directory(to, ec)};
        RewriteEditorComponents(ctx, label, [&relocation](const std::string& json) {
            return RelocateJsonAssetReferences(json, relocation);
        });
    }
    if (ownTransaction)
        CommitEditorTransaction(ctx);
    return true;
}

bool DeleteEditorAssetPath(EditorFrameContext& ctx, const fs::path& path)
{
    const fs::path target = path.lexically_normal();
    std::error_code ec;
    if (target.empty() || !fs::exists(target, ec))
        return false;
    std::vector<fs::path> originals{target};
    if (!fs::is_directory(target, ec) && fs::exists(ClipSettingsPath(target), ec))
        originals.push_back(ClipSettingsPath(target));

    const bool scripts = ContainsScriptSource(target);
    const std::vector<std::string> thumbnails = CollectThumbnailPaths(ctx, target);
    auto trash = CreateEditorTrash(ctx, std::move(originals));
    std::string error;
    if (!trash->Store(error))
    {
        ctx.lastFileMessage = Translate("common.delete_failed_prefix") + error;
        return false;
    }
    ForgetThumbnails(ctx, thumbnails);
    ForgetLiveState(ctx, target);
    if (scripts)
        RefreshScripts(ctx);

    RecordEditorFileOperation(ctx, "history.delete_asset", EditorHistoryOperation{
        [trash, scripts](EditorFrameContext& context) {
            std::string message;
            if (!trash->Restore(message))
            {
                context.lastFileMessage = message;
                return false;
            }
            if (context.state.assetBrowser)
                context.state.assetBrowser->listedDir.clear();
            if (scripts)
                RefreshScripts(context);
            return true;
        },
        [trash, target, scripts](EditorFrameContext& context) {
            std::string message;
            if (!trash->Store(message))
            {
                context.lastFileMessage = Translate("common.delete_failed_prefix") + message;
                return false;
            }
            ForgetLiveState(context, target);
            if (scripts)
                RefreshScripts(context);
            return true;
        }});
    return true;
}

} // namespace engine::renderer::addons::editor
