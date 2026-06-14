#pragma once
#include <cstdint>
#include <cstddef>

namespace engine::assets {

// Binary mesh cache format — .kmesh
//
// File layout:
//   [KMeshHeader]                          (32 bytes, fixed)
//   [KMeshAttribute × attributeCount]      (12 bytes each)
//   [KMeshSubmeshDesc × submeshCount]      (40 bytes each)
//   For each submesh:
//     [uint8_t × vertexCount * vertexStride]   interleaved vertex data
//     [uint32_t × indexCount]                  index data

static constexpr uint32_t kKMeshMagic   = 0x4B4D5348u; // 'K','M','S','H'
static constexpr uint32_t kKMeshVersion = 1u;

struct KMeshHeader
{
    uint32_t magic;
    uint32_t version;
    uint32_t submeshCount;
    uint32_t attributeCount;
    uint32_t vertexStride;
    uint32_t reserved[3];
};
static_assert(sizeof(KMeshHeader) == 32);

// One entry per vertex attribute in the shared layout.
struct KMeshAttribute
{
    uint8_t  semantic;       // renderer::VertexSemantic cast to uint8_t
    uint8_t  pad[3];
    uint32_t format;         // renderer::Format cast to uint32_t
    uint32_t offset;         // byte offset within interleaved vertex
};
static_assert(sizeof(KMeshAttribute) == 12);

struct KMeshSubmeshDesc
{
    uint32_t vertexCount;
    uint32_t indexCount;
    uint32_t materialIndex;
    uint32_t pad;
    float    aabbMin[3];
    float    aabbMax[3];
};
static_assert(sizeof(KMeshSubmeshDesc) == 40);

} // namespace engine::assets
