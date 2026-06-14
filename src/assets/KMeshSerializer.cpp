// =============================================================================
// KROM Engine — src/assets/KMeshSerializer.cpp
// Binary mesh cache: flat SubMeshData arrays -> interleaved .kmesh file.
// =============================================================================
#include "assets/KMeshSerializer.hpp"
#include "assets/KMeshFormat.hpp"
#include "renderer/RendererTypes.hpp"
#include "core/Debug.hpp"
#include <algorithm>
#include <cstring>
#include <fstream>
#include <limits>

namespace engine::assets {

using renderer::Format;
using renderer::VertexSemantic;

namespace {

struct AttrDesc
{
    VertexSemantic semantic;
    Format         format;
    uint32_t       offset;
};

struct MeshLayout
{
    std::vector<AttrDesc> attrs;
    uint32_t stride       = 0;
    uint32_t normalOff    = 0;
    uint32_t tangentOff   = 0;
    uint32_t uvOff        = 0;
    uint32_t colorOff     = 0;
    uint32_t boneWOff     = 0;
    uint32_t boneIOff     = 0;
    bool     hasNormal    = false;
    bool     hasTangent   = false;
    bool     hasUV        = false;
    bool     hasColor     = false;
    bool     hasBone      = false;
};

MeshLayout BuildLayout(const SubMeshData& sm)
{
    MeshLayout L;
    uint32_t off = 0;

    L.attrs.push_back({VertexSemantic::Position, Format::RGB32_FLOAT, off});
    off += 12;

    if (!sm.normals.empty()) {
        L.normalOff = off;
        L.attrs.push_back({VertexSemantic::Normal, Format::RGB32_FLOAT, off});
        off += 12;
        L.hasNormal = true;
    }
    if (!sm.tangents.empty()) {
        L.tangentOff = off;
        L.attrs.push_back({VertexSemantic::Tangent, Format::RGBA32_FLOAT, off});
        off += 16;
        L.hasTangent = true;
    }
    if (!sm.uvs.empty()) {
        L.uvOff = off;
        L.attrs.push_back({VertexSemantic::TexCoord0, Format::RG32_FLOAT, off});
        off += 8;
        L.hasUV = true;
    }
    if (!sm.colors.empty()) {
        L.colorOff = off;
        L.attrs.push_back({VertexSemantic::Color0, Format::RGBA32_FLOAT, off});
        off += 16;
        L.hasColor = true;
    }
    if (!sm.boneWeights.empty()) {
        L.boneWOff = off;
        L.attrs.push_back({VertexSemantic::BoneWeight, Format::RGBA32_FLOAT, off});
        off += 16;
        L.boneIOff = off;
        L.attrs.push_back({VertexSemantic::BoneIndex, Format::RGBA32_UINT, off});
        off += 16;
        L.hasBone = true;
    }

    L.stride = off;
    return L;
}

bool LayoutCompatible(const MeshLayout& L, const SubMeshData& sm)
{
    return (L.hasNormal  == !sm.normals.empty())
        && (L.hasTangent == !sm.tangents.empty())
        && (L.hasUV      == !sm.uvs.empty())
        && (L.hasColor   == !sm.colors.empty())
        && (L.hasBone    == (!sm.boneWeights.empty() || !sm.boneIndices.empty()));
}

bool ValidateSubmeshForLayout(const SubMeshData& sm, const MeshLayout& L)
{
    if (sm.positions.empty() || (sm.positions.size() % 3) != 0)
        return false;

    const size_t vc = sm.positions.size() / 3;
    if (vc > std::numeric_limits<uint32_t>::max())
        return false;

    return (!L.hasNormal  || sm.normals.size()     == vc * 3)
        && (!L.hasTangent || sm.tangents.size()    == vc * 4)
        && (!L.hasUV      || sm.uvs.size()         == vc * 2)
        && (!L.hasColor   || sm.colors.size()      == vc * 4)
        && (!L.hasBone    || (sm.boneWeights.size() == vc * 4
                           && sm.boneIndices.size() == vc * 4))
        && sm.indices.size() <= std::numeric_limits<uint32_t>::max();
}

uint32_t FormatSizeBytes(Format format)
{
    switch (format)
    {
    case Format::RG32_FLOAT:    return 8u;
    case Format::RGB32_FLOAT:   return 12u;
    case Format::RGBA32_FLOAT:  return 16u;
    case Format::RGBA32_UINT:   return 16u;
    default:                    return 0u;
    }
}

bool AddAttributeBounds(uint32_t offset, uint32_t size, uint32_t stride)
{
    return size != 0u && offset <= stride && size <= stride - offset;
}

} // anonymous namespace

// ---------------------------------------------------------------------------

std::filesystem::path KMeshCachePath(const std::filesystem::path& sourcePath)
{
    return sourcePath.parent_path() / (sourcePath.stem().string() + ".kmesh");
}

bool KMeshSave(const std::filesystem::path& cachePath, const std::vector<SubMeshData>& submeshes)
{
    if (submeshes.empty()) return false;

    const MeshLayout L = BuildLayout(submeshes[0]);

    for (size_t i = 0; i < submeshes.size(); ++i)
    {
        if (!LayoutCompatible(L, submeshes[i]))
        {
            Debug::LogWarning("KMeshSave: submesh %zu layout mismatch, skipping cache for '%s'",
                i, cachePath.string().c_str());
            return false;
        }
        if (!ValidateSubmeshForLayout(submeshes[i], L))
        {
            Debug::LogWarning("KMeshSave: submesh %zu has invalid channel lengths, skipping cache for '%s'",
                i, cachePath.string().c_str());
            return false;
        }
    }

    std::ofstream f(cachePath, std::ios::binary | std::ios::trunc);
    if (!f)
    {
        Debug::LogWarning("KMeshSave: cannot write '%s'", cachePath.string().c_str());
        return false;
    }

    // Header
    KMeshHeader hdr{};
    hdr.magic          = kKMeshMagic;
    hdr.version        = kKMeshVersion;
    hdr.submeshCount   = static_cast<uint32_t>(submeshes.size());
    hdr.attributeCount = static_cast<uint32_t>(L.attrs.size());
    hdr.vertexStride   = L.stride;
    f.write(reinterpret_cast<const char*>(&hdr), sizeof(hdr));

    // Attribute table
    for (const auto& a : L.attrs)
    {
        KMeshAttribute ka{};
        ka.semantic = static_cast<uint8_t>(a.semantic);
        ka.format   = static_cast<uint32_t>(a.format);
        ka.offset   = a.offset;
        f.write(reinterpret_cast<const char*>(&ka), sizeof(ka));
    }

    // Submesh descriptors
    for (const auto& sm : submeshes)
    {
        const uint32_t vc = static_cast<uint32_t>(sm.positions.size() / 3);

        float aabbMin[3] = { 1e30f,  1e30f,  1e30f };
        float aabbMax[3] = {-1e30f, -1e30f, -1e30f };
        for (uint32_t vi = 0; vi < vc; ++vi)
            for (int c = 0; c < 3; ++c)
            {
                aabbMin[c] = std::min(aabbMin[c], sm.positions[vi * 3 + c]);
                aabbMax[c] = std::max(aabbMax[c], sm.positions[vi * 3 + c]);
            }

        KMeshSubmeshDesc desc{};
        desc.vertexCount   = vc;
        desc.indexCount    = static_cast<uint32_t>(sm.indices.size());
        desc.materialIndex = sm.materialIndex;
        std::memcpy(desc.aabbMin, aabbMin, 12);
        std::memcpy(desc.aabbMax, aabbMax, 12);
        f.write(reinterpret_cast<const char*>(&desc), sizeof(desc));
    }

    // Interleaved vertex data + indices per submesh
    for (const auto& sm : submeshes)
    {
        const uint32_t vc = static_cast<uint32_t>(sm.positions.size() / 3);

        for (uint32_t vi = 0; vi < vc; ++vi)
        {
            f.write(reinterpret_cast<const char*>(&sm.positions[vi * 3]), 12);

            if (L.hasNormal)
                f.write(reinterpret_cast<const char*>(&sm.normals[vi * 3]), 12);

            if (L.hasTangent)
                f.write(reinterpret_cast<const char*>(&sm.tangents[vi * 4]), 16);

            if (L.hasUV)
                f.write(reinterpret_cast<const char*>(&sm.uvs[vi * 2]), 8);

            if (L.hasColor)
                f.write(reinterpret_cast<const char*>(&sm.colors[vi * 4]), 16);

            if (L.hasBone)
            {
                f.write(reinterpret_cast<const char*>(&sm.boneWeights[vi * 4]), 16);
                f.write(reinterpret_cast<const char*>(&sm.boneIndices[vi * 4]), 16);
            }
        }

        f.write(reinterpret_cast<const char*>(sm.indices.data()),
                static_cast<std::streamsize>(sm.indices.size() * sizeof(uint32_t)));
    }

    if (!f.good())
    {
        Debug::LogWarning("KMeshSave: write error for '%s'", cachePath.string().c_str());
        f.close();
        std::filesystem::remove(cachePath);
        return false;
    }

    Debug::LogVerbose("KMeshSave: cached '%s' (%zu submeshes, stride=%u)",
        cachePath.string().c_str(), submeshes.size(), L.stride);
    return true;
}

bool KMeshTryLoad(const std::filesystem::path& cachePath, std::vector<SubMeshData>& outSubmeshes)
{
    std::ifstream f(cachePath, std::ios::binary);
    if (!f) return false;

    KMeshHeader hdr{};
    f.read(reinterpret_cast<char*>(&hdr), sizeof(hdr));
    if (!f
        || hdr.magic   != kKMeshMagic
        || hdr.version != kKMeshVersion
        || hdr.submeshCount   == 0
        || hdr.attributeCount == 0
        || hdr.vertexStride   == 0)
    {
        Debug::LogWarning("KMeshTryLoad: bad header in '%s'", cachePath.string().c_str());
        return false;
    }

    // Attribute table
    uint32_t posOff = 0, normalOff = 0, tangentOff = 0, uvOff = 0;
    uint32_t colorOff  = 0, boneWOff   = 0, boneIOff = 0;
    bool hasPosition = false, hasNormal = false, hasTangent = false, hasUV = false;
    bool hasColor    = false, hasBoneWeight = false, hasBoneIndex = false;

    for (uint32_t i = 0; i < hdr.attributeCount; ++i)
    {
        KMeshAttribute ka{};
        f.read(reinterpret_cast<char*>(&ka), sizeof(ka));
        if (!f) return false;

        const auto sem = static_cast<VertexSemantic>(ka.semantic);
        const auto fmt = static_cast<Format>(ka.format);
        const uint32_t size = FormatSizeBytes(fmt);
        if (!AddAttributeBounds(ka.offset, size, hdr.vertexStride))
        {
            Debug::LogWarning("KMeshTryLoad: invalid attribute bounds in '%s'",
                cachePath.string().c_str());
            return false;
        }

        switch (sem)
        {
        case VertexSemantic::Position:
            if (fmt != Format::RGB32_FLOAT) return false;
            posOff = ka.offset; hasPosition = true; break;
        case VertexSemantic::Normal:
            if (fmt != Format::RGB32_FLOAT) return false;
            normalOff  = ka.offset; hasNormal  = true; break;
        case VertexSemantic::Tangent:
            if (fmt != Format::RGBA32_FLOAT) return false;
            tangentOff = ka.offset; hasTangent = true; break;
        case VertexSemantic::TexCoord0:
            if (fmt != Format::RG32_FLOAT) return false;
            uvOff      = ka.offset; hasUV      = true; break;
        case VertexSemantic::Color0:
            if (fmt != Format::RGBA32_FLOAT) return false;
            colorOff   = ka.offset; hasColor   = true; break;
        case VertexSemantic::BoneWeight:
            if (fmt != Format::RGBA32_FLOAT) return false;
            boneWOff   = ka.offset; hasBoneWeight = true; break;
        case VertexSemantic::BoneIndex:
            if (fmt != Format::RGBA32_UINT) return false;
            boneIOff   = ka.offset; hasBoneIndex = true; break;
        default: break;
        }
    }
    if (!hasPosition || hasBoneWeight != hasBoneIndex)
    {
        Debug::LogWarning("KMeshTryLoad: missing required attributes in '%s'",
            cachePath.string().c_str());
        return false;
    }
    const bool hasBone = hasBoneWeight && hasBoneIndex;

    // Submesh descriptors
    std::vector<KMeshSubmeshDesc> descs(hdr.submeshCount);
    for (uint32_t i = 0; i < hdr.submeshCount; ++i)
    {
        f.read(reinterpret_cast<char*>(&descs[i]), sizeof(KMeshSubmeshDesc));
        if (!f) return false;
    }

    const uint32_t stride = hdr.vertexStride;

    outSubmeshes.clear();
    outSubmeshes.reserve(hdr.submeshCount);

    for (uint32_t si = 0; si < hdr.submeshCount; ++si)
    {
        const auto& desc = descs[si];
        SubMeshData sm;
        sm.materialIndex = desc.materialIndex;

        sm.positions.resize(desc.vertexCount * 3);
        if (hasNormal)  sm.normals.resize(desc.vertexCount * 3);
        if (hasTangent) sm.tangents.resize(desc.vertexCount * 4);
        if (hasUV)      sm.uvs.resize(desc.vertexCount * 2);
        if (hasColor)   sm.colors.resize(desc.vertexCount * 4);
        if (hasBone)    { sm.boneWeights.resize(desc.vertexCount * 4);
                          sm.boneIndices.resize(desc.vertexCount * 4); }
        sm.indices.resize(desc.indexCount);

        // Block-read all vertex data at once — preserves interleaved bytes for GPU fast path.
        const size_t vtxBytes = static_cast<size_t>(desc.vertexCount) * stride;
        sm.rawInterleavedBytes.resize(vtxBytes);
        f.read(reinterpret_cast<char*>(sm.rawInterleavedBytes.data()),
               static_cast<std::streamsize>(vtxBytes));
        if (!f) return false;
        sm.rawVertexStride = stride;

        for (uint32_t vi = 0; vi < desc.vertexCount; ++vi)
        {
            const uint8_t* row = sm.rawInterleavedBytes.data() + vi * stride;

            std::memcpy(&sm.positions[vi * 3], row + posOff, 12);

            if (hasNormal)  std::memcpy(&sm.normals[vi * 3],     row + normalOff,  12);
            if (hasTangent) std::memcpy(&sm.tangents[vi * 4],    row + tangentOff, 16);
            if (hasUV)      std::memcpy(&sm.uvs[vi * 2],         row + uvOff,       8);
            if (hasColor)   std::memcpy(&sm.colors[vi * 4],      row + colorOff,   16);
            if (hasBone)  { std::memcpy(&sm.boneWeights[vi * 4], row + boneWOff,   16);
                            std::memcpy(&sm.boneIndices[vi * 4], row + boneIOff,    16); }
        }

        f.read(reinterpret_cast<char*>(sm.indices.data()),
               static_cast<std::streamsize>(desc.indexCount * sizeof(uint32_t)));
        if (!f) return false;

        outSubmeshes.push_back(std::move(sm));
    }

    Debug::LogVerbose("KMeshTryLoad: loaded '%s' (%u submeshes)",
        cachePath.string().c_str(), hdr.submeshCount);
    return true;
}

} // namespace engine::assets
