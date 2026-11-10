#pragma once
#include "krom.h"
#include "assets/AssetRegistry.hpp"

#include <cstdint>
#include <unordered_map>

// Procedural surfaces of the krom.h wrapper (CreateSurface, AddVertex, ...).
// The functions only see the State fields they need, passed in as SurfaceContext.
namespace Krom::internal {

struct SurfaceRecord
{
    LPENTITY entity = NULL_LPENTITY;
    LPMESH mesh = NULL_MESH;
    uint32_t submeshIndex = 0u;
};

using SurfaceMap = std::unordered_map<uint32_t, SurfaceRecord>;
using RuntimeMaterialCache = std::unordered_map<uint64_t, LPMATERIAL>;

// References into the wrapper State; built per call, never stored.
struct SurfaceContext
{
    SurfaceMap& surfaces;
    uint32_t& nextSurfaceId;
    engine::assets::AssetRegistry& assetRegistry;
    engine::ecs::World* world = nullptr;
};

[[nodiscard]] engine::assets::SubMeshData* GetSurfaceSubmesh(const SurfaceContext& ctx, LPSURFACE surface);
[[nodiscard]] const SurfaceRecord* GetSurfaceRecord(const SurfaceContext& ctx, LPSURFACE surface);
// Marks the mesh for re-upload, drops runtime materials built for it and refreshes the entity bounds.
void TouchSurface(const SurfaceContext& ctx, RuntimeMaterialCache& runtimeMaterialCache, LPSURFACE surface);

LPSURFACE CreateSurface(const SurfaceContext& ctx, LPENTITY meshEntity);
int AddVertex(const SurfaceContext& ctx, RuntimeMaterialCache& runtimeMaterialCache,
              LPSURFACE surface, float x, float y, float z, float u, float v);
bool AddTriangle(const SurfaceContext& ctx, RuntimeMaterialCache& runtimeMaterialCache,
                 LPSURFACE surface, int v0, int v1, int v2);
bool VertexNormal(const SurfaceContext& ctx, RuntimeMaterialCache& runtimeMaterialCache,
                  LPSURFACE surface, int index, float nx, float ny, float nz);
bool VertexTexCoords(const SurfaceContext& ctx, RuntimeMaterialCache& runtimeMaterialCache,
                     LPSURFACE surface, int index, float u, float v);
int CountVertices(const SurfaceContext& ctx, LPSURFACE surface);
int CountTriangles(const SurfaceContext& ctx, LPSURFACE surface);

} // namespace Krom::internal
