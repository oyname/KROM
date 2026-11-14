#include "addons/probes/ProbeData.hpp"

#include "core/PathUtf8.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <system_error>
#include <unordered_set>

namespace engine::addons::probes {
namespace {

constexpr char kMagic[4] = { 'K', 'P', 'R', 'B' };
constexpr uint64_t kMaxBlobBytes = 512ull * 1024ull * 1024ull;

uint64_t CubeBytes(uint32_t size) noexcept
{
    return 6ull * size * size * 4ull * sizeof(uint16_t);
}

uint64_t PrefilterBytes(uint32_t base, uint32_t mips) noexcept
{
    uint64_t total = 0u;
    uint32_t size = base;
    for (uint32_t m = 0u; m < mips; ++m)
    {
        total += CubeBytes(size);
        size = size > 1u ? size / 2u : 1u;
    }
    return total;
}

void SetError(std::string* out, const char* text)
{
    if (out)
        *out = text;
}

struct FileCloser
{
    void operator()(std::FILE* f) const noexcept { if (f) std::fclose(f); }
};
using FilePtr = std::unique_ptr<std::FILE, FileCloser>;

template <typename T>
bool WriteValue(std::FILE* f, const T& v) { return std::fwrite(&v, sizeof(T), 1u, f) == 1u; }

template <typename T>
bool ReadValue(std::FILE* f, T& v) { return std::fread(&v, sizeof(T), 1u, f) == 1u; }

bool WriteVec3(std::FILE* f, const math::Vec3& v)
{
    return WriteValue(f, v.x) && WriteValue(f, v.y) && WriteValue(f, v.z);
}

bool ReadVec3(std::FILE* f, math::Vec3& v)
{
    return ReadValue(f, v.x) && ReadValue(f, v.y) && ReadValue(f, v.z);
}

bool WriteBlob(std::FILE* f, const std::vector<uint8_t>& blob)
{
    const uint64_t n = blob.size();
    return WriteValue(f, n) && (n == 0u || std::fwrite(blob.data(), 1u, blob.size(), f) == blob.size());
}

bool ReadBlob(std::FILE* f, std::vector<uint8_t>& blob, uint64_t expected)
{
    uint64_t n = 0u;
    if (!ReadValue(f, n) || n != expected || n > kMaxBlobBytes)
        return false;
    blob.resize(static_cast<size_t>(n));
    return n == 0u || std::fread(blob.data(), 1u, blob.size(), f) == blob.size();
}

} // namespace

bool ProbeBakedData::IsValid() const noexcept
{
    return irradianceSize > 0u && prefilterBaseSize > 0u && prefilterMipCount > 0u
        && irradianceData.size() == CubeBytes(irradianceSize)
        && prefilterData.size() == PrefilterBytes(prefilterBaseSize, prefilterMipCount);
}

bool WriteProbeFile(const std::string& path, const ProbeBakedData& data, std::string* outError)
{
    if (!data.IsValid())
    {
        SetError(outError, "probe data incomplete");
        return false;
    }
    const std::filesystem::path fsPath = PathFromUtf8(path);
    std::error_code ec;
    if (fsPath.has_parent_path())
        std::filesystem::create_directories(fsPath.parent_path(), ec);

    FilePtr file(OpenFileUtf8(fsPath, "wb"));
    if (!file)
    {
        SetError(outError, "cannot open file for writing");
        return false;
    }
    std::FILE* f = file.get();
    const bool ok = std::fwrite(kMagic, 1u, sizeof(kMagic), f) == sizeof(kMagic)
        && WriteValue(f, kProbeFileVersion)
        && WriteValue(f, kProbeBakeVersion)
        && WriteValue(f, data.inputHash)
        && WriteVec3(f, data.capturePosition)
        && WriteVec3(f, data.extents)
        && WriteValue(f, data.irradianceSize)
        && WriteValue(f, data.prefilterBaseSize)
        && WriteValue(f, data.prefilterMipCount)
        && WriteBlob(f, data.irradianceData)
        && WriteBlob(f, data.prefilterData);
    if (!ok)
        SetError(outError, "write failed");
    return ok;
}

bool ReadProbeFileInputHash(const std::string& path, uint64_t& outInputHash)
{
    FilePtr file(OpenFileUtf8(PathFromUtf8(path), "rb"));
    if (!file)
        return false;
    char magic[4]{};
    uint32_t formatVersion = 0u;
    uint32_t bakeVersion = 0u;
    uint64_t hash = 0u;
    if (std::fread(magic, 1u, sizeof(magic), file.get()) != sizeof(magic) ||
        std::memcmp(magic, kMagic, sizeof(kMagic)) != 0 ||
        !ReadValue(file.get(), formatVersion) || formatVersion != kProbeFileVersion ||
        !ReadValue(file.get(), bakeVersion) || bakeVersion != kProbeBakeVersion ||
        !ReadValue(file.get(), hash))
        return false;
    outInputHash = hash;
    return true;
}

bool ReadProbeFile(const std::string& path, ProbeBakedData& outData, std::string* outError)
{
    FilePtr file(OpenFileUtf8(PathFromUtf8(path), "rb"));
    if (!file)
    {
        SetError(outError, "file not found");
        return false;
    }
    std::FILE* f = file.get();

    char magic[4]{};
    uint32_t formatVersion = 0u;
    uint32_t bakeVersion = 0u;
    if (std::fread(magic, 1u, sizeof(magic), f) != sizeof(magic) ||
        std::memcmp(magic, kMagic, sizeof(kMagic)) != 0)
    {
        SetError(outError, "not a .kprobe file");
        return false;
    }
    if (!ReadValue(f, formatVersion) || formatVersion != kProbeFileVersion)
    {
        SetError(outError, "unsupported format version");
        return false;
    }
    if (!ReadValue(f, bakeVersion) || bakeVersion != kProbeBakeVersion)
    {
        SetError(outError, "outdated bake version, bake again");
        return false;
    }

    ProbeBakedData data{};
    if (!ReadValue(f, data.inputHash) || !ReadVec3(f, data.capturePosition) || !ReadVec3(f, data.extents) ||
        !ReadValue(f, data.irradianceSize) || !ReadValue(f, data.prefilterBaseSize) ||
        !ReadValue(f, data.prefilterMipCount) ||
        data.irradianceSize == 0u || data.irradianceSize > 1024u ||
        data.prefilterBaseSize == 0u || data.prefilterBaseSize > 1024u ||
        data.prefilterMipCount == 0u || data.prefilterMipCount > 11u)
    {
        SetError(outError, "corrupt header");
        return false;
    }
    if (!ReadBlob(f, data.irradianceData, CubeBytes(data.irradianceSize)) ||
        !ReadBlob(f, data.prefilterData, PrefilterBytes(data.prefilterBaseSize, data.prefilterMipCount)))
    {
        SetError(outError, "corrupt data");
        return false;
    }
    outData = std::move(data);
    return true;
}

uint64_t ProbeIdFromFileName(const std::string& fileNameUtf8)
{
    constexpr size_t kHexDigits = 16u;
    const std::string suffix = ".kprobe";
    if (fileNameUtf8.size() != kHexDigits + suffix.size() ||
        fileNameUtf8.compare(kHexDigits, suffix.size(), suffix) != 0)
        return 0u;
    uint64_t id = 0u;
    for (size_t i = 0u; i < kHexDigits; ++i)
    {
        const char c = fileNameUtf8[i];
        uint64_t digit = 0u;
        if (c >= '0' && c <= '9') digit = static_cast<uint64_t>(c - '0');
        else if (c >= 'a' && c <= 'f') digit = static_cast<uint64_t>(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') digit = static_cast<uint64_t>(c - 'A' + 10);
        else return 0u;
        id = (id << 4u) | digit;
    }
    return id;
}

std::vector<std::string> DeleteUnusedProbeFiles(const std::string& assetsRootUtf8,
                                                const std::string& sceneNameUtf8,
                                                const std::vector<uint64_t>& usedIds)
{
    namespace fs = std::filesystem;
    std::vector<std::string> deleted;
    if (assetsRootUtf8.empty())
        return deleted;
    const fs::path assetsRoot = PathFromUtf8(assetsRootUtf8);
    const fs::path bakedDir = assetsRoot / "Baked" / PathFromUtf8(sceneNameUtf8.empty() ? "Scene" : sceneNameUtf8);
    std::error_code ec;
    if (!fs::is_directory(bakedDir, ec))
        return deleted;

    const std::unordered_set<uint64_t> used(usedIds.begin(), usedIds.end());
    std::vector<std::pair<fs::path, std::string>> candidates;   // path, id as written in files
    for (const auto& entry : fs::directory_iterator(bakedDir, ec))
    {
        std::error_code entryEc;
        if (!entry.is_regular_file(entryEc))
            continue;
        const std::string name = PathToUtf8(entry.path().filename());
        const uint64_t id = ProbeIdFromFileName(name);
        if (id != 0u && used.count(id) == 0u)
            candidates.emplace_back(entry.path(), name.substr(0u, 16u));
    }
    if (candidates.empty())
        return deleted;

    // Scene and prefab files store the id as these 16 hex digits.
    for (auto it = fs::recursive_directory_iterator(assetsRoot, fs::directory_options::skip_permission_denied, ec);
         it != fs::recursive_directory_iterator() && !candidates.empty(); it.increment(ec))
    {
        if (ec)
            break;
        std::error_code entryEc;
        if (!it->is_regular_file(entryEc))
            continue;
        const fs::path ext = it->path().extension();
        if (ext != ".json" && ext != ".kscene" && ext != ".prefab")
            continue;
        std::ifstream in(it->path(), std::ios::binary);
        const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        candidates.erase(std::remove_if(candidates.begin(), candidates.end(),
            [&](const auto& c) { return text.find(c.second) != std::string::npos; }), candidates.end());
    }

    for (const auto& [path, idText] : candidates)
    {
        std::error_code removeEc;
        if (fs::remove(path, removeEc))
        {
            std::string relative = PathToUtf8(path.lexically_relative(assetsRoot));
            std::replace(relative.begin(), relative.end(), '\\', '/');
            deleted.push_back(std::move(relative));
        }
    }
    return deleted;
}

} // namespace engine::addons::probes
