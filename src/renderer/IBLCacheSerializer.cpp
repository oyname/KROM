#include "renderer/IBLCacheSerializer.hpp"
#include "core/Debug.hpp"
#include <cstdio>
#include <cstring>
#include <filesystem>

namespace engine::renderer {
    namespace {

        static constexpr uint32_t kFileVersion = 1u;
        static constexpr uint32_t kMagic       = 0x434C4249u; // 'IBLC'

        // =====================================================================
        // Binary header — layout-stable, packed to exactly 64 bytes.
        // =====================================================================
#pragma pack(push, 1)
        struct FileHeader
        {
            uint32_t magic;            //  0  'IBLC'
            uint32_t fileVersion;      //  4  kFileVersion
            uint32_t bakeVersion;      //  8  IBLCacheSerializer::kBakeVersion
            uint32_t environmentSize;  // 12
            uint32_t irradianceSize;   // 16
            uint32_t prefilterSize;    // 20
            uint32_t mipCount;         // 24
            uint32_t _pad;             // 28
            uint64_t sourceHash;       // 32
            uint64_t envDataOffset;    // 40
            uint64_t irrDataOffset;    // 48
            uint64_t prefDataOffset;   // 56
            // Total: 64 bytes
        };
#pragma pack(pop)
        static_assert(sizeof(FileHeader) == 64u, "FileHeader must be exactly 64 bytes");

        [[nodiscard]] static bool ReadAll(std::FILE* f, void* dst, size_t bytes) noexcept
        { return std::fread(dst, 1u, bytes, f) == bytes; }

        [[nodiscard]] static bool WriteAll(std::FILE* f, const void* src, size_t bytes) noexcept
        { return std::fwrite(src, 1u, bytes, f) == bytes; }

        [[nodiscard]] static bool SeekTo(std::FILE* f, uint64_t offset) noexcept
        {
#ifdef _WIN32
            return _fseeki64(f, static_cast<__int64>(offset), SEEK_SET) == 0;
#else
            return std::fseek(f, static_cast<long>(offset), SEEK_SET) == 0;
#endif
        }

        // Describe why a header is invalid (single mismatch reason for logging).
        [[nodiscard]] static const char* HeaderInvalidReason(const FileHeader& hdr,
                                                              const IBLCacheKey& key) noexcept
        {
            if (hdr.magic       != kMagic)                  return "magic mismatch";
            if (hdr.fileVersion != kFileVersion)             return "file format version mismatch";
            if (hdr.bakeVersion != IBLCacheSerializer::kBakeVersion) return "bake version mismatch";
            if (hdr.sourceHash  != key.sourceHash)          return "source hash mismatch";
            if (hdr.environmentSize != key.environmentSize) return "environment size mismatch";
            if (hdr.irradianceSize  != key.irradianceSize)  return "irradiance size mismatch";
            if (hdr.prefilterSize   != key.prefilterSize)   return "prefilter size mismatch";
            if (hdr.mipCount        != key.mipCount)        return "mip count mismatch";
            return nullptr;
        }


        [[nodiscard]] static bool ReadHeaderOnly(const std::string& path, FileHeader& outHeader) noexcept
        {
            std::FILE* f = std::fopen(path.c_str(), "rb");
            if (!f) return false;
            const bool ok = ReadAll(f, &outHeader, sizeof(outHeader));
            std::fclose(f);
            return ok;
        }

        [[nodiscard]] static bool HeaderLooksCompatibleForAnySource(const FileHeader& hdr) noexcept
        {
            return hdr.magic == kMagic
                && hdr.fileVersion == kFileVersion
                && hdr.bakeVersion == IBLCacheSerializer::kBakeVersion;
        }

    } // namespace

    // =========================================================================
    // IBLCacheSerializer — public API
    // =========================================================================

    bool IBLCacheSerializer::IsValid(const std::string& path, const IBLCacheKey& key) noexcept
    {
        std::FILE* f = std::fopen(path.c_str(), "rb");
        if (!f) return false;   // file does not exist — caller handles the MISS log

        FileHeader hdr{};
        const bool read = ReadAll(f, &hdr, sizeof(hdr));
        std::fclose(f);

        if (!read)
        {
            Debug::LogWarning("IBLCacheSerializer: truncated header '%s'", path.c_str());
            return false;
        }

        const char* reason = HeaderInvalidReason(hdr, key);
        if (reason)
        {
            Debug::LogWarning("IBLCache: INVALID '%s' — %s", path.c_str(), reason);
            return false;
        }
        return true;
    }

    bool IBLCacheSerializer::Read(const std::string& path, IBLBakedData& outData)
    {
        std::FILE* f = std::fopen(path.c_str(), "rb");
        if (!f)
        {
            Debug::LogError("IBLCacheSerializer: cannot open '%s'", path.c_str());
            return false;
        }

        FileHeader hdr{};
        if (!ReadAll(f, &hdr, sizeof(hdr)))
        {
            std::fclose(f);
            Debug::LogError("IBLCacheSerializer: truncated header '%s'", path.c_str());
            return false;
        }
        if (hdr.magic != kMagic || hdr.fileVersion != kFileVersion)
        {
            std::fclose(f);
            Debug::LogError("IBLCacheSerializer: bad magic/version '%s'", path.c_str());
            return false;
        }

        outData.environmentSize   = hdr.environmentSize;
        outData.irradianceSize    = hdr.irradianceSize;
        outData.prefilterBaseSize = hdr.prefilterSize;
        outData.prefilterMipCount = hdr.mipCount;

        const size_t envBytes = static_cast<size_t>(6u) * hdr.environmentSize * hdr.environmentSize * 4u * 2u;
        const size_t irrBytes = static_cast<size_t>(6u) * hdr.irradianceSize  * hdr.irradianceSize  * 4u * 2u;
        size_t prefBytes = 0u;
        { uint32_t sz = hdr.prefilterSize;
          for (uint32_t m = 0u; m < hdr.mipCount; ++m) { prefBytes += static_cast<size_t>(6u)*sz*sz*4u*2u; sz = std::max(sz/2u,1u); } }

        outData.environmentData.resize(envBytes);
        outData.irradianceData.resize(irrBytes);
        outData.prefilterData.resize(prefBytes);

        bool ok = true;
        ok = ok && SeekTo(f, hdr.envDataOffset)  && ReadAll(f, outData.environmentData.data(), envBytes);
        ok = ok && SeekTo(f, hdr.irrDataOffset)  && ReadAll(f, outData.irradianceData.data(),  irrBytes);
        ok = ok && SeekTo(f, hdr.prefDataOffset) && ReadAll(f, outData.prefilterData.data(),   prefBytes);
        std::fclose(f);

        if (!ok)
        {
            outData = {};
            Debug::LogError("IBLCacheSerializer: data read failed '%s'", path.c_str());
            return false;
        }
        return true;
    }

    bool IBLCacheSerializer::Write(const std::string& path,
                                    const IBLBakedData& data,
                                    const IBLCacheKey& key)
    {
        if (!data.IsValid())
        {
            Debug::LogError("IBLCacheSerializer: refusing to write invalid IBLBakedData to '%s'",
                            path.c_str());
            return false;
        }

        {
            std::error_code ec;
            std::filesystem::create_directories(std::filesystem::path(path).parent_path(), ec);
            if (ec)
                Debug::LogWarning("IBLCacheSerializer: create_directories: %s", ec.message().c_str());
        }

        std::FILE* f = std::fopen(path.c_str(), "wb");
        if (!f)
        {
            Debug::LogError("IBLCache: write FAILED — cannot create '%s'", path.c_str());
            return false;
        }

        FileHeader hdr{};
        hdr.magic           = kMagic;
        hdr.fileVersion     = kFileVersion;
        hdr.bakeVersion     = kBakeVersion;
        hdr.environmentSize = key.environmentSize;
        hdr.irradianceSize  = key.irradianceSize;
        hdr.prefilterSize   = key.prefilterSize;
        hdr.mipCount        = key.mipCount;
        hdr.sourceHash      = key.sourceHash;
        hdr.envDataOffset   = sizeof(FileHeader);
        hdr.irrDataOffset   = hdr.envDataOffset  + static_cast<uint64_t>(data.environmentData.size());
        hdr.prefDataOffset  = hdr.irrDataOffset  + static_cast<uint64_t>(data.irradianceData.size());

        bool ok = true;
        ok = ok && WriteAll(f, &hdr,                         sizeof(hdr));
        ok = ok && WriteAll(f, data.environmentData.data(),  data.environmentData.size());
        ok = ok && WriteAll(f, data.irradianceData.data(),   data.irradianceData.size());
        ok = ok && WriteAll(f, data.prefilterData.data(),    data.prefilterData.size());
        std::fclose(f);

        if (!ok)
        {
            std::remove(path.c_str());
            Debug::LogError("IBLCache: write FAILED — I/O error '%s'", path.c_str());
            return false;
        }

        const auto fileSize = static_cast<uint64_t>(
            data.environmentData.size() + data.irradianceData.size()
            + data.prefilterData.size() + sizeof(FileHeader));
        Debug::Log("IBLCache: write OK   '%s' (%.1f KB)", path.c_str(),
                   static_cast<float>(fileSize) / 1024.f);
        return true;
    }

    uint32_t IBLCacheSerializer::CleanupDirectory(const std::string& directory) noexcept
    {
        namespace fs = std::filesystem;

        std::error_code ec;
        if (directory.empty())
            return 0u;
        if (!fs::exists(directory, ec) || ec)
            return 0u;

        uint32_t removed = 0u;
        for (fs::directory_iterator it(fs::path(directory), ec); !ec && it != fs::directory_iterator(); it.increment(ec))
        {
            const fs::directory_entry& entry = *it;
            if (!entry.is_regular_file(ec) || ec)
                continue;
            if (entry.path().extension() != ".iblcache")
                continue;

            FileHeader hdr{};
            const std::string path = entry.path().string();
            const bool haveHeader = ReadHeaderOnly(path, hdr);
            if (haveHeader && HeaderLooksCompatibleForAnySource(hdr))
                continue;

            std::error_code removeEc;
            if (fs::remove(entry.path(), removeEc) && !removeEc)
            {
                ++removed;
                if (!haveHeader)
                    Debug::LogWarning("IBLCache: cleanup removed malformed file '%s'", path.c_str());
                else
                    Debug::LogWarning("IBLCache: cleanup removed incompatible file '%s'", path.c_str());
            }
            else if (removeEc)
            {
                Debug::LogWarning("IBLCache: cleanup failed to remove '%s': %s",
                                  path.c_str(), removeEc.message().c_str());
            }
        }
        return removed;
    }


} // namespace engine::renderer
