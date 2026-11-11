#include "addons/editor/EditorModelClips.hpp"

#include "addons/mesh_renderer/MeshRendererSerialization.hpp"
#include "assets/AssetPipeline.hpp"
#include "assets/AssetRegistry.hpp"
#include "core/AtomicTextFile.hpp"
#include "core/Logger.hpp"
#include "core/PathUtf8.hpp"
#include "serialization/SceneSerializer.hpp"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <unordered_map>

namespace engine::renderer::addons::editor {

namespace {

std::string ReadFile(const std::filesystem::path& path)
{
    std::ifstream in(path, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

// The JSON part of a .glb: 12-byte header, then the first chunk (length, type
// "JSON", data). Only that part is read, not the binary buffers behind it.
std::string ReadGlbJsonChunk(const std::filesystem::path& path)
{
    std::ifstream in(path, std::ios::binary);
    uint32_t header[5] = {};
    if (!in.read(reinterpret_cast<char*>(header), sizeof(header)))
        return {};
    constexpr uint32_t kMagic = 0x46546C67u;     // "glTF"
    constexpr uint32_t kJsonChunk = 0x4E4F534Au; // "JSON"
    const uint32_t length = header[3];
    if (header[0] != kMagic || header[4] != kJsonChunk || length > header[2])
        return {};
    std::string json(length, '\0');
    if (!in.read(json.data(), static_cast<std::streamsize>(length)))
        return {};
    return json;
}

std::filesystem::file_time_type Stamp(const std::filesystem::path& path)
{
    std::error_code ec;
    const auto stamp = std::filesystem::last_write_time(path, ec);
    return ec ? std::filesystem::file_time_type{} : stamp;
}

struct CacheEntry
{
    std::filesystem::file_time_type modelStamp;
    std::filesystem::file_time_type settingsStamp;
    ModelClipList list;
};

std::unordered_map<std::string, CacheEntry>& Cache()
{
    static std::unordered_map<std::string, CacheEntry> cache;
    return cache;
}

std::string CacheKey(const std::filesystem::path& assetRoot, const std::string& modelPath)
{
    return PathToGenericUtf8(assetRoot) + "|" + modelPath;
}

} // namespace

std::vector<std::string> ReadGltfAnimationNames(const std::filesystem::path& file)
{
    std::vector<std::string> names;
    std::string extension = PathToUtf8(file.extension());
    std::transform(extension.begin(), extension.end(), extension.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    const std::string json = extension == ".gltf" ? ReadFile(file) : ReadGlbJsonChunk(file);
    if (json.empty())
        return names;
    std::string error;
    const serialization::JsonValue root = serialization::JsonParser::Parse(json, error);
    const auto* animations = root.Get("animations");
    if (!error.empty() || !animations || !animations->IsArray())
        return names;
    for (const serialization::JsonValue& animation : animations->arrayVal)
    {
        const auto* name = animation.Get("name");
        // Same fallback as the glTF importer.
        names.push_back(name && name->IsString() && !name->AsString().empty() ? name->AsString() : std::string("animation"));
    }
    return names;
}

bool ModelClipList::SourceExists(const std::string& source) const
{
    return std::find(sources.begin(), sources.end(), source) != sources.end();
}

const animation::ModelClipEntry* ModelClipList::FindByAssetPath(const std::string& assetPath) const
{
    std::string model, id;
    if (!animation::SplitModelClipAssetPath(assetPath, model, id) || model != modelPath)
        return nullptr;
    return settings.Find(id);
}

const ModelClipList& GetModelClips(const std::filesystem::path& assetRoot, const std::string& modelPath)
{
    const std::filesystem::path modelFile = assetRoot / PathFromUtf8(modelPath);
    const std::filesystem::path settingsFile = assetRoot / PathFromUtf8(animation::ModelClipSettingsPath(modelPath));
    const auto modelStamp = Stamp(modelFile);
    const auto settingsStamp = Stamp(settingsFile);
    CacheEntry& entry = Cache()[CacheKey(assetRoot, modelPath)];
    if (entry.list.modelPath == modelPath && entry.modelStamp == modelStamp && entry.settingsStamp == settingsStamp)
        return entry.list;

    ModelClipList list;
    list.modelPath = modelPath;
    list.sources = ReadGltfAnimationNames(modelFile);
    std::error_code ec;
    if (std::filesystem::is_regular_file(settingsFile, ec) &&
        !animation::ParseModelClipSettings(ReadFile(settingsFile), list.settings, &list.error))
    {
        // Never overwrite a file the user may still fix by hand.
        Debug::LogWarning("EditorModelClips: could not read '%s': %s",
                          animation::ModelClipSettingsPath(modelPath).c_str(), list.error.c_str());
    }
    else
    {
        // Written once a clip id is used (PersistModelClips), not just by looking.
        list.unsaved = animation::SyncModelClipSettings(list.settings, list.sources);
    }
    entry.list = std::move(list);
    entry.modelStamp = modelStamp;
    entry.settingsStamp = Stamp(settingsFile);
    return entry.list;
}

bool PersistModelClips(const std::filesystem::path& assetRoot, const std::string& modelPath)
{
    const ModelClipList& list = GetModelClips(assetRoot, modelPath);
    if (!list.unsaved)
        return true;
    const animation::ModelClipSettings settings = list.settings;
    return SaveModelClipSettings(assetRoot, modelPath, settings);
}

bool SaveModelClipSettings(const std::filesystem::path& assetRoot, const std::string& modelPath,
                           const animation::ModelClipSettings& settings)
{
    const std::filesystem::path settingsFile = assetRoot / PathFromUtf8(animation::ModelClipSettingsPath(modelPath));
    if (WriteTextFileAtomically(settingsFile, animation::SerializeModelClipSettings(settings)))
    {
        // Updated in place: callers may still hold the list.
        if (const auto it = Cache().find(CacheKey(assetRoot, modelPath)); it != Cache().end() && it->second.list.modelPath == modelPath)
        {
            it->second.list.settings = settings;
            it->second.list.unsaved = false;
            it->second.list.error.clear();
            it->second.settingsStamp = Stamp(settingsFile);
        }
        return true;
    }
    Debug::LogWarning("EditorModelClips: could not write '%s'", animation::ModelClipSettingsPath(modelPath).c_str());
    return false;
}

void RefreshModelClipAssets(assets::AssetPipeline& pipeline, assets::AssetRegistry& registry,
                            const std::string& modelPath, const animation::ModelClipSettings& settings)
{
    // Re-import when the model was exported again since the last import.
    static std::unordered_map<std::string, std::filesystem::file_time_type> imported;
    const std::string key = CacheKey(pipeline.GetAssetRoot(), modelPath);
    const auto stamp = Stamp(pipeline.GetAssetRoot() / PathFromUtf8(modelPath));
    const bool registered = registry.FindAnimationByPath(modelPath + "#animation/0").IsValid();
    const auto known = imported.find(key);
    // Settings last registered per model: UI code may ask every frame.
    static std::unordered_map<std::string, animation::ModelClipSettings> registeredSettings;
    if (!registered || (known != imported.end() && known->second != stamp))
    {
        // The import itself registers the saved settings.
        if (!engine::addons::mesh_renderer::ImportModelAnimationAssets(pipeline, registry, modelPath + "#animation/0", registered))
            return;
        registeredSettings.erase(key);
    }
    imported[key] = stamp;
    if (const auto it = registeredSettings.find(key); it != registeredSettings.end() && it->second == settings)
        return;
    engine::addons::mesh_renderer::RegisterModelClips(registry, modelPath, settings);
    registeredSettings[key] = settings;
}

std::string ModelClipDisplayName(const std::filesystem::path& assetRoot, const std::string& assetPath)
{
    const size_t hash = assetPath.rfind('#');
    if (hash == std::string::npos || !animation::IsModelFilePath(std::string_view(assetPath).substr(0, hash)))
        return {};
    const std::string modelPath = assetPath.substr(0, hash);
    const ModelClipList& list = GetModelClips(assetRoot, modelPath);
    if (const auto* entry = list.FindByAssetPath(assetPath))
        return entry->name;
    constexpr std::string_view kLegacy = "#animation/";
    if (assetPath.compare(hash, kLegacy.size(), kLegacy) == 0)
    {
        const size_t index = static_cast<size_t>(std::strtoul(assetPath.c_str() + hash + kLegacy.size(), nullptr, 10));
        if (index < list.sources.size())
            return list.sources[index];
    }
    return {};
}

} // namespace engine::renderer::addons::editor
