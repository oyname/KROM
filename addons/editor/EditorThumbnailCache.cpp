// =============================================================================
// KROM Engine - addons/editor/EditorThumbnailCache.cpp
// Vorschaubilder auf der Platte (siehe Header).
// =============================================================================
#include "addons/editor/EditorThumbnailCache.hpp"

#include "core/PathUtf8.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>

namespace engine::renderer::addons::editor {
namespace {

constexpr char     kMagic[4] = {'K', 'T', 'H', 'B'};
constexpr uint32_t kVersion = 1u;
constexpr uint32_t kMaxDimension = 1024u;

struct ThumbnailHeader
{
    uint32_t    width = 0u;
    uint32_t    height = 0u;
    uint64_t    stamp = 0u;
    std::string assetRelPath;
};

uint64_t Fnv1a(const std::string& text)
{
    uint64_t hash = 14695981039346656037ull;
    for (const unsigned char c : text)
    {
        hash ^= c;
        hash *= 1099511628211ull;
    }
    return hash;
}

template<typename T>
void WritePod(std::ofstream& out, const T& value)
{
    out.write(reinterpret_cast<const char*>(&value), sizeof(T));
}

template<typename T>
bool ReadPod(std::ifstream& in, T& value)
{
    return static_cast<bool>(in.read(reinterpret_cast<char*>(&value), sizeof(T)));
}

// Liest den Kopf; die Pixel folgen direkt danach im Stream.
bool ReadHeader(std::ifstream& in, ThumbnailHeader& header)
{
    char magic[4] = {};
    uint32_t version = 0u;
    uint32_t pathLength = 0u;
    if (!in.read(magic, 4) || std::memcmp(magic, kMagic, 4) != 0 || !ReadPod(in, version) ||
        version != kVersion || !ReadPod(in, header.width) || !ReadPod(in, header.height) ||
        !ReadPod(in, header.stamp) || !ReadPod(in, pathLength) || pathLength > 4096u ||
        header.width == 0u || header.height == 0u || header.width > kMaxDimension || header.height > kMaxDimension)
    {
        return false;
    }
    header.assetRelPath.resize(pathLength);
    return pathLength == 0u || static_cast<bool>(in.read(header.assetRelPath.data(), pathLength));
}

bool WriteThumbnailFile(const std::filesystem::path& file, const ThumbnailHeader& header,
                        const std::vector<uint8_t>& rgba)
{
    std::error_code ec;
    std::filesystem::create_directories(file.parent_path(), ec);
    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    if (!out)
        return false;
    out.write(kMagic, 4);
    WritePod(out, kVersion);
    WritePod(out, header.width);
    WritePod(out, header.height);
    WritePod(out, header.stamp);
    WritePod(out, static_cast<uint32_t>(header.assetRelPath.size()));
    out.write(header.assetRelPath.data(), static_cast<std::streamsize>(header.assetRelPath.size()));
    out.write(reinterpret_cast<const char*>(rgba.data()), static_cast<std::streamsize>(rgba.size()));
    return static_cast<bool>(out);
}

} // namespace

std::filesystem::path ThumbnailCacheDir(const std::filesystem::path& projectRoot)
{
    return projectRoot / ".krom" / "thumbnails";
}

std::filesystem::path ThumbnailCacheFile(const std::filesystem::path& projectRoot, const std::string& assetRelPath)
{
    char name[32];
    std::snprintf(name, sizeof(name), "%016llx.kthumb", static_cast<unsigned long long>(Fnv1a(assetRelPath)));
    return ThumbnailCacheDir(projectRoot) / name;
}

bool SaveCachedThumbnail(const std::filesystem::path& projectRoot, const std::string& assetRelPath,
                         uint64_t assetStamp, uint32_t width, uint32_t height,
                         const std::vector<uint8_t>& rgba)
{
    if (projectRoot.empty() || assetRelPath.empty() || width == 0u || height == 0u ||
        width > kMaxDimension || height > kMaxDimension ||
        rgba.size() != static_cast<size_t>(width) * height * 4u)
    {
        return false;
    }
    ThumbnailHeader header{};
    header.width = width;
    header.height = height;
    header.stamp = assetStamp;
    header.assetRelPath = assetRelPath;
    return WriteThumbnailFile(ThumbnailCacheFile(projectRoot, assetRelPath), header, rgba);
}

bool LoadCachedThumbnail(const std::filesystem::path& projectRoot, const std::string& assetRelPath,
                         uint64_t assetStamp, uint32_t& outWidth, uint32_t& outHeight,
                         std::vector<uint8_t>& outRgba)
{
    if (projectRoot.empty() || assetRelPath.empty())
        return false;
    const std::filesystem::path file = ThumbnailCacheFile(projectRoot, assetRelPath);
    std::ifstream in(file, std::ios::binary);
    ThumbnailHeader header{};
    if (!in || !ReadHeader(in, header) || header.stamp != assetStamp || header.assetRelPath != assetRelPath)
        return false;
    outRgba.resize(static_cast<size_t>(header.width) * header.height * 4u);
    if (!in.read(reinterpret_cast<char*>(outRgba.data()), static_cast<std::streamsize>(outRgba.size())))
        return false;
    outWidth = header.width;
    outHeight = header.height;
    in.close();
    // Zuletzt benutzt (fuer die Obergrenze).
    std::error_code ec;
    std::filesystem::last_write_time(file, std::filesystem::file_time_type::clock::now(), ec);
    return true;
}

void RemoveCachedThumbnail(const std::filesystem::path& projectRoot, const std::string& assetRelPath)
{
    if (projectRoot.empty() || assetRelPath.empty())
        return;
    std::error_code ec;
    std::filesystem::remove(ThumbnailCacheFile(projectRoot, assetRelPath), ec);
}

void RenameCachedThumbnail(const std::filesystem::path& projectRoot, const std::string& oldRelPath,
                           const std::string& newRelPath)
{
    if (projectRoot.empty() || oldRelPath.empty() || newRelPath.empty() || oldRelPath == newRelPath)
        return;
    const std::filesystem::path oldFile = ThumbnailCacheFile(projectRoot, oldRelPath);
    std::ifstream in(oldFile, std::ios::binary);
    ThumbnailHeader header{};
    if (!in || !ReadHeader(in, header))
        return;
    std::vector<uint8_t> rgba(static_cast<size_t>(header.width) * header.height * 4u);
    const bool readOk = static_cast<bool>(in.read(reinterpret_cast<char*>(rgba.data()),
                                                  static_cast<std::streamsize>(rgba.size())));
    in.close();
    std::error_code ec;
    std::filesystem::remove(oldFile, ec);
    if (!readOk)
        return;
    // Pfad im Kopf mitziehen; Aenderungszeit bleibt (Umbenennen aendert sie nicht).
    header.assetRelPath = newRelPath;
    WriteThumbnailFile(ThumbnailCacheFile(projectRoot, newRelPath), header, rgba);
}

size_t CleanupThumbnailCache(const std::filesystem::path& projectRoot, uint64_t maxBytes)
{
    const std::filesystem::path dir = ThumbnailCacheDir(projectRoot);
    std::error_code ec;
    if (projectRoot.empty() || !std::filesystem::exists(dir, ec))
        return 0u;

    struct Kept
    {
        std::filesystem::path           file;
        uint64_t                        size = 0u;
        std::filesystem::file_time_type lastUsed{};
    };
    std::vector<Kept> kept;
    size_t removed = 0u;
    uint64_t total = 0u;
    for (const auto& entry : std::filesystem::directory_iterator(dir, ec))
    {
        if (!entry.is_regular_file(ec) || entry.path().extension() != ".kthumb")
            continue;
        ThumbnailHeader header{};
        bool valid = false;
        {
            std::ifstream in(entry.path(), std::ios::binary);
            valid = in && ReadHeader(in, header) &&
                    ThumbnailCacheFile(projectRoot, header.assetRelPath).filename() == entry.path().filename();
        }
        std::error_code existsEc;
        if (!valid || !std::filesystem::exists(projectRoot / "Assets" / PathFromUtf8(header.assetRelPath), existsEc))
        {
            std::error_code removeEc;
            if (std::filesystem::remove(entry.path(), removeEc))
                ++removed;
            continue;
        }
        Kept item{};
        item.file = entry.path();
        item.size = entry.file_size(ec);
        item.lastUsed = entry.last_write_time(ec);
        total += item.size;
        kept.push_back(std::move(item));
    }

    if (total > maxBytes)
    {
        // Am laengsten nicht benutzte zuerst.
        std::sort(kept.begin(), kept.end(), [](const Kept& a, const Kept& b) { return a.lastUsed < b.lastUsed; });
        for (const Kept& item : kept)
        {
            if (total <= maxBytes)
                break;
            std::error_code removeEc;
            if (std::filesystem::remove(item.file, removeEc))
            {
                total -= item.size;
                ++removed;
            }
        }
    }
    return removed;
}

std::vector<uint8_t> DownscaleRgba(const std::vector<uint8_t>& rgba, uint32_t width, uint32_t height,
                                   uint32_t factor)
{
    if (factor <= 1u || width < factor || height < factor || rgba.size() < static_cast<size_t>(width) * height * 4u)
        return rgba;
    const uint32_t outW = width / factor;
    const uint32_t outH = height / factor;
    std::vector<uint8_t> out(static_cast<size_t>(outW) * outH * 4u);
    const uint32_t samples = factor * factor;
    for (uint32_t y = 0u; y < outH; ++y)
    {
        for (uint32_t x = 0u; x < outW; ++x)
        {
            uint32_t sum[4] = {};
            for (uint32_t sy = 0u; sy < factor; ++sy)
            {
                const uint8_t* row = rgba.data() + (static_cast<size_t>(y * factor + sy) * width + x * factor) * 4u;
                for (uint32_t sx = 0u; sx < factor; ++sx)
                    for (uint32_t c = 0u; c < 4u; ++c)
                        sum[c] += row[sx * 4u + c];
            }
            uint8_t* dst = out.data() + (static_cast<size_t>(y) * outW + x) * 4u;
            for (uint32_t c = 0u; c < 4u; ++c)
                dst[c] = static_cast<uint8_t>((sum[c] + samples / 2u) / samples);
        }
    }
    return out;
}

} // namespace engine::renderer::addons::editor
