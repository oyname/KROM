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

} // namespace Krom::internal
